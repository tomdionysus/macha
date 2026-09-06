// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster.hpp"
#include "diagnostics.hpp"
#include "durable_file.hpp"

#include "codec.hpp"
#include "log.hpp"
#include "macha_version.hpp"
#include "supervised.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <thread>
#include <unistd.h>

namespace macha {
namespace {
NodeInfo self_info(const Config& config, const NodeId& id, uint64_t used, uint64_t capacity,
                   uint64_t metadata_generation) {
    NodeInfo node;
    node.id = id;
    node.host = config.advertise_host;
    if (node.host.empty()) {
        if (config.listen_host == "127.0.0.1" || config.listen_host == "::1") {
            node.host = config.listen_host;
        } else {
            char hostname[256]{};
            if (!gethostname(hostname, 255))
                node.host = hostname;
            if (node.host.empty())
                node.host = "127.0.0.1";
        }
    }
    node.failure_domain = config.failure_domain.empty() ? node.host : config.failure_domain;
    node.port = config.port;
    node.capacity = capacity;
    node.used = used;
    node.seen_unix_ms = unix_ms();
    node.metadata_generation = metadata_generation;
    node.metadata_write_replicas_required =
        static_cast<uint32_t>(config.metadata_min_write_replicas);
    return node;
}

RpcMessage error_reply(const std::string& text) {
    Writer writer;
    writer.string(text);
    return {MessageType::error, writer.take()};
}

constexpr std::array<uint8_t, 8> identity_reset_magic{'M', 'A', 'C', 'H', 'I', 'D', 'R', '1'};

void encode_identity_reset(Writer& writer, const IdentityAssociationReset& reset) {
    writer.string(reset.host);
    writer.u16(reset.port);
    writer.fixed(reset.stale_node_id.bytes);
    writer.u64(reset.epoch);
    writer.u64(reset.reset_unix_ms);
    writer.fixed(reset.reset_by.bytes);
    writer.string(reset.reason);
}

Bytes encode_identity_resets(const std::vector<IdentityAssociationReset>& resets) {
    Writer writer;
    writer.raw(identity_reset_magic);
    writer.u32(static_cast<uint32_t>(resets.size()));
    for (const auto& reset : resets)
        encode_identity_reset(writer, reset);
    return writer.take();
}

std::vector<IdentityAssociationReset> decode_identity_resets(std::span<const uint8_t> payload) {
    Reader reader(payload);
    const auto magic = reader.raw(identity_reset_magic.size());
    if (!std::equal(magic.begin(), magic.end(), identity_reset_magic.begin()))
        throw DecodeError("bad identity reset payload");
    const auto count = reader.u32();
    if (count > 65536)
        throw DecodeError("too many identity reset records");
    std::vector<IdentityAssociationReset> out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        IdentityAssociationReset reset;
        reset.host = reader.string(4096);
        reset.port = reader.u16();
        reset.stale_node_id.bytes = reader.fixed<16>();
        reset.epoch = reader.u64();
        reset.reset_unix_ms = reader.u64();
        reset.reset_by.bytes = reader.fixed<16>();
        reset.reason = reader.string(4096);
        if (reset.host.empty() || !reset.epoch)
            throw DecodeError("bad identity reset record");
        out.push_back(std::move(reset));
    }
    reader.finish();
    return out;
}

RpcMessage metadata_identity_reply(const MetadataIdentity& identity) {
    Writer writer;
    writer.u64(identity.generation);
    writer.fixed(identity.hash.bytes);
    return {MessageType::metadata_identity_reply, writer.take()};
}

int64_t activity_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
        .count();
}

NodeId load_v18_node_id(const std::filesystem::path& state) {
    static constexpr std::string_view expected = "macha-state-layout-v18";
    const auto marker = state / "storage-layout";
    if (std::filesystem::exists(marker)) {
        std::ifstream input(marker);
        std::string value;
        std::getline(input, value);
        if (!input && value.empty())
            throw std::runtime_error("cannot read storage layout marker");
        if (value != expected)
            throw std::runtime_error(
                "incompatible Macha storage layout; 0.18 requires a fresh namespace");
    } else {
        // 0.18 intentionally has no live migration path. Refuse to reinterpret an
        // older namespace/backend layout as the new storage contract. StorageLock
        // has already created .macha.lock, which is the only allowed pre-existing
        // entry for a fresh state directory.
        for (const auto& entry : std::filesystem::directory_iterator(state)) {
            if (entry.path().filename() == ".macha.lock")
                continue;
            throw std::runtime_error(
                "existing unversioned Macha state detected; 0.18 requires a fresh namespace");
        }
        durable_replace_file(marker, std::string(expected) + "\n");
    }
    return load_or_create_node_id(state);
}
} // namespace

NodeRuntime::NodeRuntime(Config config, ClusterKeys keys, StartupStageHook startup_stage_hook)
    : cfg_(normalize_config(std::move(config))), keys_(keys), state_lock_(cfg_.state_path),
      id_(load_v18_node_id(cfg_.state_path)), durability_epoch_(random_node_id()),
      data_resources_(cfg_.data_inflight_bytes, cfg_.data_viewer_reserve_bytes),
      retained_memory_(cfg_.runtime.retained_memory_bytes,
                       cfg_.runtime.control_memory_reserve_bytes,
                       cfg_.runtime.viewer_memory_reserve_bytes,
                       cfg_.runtime.loader_memory_reserve_bytes),
      members_(self_info(cfg_, id_, 0, 0, 0), cfg_.dead_after,
               cfg_.state_path / "membership" / "known-nodes.bin"),
      public_connectivity_(cfg_, id_, Endpoint{members_.self().host, members_.self().port}),
      telemetry_(id_, cfg_.state_path / "telemetry" / "last-known.bin"),
      sessions_(cfg_.session.anonymous_ttl, cfg_.session.max_sessions,
               cfg_.state_path / "sessions" / "sessions.bin"),
      client_(
          keys_, [this] { return members_.self(); },
          [this](const NodeInfo& peer) {
              const auto active_before = members_.active();
              const auto previous =
                  std::find_if(active_before.begin(), active_before.end(),
                               [&](const NodeInfo& item) { return item.id == peer.id; });
              const bool topology_changed =
                  previous == active_before.end() || previous->host != peer.host ||
                  previous->port != peer.port || previous->failure_domain != peer.failure_domain;
              const auto previous_generation = remote_metadata_generation_.load();
              members_.observe(peer, true);
              signal_telemetry_refresh();
              remote_metadata_generation_.store(
                  std::max(previous_generation, peer.metadata_generation));
              if (topology_changed || peer.metadata_generation > previous_generation)
                  signal_service_event(topology_changed ? ServiceEvent::topology
                                                        : ServiceEvent::metadata);
          },
          [this](uint64_t generation) {
              auto current = remote_metadata_generation_.load();
              while (current < generation &&
                     !remote_metadata_generation_.compare_exchange_weak(current, generation)) {
              }
              // An explicit metadata notice is emitted only when a peer's
              // accepted-head set changes. Equal generation can therefore be
              // new sibling/topology information even though it does not raise
              // the numeric high-water mark. Ignore only a notice made stale by
              // a strictly newer generation already observed. Ordinary equal-
              // generation membership heartbeats use the separate membership
              // observer above and remain non-events.
              if (current <= generation) {
                  remote_metadata_epoch_.fetch_add(1, std::memory_order_acq_rel);
                  signal_service_event(ServiceEvent::metadata);
              }
          },
          cfg_.connect_timeout, cfg_.heartbeat, cfg_.dead_after, cfg_.max_frame_size,
          &retained_memory_),
      server_(
          cfg_.listen_host, cfg_.port, keys_, members_.self(),
          [this](const NodeInfo& peer, FrameType frame_type, const RpcMessage& request) {
              return handle(peer, frame_type, request);
          },
          [this](const NodeInfo& peer) {
              const auto active_before = members_.active();
              const auto previous =
                  std::find_if(active_before.begin(), active_before.end(),
                               [&](const NodeInfo& item) { return item.id == peer.id; });
              const bool topology_changed =
                  previous == active_before.end() || previous->host != peer.host ||
                  previous->port != peer.port || previous->failure_domain != peer.failure_domain;
              const auto previous_generation = remote_metadata_generation_.load();
              members_.observe(peer, true);
              signal_telemetry_refresh();
              auto current = previous_generation;
              while (current < peer.metadata_generation &&
                     !remote_metadata_generation_.compare_exchange_weak(current,
                                                                        peer.metadata_generation)) {
              }
              if (topology_changed || peer.metadata_generation > previous_generation)
                  signal_service_event(topology_changed ? ServiceEvent::topology
                                                        : ServiceEvent::metadata);
          },
          cfg_.max_frame_size, {}, &retained_memory_),
      startup_stage_hook_(std::move(startup_stage_hook)), startup_unix_ms_(unix_ms()) {
    server_.attach_client(client_);
    // Membership loads locally durable identity-reset tombstones before the
    // transport exists. Seed the other operational consumers now so stale
    // routes and telemetry are fenced before the control plane starts.
    for (const auto& reset : members_.identity_resets())
        apply_identity_reset(reset);
}

