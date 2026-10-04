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

NodeServices::NodeServices(NodeRuntime& node, NodeResources& resources, LocalState& local,
                           MetadataServer& metadata_server, MessageRoutes& routes,
                           SubsystemRegistry& registry, MaintenancePort& port,
                           NodeServicesInstruments instruments)
    : node_(node), resources_(resources), local_(local), metadata_server_(metadata_server),
      routes_(routes), registry_(registry), port_(port), instruments_(std::move(instruments)),
      store_(node_, local_, resources_.activity, resources_.data, resources_.memory,
             resources_.events,
             DistributedStoreOptions{node_.config().state_path / "repair" / "push-position",
                                     instruments_.trace}),
      // The guard reaches the catalogue, declared after this: it runs only
      // for a commit, which nothing makes before construction finishes.
      metadata_(node_, local_, metadata_server_, &store_,
                [this](const MetadataPublicationContext& context) {
                    retain_metadata_publication(context);
                },
                steady_time_source()),
      ledger_(local_.retention(), local_.data(), local_.control()),
      availability_(node_, local_, store_, ledger_, resources_.events, routes_,
                    node_.config().state_path / "availability" / "last-survey.bin"),
      catalogue_(node_, local_, metadata_server_, store_, metadata_, ledger_),
      filesystem_(node_.config(), node_.node_id(), node_.membership(), local_, metadata_server_,
                  store_, metadata_, resources_.memory, &playback_),
      catalogue_hints_(node_.config().state_path), media_engine_(media_engine_for(node_.config())),
      media_information_(filesystem_, catalogue_, media_engine_, node_.config().state_path),
      scanner_(node_, metadata_server_, filesystem_, catalogue_, catalogue_hints_,
               node_.config().catalogue.scanner, std::unique_ptr<HttpClient>{},
               std::chrono::seconds(5), media_engine_, &media_information_),
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
          },
          [this] { return availability_.snapshot(); }),
      manage_api_(node_, metadata_, filesystem_, catalogue_, catalogue_hints_, scanner_),
      streaming_(
          filesystem_, resources_.transcode_rates, resources_.memory, catalogue_,
          node_.config().catalogue.api, node_.config().streaming, media_engine_,
          [this](const std::vector<std::string>& media_ids) {
              return scanner_.request_media_profiles(media_ids);
          },
          &media_information_,
          [this](Json::Object& entry, std::string_view media_id) {
              put_media_availability(entry, availability_.snapshot().get(), media_id);
          },
          steady_time_source()),
      subsystems_(node_.config().plugin_path.value_or(std::filesystem::path{})),
      horizon_builder_(filesystem_, catalogue_, local_.control(), store_),
      files_api_(filesystem_, availability_),
      maintenance_(
          MaintenanceDependencies{node_, local_, metadata_server_, store_, metadata_, metadata_,
                                  catalogue_, horizon_builder_, ledger_, availability_,
                                  media_information_,
                                  resources_.events, port_, instruments_.clock, instruments_.trace,
                                  instruments_.maintenance_stage_hook, instruments_.constructed}) {
    routes_.bind(MessageType::get_ingest_jobs,
                 [this](const NodeInfo&, FrameType, const RpcMessage& request) {
                     return RpcMessage{MessageType::ingest_jobs_reply,
                                       ingest_.handle_jobs_query(request.payload)};
                 });
    routes_.bind(MessageType::ingest_job_action,
                 [this](const NodeInfo&, FrameType, const RpcMessage& request) {
                     return RpcMessage{MessageType::ingest_job_action_reply,
                                       ingest_.handle_job_action(request.payload)};
                 });
    routes_.bind(MessageType::torrent_intent,
                 [this](const NodeInfo&, FrameType, const RpcMessage& request) {
                     return RpcMessage{MessageType::torrent_intent_reply,
                                       torrent_coordinator_.handle_intent(request.payload)};
                 });
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
    routes_.unbind(MessageType::torrent_intent);
    routes_.unbind(MessageType::ingest_job_action);
    routes_.unbind(MessageType::get_ingest_jobs);
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
    context.data_resources = &resources_.data;
    context.retained_memory = &resources_.memory;
    context.routes = &routes_;
    context.local_state = &local_;
    context.ingest = &ingest_;
    context.registry = &registry_;
    context.filesystem = &filesystem_;
    context.hydration = &hydration_;
    context.time = &steady_time_source();
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

// Dependants stop before providers. The producers (plugins, torrent
// coordinator, cluster job view) stop before maintenance, while the node still
// admits DATA work and outbound RPC; then outbound calls are cancelled so the
// joins that follow cannot wait on a slow peer.
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
    // Per-phase timing, so a slow barrier names its phase.
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
    // Either namespace may be a tree, so reads go through the namespace
    // primitives. An entry missed here never gets liveness evidence and can be
    // collected while still referenced.
    auto namespace_nodes = ControlNamespaceNodeStore::for_reading(local_.control(), store_);
    const bool establish_baseline =
        !before.retention_baseline_complete && context.proposed.retention_baseline_complete;
    if (establish_baseline) {
        // Baseline: before retention-aware GC is enabled for a namespace, every
        // object reachable from the reconciled view must acquire liveness evidence.
        // One-time and potentially large.
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
        // Tree nodes are control objects, all reachable only from the root.
        if (context.proposed.namespace_root)
            collect_namespace_tree_nodes(*context.proposed.namespace_root, namespace_nodes,
                                         control);
    } else {
        if (context.delta) {
            for (const auto& [_, entry] : context.delta->upsert_entries)
                add_entry(entry);
            // An append carries only new extents but changes the whole file: every
            // extent it now holds needs a fresh dot (a touch with no extents too), or a
            // concurrent delete could release the inherited claim.
            for (const auto& [path, _] : context.delta->append_entries) {
                if (auto found = namespace_entry(context.proposed, &namespace_nodes, path))
                    add_entry(*found);
            }
        } else {
            // Fallback without a delta: compare the namespaces entry by entry (under
            // trees, a walk plus a lookup per path). Ordinary mutations carry a delta.
            for_each_namespace_entry(
                context.proposed, &namespace_nodes,
                [&](const std::string& path, const FsEntry& entry) {
                    const auto found = namespace_entry(before, &namespace_nodes, path);
                    if (!found || *found != entry)
                        add_entry(entry);
                });
        }

        // Tree nodes this commit introduced need claims, as changed catalogue shards
        // do; otherwise the collector sees them as unreferenced.
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
    resources_.events.notify(NodeEvent::storage);
}


} // namespace macha
