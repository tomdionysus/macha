// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/node_services.hpp"

#include "log.hpp"
#include "media/media_engine.hpp"
#include "metadata/namespace_control_store.hpp"
#include "supervised.hpp"

#include <algorithm>
#include <set>

namespace macha {

namespace {

std::shared_ptr<MediaEngine> media_engine_for(const Config& config) {
    if (!config.streaming.enabled)
        return {};
    return make_libav_media_engine(config.streaming);
}

} // namespace

NodeServices::NodeServices(NodeRuntime& node, SubsystemRegistry& registry, MaintenancePort& port,
                           std::function<void(ServiceEvent)> signal_maintenance,
                           NodeServicesInstruments instruments)
    : node_(node), registry_(registry), port_(port),
      signal_maintenance_(std::move(signal_maintenance)), instruments_(std::move(instruments)),
      store_(node_, DistributedStoreOptions{node_.config().state_path / "repair" / "push-position",
                                            instruments_.trace}),
      // The guard reaches the catalogue, declared after this: it runs only
      // for a commit, which nothing makes before construction finishes.
      metadata_(node_, &store_,
                [this](const MetadataPublicationContext& context) {
                    retain_metadata_publication(context);
                }),
      catalogue_(node_, store_, metadata_), filesystem_(node_, store_, metadata_, &playback_),
      catalogue_hints_(node_.config().state_path), media_engine_(media_engine_for(node_.config())),
      media_information_(filesystem_, catalogue_, media_engine_, node_.config().state_path),
      scanner_(node_, filesystem_, catalogue_, catalogue_hints_, node_.config().catalogue.scanner,
               std::unique_ptr<HttpClient>{}, std::chrono::seconds(5), media_engine_,
               &media_information_),
      hydration_(store_, playback_, filesystem_, catalogue_, node_.config().hydration,
                 node_.config().read_ahead_extents),
      ingest_(node_, filesystem_, catalogue_hints_, node_.config().ingest, &media_information_),
      torrent_search_(node_.config().torrent), cluster_jobs_(node_, ingest_, registry_),
      torrent_coordinator_(node_, metadata_, registry_, cluster_jobs_, node_.config().state_path),
      acquisition_api_(ingest_, registry_, torrent_search_, cluster_jobs_, torrent_coordinator_),
      catalogue_api_(
          catalogue_, catalogue_hints_,
          [this](const std::vector<std::string>& media_ids) {
              scanner_.request_media_rescan(media_ids);
          },
          [this](const std::vector<std::string>& media_ids) {
              return scanner_.request_media_profiles(media_ids);
          },
          [this](const std::string& media_id) -> std::optional<MediaProbeResult> {
              // Facts on demand: resolve at foreground priority and persist,
              // so a client asking what a file is never gets "not yet".
              auto found = filesystem_.find_media(media_id);
              if (!found)
                  return std::nullopt;
              return media_information_.resolve_playback(media_id, found->first, found->second,
                                                          Clock::now() + std::chrono::seconds(30));
          },
          node_.config().catalogue.api.artwork_capability_ttl,
          [this](const std::string& media_id) -> std::optional<uint64_t> {
              auto found = filesystem_.find_media(media_id);
              if (!found)
                  return std::nullopt;
              return found->second.size;
          },
          [this](const std::string& media_id) -> std::optional<Bytes> {
              return media_information_.keyframe_index(media_id,
                                                       Clock::now() + std::chrono::seconds(30));
          }),
      manage_api_(node_, metadata_, filesystem_, catalogue_, catalogue_hints_, scanner_),
      streaming_(filesystem_, catalogue_, node_.config().catalogue.api, node_.config().streaming,
                 media_engine_,
                 [this](const std::vector<std::string>& media_ids) {
                     return scanner_.request_media_profiles(media_ids);
                 },
                 &media_information_),
      subsystems_(node_.config().plugin_path.value_or(std::filesystem::path{})),
      ledger_(node_.claims(), node_.local_store(), node_.control_store()),
      horizon_builder_(filesystem_, catalogue_, node_, store_),
      maintenance_(MaintenanceDependencies{node_, store_, metadata_, metadata_, catalogue_,
                                           horizon_builder_, ledger_, port_, instruments_.clock,
                                           instruments_.trace, instruments_.maintenance_stage_hook,
                                           instruments_.constructed}) {
    note("services constructed");
}

void NodeServices::reconfigure(const Config& updated) {
    scanner_.reconfigure(updated.catalogue.scanner);
    hydration_.reconfigure(updated.hydration, updated.read_ahead_extents);
    ingest_.reconfigure(updated.ingest);
    cluster_jobs_.reconfigure(updated.torrent);
    streaming_.reconfigure(updated.streaming);
}

NodeServices::~NodeServices() {
    stop();
}

void NodeServices::start() {
    started_ = true;
    note("start media-information");
    media_information_.start();
    note("start scanner");
    scanner_.start();
    note("start hydration");
    hydration_.start();
    note("start ingest");
    ingest_.start();
    note("start cluster-jobs");
    cluster_jobs_.start();
    note("start torrent-coordinator");
    torrent_coordinator_.start();
    note("start streaming");
    streaming_.start();
    // A plugin's context is a view of this graph: the services it may use.
    SubsystemContext context;
    context.config = &node_.config();
    context.node = &node_;
    context.ingest = &ingest_;
    context.registry = &registry_;
    context.filesystem = &filesystem_;
    context.hydration = &hydration_;
    note("start subsystems");
    subsystems_.start(context);
    for (const auto& subsystem : subsystems_.statuses())
        note("subsystem " + subsystem.name);
    note("start maintenance");
    maintenance_.start();
}

void NodeServices::request_stop() {
    if (!started_ || stopped_)
        return;
    // A FUSE publication in flight ends promptly rather than holding the
    // stop on a slow peer.
    note("cancel-io filesystem");
    filesystem_.request_io_cancellation();
    note("request_stop maintenance");
    maintenance_.request_stop();
    note("request_stop streaming");
    streaming_.request_stop();
    note("request_stop manage-api");
    manage_api_.request_stop();
    note("request_stop ingest");
    ingest_.request_stop();
    note("request_stop hydration");
    hydration_.request_stop();
    note("request_stop scanner");
    scanner_.request_stop();
    note("request_stop media-information");
    media_information_.request_stop();
}

// Dependants stop before providers. The graph leaves free whether the
// producers -- the plugins, the torrent coordinator and the cluster job view
// -- stop before or after maintenance; they stop first, while the node still
// admits DATA work and outbound RPC (a plugin writes into the store until it
// is stopped), and then outbound calls are cancelled, so the joins that
// follow, maintenance's among them, cannot wait on a slow peer.
void NodeServices::stop() {
    if (!started_ || stopped_)
        return;
    request_stop();
    stopped_ = true;
    note("stop subsystems");
    subsystems_.stop();
    note("stop torrent-coordinator");
    torrent_coordinator_.stop();
    note("stop cluster-jobs");
    cluster_jobs_.stop();
    note("cancel-io outbound-rpc");
    node_.cancel_outbound_calls();
    note("stop maintenance");
    maintenance_.stop();
    note("stop streaming");
    streaming_.stop();
    note("stop manage-api");
    manage_api_.stop();
    note("stop ingest");
    ingest_.stop();
    note("stop hydration");
    hydration_.stop();
    note("stop scanner");
    scanner_.stop();
    note("stop media-information");
    media_information_.stop();
}

void NodeServices::retain_metadata_publication(const MetadataPublicationContext& context) {
    const RetentionDot dot{context.origin, context.sequence};
    std::vector<ObjectId> data;
    std::vector<ObjectId> control;
    // Phase timing for the pre-publication barrier: on the live cluster
    // (2026-09-07) mutations spent 9-15 s here while retain_data() itself
    // reported nothing over 250 ms, so the seconds were in the collection
    // step (a full parent snapshot decode, catalogue root diffs) or the
    // CONTROL claim. Name the phase instead of guessing.
    const auto barrier_started = Clock::now();
    uint64_t decode_ms = 0, collect_ms = 0, catalogue_ms = 0, data_ms = 0, control_ms = 0;
    const auto since_ms = [](Clock::time_point t) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
    };
    const auto report = [&](const char* outcome) {
        const auto total = since_ms(barrier_started);
        const bool failed = std::string_view(outcome) != "ok";
        if ((total >= 250 || failed) && Log::enabled(LogLevel::debug))
            Log::debug("metadata retention barrier dot=" + to_string(dot.origin).substr(0, 6) +
                       ":" + std::to_string(dot.sequence) + " total_ms=" + std::to_string(total) +
                       " decode_ms=" + std::to_string(decode_ms) +
                       " collect_ms=" + std::to_string(collect_ms) +
                       " catalogue_ms=" + std::to_string(catalogue_ms) +
                       " data_ms=" + std::to_string(data_ms) +
                       " control_ms=" + std::to_string(control_ms) +
                       " data_objects=" + std::to_string(data.size()) +
                       " control_objects=" + std::to_string(control.size()) +
                       " outcome=" + outcome);
    };