NodeRuntime::~NodeRuntime() {
    stop();
}

void NodeRuntime::mark_ready(ReadyBit bit) {
    ready_bits_.fetch_or(static_cast<uint32_t>(bit), std::memory_order_release);
    if (all_local_state_ready() && !ready_unix_ms_.load(std::memory_order_relaxed))
        ready_unix_ms_.store(unix_ms(), std::memory_order_release);
    readiness_cv_.notify_all();
    // Telemetry's reported phase must not lag actual readiness by up to the
    // ordinary 5s sampling interval: a peer (or this node's own first sample,
    // published as soon as the control plane starts) would otherwise keep
    // reporting "recovering" for that whole window after actually becoming
    // ready.
    signal_telemetry_refresh();
}

void NodeRuntime::mark_recovery_failed(std::string error) {
    {
        std::lock_guard lock(readiness_mutex_);
        if (recovery_error_.empty())
            recovery_error_ = std::move(error);
    }
    ready_bits_.fetch_or(static_cast<uint32_t>(ready_failed), std::memory_order_release);
    readiness_cv_.notify_all();
    signal_telemetry_refresh();
}

bool NodeRuntime::all_local_state_ready() const noexcept {
    constexpr uint32_t required =
        ready_data_storage | ready_control_storage | ready_cache | ready_retention | ready_metadata;
    const auto bits = ready_bits_.load(std::memory_order_acquire);
    return (bits & required) == required && !(bits & ready_failed);
}

NodeReadiness NodeRuntime::readiness() const {
    const auto bits = ready_bits_.load(std::memory_order_acquire);
    NodeReadiness out;
    out.control_plane_online = (bits & ready_control_plane) != 0;
    out.data_storage_ready = (bits & ready_data_storage) != 0;
    out.control_storage_ready = (bits & ready_control_storage) != 0;
    out.cache_ready = (bits & ready_cache) != 0;
    out.retention_ready = (bits & ready_retention) != 0;
    out.metadata_ready = (bits & ready_metadata) != 0;
    out.local_state_ready = all_local_state_ready();
    out.failed = (bits & ready_failed) != 0;
    out.started_unix_ms = startup_unix_ms_;
    out.ready_unix_ms = ready_unix_ms_.load(std::memory_order_acquire);
    {
        std::lock_guard lock(readiness_mutex_);
        out.error = recovery_error_;
    }
    return out;
}

bool NodeRuntime::wait_local_state_ready(std::chrono::milliseconds timeout) {
    if (all_local_state_ready())
        return true;
    std::unique_lock lock(readiness_mutex_);
    readiness_cv_.wait_for(lock, timeout, [this] {
        const auto bits = ready_bits_.load(std::memory_order_acquire);
        return all_local_state_ready() || (bits & ready_failed) != 0 || !started_.load();
    });
    return all_local_state_ready();
}

StoragePool& NodeRuntime::local_store() {
    if (!ready(ready_data_storage) || !local_)
        throw std::runtime_error("data storage is still recovering");
    return *local_;
}
const StoragePool& NodeRuntime::local_store() const {
    if (!ready(ready_data_storage) || !local_)
        throw std::runtime_error("data storage is still recovering");
    return *local_;
}
LocalStore& NodeRuntime::control_store() {
    if (!ready(ready_control_storage) || !control_)
        throw std::runtime_error("control storage is still recovering");
    return *control_;
}
const LocalStore& NodeRuntime::control_store() const {
    if (!ready(ready_control_storage) || !control_)
        throw std::runtime_error("control storage is still recovering");
    return *control_;
}
PersistentBlockCache& NodeRuntime::block_cache() {
    if (!ready(ready_cache) || !cache_)
        throw std::runtime_error("persistent cache is still recovering");
    return *cache_;
}
RetentionStore& NodeRuntime::retention_store() {
    if (!ready(ready_retention) || !retention_)
        throw std::runtime_error("retention state is still recovering");
    return *retention_;
}
const RetentionStore& NodeRuntime::retention_store() const {
    if (!ready(ready_retention) || !retention_)
        throw std::runtime_error("retention state is still recovering");
    return *retention_;
}
MetadataReplica& NodeRuntime::metadata_replica() {
    if (!ready(ready_metadata) || !meta_)
        throw std::runtime_error("metadata replica is still recovering");
    return *meta_;
}
const MetadataReplica& NodeRuntime::metadata_replica() const {
    if (!ready(ready_metadata) || !meta_)
        throw std::runtime_error("metadata replica is still recovering");
    return *meta_;
}

void NodeRuntime::recover_storage(std::stop_token stop) {
    try {
        if (startup_stage_hook_)
            startup_stage_hook_("data-storage");
        if (stop.stop_requested())
            return;
        auto local = std::make_unique<StoragePool>(cfg_.state_path, id_, cfg_.storage_backends,
                                                   keys_.storage, std::chrono::milliseconds(500),
                                                   cfg_.storage_packing);
        if (stop.stop_requested())
            return;
        const auto used = local->used();
        const auto capacity = local->limit();
        local_ = std::move(local);
        members_.storage(used, capacity);
        server_.set_local(members_.self());
        telemetry_storage_used_.store(used, std::memory_order_relaxed);
        telemetry_storage_capacity_.store(capacity, std::memory_order_relaxed);
        mark_ready(ready_data_storage);
        Log::info("node data storage ready used=" + std::to_string(used) +
                  " capacity=" + std::to_string(capacity));
    } catch (const std::exception& error) {
        Log::error("node data storage recovery failed: " + std::string(error.what()));
        mark_recovery_failed("data storage: " + std::string(error.what()));
    }
}

