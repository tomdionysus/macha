// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/node_services.hpp"

#include "fuse/fuse_frontend.hpp"
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
                MetadataPublicationRetention{
                    [this](const MetadataPublicationContext& context) {
                        return retain_metadata_publication(context);
                    },
                    [this](const NodeId& origin, uint64_t sequence,
                           const MetadataPublicationClaims& claims) {
                        retain_on_peers(origin, sequence, claims);
                    }},
                steady_time_source()),
      ledger_(local_.retention(), local_.data(), local_.control()),
      data_unreferenced_(node_.config().state_path / "retention" / "unreferenced-data.bin"),
      control_unreferenced_(node_.config().state_path / "retention" / "unreferenced-control.bin"),
      availability_(node_, local_, store_, ledger_, resources_.events, routes_,
                    node_.config().state_path / "availability" / "last-survey.bin"),
      catalogue_(node_, local_, metadata_server_, store_, metadata_, ledger_),
      catalogue_installer_(catalogue_, resources_.events),
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
                                  catalogue_, horizon_builder_, ledger_, data_unreferenced_,
                                  control_unreferenced_, availability_,
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

std::map<std::string, uint64_t> NodeServices::observation_gauges() {
    std::map<std::string, uint64_t> gauges;
    const auto idle_ms = [](std::chrono::milliseconds idle) {
        return static_cast<uint64_t>(std::max<int64_t>(0, idle.count()));
    };
    gauges["viewer_idle_ms"] = idle_ms(store_.idle_for(WorkClass::viewer));
    gauges["loader_idle_ms"] = idle_ms(store_.idle_for(WorkClass::loader));
    const auto repair = store_.repair_diagnostics();
    gauges["repair_push_examined"] = repair.push_examined;
    gauges["repair_pull_examined"] = repair.pull_examined;
    gauges["repair_bytes_transferred"] = repair.bytes_transferred;
    gauges["repair_passes_completed"] = repair.passes_completed;
    gauges["repair_pull_unsourceable"] = repair.pull_unsourceable;
    gauges["repair_gate_ran"] = repair.gate_ran;
    gauges["repair_gate_share"] = repair.gate_share;
    gauges["repair_gate_credit"] = repair.gate_credit;
    gauges["repair_prompt_copies"] = repair.prompt_copies;
    // Held for the call, so a subsystem restart cannot pull the frontend out
    // from under it.
    if (auto frontend = registry_.fuse()) {
        const auto fuse = frontend->diagnostics();
        gauges["fuse_publications_completed"] = fuse.data_publications_completed;
        gauges["fuse_publication_bytes_committed"] = fuse.data_publication_bytes_committed;
        gauges["fuse_publication_bytes_confirmed"] = fuse.data_publication_bytes_confirmed;
        gauges["fuse_spool_bytes"] = fuse.spool_bytes;
        gauges["fuse_parked_publications"] = fuse.parked_publications;
    }
    return gauges;
}

NodeServices::~NodeServices() {
    stop();
    routes_.unbind(MessageType::torrent_intent);
    routes_.unbind(MessageType::ingest_job_action);
    routes_.unbind(MessageType::get_ingest_jobs);
}