    auto add_entry = [&](const FsEntry& entry) {
        if (entry.type != EntryType::file)
            return;
        for (const auto& extent : entry.extents)
            if (!extent.hole)
                data.push_back(extent.id);
    };

    const auto decode_started = Clock::now();
    auto before = decode_snapshot(context.parent.payload);
    decode_ms = since_ms(decode_started);
    const auto collect_started = Clock::now();
    // Both namespaces below may be trees rather than maps, so every read of
    // them goes through the namespace primitives. These are retention claims:
    // an entry missed here is an object that never acquires liveness evidence
    // and can be collected while it is still referenced.
    auto namespace_nodes = ControlNamespaceNodeStore::for_reading(node_, store_);
    const bool establish_baseline =
        !before.retention_baseline_complete && context.proposed.retention_baseline_complete;
    if (establish_baseline) {
        // Migration safety: before protocol-20 retention-aware GC is enabled for
        // an upgraded namespace, every object reachable from the reconciled
        // migration view must acquire physical liveness evidence. This is a
        // one-time potentially-large publication; normal partition-time GC does
        // not require global convergence after the baseline exists.
        for_each_namespace_entry(context.proposed, &namespace_nodes,
                                 [&](const std::string&, const FsEntry& entry) {
                                     add_entry(entry);
                                 });
        const auto conflict_extents = metadata_conflict_extent_roots(context.proposed);
        data.insert(data.end(), conflict_extents.begin(), conflict_extents.end());
        for (const auto& root : metadata_catalogue_root_set(context.proposed)) {
            auto objects = catalogue_.retention_objects(std::nullopt, root);
            data.insert(data.end(), objects.data.begin(), objects.data.end());
            control.insert(control.end(), objects.control.begin(), objects.control.end());
        }
        // The namespace tree is control objects too, every one of them
        // reachable from the root and none of them from anything else.
        if (context.proposed.namespace_root)
            collect_namespace_tree_nodes(*context.proposed.namespace_root, namespace_nodes,
                                         control);
    } else {
        if (context.delta) {
            for (const auto& [_, entry] : context.delta->upsert_entries)
                add_entry(entry);
            // A DLT8 append carries only the new extents, but the semantic
            // change is to the whole file: like the upsert it replaces, it
            // needs a fresh retention dot on every extent the file now holds
            // (a touch that carries no extents included), or a concurrent
            // delete could release the inherited claim.
            for (const auto& [path, _] : context.delta->append_entries) {
                if (auto found = namespace_entry(context.proposed, &namespace_nodes, path))
                    add_entry(*found);
            }
        } else {
            // The no-delta path: rediscover what changed by comparing the
            // two namespaces entry by entry. Under trees that is a walk of
            // one plus a lookup per path in the other, which is worse than
            // the two map walks it replaces -- and it is exactly the cost
            // Stage C removes by carrying the change set into the commit
            // instead. This branch is the fallback; every ordinary mutation
            // arrives with a delta and takes the cheap path above.
            for_each_namespace_entry(
                context.proposed, &namespace_nodes,
                [&](const std::string& path, const FsEntry& entry) {
                    const auto found = namespace_entry(before, &namespace_nodes, path);
                    if (!found || *found != entry)
                        add_entry(entry);
                });
        }

        // The tree nodes this commit introduced need claims exactly as the
        // catalogue shards it changed do. Without them the nodes that say
        // where every file lives are unreferenced control objects to the
        // collector, which is what they were from the cutover until this line.
        if (context.proposed.namespace_root &&
            before.namespace_root != context.proposed.namespace_root)
            collect_namespace_tree_changes(before.namespace_root, *context.proposed.namespace_root,
                                           namespace_nodes, control);

        const bool catalogue_changed =
            context.delta ? context.delta->catalogue != CatalogueDelta::unchanged
                          : before.catalogue_root != context.proposed.catalogue_root;
        if (catalogue_changed && context.proposed.catalogue_root) {
            const auto catalogue_started = Clock::now();
            auto objects = catalogue_.retention_objects(before.catalogue_root,
                                                         context.proposed.catalogue_root);
            catalogue_ms += since_ms(catalogue_started);
            data.insert(data.end(), objects.data.begin(), objects.data.end());
            control.insert(control.end(), objects.control.begin(), objects.control.end());
        }
        // A reconciliation may preserve catalogue conflict alternatives which
        // are not the effective root. New alternatives must be retained before
        // the merge commit can become accepted.
        if (!context.delta) {
            auto before_roots = metadata_catalogue_root_set(before);
            auto after_roots = metadata_catalogue_root_set(context.proposed);
            for (const auto& root : after_roots) {
                if (before_roots.contains(root))
                    continue;
                auto objects = catalogue_.retention_objects(std::nullopt, root);
                data.insert(data.end(), objects.data.begin(), objects.data.end());
                control.insert(control.end(), objects.control.begin(), objects.control.end());
            }
        }
    }

    std::sort(data.begin(), data.end());
    data.erase(std::unique(data.begin(), data.end()), data.end());
    std::sort(control.begin(), control.end());
    control.erase(std::unique(control.begin(), control.end()), control.end());
    collect_ms = since_ms(collect_started) - catalogue_ms;

    const auto data_started = Clock::now();
    const bool data_ok = data.empty() || store_.retain_data(data, dot);
    data_ms = since_ms(data_started);
    if (!data_ok) {
        report("data-floor-unavailable");
        throw MetadataNotReady("DATA retention floor unavailable before metadata publication");
    }
    const auto control_started = Clock::now();
    const bool control_ok =
        control.empty() ||
        store_.retain_control(control, dot, context.proposed.metadata_write_replicas_required);
    control_ms = since_ms(control_started);
    if (!control_ok) {
        report("control-floor-unavailable");
        throw MetadataNotReady("CONTROL retention floor unavailable before metadata publication");
    }
    report("ok");
    signal_maintenance_(ServiceEvent::storage);
}


} // namespace macha