void NodeRuntime::recover_state(std::stop_token stop) {
    try {
        if (startup_stage_hook_)
            startup_stage_hook_("control-storage");
        if (stop.stop_requested())
            return;
        control_ = std::make_unique<LocalStore>(
            cfg_.metadata_store.path,
            LocalStoreOptions{cfg_.metadata_store.limit, 0, cfg_.metadata_store.packing.threshold,
                              cfg_.metadata_store.packing.target_size},
            keys_.storage);
        mark_ready(ready_control_storage);

        if (startup_stage_hook_)
            startup_stage_hook_("cache");
        if (stop.stop_requested())
            return;
        cache_ = std::make_unique<PersistentBlockCache>(cfg_.cache, keys_.storage);
        mark_ready(ready_cache);

        if (startup_stage_hook_)
            startup_stage_hook_("retention");
        if (stop.stop_requested())
            return;
        retention_ = std::make_unique<RetentionStore>(cfg_.state_path, keys_.storage);
        mark_ready(ready_retention);

        if (startup_stage_hook_)
            startup_stage_hook_("metadata");
        if (stop.stop_requested())
            return;
        meta_ = std::make_unique<MetadataReplica>(cfg_.state_path, keys_.storage,
                                                 cache_->metadata(), cfg_.bootstrap.empty(),
                                                 cfg_.metadata_materialization_cache_bytes);

        // Identity-reset tombstones must be active before metadata exchange.
        try {
            const auto committed_snapshot = decode_snapshot(meta_->committed().payload);
            for (const auto& [_, reset] : committed_snapshot.identity_resets)
                apply_identity_reset(reset);
        } catch (const std::exception& error) {
            Log::debug("cannot preload identity reset tombstones: " + std::string(error.what()));
        }

        cache_->remember_metadata(meta_->committed());
        const auto generation = meta_->committed().generation;
        members_.metadata_generation(generation);
        server_.set_local(members_.self());
        telemetry_metadata_generation_.store(generation, std::memory_order_relaxed);
        mark_ready(ready_metadata);
        Log::info("node metadata ready generation=" + std::to_string(generation));
    } catch (const std::exception& error) {
        Log::error("node local state recovery failed: " + std::string(error.what()));
        mark_recovery_failed("local state: " + std::string(error.what()));
    }
}

void NodeRuntime::start() {
    if (started_.exchange(true))
        return;

    if (startup_stage_hook_)
        startup_stage_hook_("control-plane");

    // Bring the control plane online before any potentially expensive local
    // backend recovery. Peers can authenticate this node immediately and Status
    // can distinguish reachability from readiness.
    server_.start();
    mark_ready(ready_control_plane);
    Log::info("node " + to_string(id_).substr(0, 12) + " listening on " +
              std::to_string(server_.bound_port()) + " domain=" + members_.self().failure_domain +
              " state=recovering");

    telemetry_worker_ = std::jthread([this](std::stop_token stop) {
        run_supervised("cluster-telemetry", [this, stop] { telemetry_loop(stop); });
    });
    local_writer_ = std::jthread([this](std::stop_token stop) {
        run_supervised("cluster-local-writer", [this, stop] { local_writer_loop(stop); });
    });
    maintenance_ = std::jthread([this](std::stop_token stop) {
        run_supervised("cluster-maintenance", [this, stop] { loop(stop); });
    });
    storage_recovery_ = std::jthread([this](std::stop_token stop) {
        run_supervised("cluster-storage-recovery", [this, stop] { recover_storage(stop); });
    });
    state_recovery_ = std::jthread([this](std::stop_token stop) {
        run_supervised("cluster-state-recovery", [this, stop] { recover_state(stop); });
    });
    connectivity_worker_ = std::jthread([this](std::stop_token stop) {
        run_supervised("cluster-connectivity", [this, stop] {
            if (stop.stop_requested())
                return;
            (void)refresh_public_connectivity(false);
            if (cfg_.connectivity_check.enabled && !stop.stop_requested())
                (void)public_connectivity_.probe(false);
        });
    });
}

void NodeRuntime::request_stop() {
    data_resources_.stop();
    retained_memory_.stop();
    if (storage_recovery_.joinable())
        storage_recovery_.request_stop();
    if (state_recovery_.joinable())
        state_recovery_.request_stop();
    if (connectivity_worker_.joinable())
        connectivity_worker_.request_stop();
    if (telemetry_worker_.joinable()) {
        telemetry_worker_.request_stop();
        telemetry_wait_cv_.notify_all();
    }
    if (maintenance_.joinable()) {
        Log::debug("shutdown: node maintenance request_stop");
        maintenance_.request_stop();
        maintenance_wait_cv_.notify_all();
    }
    if (local_writer_.joinable()) {
        Log::debug("shutdown: local writer request_stop");
        local_writer_.request_stop();
        local_copy_cv_.notify_all();
    }
    readiness_cv_.notify_all();
}

void NodeRuntime::cancel_outbound_calls() {
    if (outbound_calls_stopped_.exchange(true))
        return;
    Log::debug("shutdown: cancelling outbound RPC calls");
    client_.stop();
}

void NodeRuntime::stop() {
    if (!started_.exchange(false)) {
        Log::debug("shutdown: NodeRuntime::stop already stopped");
        return;
    }
    Log::debug("shutdown: NodeRuntime::stop begin");
    request_stop();

    // Close transport promptly; recovery never owns transport state.
    Log::debug("shutdown: RpcServer::stop calling");
    server_.stop();
    Log::debug("shutdown: RpcServer::stop returned");
    Log::debug("shutdown: RpcClient::stop calling");
    cancel_outbound_calls();
    Log::debug("shutdown: RpcClient::stop returned");

    for (auto* worker : {&connectivity_worker_, &storage_recovery_, &state_recovery_,
                         &telemetry_worker_, &maintenance_, &local_writer_}) {
        if (worker->joinable())
            worker->join();
    }
    Log::debug("shutdown: NodeRuntime::stop complete");
}

std::chrono::milliseconds NodeRuntime::stall_notice_for(MessageType type) const {
    if (type == MessageType::get_object || type == MessageType::put_object ||
        type == MessageType::put_object_deferred || type == MessageType::object_durability_barrier)
        return cfg_.data_stall_notice;
    return cfg_.control_stall_notice;
}

RpcReply NodeRuntime::call(const NodeInfo& node, MessageType type,
                           std::span<const uint8_t> payload) {
    if (outbound_calls_stopped_.load(std::memory_order_acquire))
        throw std::runtime_error("node is stopping; outbound RPC is unavailable");
    return client_.call(node, type, payload, stall_notice_for(type));
}

RpcReply NodeRuntime::call(const Endpoint& endpoint, MessageType type,
                           std::span<const uint8_t> payload) {
    if (outbound_calls_stopped_.load(std::memory_order_acquire))
        throw std::runtime_error("node is stopping; outbound RPC is unavailable");
    return client_.call(endpoint, type, payload, stall_notice_for(type));
}

RpcReply NodeRuntime::call(const NodeInfo& node, MessageType type, std::span<const uint8_t> payload,
                           FrameType frame_type) {
    if (outbound_calls_stopped_.load(std::memory_order_acquire))
        throw std::runtime_error("node is stopping; outbound RPC is unavailable");
    return client_.call(node, type, payload, frame_type, stall_notice_for(type));
}

RpcReply NodeRuntime::call(const Endpoint& endpoint, MessageType type,
                           std::span<const uint8_t> payload, FrameType frame_type) {
    if (outbound_calls_stopped_.load(std::memory_order_acquire))
        throw std::runtime_error("node is stopping; outbound RPC is unavailable");
    return client_.call(endpoint, type, payload, frame_type, stall_notice_for(type));
}

AsyncRpc NodeRuntime::call_async(const NodeInfo& node, MessageType type,
                                 std::span<const uint8_t> payload) {
    if (outbound_calls_stopped_.load(std::memory_order_acquire))
        throw std::runtime_error("node is stopping; outbound RPC is unavailable");
    return client_.call_async(node, type, payload);
}

AsyncRpc NodeRuntime::call_async(const Endpoint& endpoint, MessageType type,
                                 std::span<const uint8_t> payload) {
    if (outbound_calls_stopped_.load(std::memory_order_acquire))
        throw std::runtime_error("node is stopping; outbound RPC is unavailable");
    return client_.call_async(endpoint, type, payload);
}

AsyncRpc NodeRuntime::call_async(const NodeInfo& node, MessageType type,
                                 std::span<const uint8_t> payload, FrameType frame_type) {
    if (outbound_calls_stopped_.load(std::memory_order_acquire))
        throw std::runtime_error("node is stopping; outbound RPC is unavailable");
    return client_.call_async(node, type, payload, frame_type);
}