void NodeServices::start() {
    started_ = true;
    note("start catalogue-installer");
    catalogue_installer_.start();
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
    note("request_stop catalogue-installer");
    catalogue_installer_.request_stop();
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
    note("request_stop metadata-replicator");
    metadata_.request_replication_stop();
    note("cancel-io outbound-rpc");
    node_.cancel_outbound_calls();
    note("stop metadata-replicator");
    metadata_.stop_replication();
    note("stop maintenance");
    maintenance_.stop();
    note("stop catalogue-installer");
    catalogue_installer_.stop();
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

MetadataPublicationClaims
NodeServices::retain_metadata_publication(const MetadataPublicationContext& context) {
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
    // Objects this commit brings into the namespace: some node present must
    // hold each, or the commit would publish bytes nobody here has. Objects
    // the parent already named are not checked: their holder may be away.
    std::set<ObjectId> carried;
    std::vector<ObjectId> introduced;
    auto carry = [&](const std::optional<FsEntry>& entry) {
        if (!entry || entry->type != EntryType::file)
            return;
        for (const auto& extent : entry->extents)
            if (!extent.hole)
                carried.insert(extent.id);
    };
    auto introduce = [&](const std::vector<ExtentRef>& extents) {
        for (const auto& extent : extents)
            if (!extent.hole)
                introduced.push_back(extent.id);
    };

    const auto decode_started = Clock::now();
    auto before = decode_snapshot(context.parent.payload);
    decode_ms = since_ms(decode_started);
    const auto collect_started = Clock::now();
    // Either namespace may be a tree, so reads go through the namespace
    // primitives. An entry missed here never gets liveness evidence and can be
    // collected while still referenced.
    auto namespace_nodes = ControlNamespaceNodeStore::for_reading(local_.control(), store_);
    {
        const bool merge = !context.proposed.merge_parents.empty();
        if (context.delta) {
            if (!merge)
                for (const auto& path : context.delta->erase_entries)
                    carry(namespace_entry(before, &namespace_nodes, path));
            for (const auto& [path, entry] : context.delta->upsert_entries) {
                add_entry(entry);
                if (!merge && entry.type == EntryType::file) {
                    carry(namespace_entry(before, &namespace_nodes, path));
                    introduce(entry.extents);
                }
            }
            // An append carries only new extents but changes the whole file: every
            // extent it now holds needs a fresh dot (a touch with no extents too), or a
            // concurrent delete could release the inherited claim.
            for (const auto& [path, append] : context.delta->append_entries) {
                if (auto found = namespace_entry(context.proposed, &namespace_nodes, path))
                    add_entry(*found);
                if (!merge)
                    introduce(append.extents);
            }
        } else {
            // Fallback without a delta: compare the namespaces entry by entry (under
            // trees, a walk plus a lookup per path). Ordinary mutations carry a delta.
            for_each_namespace_entry(
                context.proposed, &namespace_nodes,
                [&](const std::string& path, const FsEntry& entry) {
                    const auto found = namespace_entry(before, &namespace_nodes, path);
                    if (!found || *found != entry) {
                        add_entry(entry);
                        if (!merge && entry.type == EntryType::file)
                            introduce(entry.extents);
                    }
                });
            if (!merge && !introduced.empty())
                for_each_namespace_entry(before, &namespace_nodes,
                                         [&](const std::string&, const FsEntry& entry) {
                                             carry(entry);
                                         });
        }

        // Tree nodes this commit introduced need claims, as changed catalogue shards
        // do; otherwise the collector sees them as unreferenced.
        if (context.proposed.namespace_root &&
            before.namespace_root != context.proposed.namespace_root)
            collect_namespace_tree_changes(before.namespace_root, *context.proposed.namespace_root,
                                           namespace_nodes, control);

        // Tombstone batches this commit names, likewise.
        {
            const auto& held = before.tombstone_batches;
            for (const auto& batch : context.proposed.tombstone_batches)
                if (!std::binary_search(held.begin(), held.end(), batch,
                                        [](const TombstoneBatch& a, const TombstoneBatch& b) {
                                            return a.id < b.id;
                                        }))
                    control.push_back(batch.id);
        }

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
        // A merge (and a commit without a delta) may introduce conflict
        // alternatives: catalogue roots that are not the effective root, and
        // namespace entries whose extents no path names. New ones must be
        // retained before the commit can become accepted.
        if (!context.delta || !context.proposed.merge_parents.empty()) {
            auto before_roots = metadata_catalogue_root_set(before);
            auto after_roots = metadata_catalogue_root_set(context.proposed);
            for (const auto& root : after_roots) {
                if (before_roots.contains(root))
                    continue;
                auto objects = catalogue_.retention_objects(std::nullopt, root);
                data.insert(data.end(), objects.data.begin(), objects.data.end());
                control.insert(control.end(), objects.control.begin(), objects.control.end());
            }
            const auto before_extents = metadata_conflict_extent_roots(before);
            for (const auto& extent : metadata_conflict_extent_roots(context.proposed))
                if (!before_extents.contains(extent))
                    data.push_back(extent);
        }
    }

    std::sort(data.begin(), data.end());
    data.erase(std::unique(data.begin(), data.end()), data.end());
    std::sort(control.begin(), control.end());
    control.erase(std::unique(control.begin(), control.end()), control.end());
    collect_ms = since_ms(collect_started) - catalogue_ms;

    // This node's own claims, asking no peer. Only an object this commit
    // brings in that is not held here is looked for on the nodes present.
    const auto data_started = Clock::now();
    const auto elsewhere = store_.retain_data_here(data, dot);
    std::vector<ObjectId> sought;
    for (const auto& id : introduced)
        if (!carried.contains(id) && std::binary_search(elsewhere.begin(), elsewhere.end(), id))
            sought.push_back(id);
    if (!sought.empty()) {
        const auto unheld = store_.retain_data(sought, dot);
        if (!unheld.empty()) {
            data_ms = since_ms(data_started);
            report("data-unheld");
            throw MetadataNotReady(
                "DATA object is held by no node present before metadata publication");
        }
    }
    data_ms = since_ms(data_started);
    const auto control_started = Clock::now();
    const bool control_ok =
        control.empty() ||
        store_.retain_control(control, dot, DistributedStore::ClaimScope::here);
    control_ms = since_ms(control_started);
    if (!control_ok) {
        report("control-claim-failed");
        throw std::runtime_error("CONTROL retention claim could not be recorded on this node");
    }
    report("ok");
    resources_.events.notify(NodeEvent::claims);
    return {std::move(data), std::move(control)};
}

void NodeServices::retain_on_peers(const NodeId& origin, uint64_t sequence,
                                   const MetadataPublicationClaims& claims) {
    // The peers' share of a commit already accepted here. An object no node
    // present holds is repair's to find, not a reason to stop.
    const RetentionDot dot{origin, sequence};
    const auto ms_since = [](Clock::time_point t) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
    };
    const auto started = Clock::now();
    (void)store_.retain_data(claims.data, dot);
    const auto data_ms = ms_since(started);
    if (!claims.control.empty())
        (void)store_.retain_control(claims.control, dot, DistributedStore::ClaimScope::every_node);
    const auto total_ms = ms_since(started);
    if (total_ms >= 250 && Log::enabled(LogLevel::debug))
        Log::debug("metadata claims on peers dot=" + to_string(origin).substr(0, 6) + ":" +
                   std::to_string(sequence) + " total_ms=" + std::to_string(total_ms) +
                   " data_ms=" + std::to_string(data_ms) +
                   " data_objects=" + std::to_string(claims.data.size()) +
                   " control_objects=" + std::to_string(claims.control.size()));
}


} // namespace macha
