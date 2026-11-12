// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/cluster.hpp"
#include "cluster/call_deadline.hpp"
#include "cluster/dannan_bridge.hpp"
#include "cluster/rpc_wait.hpp"
#include "cluster/telemetry_bridge.hpp"
#include "diagnostics.hpp"

#include "codec.hpp"
#include "log.hpp"
#include "macha_version.hpp"
#include "supervised.hpp"

#include <dannan/reach/upnp.h>

#include <algorithm>
#include <array>
#include <future>
#include <utility>

namespace macha {

namespace {

NodeInfo self_info(const Config& config, const NodeId& id, uint8_t flags) {
    NodeInfo node;
    node.id = id;
    node.flags = flags;
    node.host = advertised_host(config);
    node.failure_domain = advertised_failure_domain(config, node.host);
    node.port = config.port;
    node.seen_unix_ms = unix_ms();
    node.metadata_write_replicas_required = static_cast<uint32_t>(config.metadata_write_copies);
    return node;
}

// Macha's recovery planes, by the names the node's readiness declares.
constexpr std::array<std::pair<RecoveryProgress::Plane, const char*>, 5> recovery_planes{{
    {RecoveryProgress::data_storage, "data_storage"},
    {RecoveryProgress::control_storage, "control_storage"},
    {RecoveryProgress::cache, "cache"},
    {RecoveryProgress::retention, "retention"},
    {RecoveryProgress::metadata, "metadata"},
}};

bool object_transfer(MessageType type) {
    return type == MessageType::get_object || type == MessageType::put_object ||
           type == MessageType::put_object_deferred ||
           type == MessageType::object_durability_barrier;
}

std::string peer_label(const NodeInfo& node) {
    return to_string(node.id).substr(0, 12);
}

std::string peer_label(const Endpoint& endpoint) {
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

} // namespace

std::string advertised_api_endpoint(const CatalogueApiConfig& api, std::string host) {
    if (!api.enabled)
        return {};
    if (!api.advertised_endpoint.empty())
        return api.advertised_endpoint;
    // IPv6 literals are bracketed for the URL.
    if (host.find(':') != std::string::npos && host.front() != '[')
        host = "[" + host + "]";
    return "http://" + host + ":" + std::to_string(api.port);
}

PlaybackBudgets enforced_playback_budgets(const StreamingConfig& streaming) {
    PlaybackBudgets playback;
    if (!streaming.enabled)
        return playback;
    const auto ms = [](std::chrono::milliseconds value) {
        return static_cast<uint32_t>(std::max<int64_t>(0, value.count()));
    };
    playback.startup_timeout_ms = ms(streaming.startup_timeout);
    playback.segment_timeout_ms = ms(streaming.segment_timeout);
    playback.pipeline_idle_ms = ms(streaming.pipeline_idle);
    playback.session_idle_ms = ms(streaming.session_idle);
    playback.max_sessions_per_account = static_cast<uint32_t>(streaming.max_sessions_per_account);
    playback.max_transcodes_per_account =
        static_cast<uint32_t>(streaming.max_transcodes_per_account);
    playback.max_sessions = static_cast<uint32_t>(streaming.max_sessions);
    playback.transcode_entitlement_idle_ms = ms(streaming.transcode_entitlement_idle);
    playback.startup_no_progress_ms = ms(streaming.startup_no_progress);
    playback.start_wait_max_ms = ms(streaming.start_wait_max);
    playback.start_failed_retention_ms = ms(streaming.start_failed_retention);
    return playback;
}

NodeRuntime::NodeRuntime(Config config, const NodeIdentity& identity, RecoveryProgress& progress,
                         TranscodeRateBook& transcode_rates, MessageRoutes& routes,
                         NodeEvents& events, StartupStageHook startup_stage_hook)
    : cfg_(normalize_config(std::move(config))), identity_(identity), progress_(progress),
      transcode_rates_(transcode_rates), events_(events),
      startup_stage_hook_(std::move(startup_stage_hook)), startup_unix_ms_(unix_ms()),
      host_(node_config(), routes, 2,
            [this](const NodeInfo&, const RpcMessage& notice) { on_metadata_notice(notice); }),
      members_(
          self_info(cfg_, identity_.id,
                    node_flags_for(host_.node().inbound().capable(),
                                   resolve_hosts_extents(cfg_, host_.node().inbound().capable()))),
          cfg_.dead_after),
      telemetry_(std::make_shared<DannanTelemetry>(host_.node().telemetry())) {
    // One identity file, read by both: a mismatch is a state path in two minds.
    if (host_.node().identity().id() != dannan_id(identity_.id))
        throw std::logic_error(
            "the node's id is not Macha's: " + to_string(macha_id(host_.node().identity().id())) +
            " and " + to_string(identity_.id));
    auto& node = host_.node();
    members_.follow(std::make_shared<DannanMembership>(node.nodes(), node.nodes().self().id));
    // Ready once the planes are and recovery has built what runs on them.
    node.readiness().hold_until_marked();
    node.nodes().advertise(advert::metadata_write_replicas,
                           dannan::membership::u64_advert(cfg_.metadata_write_copies));
    follow_inbound();
    publish_self();
}

NodeRuntime::~NodeRuntime() {
    stop();
}

dannan::NodeConfig NodeRuntime::node_config() {
    auto config = dannan_node_config(cfg_, dannan_cluster_key(identity_.keys));
    config.version = std::string(kServerVersion);
    for (const auto& [plane, name] : recovery_planes)
        config.planes.emplace_back(name);
    config.telemetry_layout = hosted_telemetry_layout();
    config.telemetry_fields = [this](dannan::status::Telemetry& record) {
        write_telemetry(record);
    };
    config.inbound_journal_fields = [this](bool capable) {
        wake_runtime();
        return std::string(" hosts_extents=") +
               (resolve_hosts_extents(cfg_, capable) ? "true" : "false");
    };
    if (cfg_.upnp.enabled)
        config.gateway = std::make_shared<dannan::reach::UpnpGateway>();
    // Service's recorder is the one writer of the observation log.
    config.observation_interval = std::chrono::milliseconds(0);
    config.log = macha_log_sink();
    return config;
}

void NodeRuntime::write_telemetry(dannan::status::Telemetry& record) const {
    // Telemetry runs during recovery: unready planes report zero capacity and
    // usage rather than the node vanishing from the cluster.
    HostedTelemetry hosted;
    hosted.storage_capacity = advertised_storage_capacity_.load(std::memory_order_relaxed);
    hosted.storage_used = advertised_storage_used_.load(std::memory_order_relaxed);
    hosted.cache_capacity = advertised_cache_capacity_.load(std::memory_order_relaxed);
    hosted.cache_used = advertised_cache_used_.load(std::memory_order_relaxed);
    hosted.metadata_generation = advertised_metadata_generation_.load(std::memory_order_relaxed);
    hosted.storage_backends_online = advertised_backends_online_.load(std::memory_order_relaxed);
    hosted.api_endpoint = advertised_api_endpoint(cfg_.catalogue.api, members_.self().host);
    hosted.playback = enforced_playback_budgets(cfg_.streaming);
    if (cfg_.streaming.enabled)
        hosted.playback.transcode_rates = transcode_rates_.summary();
    hosted.cache.hits = advertised_cache_hits_.load(std::memory_order_relaxed);
    hosted.cache.misses = advertised_cache_misses_.load(std::memory_order_relaxed);
    hosted.cache.evictions = advertised_cache_evictions_.load(std::memory_order_relaxed);
    write_hosted_telemetry(record, hosted);
    // A core field the node leaves unwritten: the routes usable now.
    if (const auto stats = readable_node().client_stats())
        record.set_u64(dannan::status::field::connections_canonical, stats->canonical_connections);
}

bool NodeRuntime::all_local_state_ready() const noexcept {
    return progress_.complete() && !progress_.failed();
}

NodeReadiness NodeRuntime::readiness() const {
    NodeReadiness out;
    out.control_plane_online = control_plane_online_.load(std::memory_order_acquire);
    out.data_storage_ready = progress_.has(RecoveryProgress::data_storage);
    out.data_storage_degraded = out.data_storage_ready && !cfg_.storage_backends.empty() &&
                                advertised_backends_online_.load(std::memory_order_relaxed) == 0;
    out.control_storage_ready = progress_.has(RecoveryProgress::control_storage);
    out.cache_ready = progress_.has(RecoveryProgress::cache);
    out.retention_ready = progress_.has(RecoveryProgress::retention);
    out.metadata_ready = progress_.has(RecoveryProgress::metadata);
    out.local_state_ready = all_local_state_ready();
    out.failed = progress_.failed();
    out.started_unix_ms = startup_unix_ms_;
    out.ready_unix_ms = progress_.ready_unix_ms();
    out.error = progress_.error();
    return out;
}

void NodeRuntime::publish_self() {
    auto& nodes = host_.node().nodes();
    nodes.advertise(advert::capacity,
                    dannan::membership::u64_advert(
                        advertised_storage_capacity_.load(std::memory_order_relaxed)));
    nodes.advertise(advert::used, dannan::membership::u64_advert(
                                      advertised_storage_used_.load(std::memory_order_relaxed)));
    nodes.advertise(advert::metadata_generation,
                    dannan::membership::u64_advert(
                        advertised_metadata_generation_.load(std::memory_order_relaxed)));
}

void NodeRuntime::advertise_storage(uint64_t used, uint64_t capacity) {
    members_.storage(used, capacity);
    advertised_storage_used_.store(used, std::memory_order_relaxed);
    advertised_storage_capacity_.store(capacity, std::memory_order_relaxed);
    auto& nodes = host_.node().nodes();
    nodes.advertise(advert::capacity, dannan::membership::u64_advert(capacity));
    nodes.advertise(advert::used, dannan::membership::u64_advert(used));
}

void NodeRuntime::advertise_storage_backends(uint32_t online) {
    advertised_backends_online_.store(online, std::memory_order_relaxed);
}

void NodeRuntime::advertise_metadata_generation(uint64_t generation) {
    members_.metadata_generation(generation);
    advertised_metadata_generation_.store(generation, std::memory_order_relaxed);
    host_.node().nodes().advertise(advert::metadata_generation,
                                   dannan::membership::u64_advert(generation));
}

void NodeRuntime::advertise_cache(uint64_t capacity, uint64_t used, const CacheActivity& activity) {
    advertised_cache_capacity_.store(capacity, std::memory_order_relaxed);
    advertised_cache_used_.store(used, std::memory_order_relaxed);
    advertised_cache_hits_.store(activity.hits, std::memory_order_relaxed);
    advertised_cache_misses_.store(activity.misses, std::memory_order_relaxed);
    advertised_cache_evictions_.store(activity.evictions, std::memory_order_relaxed);
}

bool NodeRuntime::wait_local_state_ready(std::chrono::milliseconds timeout) {
    if (!started_.load(std::memory_order_acquire) && !all_local_state_ready())
        return false;
    return progress_.wait_complete(timeout) && !progress_.failed();
}

void NodeRuntime::start() {
    if (started_.load(std::memory_order_acquire))
        return;

    // Legal but worth saying once.
    for (const auto& warning : configuration_warnings(cfg_))
        Log::warn("configuration: " + warning);
    {
        const auto resolution = inbound_resolution();
        Log::info(
            std::string("node inbound_capable=") + (resolution.inbound_capable ? "true" : "false") +
            " (" + std::string(tristate_name(resolution.inbound_capable_mode)) + ", " +
            resolution.source + ") hosts_extents=" + (resolution.hosts_extents ? "true" : "false") +
            " (" + std::string(tristate_name(resolution.hosts_extents_mode)) + ")");
    }

    if (startup_stage_hook_)
        startup_stage_hook_("control-plane");

    auto& node = host_.node();
    // Every change from the first: a joined for each node active now, then each one.
    if (!roster_watch_)
        roster_watch_ = node.nodes().watch(
            boost::asio::make_strand(node.time().executor()),
            [this](const dannan::RosterChange& change) { on_roster_change(change); });

    // Control plane before any expensive backend recovery: peers can
    // authenticate this node at once and Status can tell reachability from
    // readiness. The node refuses a cluster nobody could reach.
    host_.start();
    started_.store(true, std::memory_order_release);
    control_plane_online_.store(true, std::memory_order_release);
    Log::info("node " + to_string(identity_.id).substr(0, 12) + " listening on " +
              std::to_string(node.port()) + " domain=" + members_.self().failure_domain +
              " state=recovering");

    runtime_ = std::jthread([this](const std::stop_token& stop) {
        run_supervised_loop("cluster-runtime", stop, [this, stop] { runtime_loop(stop); });
    });
}

void NodeRuntime::request_stop() {
    if (runtime_.joinable()) {
        runtime_.request_stop();
        runtime_wait_cv_.notify_all();
    }
    host_.request_stop();
}

void NodeRuntime::cancel_outbound_calls() {
    if (outbound_calls_stopped_.exchange(true))
        return;
    Log::debug("shutdown: cancelling outbound RPC calls");
    host_.cancel_outbound_calls();
}

void NodeRuntime::stop() {
    if (!started_.exchange(false)) {
        Log::debug("shutdown: NodeRuntime::stop already stopped");
        host_.stop();
        return;
    }
    Log::debug("shutdown: NodeRuntime::stop begin");
    request_stop();
    cancel_outbound_calls();
    if (runtime_.joinable())
        runtime_.join();
    if (roster_watch_) {
        host_.node().nodes().unwatch(*roster_watch_);
        roster_watch_.reset();
    }
    Log::debug("shutdown: node transport stop calling");
    host_.stop();
    Log::debug("shutdown: node transport stop returned");
    Log::debug("shutdown: NodeRuntime::stop complete");
}

void NodeRuntime::refuse_when_stopping() const {
    if (outbound_calls_stopped_.load(std::memory_order_acquire))
        throw std::runtime_error("node is stopping; outbound RPC is unavailable");
}

std::chrono::milliseconds NodeRuntime::stall_notice_for(MessageType type) const {
    return object_transfer(type) ? cfg_.data_stall_notice : cfg_.control_stall_notice;
}

std::chrono::milliseconds NodeRuntime::no_progress_deadline_for(MessageType type) const {
    return object_transfer(type) ? cfg_.data_no_progress_deadline
                                 : cfg_.control_no_progress_deadline;
}

RpcReply NodeRuntime::call(const NodeInfo& node, MessageType type, std::span<const uint8_t> payload,
                           std::chrono::milliseconds deadline) {
    return call(node, type, payload, default_frame_type(type), deadline);
}

RpcReply NodeRuntime::call(const Endpoint& endpoint, MessageType type,
                           std::span<const uint8_t> payload, std::chrono::milliseconds deadline) {
    return call(endpoint, type, payload, default_frame_type(type), deadline);
}

AsyncRpc NodeRuntime::place(const NodeInfo& node, MessageType type,
                            std::span<const uint8_t> payload, FrameType frame_type,
                            std::chrono::milliseconds deadline) {
    if (call_fault_)
        if (auto faulted = call_fault_(node, type))
            return std::move(*faulted);
    return host_.call_async(node, type, payload, frame_type, deadline);
}

RpcReply NodeRuntime::call(const NodeInfo& node, MessageType type, std::span<const uint8_t> payload,
                           FrameType frame_type, std::chrono::milliseconds deadline) {
    refuse_when_stopping();
    const auto started = Clock::now();
    auto async = place(node, type, payload, frame_type, deadline);
    return wait_for_reply(async, peer_label(node), type, frame_type, started,
                          stall_notice_for(type), no_progress_deadline_for(type));
}

RpcReply NodeRuntime::call(const Endpoint& endpoint, MessageType type,
                           std::span<const uint8_t> payload, FrameType frame_type,
                           std::chrono::milliseconds deadline) {
    refuse_when_stopping();
    const auto started = Clock::now();
    auto async = host_.call_async(endpoint, type, payload, frame_type, deadline);
    return wait_for_reply(async, peer_label(endpoint), type, frame_type, started,
                          stall_notice_for(type), no_progress_deadline_for(type));
}

AsyncRpc NodeRuntime::call_async(const NodeInfo& node, MessageType type,
                                 std::span<const uint8_t> payload,
                                 std::chrono::milliseconds deadline) {
    return call_async(node, type, payload, default_frame_type(type), deadline);
}

AsyncRpc NodeRuntime::call_async(const Endpoint& endpoint, MessageType type,
                                 std::span<const uint8_t> payload,
                                 std::chrono::milliseconds deadline) {
    return call_async(endpoint, type, payload, default_frame_type(type), deadline);
}

AsyncRpc NodeRuntime::call_async(const NodeInfo& node, MessageType type,
                                 std::span<const uint8_t> payload, FrameType frame_type,
                                 std::chrono::milliseconds deadline) {
    refuse_when_stopping();
    return place(node, type, payload, frame_type, deadline);
}

AsyncRpc NodeRuntime::call_async(const Endpoint& endpoint, MessageType type,
                                 std::span<const uint8_t> payload, FrameType frame_type,
                                 std::chrono::milliseconds deadline) {
    refuse_when_stopping();
    return host_.call_async(endpoint, type, payload, frame_type, deadline);
}

size_t NodeRuntime::broadcast_best_effort(const RpcMessage& message, FrameType frame_type) {
    return host_.node().channels().broadcast_best_effort(dannan_message(message),
                                                         dannan_frame(frame_type));
}

size_t NodeRuntime::broadcast_notify(const RpcMessage& message, FrameType frame_type) {
    return host_.node().channels().notify(dannan_message(message), dannan_frame(frame_type));
}

TrafficTotals NodeRuntime::traffic_totals() const {
    const auto traffic = host_.node().traffic();
    TrafficTotals totals;
    totals.in_bytes = traffic.in_bytes;
    totals.out_bytes = traffic.out_bytes;
    return totals;
}

bool NodeRuntime::peer_viewers_active(std::chrono::milliseconds fresh_for) const {
    for (const auto& view : telemetry_.views(fresh_for)) {
        if (view.telemetry.node_id == node_id() || !view.fresh)
            continue;
        for (const auto& entry : view.telemetry.traffic) {
            const auto type = static_cast<FrameType>(entry.frame_class);
            if ((type == FrameType::foreground || type == FrameType::read_ahead) &&
                (entry.in_bytes_per_s || entry.out_bytes_per_s))
                return true;
        }
    }
    return false;
}

void NodeRuntime::announce_metadata_generation(uint64_t generation) {
    // The accepted-head set can change without a higher generation (a
    // concurrent same-generation sibling). MetadataManager caches key off
    // this epoch too, so advance it before broadcasting, or a node serves its
    // pre-sibling snapshot until the cache TTL expires.
    metadata_announcements_.fetch_add(1, std::memory_order_relaxed);
    remote_metadata_epoch_.fetch_add(1, std::memory_order_acq_rel);
    events_.notify(NodeEvent::metadata);
    advertise_metadata_generation(generation);
    Writer writer;
    writer.u64(generation);
    host_.node().channels().broadcast(
        dannan_message(RpcMessage(MessageType::metadata_notice, writer.take())));
}

void NodeRuntime::on_metadata_notice(const RpcMessage& notice) {
    uint64_t generation = 0;
    try {
        Reader reader(notice.payload);
        generation = reader.u64();
        reader.finish();
    } catch (const std::exception& error) {
        Log::debug("metadata notice ignored: " + std::string(error.what()));
        return;
    }
    auto current = remote_metadata_generation_.load();
    while (current < generation &&
           !remote_metadata_generation_.compare_exchange_weak(current, generation)) {
    }
    // A notice is sent only when a peer's accepted-head set changes, so an
    // equal generation can still carry new sibling or topology information.
    // Only a notice made stale by a strictly newer generation is ignored.
    if (current <= generation) {
        remote_metadata_epoch_.fetch_add(1, std::memory_order_acq_rel);
        events_.notify(NodeEvent::metadata);
    }
}

void NodeRuntime::on_roster_change(const dannan::RosterChange& change) {
    auto node = node_info_from(change.node);
    if (node.id == identity_.id)
        return;
    const auto previous_generation = remote_metadata_generation_.load();
    bool topology = false;
    switch (change.kind) {
    case dannan::RosterChange::Kind::joined:
        topology = true;
        roster_told_[node.id] = node;
        break;
    case dannan::RosterChange::Kind::left:
        topology = true;
        roster_told_.erase(node.id);
        break;
    case dannan::RosterChange::Kind::changed: {
        auto [told, inserted] = roster_told_.try_emplace(node.id, node);
        const auto& before = told->second;
        topology = inserted || before.host != node.host || before.port != node.port ||
                   before.failure_domain != node.failure_domain || before.flags != node.flags ||
                   before.metadata_write_replicas_required != node.metadata_write_replicas_required;
        told->second = node;
        break;
    }
    }
    bool newer = false;
    if (change.kind != dannan::RosterChange::Kind::left) {
        raise_remote_metadata_generation(node.metadata_generation);
        newer = node.metadata_generation > previous_generation;
    }
    if (topology || newer)
        events_.notify(topology ? NodeEvent::topology : NodeEvent::metadata);
}

void NodeRuntime::wake_runtime() {
    runtime_demand_.fetch_add(1, std::memory_order_release);
    runtime_wait_cv_.notify_all();
}

void NodeRuntime::runtime_loop(const std::stop_token& stop) {
    ThreadCpuReporter cpu_reporter("macha-node", std::chrono::seconds(5), true);
    while (!stop.stop_requested()) {
        const auto demand = runtime_demand_.load(std::memory_order_acquire);
        mirror_readiness();
        follow_self();
        follow_inbound();
        settle_remote_generation();
        cpu_reporter.tick();
        Lock lock(runtime_wait_mutex_);
        runtime_wait_cv_.wait_for(lock.native(), stop, cfg_.heartbeat, [&] {
            return runtime_demand_.load(std::memory_order_acquire) != demand;
        });
    }
}

void NodeRuntime::mirror_readiness() {
    auto& readiness = host_.node().readiness();
    for (const auto& [plane, name] : recovery_planes)
        if (progress_.has(plane) && !readiness.has(name))
            readiness.ready(name);
    if (progress_.failed() && !readiness.failed())
        readiness.fail(progress_.error());
    if (progress_.complete() && !readiness.complete())
        readiness.mark_complete();
}

void NodeRuntime::follow_self() {
    const auto record = host_.node().nodes().self();
    const auto before = members_.self();
    if (record.host == before.host && record.port == before.port)
        return;
    members_.endpoint(record.host, record.port);
    const auto connectivity = host_.node().connectivity();
    Log::info(
        "node advertised endpoint changed from=" + before.host + ":" + std::to_string(before.port) +
        " to=" + record.host + ":" + std::to_string(record.port) +
        " source=" + (connectivity ? connectivity->advertised_source : std::string("configured")));
}

// The node logs its inbound change, with hosts_extents beside it; here placement
// moves as for a join or leave.
void NodeRuntime::follow_inbound() {
    const bool capable = host_.node().inbound().capable();
    if (inbound_applied_ == capable)
        return;
    const bool first = !inbound_applied_;
    inbound_applied_ = capable;
    const bool hosts = resolve_hosts_extents(cfg_, capable);
    if (hosts)
        host_.node().nodes().advertise(advert::hosts_extents, dannan::net::Bytes{1});
    else
        host_.node().nodes().withdraw(advert::hosts_extents);
    if (members_.set_flags(capable, hosts) && !first)
        events_.notify(NodeEvent::topology);
}

// The highest generation heard of is only worth chasing while some node
// present advertises it. One that no active node has backed for dead_after
// belonged to a node that has gone, and is dropped to what the nodes present
// advertise.
void NodeRuntime::settle_remote_generation() {
    uint64_t backed = 0;
    for (const auto& peer : members_.active())
        if (peer.id != identity_.id)
            backed = std::max(backed, peer.metadata_generation);
    const auto heard = remote_metadata_generation_.load();
    const auto now = Clock::now();
    if (backed >= heard) {
        remote_generation_unbacked_since_.reset();
        return;
    }
    if (!remote_generation_unbacked_since_) {
        remote_generation_unbacked_since_ = now;
        return;
    }
    if (now - *remote_generation_unbacked_since_ < cfg_.dead_after)
        return;
    auto expected = heard;
    if (remote_metadata_generation_.compare_exchange_strong(expected, backed))
        events_.notify(NodeEvent::metadata);
    remote_generation_unbacked_since_.reset();
}

void NodeRuntime::signal_telemetry_refresh() {
    mirror_readiness();
    if (started_.load(std::memory_order_acquire))
        host_.node().telemetry().refresh();
}

bool NodeRuntime::apply_identity_reset(const IdentityAssociationReset& reset) {
    const bool changed = host_.node().nodes().apply_identity_reset(dannan_identity_reset(reset));
    if (changed) {
        Log::info(
            "node identity association reset scope=" + identity_reset_key(reset.host, reset.port) +
            " stale_node_id=" +
            (reset.stale_node_id == NodeId{} ? std::string("<any>")
                                             : to_string(reset.stale_node_id)) +
            " epoch=" + std::to_string(reset.epoch) + " reset_by=" + to_string(reset.reset_by) +
            (reset.reason.empty() ? std::string{} : " reason=" + reset.reason));
    }
    return changed;
}

void NodeRuntime::propagate_identity_reset(const IdentityAssociationReset& reset) {
    (void)apply_identity_reset(reset);
    host_.node().nodes().propagate_identity_reset(dannan_identity_reset(reset),
                                                  call_deadline::control);
}

std::vector<IdentityAssociationReset> NodeRuntime::identity_resets() const {
    std::vector<IdentityAssociationReset> out;
    for (const auto& reset : host_.node().nodes().identity_resets())
        out.push_back(macha_identity_reset(reset));
    return out;
}

PublicConnectivityStatus NodeRuntime::public_connectivity_status() const {
    if (const auto status = host_.node().connectivity())
        return macha_connectivity(*status);
    return {};
}

PublicConnectivityStatus NodeRuntime::refresh_public_connectivity(bool probe, bool force_probe) {
    auto answer =
        std::make_shared<std::promise<std::optional<dannan::reach::PublicConnectivityStatus>>>();
    auto future = answer->get_future();
    auto& node = host_.node();
    node.nodes().refresh_public_connectivity(
        probe, force_probe, node.time().executor(),
        [answer](std::optional<dannan::reach::PublicConnectivityStatus> status) {
            answer->set_value(std::move(status));
        });
    // The node's work is bounded by its connectivity timeouts; a refresh that has
    // not answered by then reports what is known.
    const auto bound = cfg_.upnp.discovery_timeout + cfg_.external_ip.timeout +
                       cfg_.connectivity_check.timeout + std::chrono::seconds(5);
    if (future.wait_for(bound) == std::future_status::ready)
        if (const auto status = future.get())
            return macha_connectivity(*status);
    return public_connectivity_status();
}

dannan::Node& NodeRuntime::readable_node() const {
    // A read that Dannan's Node offers without a const overload (inbound(),
    // threads()); what is read through it is const.
    return const_cast<DannanHost&>(host_).node();
}

InboundResolution NodeRuntime::inbound_resolution() const {
    return macha_inbound(cfg_, readable_node().inbound().state());
}

std::vector<SupervisedThreadStatus> NodeRuntime::thread_statuses() const {
    // Macha's threads, then the node's under the names they share with Macha;
    // the node's others have no counterpart in Status and are left out.
    static constexpr std::array<std::string_view, 3> shared{
        "cluster-telemetry", "cluster-maintenance", "cluster-connectivity"};
    auto out = supervised_thread_statuses();
    for (const auto& status : readable_node().threads().statuses()) {
        if (std::find(shared.begin(), shared.end(), status.name) == shared.end())
            continue;
        SupervisedThreadStatus entry;
        entry.name = status.name;
        entry.running = status.running;
        entry.restarting = status.restarting;
        entry.faults = status.faults;
        entry.last_fault_code = status.last_fault_code;
        entry.last_fault = status.last_fault;
        entry.last_fault_unix_ms = status.last_fault_unix_ms;
        out.push_back(std::move(entry));
    }
    return out;
}

RpcStats NodeRuntime::rpc_stats() const {
    if (const auto stats = host_.node().client_stats())
        return macha_rpc_stats(*stats);
    return {};
}

std::optional<std::chrono::milliseconds> NodeRuntime::peer_latency(const NodeId& peer) const {
    return host_.node().nodes().latency(dannan_id(peer));
}

std::map<NodeId, std::chrono::milliseconds> NodeRuntime::peer_latencies() const {
    std::map<NodeId, std::chrono::milliseconds> out;
    for (const auto& [id, latency] : host_.node().nodes().latencies())
        out.emplace(macha_id(id), latency);
    return out;
}

RpcServerWorkStats NodeRuntime::rpc_server_work_stats() const {
    if (const auto stats = host_.node().work_stats())
        return macha_work_stats(*stats);
    return {};
}

void NodeRuntime::reconfigure_local(const Config& config) {
    auto updated = normalize_config(config);
    // Node-local policy, unused by the long-lived network/metadata threads:
    // keep the snapshot in sync with a live reload without changing cluster policy.
    cfg_.storage_backends = updated.storage_backends;
    cfg_.cache = updated.cache;
    cfg_.hydration = updated.hydration;
    cfg_.read_ahead_extents = updated.read_ahead_extents;
}
} // namespace macha