AsyncRpc NodeRuntime::call_async(const Endpoint& endpoint, MessageType type,
                                 std::span<const uint8_t> payload, FrameType frame_type) {
    if (outbound_calls_stopped_.load(std::memory_order_acquire))
        throw std::runtime_error("node is stopping; outbound RPC is unavailable");
    return client_.call_async(endpoint, type, payload, frame_type);
}

void NodeRuntime::note_activity(FrameType type, uint64_t bytes) {
    const auto now = activity_now_ms();
    if (type == FrameType::foreground) {
        playback_activity_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        last_playback_activity_ms_.store(now, std::memory_order_relaxed);
    } else if (type == FrameType::read_ahead) {
        interactive_activity_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        last_interactive_activity_ms_.store(now, std::memory_order_relaxed);
    }
}

void NodeRuntime::set_service_event_callback(std::function<void(ServiceEvent)> callback) {
    std::lock_guard lock(service_event_mutex_);
    service_event_ = std::move(callback);
}

void NodeRuntime::set_ingest_bridge(JobsQueryHandler jobs, JobActionHandler action) {
    std::lock_guard lock(job_bridge_mutex_);
    ingest_jobs_handler_ = std::move(jobs);
    ingest_action_handler_ = std::move(action);
}

void NodeRuntime::set_torrent_bridge(JobsQueryHandler jobs, JobActionHandler action) {
    std::lock_guard lock(job_bridge_mutex_);
    torrent_jobs_handler_ = std::move(jobs);
    torrent_action_handler_ = std::move(action);
}

void NodeRuntime::notify_storage_mutation() {
    signal_service_event(ServiceEvent::storage);
}

void NodeRuntime::signal_service_event(ServiceEvent event) {
    std::function<void(ServiceEvent)> callback;
    {
        std::lock_guard lock(service_event_mutex_);
        callback = service_event_;
    }
    if (callback)
        callback(event);
}

uint64_t NodeRuntime::take_activity_bytes(FrameType type) {
    if (type == FrameType::foreground)
        return playback_activity_bytes_.exchange(0, std::memory_order_relaxed);
    if (type == FrameType::read_ahead)
        return interactive_activity_bytes_.exchange(0, std::memory_order_relaxed);
    return 0;
}

std::chrono::milliseconds NodeRuntime::activity_idle_for(FrameType type) const {
    int64_t last = 0;
    if (type == FrameType::foreground)
        last = last_playback_activity_ms_.load(std::memory_order_relaxed);
    else if (type == FrameType::read_ahead)
        last = last_interactive_activity_ms_.load(std::memory_order_relaxed);
    if (!last)
        return std::chrono::hours(24);
    return std::chrono::milliseconds(std::max<int64_t>(0, activity_now_ms() - last));
}

void NodeRuntime::announce_metadata_generation(uint64_t generation) {
    // Accepted-head topology can change without increasing the maximum metadata
    // generation (for example, a concurrent same-generation sibling arriving
    // over RPC).  MetadataManager caches key off this epoch as well as the
    // generation, so advance it for local acceptance changes before broadcasting
    // the notice.  Otherwise a node can keep serving its pre-sibling snapshot
    // until the cache TTL expires even though the sibling is already durably
    // accepted locally.
    metadata_announcements_.fetch_add(1, std::memory_order_relaxed);
    remote_metadata_epoch_.fetch_add(1, std::memory_order_acq_rel);
    signal_service_event(ServiceEvent::metadata);
    members_.metadata_generation(generation);
    server_.set_local(members_.self());
    Writer writer;
    writer.u64(generation);
    client_.broadcast({MessageType::metadata_notice, writer.take()});
}

bool NodeRuntime::store_metadata_commit(const MetadataHistoryEntry& entry) {
    return metadata_replica().import_history(entry);
}

bool NodeRuntime::accept_metadata_commit(const MetadataAcceptance& acceptance) {
    // A protocol-20 node accepts only branches whose *resulting* cluster policy
    // matches its configured policy. The certificate's own `required` value may
    // be stronger during a safe policy transition (for example W=3 -> W=2), so
    // comparing it directly with the local configuration would incorrectly
    // reject the transition. MetadataReplica validates the certificate against
    // the commit and its parent policies.
    if (acceptance.required) {
        auto materialized = metadata_replica().materialized(acceptance.hash);
        if (!materialized)
            return false;
        if (materialized->snapshot->metadata_write_replicas_required !=
            cfg_.metadata_min_write_replicas)
            return false;
    }
    const auto heads_before = metadata_replica().accepted_head_certificates();
    const auto before = metadata_replica().committed();
    if (!metadata_replica().accept_commit(acceptance))
        return false;
    const auto heads_after = metadata_replica().accepted_head_certificates();
    const auto after = metadata_replica().committed();
    members_.metadata_generation(std::max(after.generation, acceptance.generation));
    if (heads_after == heads_before)
        return true;
    if (after.hash != before.hash)
        block_cache().remember_metadata(after);
    // A same-generation sibling may not change the materialised preferred head,
    // but peers still need an ordinary metadata wake-up so foreground cache
    // validation and background reconciliation notice the changed head set.
    announce_metadata_generation(std::max(after.generation, acceptance.generation));
    return true;
}

std::vector<MetadataAcceptance> NodeRuntime::metadata_heads() const {
    return metadata_replica().accepted_head_certificates();
}

bool NodeRuntime::accept_history_checkpoint_proposal(const HistoryCheckpointProof& proposal) {
    return metadata_replica().record_checkpoint_ack(proposal);
}

bool NodeRuntime::commit_history_checkpoint(const Hash256& floor_hash, const Hash256& epoch) {
    return metadata_replica().record_checkpoint_commit(floor_hash, epoch);
}

RpcMessage NodeRuntime::handle(const NodeInfo&, FrameType frame_type, const RpcMessage& request) {
    try {
        // Health/control must never depend on storage I/O. Capacity is refreshed
        // by the node maintenance loop and after successful mutations below.
        switch (request.type) {
        case MessageType::ping:
            return {MessageType::ok, {}};
        case MessageType::members: {
            auto nodes = members_.all();
            Writer writer;
            writer.u32(nodes.size());
            for (const auto& node : nodes)
                encode_node_info(writer, node);
            return {MessageType::members_reply, writer.take()};
        }
        case MessageType::telemetry: {
            if (!request.payload.empty()) {
                try {
                    for (auto& value : decode_telemetry_set(request.payload))
                        telemetry_.observe(std::move(value));
                } catch (const DecodeError&) {
                    // Accept the short-lived request/reply form emitted by the
                    // first 0.18.2 build during a rolling patch update.
                    telemetry_.observe(decode_node_telemetry(request.payload), true);
                }
            }
            const auto gossip_ttl = std::max(cfg_.dead_after * 2, std::chrono::milliseconds(60000));
            return {MessageType::telemetry_reply,
                    encode_telemetry_set(telemetry_.recent(gossip_ttl, 64))};
        }
        case MessageType::identity_resets: {
            if (!request.payload.empty())
                for (const auto& reset : decode_identity_resets(request.payload))
                    apply_identity_reset(reset);
            return {MessageType::identity_resets_reply, encode_identity_resets(identity_resets())};
        }
        case MessageType::session_sync: {
            if (!request.payload.empty())
                for (const auto& session : decode_sessions(request.payload))
                    apply_session(session);
            const auto gossip_ttl = std::max(cfg_.dead_after * 2, std::chrono::milliseconds(60000));
            return {MessageType::session_sync_reply, encode_sessions(sessions_.recent(gossip_ttl, 64))};
        }
        case MessageType::have_object: {
            Reader reader(request.payload);
            ObjectId id{reader.fixed<32>()};
            reader.finish();
            Writer writer;
            auto resource = data_resources_.try_acquire(
                DataWorkContext(frame_type, cfg_.extent_size), cfg_.extent_size);
            if (!resource)
                return error_reply("DATA resource admission busy or stopping");
            // This single-object probe is shared by repair/rebalance placement
            // logic that has no separate re-verification step before trusting
            // "yes, already present" -- unlike the batched have_objects below,
            // which is used only by retain_data()'s candidate selection, where
            // retain_objects always re-verifies before persisting a claim.
            // Authenticate/decrypt/hash here so a corrupt remote replica is
            // never counted as healthy placement.
            writer.u8(local_store().valid(id));
            return {MessageType::bool_reply, writer.take()};
        }
        case MessageType::have_objects: {
            Reader reader(request.payload);
            const auto count = reader.u32();
            if (!count || count > 200000)
                return error_reply("invalid presence batch count");
            std::vector<ObjectId> ids;
            ids.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                ObjectId id;
                id.bytes = reader.fixed<32>();
                ids.push_back(id);
            }
            reader.finish();
            // Unlike have_object above, this batched form is used exclusively
            // by retain_data()'s candidate-selection scan (DistributedStore::
            // select_present_batched), never by repair/rebalance. A "present"
            // answer here only makes a node a *candidate*; retain_objects
            // still fully verifies before persisting a retain_batch claim on
            // it, so a stale/corrupt local copy is caught there, not lost.
            // That downstream re-verification is what makes it safe for this
            // one caller to skip the per-object decrypt at candidate-selection
            // scale. One admission charge for the whole batch, not one per
            // id, since this no longer does per-object I/O worth separately
            // metering against the DATA budget ordinary reads/writes consume.
            auto resource = data_resources_.try_acquire(
                DataWorkContext(frame_type, cfg_.extent_size), cfg_.extent_size);
            if (!resource)
                return error_reply("DATA resource admission busy or stopping");
            Writer writer;
            writer.u32(count);
            for (const auto& id : ids)
                writer.u8(local_store().has(id));
            return {MessageType::have_objects_reply, writer.take()};
        }
        case MessageType::get_object: {
            Reader reader(request.payload);
            ObjectId id{reader.fixed<32>()};
            reader.finish();
            auto resource = data_resources_.acquire(
                DataWorkContext(frame_type, cfg_.extent_size), cfg_.extent_size);
            if (!resource)
                return error_reply("DATA resource admission stopping");
            auto data = local_store().get(id);
            if (!data)
                return error_reply("object not found");
            note_activity(frame_type, data->size());
            Writer writer;
            writer.fixed(id.bytes);
            writer.bytes(*data);
            return {MessageType::object_reply, writer.take()};
        }
        case MessageType::get_control_object: {
            Reader reader(request.payload);
            ObjectId id{reader.fixed<32>()};
            reader.finish();
            auto data = control_store().get(id);
            if (!data)
                return error_reply("control object not found");
            Writer writer;
            writer.fixed(id.bytes);
            writer.bytes(*data);
            return {MessageType::control_object_reply, writer.take()};
        }
        case MessageType::put_object:
        case MessageType::put_object_deferred: {
            Reader reader(request.payload);
            ObjectId id{reader.fixed<32>()};
            auto data = reader.bytes(128 * 1024 * 1024);
            reader.finish();
            if (data.size() > cfg_.extent_size)
                return error_reply("DATA object exceeds configured extent size");
            auto resource = data_resources_.acquire(
                DataWorkContext(frame_type, data.size()), data.size());
            if (!resource)
                return error_reply("DATA resource admission stopping");
            note_activity(frame_type, data.size());
            if (!local_store().has(id))
                notify_storage_mutation();
            if (request.type == MessageType::put_object_deferred) {
                const auto generation = local_store().put_deferred(id, data);
                if (!generation)
                    return error_reply("storage limit reached");
                members_.storage(local_store().used(), local_store().limit());
                // Bind provisional placement to this exact process lifetime and
                // exact node-wide mutation generation. A later barrier for an
                // already-covered generation is a no-op even when unrelated
                // newer writes are currently dirty on this node.
                Writer reply;
                reply.fixed(durability_epoch_.bytes);
                reply.u64(generation->domain);
                reply.u64(generation->generation);
                reply.u64(generation->backend_instance);
                return {MessageType::ok, reply.take()};
            }
            if (!local_store().put(id, data))
                return error_reply("storage limit reached");
            members_.storage(local_store().used(), local_store().limit());
            return {MessageType::ok, {}};
        }
        case MessageType::put_control_object: {
            Reader reader(request.payload);
            ObjectId id{reader.fixed<32>()};
            auto data = reader.bytes(128 * 1024 * 1024);
            reader.finish();
            if (!control_store().put(id, data))
                return error_reply("control storage limit reached");
            return {MessageType::ok, {}};
        }
        case MessageType::object_durability_barrier: {
            Reader reader(request.payload);
            NodeId expected_epoch{reader.fixed<16>()};
            const auto domain = reader.u64();
            const auto required_generation = reader.u64();
            const auto backend_instance = reader.u64();
            // 0.29: an optional trailing id list turns a refusal into a probe.
            std::vector<ObjectId> probe_ids;
            if (reader.remaining()) {
                const auto count = reader.u32();
                if (count > 4096)
                    return error_reply("too many durability probe ids");
                probe_ids.reserve(count);
                for (uint32_t i = 0; i < count; ++i)
                    probe_ids.push_back(ObjectId{reader.fixed<32>()});
            }
            reader.finish();
            if (expected_epoch != durability_epoch_) {
                // The requester holds a placement token from a previous
                // incarnation of this process. Nothing can make that token
                // true again -- but the *objects* may well be on disk, and
                // that is the fact the requester actually needs. With ids,
                // answer from the disk and hand out fresh tokens (discipline
                // 1 of the self-healing plan: re-derive, don't assert).
                // Without ids (a pre-0.29 requester), refuse as before.
                if (probe_ids.empty()) {
                    Log::debug("object durability barrier refused: epoch changed expected=" +
                               to_string(expected_epoch).substr(0, 8) +
                               " current=" + to_string(durability_epoch_).substr(0, 8) +
                               " domain=" + std::to_string(domain) +
                               " generation=" + std::to_string(required_generation));
                    return error_reply("storage durability epoch changed");
                }
                Writer reply;
                reply.fixed(durability_epoch_.bytes);
                std::vector<std::pair<ObjectId, StoragePool::DurabilityToken>> present;
                present.reserve(probe_ids.size());
                for (const auto& id : probe_ids)
                    if (auto token = local_store().reassert_durable(id))
                        present.emplace_back(id, *token);
                reply.u32(static_cast<uint32_t>(present.size()));
                for (const auto& [id, token] : present) {
                    reply.fixed(id.bytes);
                    reply.u64(token.domain);
                    reply.u64(token.generation);
                    reply.u64(token.backend_instance);
                }
                Log::info("object durability re-derived after epoch change present=" +
                          std::to_string(present.size()) + "/" +
                          std::to_string(probe_ids.size()) + " expected=" +
                          to_string(expected_epoch).substr(0, 8) +
                          " current=" + to_string(durability_epoch_).substr(0, 8));
                members_.storage(local_store().used(), local_store().limit());
                return {MessageType::ok, reply.take()};
            }
            try {
                local_store().durability_barrier({domain, required_generation, backend_instance},
                                                 DurabilityUrgency::batchable);
                members_.storage(local_store().used(), local_store().limit());
                return {MessageType::ok, {}};
            } catch (const std::exception& error) {
                return error_reply(std::string("storage durability barrier failed: ") +
                                   error.what());
            }
        }
        case MessageType::retain_objects: {
            Reader reader(request.payload);
            const auto raw_class = reader.u8();
            if (raw_class < static_cast<uint8_t>(RetentionClass::data) ||
                raw_class > static_cast<uint8_t>(RetentionClass::control))
                return error_reply("invalid retention object class");
            const auto object_class = static_cast<RetentionClass>(raw_class);
            RetentionDot dot;
            dot.origin.bytes = reader.fixed<16>();
            dot.sequence = reader.u64();
            const auto count = reader.u32();
            if (!count || count > 1000000)
                return error_reply("invalid retention object count");
            std::vector<ObjectId> ids;
            ids.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                ObjectId id;
                id.bytes = reader.fixed<32>();
                ids.push_back(id);
            }
            reader.finish();
            for (const auto& id : ids) {
                auto resource = data_resources_.try_acquire(
                    DataWorkContext(frame_type, cfg_.extent_size), cfg_.extent_size);
                if (!resource)
                    return error_reply("DATA resource admission busy or stopping");
                const bool present = object_class == RetentionClass::data
                                         ? local_store().valid(id)
                                         : control_store().valid(id);
                if (!present)
                    return error_reply("retention object is not durably present");
            }
            retention_store().retain_batch(object_class, ids, dot);
            return {MessageType::ok, {}};
        }
        case MessageType::delete_object: {
            Reader reader(request.payload);
            ObjectId id{reader.fixed<32>()};
            reader.finish();
            if (retention_store().retained(RetentionClass::data, id))
                return error_reply("object has an active retention claim");
            auto resource = data_resources_.try_acquire(
                DataWorkContext(frame_type, cfg_.extent_size), cfg_.extent_size);
            if (!resource)
                return error_reply("DATA resource admission busy or stopping");
            (void)local_store().remove(id);
            if (ready(ready_cache))
                (void)block_cache().remove(id);
            members_.storage(local_store().used(), local_store().limit());
            return {MessageType::ok, {}};
        }
        case MessageType::get_metadata:
            return {MessageType::metadata_reply,
                    encode_metadata_record(metadata_replica().current())};
        case MessageType::get_committed_metadata:
            return {MessageType::metadata_reply,
                    encode_metadata_record(metadata_replica().committed())};
        case MessageType::get_metadata_identity:
            return metadata_identity_reply(metadata_replica().committed_identity());
        case MessageType::get_metadata_history_entry: {
            Reader reader(request.payload);
            Hash256 hash;
            hash.bytes = reader.fixed<32>();
            reader.finish();
            auto entry = metadata_replica().history_entry(hash);
            if (!entry)
                return error_reply("metadata history entry unavailable");
            return {MessageType::metadata_history_entry_reply,
                    encode_metadata_history_entry(*entry)};
        }
        case MessageType::get_metadata_history_record: {
            // Live repair of a peer's unreconstructable accepted head: serve the
            // record materialized here as a full body, whatever frame shape this
            // replica happens to store it in. See MetadataManager::
            // repair_unreconstructable_heads().
            Reader reader(request.payload);
            Hash256 hash;
            hash.bytes = reader.fixed<32>();
            reader.finish();
            auto entry = metadata_replica().full_history_record(hash);
            if (!entry)
                return error_reply("metadata history record unavailable");
            return {MessageType::metadata_history_entry_reply,
                    encode_metadata_history_entry(*entry)};
        }
        case MessageType::has_metadata_history_entry: {
            Reader reader(request.payload);
            Hash256 hash;
            hash.bytes = reader.fixed<32>();
            reader.finish();
            Writer writer;
            writer.u8(metadata_replica().history_contains(hash));
            return {MessageType::bool_reply, writer.take()};
        }
        case MessageType::put_metadata_history_entry: {
            auto entry = decode_metadata_history_entry(request.payload);
            Writer writer;
            writer.u8(metadata_replica().import_history(entry));
            return {MessageType::bool_reply, writer.take()};
        }
        case MessageType::get_metadata_heads:
            return {MessageType::metadata_heads_reply,
                    encode_metadata_acceptance_set(metadata_heads())};
        case MessageType::get_ingest_jobs: {
            JobsQueryHandler handler;
            {
                std::lock_guard lock(job_bridge_mutex_);
                handler = ingest_jobs_handler_;
            }
            if (!handler) return error_reply("ingest not available on this node");
            return {MessageType::ingest_jobs_reply, handler(request.payload)};
        }
        case MessageType::ingest_job_action: {
            JobActionHandler handler;
            {
                std::lock_guard lock(job_bridge_mutex_);
                handler = ingest_action_handler_;
            }
            if (!handler) return error_reply("ingest not available on this node");
            return {MessageType::ingest_job_action_reply, handler(request.payload)};
        }
        case MessageType::get_torrent_jobs: {
            JobsQueryHandler handler;
            {
                std::lock_guard lock(job_bridge_mutex_);
                handler = torrent_jobs_handler_;
            }
            if (!handler) return error_reply("torrents not available on this node");
            return {MessageType::torrent_jobs_reply, handler(request.payload)};
        }
        case MessageType::torrent_job_action: {
            JobActionHandler handler;
            {
                std::lock_guard lock(job_bridge_mutex_);
                handler = torrent_action_handler_;
            }
            if (!handler) return error_reply("torrents not available on this node");
            return {MessageType::torrent_job_action_reply, handler(request.payload)};
        }
        case MessageType::put_metadata_commit: {
            auto entry = decode_metadata_history_entry(request.payload);
            Writer writer;
            writer.u8(store_metadata_commit(entry));
            return {MessageType::bool_reply, writer.take()};
        }
        case MessageType::accept_metadata_commit: {
            auto acceptance = decode_metadata_acceptance(request.payload);
            Writer writer;
            writer.u8(accept_metadata_commit(acceptance));
            return {MessageType::bool_reply, writer.take()};
        }
        case MessageType::propose_history_floor: {
            auto proposal = decode_history_checkpoint_proof(request.payload);
            Writer writer;
            writer.u8(accept_history_checkpoint_proposal(proposal));
            return {MessageType::bool_reply, writer.take()};
        }
        case MessageType::commit_history_floor: {
            auto commit = decode_history_checkpoint_proof(request.payload);
            Writer writer;
            writer.u8(commit_history_checkpoint(commit.floor_hash, commit.epoch));
            return {MessageType::bool_reply, writer.take()};
        }
        case MessageType::seed_metadata:
        case MessageType::checkpoint_metadata:
        case MessageType::commit_metadata:
        case MessageType::cas_metadata:
        case MessageType::cas_metadata_delta:
            return error_reply(
                "legacy metadata CAS/PREPARE/COMMIT RPC is unavailable in protocol 20");
        case MessageType::metadata_notice:
            return error_reply("metadata notice is server-originated");
        default:
            return error_reply("unsupported request");
        }
    } catch (const std::exception& error) {
        return error_reply(error.what());
    }
}

void NodeRuntime::merge(std::span<const uint8_t> payload) {
    const auto before_all = members_.all();
    const auto before_active = members_.active();
    const auto previous_generation = remote_metadata_generation_.load();
    bool membership_changed = false;
    Reader reader(payload);
    auto count = reader.u32();
    if (count > 100000)
        throw DecodeError("member list too large");
    uint64_t newest_metadata = previous_generation;
    for (uint32_t i = 0; i < count; ++i) {
        auto node = decode_node_info(reader);
        newest_metadata = std::max(newest_metadata, node.metadata_generation);
        const auto previous =
            std::find_if(before_all.begin(), before_all.end(),
                         [&](const NodeInfo& item) { return item.id == node.id; });
        membership_changed =
            membership_changed || previous == before_all.end() || previous->host != node.host ||
            previous->port != node.port || previous->failure_domain != node.failure_domain ||
            previous->metadata_write_replicas_required != node.metadata_write_replicas_required;
        members_.observe(std::move(node));
    }
    reader.finish();
    remote_metadata_generation_.store(newest_metadata);

    // Membership exchange is a heartbeat. Repeated identical gossip must not
    // wake event-driven maintenance (and, in particular, must not perpetually
    // restart its GC quiet window). Wake only for scheduler-relevant state:
    // roster/endpoint changes, an active-set transition, or newer metadata.
    auto active_ids = [](const std::vector<NodeInfo>& nodes) {
        std::vector<NodeId> ids;
        ids.reserve(nodes.size());
        for (const auto& node : nodes)
            ids.push_back(node.id);
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    const bool topology_changed =
        membership_changed || active_ids(before_active) != active_ids(members_.active());
    if (topology_changed || newest_metadata > previous_generation)
        signal_service_event(topology_changed ? ServiceEvent::topology : ServiceEvent::metadata);
}

PublicConnectivityStatus NodeRuntime::public_connectivity_status() const {
    return public_connectivity_.status();
}

PublicConnectivityStatus NodeRuntime::refresh_public_connectivity(bool probe, bool force_probe) {
    const auto before = members_.self();
    const auto status = public_connectivity_.refresh(probe, force_probe);
    if (!status.advertised.host.empty() && status.advertised.port &&
        (status.advertised.host != before.host || status.advertised.port != before.port)) {
        members_.endpoint(status.advertised.host, status.advertised.port);
        server_.set_local(members_.self());
        Log::info("node advertised endpoint changed from=" + before.host + ":" +
                  std::to_string(before.port) + " to=" + status.advertised.host + ":" +
                  std::to_string(status.advertised.port) + " source=" + status.advertised_source);
    }
    return status;
}

void NodeRuntime::refresh_telemetry() {
    // Telemetry is valid during recovery. Unready local planes report zero
    // online capacity/usage rather than making the node disappear from the
    // cluster while recovery is in progress.
    auto info = members_.self();
    info.used = telemetry_storage_used_.load(std::memory_order_relaxed);
    info.capacity = telemetry_storage_capacity_.load(std::memory_order_relaxed);
    info.metadata_generation = telemetry_metadata_generation_.load(std::memory_order_relaxed);
    uint64_t cache_capacity = 0;
    uint64_t cache_used = 0;
    uint32_t storage_backends_online = 0;
    if (ready(ready_cache) && cache_) {
        cache_capacity = static_cast<uint64_t>(cfg_.cache.max_blocks) * cfg_.extent_size;
        cache_used = static_cast<uint64_t>(cache_->blocks()) * cfg_.extent_size;
    }
    if (ready(ready_data_storage) && local_)
        storage_backends_online = static_cast<uint32_t>(local_->online_backends());
    const auto peers_known = telemetry_peers_known_.load(std::memory_order_relaxed);
    const auto peers_active = telemetry_peers_active_.load(std::memory_order_relaxed);

    // Mirror the local root.startup.phase vocabulary (see
    // ClusterStatusService::status_response) so a peer observing this node's
    // telemetry can tell a genuinely current measurement (ready) from one
    // whose zeroed capacity/usage above is only a recovery artefact, rather
    // than treating every fresh sample as authoritative. A failed node is
    // reported as "recovering" here: telemetry has no separate wire state for
    // it, and a caller can always query this node's own Status root for the
    // precise "failed" detail.
    const auto local_readiness = readiness();
    const auto phase = local_readiness.failed         ? NodePhase::recovering
                        : local_readiness.local_state_ready ? NodePhase::ready
                        : local_readiness.control_plane_online ? NodePhase::recovering
                                                                : NodePhase::starting;

    // Advertised API address for cluster peers (Status nodes[].api_host/
    // api_port); empty/0 when this node runs no catalogue API, letting peers
    // correctly treat it as unreported rather than guessing. Defaults to
    // `info.host` -- this node's already-resolved RPC advertise address --
    // rather than catalogue.api.listen: the API, like RPC, conventionally
    // binds a wildcard address (0.0.0.0), which is not itself dialable by a
    // peer, so falling back to the raw listen address would readvertise that
    // wildcard instead of a real endpoint.
    std::string api_host;
    uint16_t api_port = 0;
    if (cfg_.catalogue.api.enabled) {
        api_host = cfg_.catalogue.api.advertised_host.empty() ? info.host
                                                               : cfg_.catalogue.api.advertised_host;
        api_port = cfg_.catalogue.api.advertised_port ? cfg_.catalogue.api.advertised_port
                                                       : cfg_.catalogue.api.port;
    }

    telemetry_.refresh_local(info, std::string(kServerVersion), cache_capacity, cache_used,
                             storage_backends_online, peers_known, peers_active, 0, 0,
                             peers_active > 0 ? peers_active - 1 : 0, phase, std::move(api_host),
                             api_port);
}

void NodeRuntime::signal_telemetry_refresh() {
    telemetry_demand_.fetch_add(1, std::memory_order_release);
    telemetry_wait_cv_.notify_all();
}

void NodeRuntime::telemetry_loop(std::stop_token stop) {
    ThreadCpuReporter cpu_reporter("macha-telemetry", std::chrono::seconds(5), true);
    const auto interval = std::chrono::seconds(5);
    const auto idle_before_gossip = std::chrono::seconds(2);
    const auto gossip_ttl = std::max(cfg_.dead_after * 2, std::chrono::milliseconds(60000));
    uint64_t handled_demand = 0;
    while (!stop.stop_requested()) {
        const auto demand = telemetry_demand_.load(std::memory_order_acquire);
        try {
            refresh_telemetry();
            // Sampling is always local. Network gossip is suppressed while the
            // node has recent foreground/read-ahead work, then additionally
            // uses no-wait/idle-writer admission in RpcClient. Telemetry is the
            // first thing dropped when the node is doing useful work.
            const bool operationally_idle =
                activity_idle_for(FrameType::foreground) >= idle_before_gossip &&
                activity_idle_for(FrameType::read_ahead) >= idle_before_gossip;
            if (operationally_idle) {
                auto values = telemetry_.recent(gossip_ttl, 64);
                if (!values.empty()) {
                    // This is a no-dial, no-wait notification. It is admitted only
                    // if the RPC routing and per-peer outbound locks are immediately
                    // available, and speculative priority keeps it behind all
                    // operational control/foreground/read-ahead traffic.
                    (void)client_.broadcast_best_effort(
                        {MessageType::telemetry, encode_telemetry_set(values)},
                        FrameType::speculative);
                }
            }
        } catch (const std::exception& error) {
            Log::debug("telemetry refresh skipped: " + std::string(error.what()));
        }
        // Session mutations are already pushed synchronously to every reachable
        // peer (propagate_session), so this is only the self-healing backstop
        // for a peer that was briefly unreachable at mutation time, piggybacked
        // on the existing periodic gossip tick rather than a dedicated thread.
        try {
            sessions_.prune_expired(unix_ms());
            auto values = sessions_.recent(gossip_ttl, 64);
            if (!values.empty())
                (void)client_.broadcast_best_effort(
                    {MessageType::session_sync, encode_sessions(values)}, FrameType::speculative);
        } catch (const std::exception& error) {
            Log::debug("session gossip skipped: " + std::string(error.what()));
        }
        handled_demand = demand;
        cpu_reporter.tick();
        std::unique_lock lock(telemetry_wait_mutex_);
        telemetry_wait_cv_.wait_for(lock, stop, interval, [&] {
            return telemetry_demand_.load(std::memory_order_acquire) != handled_demand;
        });
    }
}

bool NodeRuntime::apply_identity_reset(const IdentityAssociationReset& reset) {
    const bool changed = members_.apply_identity_reset(reset);
    // Keep all consumers idempotently aligned even if one of them learned the
    // tombstone first through a different path.
    telemetry_.apply_identity_reset(reset);
    client_.invalidate_identity_association(reset);
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
    const auto payload = encode_identity_resets({reset});
    for (const auto& peer : members_.active()) {
        if (peer.id == id_)
            continue;
        try {
            auto reply = call(peer, MessageType::identity_resets, payload);
            if (reply.message.type == MessageType::identity_resets_reply)
                for (const auto& learned : decode_identity_resets(reply.message.payload))
                    apply_identity_reset(learned);
        } catch (const std::exception& error) {
            Log::debug("identity reset propagation to " + peer.host + ": " + error.what());
        }
    }
}

bool NodeRuntime::apply_session(const AuthSession& session) {
    return sessions_.apply(session);
}

void NodeRuntime::propagate_session(const AuthSession& session) {
    (void)apply_session(session);
    const auto payload = encode_sessions({session});
    for (const auto& peer : members_.active()) {
        if (peer.id == id_)
            continue;
        try {
            auto reply = call(peer, MessageType::session_sync, payload);
            if (reply.message.type == MessageType::session_sync_reply)
                for (const auto& learned : decode_sessions(reply.message.payload))
                    apply_session(learned);
        } catch (const std::exception& error) {
            Log::debug("session sync propagation to " + peer.host + ": " + error.what());
        }
    }
}

void NodeRuntime::exchange(const Endpoint& endpoint) {
    auto reply = call(endpoint, MessageType::members);
    if (reply.message.type != MessageType::members_reply)
        throw std::runtime_error("membership rejected");
    merge(reply.message.payload);
}

void NodeRuntime::exchange(const NodeInfo& node) {
    // Once membership has authenticated a NodeId, preserve that identity when
    // selecting the route. This lets RpcClient reuse an inbound canonical route
    // immediately instead of treating an advertised endpoint as a fresh dial.
    auto reply = call(node, MessageType::members);
    if (reply.message.type != MessageType::members_reply)
        throw std::runtime_error("membership rejected");
    merge(reply.message.payload);
}

void NodeRuntime::loop(std::stop_token stop) {
    ThreadCpuReporter cpu_reporter("macha-node", std::chrono::seconds(5), true);
    while (!stop.stop_requested()) {
        // Local readiness is orthogonal to membership. Refresh whichever local
        // planes are available, then perform membership exchange regardless.
        if (ready(ready_data_storage) && local_) {
            const auto refresh_started = Clock::now();
            local_->refresh();
            const auto refresh_ms = elapsed_ms(refresh_started);
            if (refresh_ms >= 100 && Log::enabled(LogLevel::all))
                Log::trace("DIAG node-stage stage=storage-refresh elapsed_ms=" +
                           std::to_string(refresh_ms));
            const auto storage_used = local_->used();
            const auto storage_capacity = local_->limit();
            members_.storage(storage_used, storage_capacity);
            telemetry_storage_used_.store(storage_used, std::memory_order_relaxed);
            telemetry_storage_capacity_.store(storage_capacity, std::memory_order_relaxed);
        }
        if (ready(ready_metadata) && meta_) {
            const auto metadata_generation = meta_->generation();
            members_.metadata_generation(metadata_generation);
            telemetry_metadata_generation_.store(metadata_generation, std::memory_order_relaxed);
        }

        std::set<std::pair<std::string, uint16_t>> exchanged;
        const auto known_nodes = members_.all();
        telemetry_peers_known_.store(static_cast<uint32_t>(known_nodes.size()),
                                     std::memory_order_relaxed);
        uint32_t active_peers = 1;
        for (const auto& endpoint : cfg_.bootstrap) {
            exchanged.emplace(endpoint.host, endpoint.port);
            try {
                auto known =
                    std::find_if(known_nodes.begin(), known_nodes.end(), [&](const NodeInfo& node) {
                        return node.id != id_ && node.host == endpoint.host &&
                               node.port == endpoint.port;
                    });
                if (known != known_nodes.end())
                    exchange(*known);
                else
                    exchange(endpoint);
                ++active_peers;
            } catch (const std::exception& error) {
                Log::debug("bootstrap: " + std::string(error.what()));
            }
        }
        for (const auto& node : known_nodes) {
            if (node.id == id_)
                continue;
            if (!exchanged.emplace(node.host, node.port).second)
                continue;
            try {
                exchange(node);
                ++active_peers;
            } catch (const std::exception& error) {
                Log::debug("peer " + node.host + ": " + error.what());
            }
        }
        telemetry_peers_active_.store(active_peers, std::memory_order_relaxed);
        cpu_reporter.tick();
        std::unique_lock wait_lock(maintenance_wait_mutex_);
        maintenance_wait_cv_.wait_for(wait_lock, stop, cfg_.heartbeat, [] { return false; });
    }
}

void NodeRuntime::enqueue_fetched(const ObjectId& id, std::span<const uint8_t> data, bool promote) {
    const bool cache = ready(ready_cache) && cache_ && cache_->enabled();
    if (promote && (!ready(ready_data_storage) || !local_))
        promote = false;
    if (!cache && !promote)
        return;

    auto memory = retained_memory_.try_acquire(MemoryClass::speculative,
                                               MemoryOwner::object_payload, data.size());
    if (!memory)
        return;

    // Do not let opportunistic persistence become back-pressure on playback.
    // If the bounded memory queue is full we simply drop this opportunity; the
    // normal repair loop will converge authoritative replicas later.
    constexpr size_t max_queued_bytes = 256ULL * 1024 * 1024;
    std::lock_guard lock(local_copy_mutex_);
    if (data.size() > max_queued_bytes || local_copy_bytes_ + data.size() > max_queued_bytes)
        return;
    LocalCopyJob job;
    job.id = id;
    job.data.assign(data.begin(), data.end());
    job.promote = promote;
    job.cache = cache;
    job.memory = std::move(*memory);
    local_copy_bytes_ += job.data.size();
    local_copies_.push_back(std::move(job));
    local_copy_cv_.notify_one();
}

void NodeRuntime::local_writer_loop(std::stop_token stop) {
    ThreadCpuReporter cpu_reporter("macha-local-wr");
    while (true) {
        LocalCopyJob job;
        {
            std::unique_lock lock(local_copy_mutex_);
            local_copy_cv_.wait(lock,
                                [&] { return stop.stop_requested() || !local_copies_.empty(); });
            if (stop.stop_requested() && local_copies_.empty())
                return;
            job = std::move(local_copies_.front());
            local_copies_.pop_front();
            local_copy_bytes_ -= job.data.size();
        }
        auto resource = data_resources_.acquire(
            DataWorkContext(FrameType::speculative, job.data.size()), job.data.size());
        if (!resource) {
            if (stop.stop_requested())
                return;
            continue;
        }
        bool cached = false;
        if (job.cache && ready(ready_cache) && cache_)
            cached = cache_->put(job.id, job.data);
        // With a persistent cache, foreground fetches are made durable on the
        // cache device first and authoritative HDD promotion is left to idle
        // maintenance. If the cache write fails (or cache is disabled), retain
        // the already-fetched bytes by promoting here rather than forcing a
        // second network transfer later.
        if (job.promote && (!job.cache || !cached) && ready(ready_data_storage) && local_) {
            (void)local_->put(job.id, job.data);
            members_.storage(local_->used(), local_->limit());
        }
        cpu_reporter.tick();
    }
}

void NodeRuntime::reconfigure_local(const Config& config) {
    auto updated = normalize_config(config);
    if (!all_local_state_ready())
        throw std::runtime_error("node local state is still recovering");
    local_->reconfigure(updated.storage_backends);
    local_->refresh();
    cache_->reconfigure(updated.cache);
    // These fields are node-local policy only and are not consumed by the
    // long-lived networking/metadata threads, so keep the public snapshot in
    // sync with a successful live reload without changing cluster policy.
    cfg_.storage_backends = updated.storage_backends;
    cfg_.cache = updated.cache;
    cfg_.hydration = updated.hydration;
    cfg_.read_ahead_extents = updated.read_ahead_extents;
    members_.storage(local_store().used(), local_store().limit());
}
} // namespace macha
