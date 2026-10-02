// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/cluster.hpp"
#include "diagnostics.hpp"
#include "durable_file.hpp"

#include "codec.hpp"
#include "log.hpp"
#include "startup_progress.hpp"
#include "macha_version.hpp"
#include "supervised.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <thread>
#include <unistd.h>

namespace macha {
namespace {
// Retry delay for a gossip broadcast that missed a peer: long enough not to
// become a load source, short enough for a rejoining node to converge promptly.
constexpr auto gossip_retry_floor = std::chrono::seconds(1);
// Re-announce this often even when nothing changed and every peer is
// believed told: broadcast_best_effort() counts frames QUEUED, not applied,
// so a peer whose inbound route is not yet usable (as while it restarts) can
// be marked told without receiving anything. Announcing on change is the
// optimisation; this is the guarantee.
constexpr auto gossip_reannounce_interval = std::chrono::seconds(30);
} // namespace

namespace {
NodeInfo self_info(const Config& config, const NodeId& id, uint64_t used, uint64_t capacity,
                   uint64_t metadata_generation, uint8_t flags) {
    NodeInfo node;
    node.id = id;
    node.flags = flags;
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

// storage.hosts_extents resolved: `auto` is "yes if I have somewhere to put
// them and peers can fetch them".
bool resolve_hosts_extents(const Config& config, bool inbound_capable) {
    switch (config.hosts_extents) {
    case Tristate::yes:
        return true;
    case Tristate::no:
        return false;
    case Tristate::automatic:
        break;
    }
    return !config.storage_backends.empty() && inbound_capable;
}

constexpr std::array<uint8_t, 8> inbound_resolution_magic{'M', 'A', 'C', 'H', 'I', 'N', 'B', '1'};

std::filesystem::path inbound_resolution_path(const Config& config) {
    return config.state_path / "connectivity" / "inbound.bin";
}

// The starting answer to "can peers connect to me?". A configured value is
// final; `auto` starts from the persisted resolution (so a restart does not
// look like a join/leave to placement), else behaves as capable until a
// dial-back says otherwise.
InboundResolution initial_inbound_resolution(const Config& config) {
    InboundResolution out;
    out.inbound_capable_mode = config.inbound_capable;
    out.hosts_extents_mode = config.hosts_extents;
    switch (config.inbound_capable) {
    case Tristate::yes:
        out.inbound_capable = true;
        out.source = "configured";
        break;
    case Tristate::no:
        out.inbound_capable = false;
        out.source = "configured";
        break;
    case Tristate::automatic: {
        out.inbound_capable = true;
        out.source = "default";
        const auto path = inbound_resolution_path(config);
        try {
            if (std::filesystem::exists(path)) {
                std::ifstream input(path, std::ios::binary);
                Bytes bytes((std::istreambuf_iterator<char>(input)), {});
                Reader reader(bytes);
                if (reader.fixed<8>() != inbound_resolution_magic)
                    throw DecodeError("bad inbound resolution magic");
                out.inbound_capable = reader.u8() != 0;
                out.decided_unix_ms = reader.u64();
                (void)reader.string(4096); // the peer that decided it, informational
                reader.finish();
                out.source = "persisted";
            }
        } catch (const std::exception& error) {
            // Persisted evidence is a convenience; the probe decides again.
            Log::warn("inbound resolution ignored: " + std::string(error.what()));
            out.inbound_capable = true;
            out.source = "default";
        }
        break;
    }
    }
    out.hosts_extents = resolve_hosts_extents(config, out.inbound_capable);
    return out;
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

} // namespace

NodeRuntime::NodeRuntime(Config config, const NodeIdentity& identity, ActivityClocks& activity,
                         DataResourceArbiter& data_resources,
                         RetainedMemoryLedger& retained_memory,
                         TranscodeRateBook& transcode_rates, MessageRoutes& routes, NodeEvents& events,
                         StartupStageHook startup_stage_hook)
    : cfg_(normalize_config(std::move(config))), identity_(identity),
      activity_(activity), data_resources_(data_resources), retained_memory_(retained_memory),
      transcode_rates_(transcode_rates), routes_(routes), events_(events),
      inbound_(initial_inbound_resolution(cfg_)),
      members_(self_info(cfg_, identity_.id, 0, 0, 0,
                         node_flags_for(inbound_.inbound_capable, inbound_.hosts_extents)),
               cfg_.dead_after, cfg_.state_path / "membership" / "known-nodes.bin"),
      public_connectivity_(cfg_, identity_.id, Endpoint{members_.self().host, members_.self().port}),
      telemetry_(identity_.id, cfg_.state_path / "telemetry" / "last-known.bin"),
      sessions_(cfg_.session.anonymous_ttl, cfg_.session.max_sessions,
               cfg_.state_path / "sessions" / "sessions.bin"),
      users_(cfg_.session.max_users, cfg_.state_path / "users" / "users.bin",
             hkdf_sha256(identity_.keys.master, {},
                         std::span<const uint8_t>(
                             reinterpret_cast<const uint8_t*>("macha/users/v1"), 14))),
      client_(
          identity_.keys, [this] { return members_.self(); },
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
                  events_.notify(topology_changed ? NodeEvent::topology
                                                        : NodeEvent::metadata);
          },
          [this](uint64_t generation) {
              auto current = remote_metadata_generation_.load();
              while (current < generation &&
                     !remote_metadata_generation_.compare_exchange_weak(current, generation)) {
              }
              // A notice is sent only when a peer's accepted-head set changes,
              // so an equal generation can still carry new sibling/topology
              // information. Ignore only a notice made stale by a strictly
              // newer generation. Equal-generation heartbeats go through the
              // membership observer above and are non-events.
              if (current <= generation) {
                  remote_metadata_epoch_.fetch_add(1, std::memory_order_acq_rel);
                  events_.notify(NodeEvent::metadata);
              }
          },
          cfg_.connect_timeout, cfg_.heartbeat, cfg_.dead_after, cfg_.max_frame_size,
          &retained_memory_),
      server_(
          cfg_.listen_host, cfg_.port, identity_.keys, members_.self(),
          [this](const NodeInfo& peer, FrameType frame_type, const RpcMessage& request) {
              return routes_.dispatch(peer, frame_type, request);
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
                  events_.notify(topology_changed ? NodeEvent::topology
                                                        : NodeEvent::metadata);
          },
          cfg_.max_frame_size, {}, &retained_memory_),
      startup_stage_hook_(std::move(startup_stage_hook)), startup_unix_ms_(unix_ms()) {
    server_.attach_client(client_);
    // While inbound-incapable, this node keeps both lanes dialled to every
    // capable peer (RpcClient::open_requested_lanes); membership names them.
    client_.set_maintained_peers([this] {
        std::vector<NodeInfo> out;
        for (auto& node : members_.active())
            if (node.id != identity_.id && node_inbound_capable(node))
                out.push_back(std::move(node));
        return out;
    });
    // The transport must know non-dialable peers before the first exchange,
    // not after the first refused dial.
    for (const auto& node : members_.all())
        if (node.id != identity_.id)
            client_.note_peer(node);
    // Membership loaded durable identity-reset tombstones before the
    // transport existed; seed the other consumers so stale routes and
    // telemetry are fenced before the control plane starts.
    for (const auto& reset : members_.identity_resets())
        apply_identity_reset(reset);
    bind_control_routes();
    bind_storage_routes();
}

// The server is stopped first, so no request is in flight.
NodeRuntime::~NodeRuntime() {
    stop();
    unbind_routes();
    metadata_server_.reset();
}

void NodeRuntime::mark_ready(ReadyBit bit) {
    note_startup_progress();
    ready_bits_.fetch_or(static_cast<uint32_t>(bit), std::memory_order_release);
    if (all_local_state_ready() && !ready_unix_ms_.load(std::memory_order_relaxed))
        ready_unix_ms_.store(unix_ms(), std::memory_order_release);
    readiness_cv_.notify_all();
    // Publish the phase now rather than up to a sampling interval later, so
    // peers do not keep seeing "recovering" after the node is ready.
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

void NodeRuntime::advertise_storage(uint64_t used, uint64_t capacity) {
    members_.storage(used, capacity);
    advertised_storage_used_.store(used, std::memory_order_relaxed);
    advertised_storage_capacity_.store(capacity, std::memory_order_relaxed);
}

void NodeRuntime::advertise_storage_backends(uint32_t online) {
    advertised_backends_online_.store(online, std::memory_order_relaxed);
}

void NodeRuntime::advertise_metadata_generation(uint64_t generation) {
    members_.metadata_generation(generation);
    advertised_metadata_generation_.store(generation, std::memory_order_relaxed);
}

void NodeRuntime::advertise_cache(uint64_t capacity, uint64_t used, const CacheActivity& activity) {
    advertised_cache_capacity_.store(capacity, std::memory_order_relaxed);
    advertised_cache_used_.store(used, std::memory_order_relaxed);
    advertised_cache_hits_.store(activity.hits, std::memory_order_relaxed);
    advertised_cache_misses_.store(activity.misses, std::memory_order_relaxed);
    advertised_cache_evictions_.store(activity.evictions, std::memory_order_relaxed);
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
MetadataServer& NodeRuntime::metadata_server() {
    if (!ready(ready_metadata) || !metadata_server_)
        throw std::runtime_error("metadata replica is still recovering");
    return *metadata_server_;
}

PersistentBlockCache& NodeRuntime::block_cache() {
    if (!ready(ready_cache) || !cache_)
        throw std::runtime_error("persistent cache is still recovering");
    return *cache_;
}
ClaimStore& NodeRuntime::claims() {
    if (!ready(ready_retention) || !retention_)
        throw std::runtime_error("retention state is still recovering");
    return *retention_;
}
const ClaimStore& NodeRuntime::claims() const {
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
        auto local = std::make_unique<StoragePool>(cfg_.state_path, identity_.id, cfg_.storage_backends,
                                                   identity_.keys.storage, std::chrono::milliseconds(500),
                                                   cfg_.storage_packing);
        if (stop.stop_requested())
            return;
        const auto used = local->used();
        const auto capacity = local->limit();
        local_ = std::move(local);
        // Device-pressure admission; a zero target disables it.
        if (cfg_.io_pressure_slowdown_percent) {
            local_->configure_service_monitor(DiskServiceMonitor::Thresholds{
                std::chrono::milliseconds(cfg_.io_pressure_overhead_ms),
                std::chrono::milliseconds(cfg_.io_pressure_per_mib_ms),
                cfg_.io_pressure_slowdown_percent, cfg_.io_pressure_release_percent,
                cfg_.io_pressure_outlier_percent});
            data_resources_.observe_device(&local_->service_monitor(),
                                          cfg_.io_pressure_min_background);
            Log::info("data io pressure gate enabled expected_ms=" +
                      std::to_string(cfg_.io_pressure_overhead_ms) + "+" +
                      std::to_string(cfg_.io_pressure_per_mib_ms) + "/MiB slowdown_percent=" +
                      std::to_string(cfg_.io_pressure_slowdown_percent) + " release_percent=" +
                      std::to_string(cfg_.io_pressure_release_percent) + " outlier_percent=" +
                      std::to_string(cfg_.io_pressure_outlier_percent) + " min_background=" +
                      std::to_string(cfg_.io_pressure_min_background));
        }
        advertise_storage(used, capacity);
        advertise_storage_backends(static_cast<uint32_t>(local_->online_backends()));
        server_.set_local(members_.self());
        mark_ready(ready_data_storage);
        // An edge node runs an empty pool, so local_store() callers need no
        // special case. Said explicitly, or capacity=0 reads like a missing disk.
        Log::info("node data storage ready used=" + std::to_string(used) +
                  " capacity=" + std::to_string(capacity) +
                  (cfg_.storage_backends.empty() ? " (hosts no extents)" : ""));
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
            identity_.keys.storage);
        mark_ready(ready_control_storage);

        if (startup_stage_hook_)
            startup_stage_hook_("cache");
        if (stop.stop_requested())
            return;
        cache_ = std::make_unique<PersistentBlockCache>(cfg_.cache, identity_.keys.storage);
        mark_ready(ready_cache);

        if (startup_stage_hook_)
            startup_stage_hook_("retention");
        if (stop.stop_requested())
            return;
        retention_ = std::make_unique<RetentionStore>(cfg_.state_path, identity_.keys.storage);
        mark_ready(ready_retention);

        if (startup_stage_hook_)
            startup_stage_hook_("metadata");
        if (stop.stop_requested())
            return;
        meta_ = std::make_unique<MetadataReplica>(cfg_.state_path, identity_.keys.storage,
                                                 cache_->metadata(), cfg_.bootstrap.empty(),
                                                 cfg_.metadata_materialization_cache_bytes);

        metadata_server_ = std::make_unique<MetadataServer>(
            *this, *meta_, *cache_, routes_, cfg_.metadata_min_write_replicas);

        cache_->remember_metadata(meta_->committed());
        const auto generation = meta_->committed().generation;
        advertise_metadata_generation(generation);
        server_.set_local(members_.self());
        mark_ready(ready_metadata);
        Log::info("node metadata ready generation=" + std::to_string(generation));
    } catch (const std::exception& error) {
        Log::error("node local state recovery failed: " + std::string(error.what()));
        mark_recovery_failed("local state: " + std::string(error.what()));
    }
}

void NodeRuntime::start() {
    // A cluster nobody could ever connect to is refused before anything
    // listens or is marked started.
    refuse_impossible_cluster();
    if (started_.exchange(true))
        return;

    // Legal but worth saying once.
    for (const auto& warning : configuration_warnings(cfg_))
        Log::warn("configuration: " + warning);
    {
        const auto resolution = inbound_resolution();
        Log::info(std::string("node inbound_capable=") +
                  (resolution.inbound_capable ? "true" : "false") + " (" +
                  std::string(tristate_name(resolution.inbound_capable_mode)) + ", " +
                  resolution.source + ") hosts_extents=" +
                  (resolution.hosts_extents ? "true" : "false") + " (" +
                  std::string(tristate_name(resolution.hosts_extents_mode)) + ")");
    }

    if (startup_stage_hook_)
        startup_stage_hook_("control-plane");

    // Control plane before any expensive backend recovery: peers can
    // authenticate this node at once and Status can tell reachability from
    // readiness.
    server_.start();
    mark_ready(ready_control_plane);
    Log::info("node " + to_string(identity_.id).substr(0, 12) + " listening on " +
              std::to_string(server_.bound_port()) + " domain=" + members_.self().failure_domain +
              " state=recovering");

    // A node with no bootstrap peers founds the cluster (the same test
    // MetadataReplica uses for genesis authority, see below): the only moment
    // an account may be created without one to authorise it.
    // A joining node with bootstrap peers and an empty user table cannot
    // authenticate anything (403 everywhere), so it says so loudly.
    if (!cfg_.bootstrap.empty() && users_.all().empty()) {
        Log::warn("accounts: this node holds no user accounts, so nothing can sign in and "
                  "every API route will refuse with 403");
        Log::warn("accounts: if this cluster has just been upgraded, stop one node and run: "
                  "macha-users " + cfg_.state_path.string() + " <cluster.key> init");
        Log::warn("accounts: if it has not, this node has simply not received the user table "
                  "yet and will converge shortly");
    }

    if (cfg_.bootstrap.empty()) {
        if (auto initial = create_initial_accounts(users_, identity_.keys, cfg_.state_path, identity_.id)) {
            // The password is in the 0600 file, never in a log line.
            Log::warn("accounts: created the '" + initial->root.username + "' and '" +
                      initial->anonymous.username + "' accounts for this new cluster");
            Log::warn("accounts: the generated " + initial->root.username + " password is in " +
                      initial->path.string() + " -- sign in, change it, delete that file");
            propagate_users();
        }
    }

    telemetry_worker_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("cluster-telemetry", stop, [this, stop] { telemetry_loop(stop); });
    });
    maintenance_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("cluster-maintenance", stop, [this, stop] { loop(stop); });
    });
    storage_recovery_ = std::jthread([this](std::stop_token stop) {
        run_supervised_once("cluster-storage-recovery", [this, stop] { recover_storage(stop); });
    });
    state_recovery_ = std::jthread([this](std::stop_token stop) {
        run_supervised_once("cluster-state-recovery", [this, stop] { recover_state(stop); });
    });
    connectivity_worker_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("cluster-connectivity", stop, [this, stop] { connectivity_loop(stop); });
    });
}

void NodeRuntime::refuse_impossible_cluster() const {
    if (cfg_.inbound_capable != Tristate::no)
        return;
    if (cfg_.bootstrap.empty())
        throw std::runtime_error(
            "network.inbound_capable is false and no bootstrap peers are configured: a founding "
            "node must accept inbound connections, or nothing could ever join this cluster");
    // Only a bootstrap peer met before can be known to be incapable.
    const auto known = members_.all();
    for (const auto& endpoint : cfg_.bootstrap) {
        const auto found =
            std::find_if(known.begin(), known.end(), [&](const NodeInfo& node) {
                return node.id != identity_.id && node.host == endpoint.host && node.port == endpoint.port;
            });
        if (found == known.end() || node_inbound_capable(*found))
            return;
    }
    throw std::runtime_error(
        "network.inbound_capable is false and every bootstrap peer is known to accept no inbound "
        "connections either: no node in this cluster could be reached by anyone");
}

bool NodeRuntime::resolve_hosts_extents_for(bool inbound_capable) const {
    return resolve_hosts_extents(cfg_, inbound_capable);
}

InboundResolution NodeRuntime::inbound_resolution() const {
    std::lock_guard lock(inbound_mutex_);
    return inbound_;
}

void NodeRuntime::persist_inbound_resolution_locked() const {
    const auto path = inbound_resolution_path(cfg_);
    std::filesystem::create_directories(path.parent_path());
    Writer writer;
    writer.fixed(inbound_resolution_magic);
    writer.u8(inbound_.inbound_capable ? 1 : 0);
    writer.u64(inbound_.decided_unix_ms);
    writer.string(inbound_.source);
    const auto& bytes = writer.data();
    durable_replace_file(path, std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                                bytes.size()));
}

void NodeRuntime::apply_inbound_resolution(bool inbound_capable, std::string source) {
    bool changed = false;
    bool hosts = false;
    InboundResolution before;
    {
        std::lock_guard lock(inbound_mutex_);
        before = inbound_;
        hosts = resolve_hosts_extents_for(inbound_capable);
        changed = inbound_.inbound_capable != inbound_capable || inbound_.hosts_extents != hosts ||
                  inbound_.source != source;
        inbound_.inbound_capable = inbound_capable;
        inbound_.hosts_extents = hosts;
        inbound_.source = std::move(source);
        if (changed)
            inbound_.decided_unix_ms = unix_ms();
        try {
            persist_inbound_resolution_locked();
        } catch (const std::exception& error) {
            Log::warn("inbound resolution not persisted: " + std::string(error.what()));
        }
    }
    if (!changed)
        return;
    // The flags travel with every handshake and members reply; placement
    // moves as for a join or leave.
    if (members_.set_flags(inbound_capable, hosts)) {
        server_.set_local(members_.self());
        events_.notify(NodeEvent::topology);
        signal_telemetry_refresh();
    }
    Log::info(std::string("node inbound resolution changed inbound_capable=") +
              (inbound_capable ? "true" : "false") + " hosts_extents=" +
              (hosts ? "true" : "false") + " previous_inbound_capable=" +
              (before.inbound_capable ? "true" : "false") + " source=" +
              inbound_resolution().source);
}

void NodeRuntime::connectivity_loop(std::stop_token stop) {
    if (stop.stop_requested())
        return;
    (void)refresh_public_connectivity(false);
    if (cfg_.connectivity_check.enabled && !stop.stop_requested())
        (void)public_connectivity_.probe(false);
    if (cfg_.inbound_capable != Tristate::automatic)
        return;

    // `auto` resolution. Evidence is a peer reachable over CONTROL reporting
    // whether a fresh connection to our advertised endpoint completed a
    // handshake. Sticky: capable -> incapable needs two consecutive failures,
    // incapable -> capable one success. An unaskable peer is no evidence.
    auto next_probe = Clock::now();
    uint64_t wake_seen = connectivity_wake_.load(std::memory_order_acquire);
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(connectivity_wait_mutex_);
            const auto now = Clock::now();
            if (next_probe > now)
                connectivity_wait_cv_.wait_for(lock, stop, next_probe - now, [&] {
                    return connectivity_wake_.load(std::memory_order_acquire) != wake_seen;
                });
            wake_seen = connectivity_wake_.load(std::memory_order_acquire);
        }
        if (stop.stop_requested())
            return;
        next_probe = Clock::now() + cfg_.heartbeat;

        std::optional<NodeInfo> peer;
        for (const auto& node : members_.active()) {
            if (node.id == identity_.id || !node_inbound_capable(node))
                continue;
            if (client_.has_route(node.id, TransportLane::control)) {
                peer = node;
                break;
            }
        }
        if (!peer)
            continue;

        const auto self = members_.self();
        Writer writer;
        writer.string(self.host);
        writer.u16(self.port);
        bool asked = false;
        bool reachable = false;
        std::string error;
        try {
            const auto reply = call(*peer, MessageType::dial_back_probe, writer.data());
            if (reply.message.type == MessageType::dial_back_probe_reply) {
                Reader reader(reply.message.payload);
                reachable = reader.u8() != 0;
                error = reader.string(4096);
                reader.finish();
                asked = true;
            } else if (reply.message.type == MessageType::error) {
                Reader reader(reply.message.payload);
                error = reader.remaining() ? reader.string(4096) : "dial-back probe refused";
            }
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!asked) {
            Log::debug("dial-back probe not answered peer=" + to_string(peer->id).substr(0, 12) +
                       " error=" + error);
            continue; // Try again next heartbeat, ideally with another peer.
        }

        bool currently_capable = false;
        unsigned failures = 0;
        {
            std::lock_guard lock(inbound_mutex_);
            inbound_.last_probe_unix_ms = unix_ms();
            inbound_.last_probe_peer = to_string(peer->id);
            inbound_.last_probe_error = reachable ? std::string{} : error;
            inbound_.consecutive_probe_failures =
                reachable ? 0 : inbound_.consecutive_probe_failures + 1;
            currently_capable = inbound_.inbound_capable;
            failures = inbound_.consecutive_probe_failures;
        }
        Log::debug("dial-back probe peer=" + to_string(peer->id).substr(0, 12) + " endpoint=" +
                   self.host + ":" + std::to_string(self.port) + " reachable=" +
                   (reachable ? "true" : "false") + (error.empty() ? "" : " error=" + error) +
                   " consecutive_failures=" + std::to_string(failures));

        if (reachable) {
            apply_inbound_resolution(true, "probe:" + to_string(peer->id));
            next_probe = Clock::now() + cfg_.inbound_reprobe_while_capable;
        } else if (currently_capable && failures < 2) {
            // One failure is not a verdict; confirm on the next round.
            next_probe = Clock::now() + cfg_.heartbeat;
        } else {
            apply_inbound_resolution(false, "probe:" + to_string(peer->id));
            next_probe = Clock::now() + cfg_.inbound_reprobe_while_incapable;
        }
    }
}

void NodeRuntime::request_stop() {
    if (storage_recovery_.joinable())
        storage_recovery_.request_stop();
    if (state_recovery_.joinable())
        state_recovery_.request_stop();
    if (connectivity_worker_.joinable()) {
        connectivity_worker_.request_stop();
        connectivity_wait_cv_.notify_all();
    }
    if (telemetry_worker_.joinable()) {
        telemetry_worker_.request_stop();
        telemetry_wait_cv_.notify_all();
    }
    if (maintenance_.joinable()) {
        Log::debug("shutdown: node maintenance request_stop");
        maintenance_.request_stop();
        maintenance_wait_cv_.notify_all();
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
                         &telemetry_worker_, &maintenance_}) {
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

std::chrono::milliseconds NodeRuntime::no_progress_deadline_for(MessageType type) const {
    if (type == MessageType::get_object || type == MessageType::put_object ||
        type == MessageType::put_object_deferred || type == MessageType::object_durability_barrier)
        return cfg_.data_no_progress_deadline;
    return cfg_.control_no_progress_deadline;
}

RpcReply NodeRuntime::call(const NodeInfo& node, MessageType type,
                           std::span<const uint8_t> payload) {
    if (outbound_calls_stopped_.load(std::memory_order_acquire))
        throw std::runtime_error("node is stopping; outbound RPC is unavailable");
    return client_.call(node, type, payload, stall_notice_for(type),
                        no_progress_deadline_for(type));
}

RpcReply NodeRuntime::call(const Endpoint& endpoint, MessageType type,
                           std::span<const uint8_t> payload) {
    if (outbound_calls_stopped_.load(std::memory_order_acquire))
        throw std::runtime_error("node is stopping; outbound RPC is unavailable");
    return client_.call(endpoint, type, payload, stall_notice_for(type),
                        no_progress_deadline_for(type));
}

RpcReply NodeRuntime::call(const NodeInfo& node, MessageType type, std::span<const uint8_t> payload,
                           FrameType frame_type) {
    if (outbound_calls_stopped_.load(std::memory_order_acquire))
        throw std::runtime_error("node is stopping; outbound RPC is unavailable");
    return client_.call(node, type, payload, frame_type, stall_notice_for(type),
                        no_progress_deadline_for(type));
}

RpcReply NodeRuntime::call(const Endpoint& endpoint, MessageType type,
                           std::span<const uint8_t> payload, FrameType frame_type) {
    if (outbound_calls_stopped_.load(std::memory_order_acquire))
        throw std::runtime_error("node is stopping; outbound RPC is unavailable");
    return client_.call(endpoint, type, payload, frame_type, stall_notice_for(type),
                        no_progress_deadline_for(type));
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

TrafficTotals NodeRuntime::traffic_totals() const {
    TrafficTotals totals;
    const auto& traffic = client_.traffic();
    for (size_t index = 0; index < totals.in_bytes.size() && index < TransportTraffic::classes;
         ++index) {
        totals.in_bytes[index] = traffic->in_bytes[index].load(std::memory_order_relaxed);
        totals.out_bytes[index] = traffic->out_bytes[index].load(std::memory_order_relaxed);
    }
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
    server_.set_local(members_.self());
    Writer writer;
    writer.u64(generation);
    client_.broadcast({MessageType::metadata_notice, writer.take()});
}

// Health, dialling, membership, telemetry and account gossip.
void NodeRuntime::bind_control_routes() {
    route(MessageType::ping,
          []([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
             [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              return {MessageType::ok, {}};
          });
    route(MessageType::dial_request,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              // A non-dialable peer wants a lane. The handshake behind `peer`
              // authenticates it; the health thread dials under its backoff.
              Reader reader(request.payload);
              const auto lane = static_cast<TransportLane>(reader.u8());
              reader.finish();
              if (lane != TransportLane::control && lane != TransportLane::data)
                  return error_reply("invalid transport lane");
              Log::debug("dial request received peer=" + to_string(peer.id).substr(0, 12) +
                         " lane=" + transport_lane_name(lane));
              client_.request_lane(peer, lane);
              return {MessageType::ok, {}};
          });
    route(MessageType::dial_back_probe,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              // "Can you connect to me at this address?" Answered with a fresh
              // connection whose handshake must authenticate as the asker, never
              // an existing route. Rate limited per peer, so it cannot be used
              // to make this node hammer an address.
              Reader reader(request.payload);
              Endpoint target;
              target.host = reader.string(4096);
              target.port = reader.u16();
              reader.finish();
              if (target.host.empty() || !target.port)
                  return error_reply("dial-back probe needs a host and port");
              {
                  std::lock_guard lock(dial_back_mutex_);
                  const auto now = Clock::now();
                  auto& last = dial_back_last_[peer.id];
                  if (last != Clock::time_point{} && now - last < cfg_.dial_back_probe_min_interval)
                      return error_reply("dial-back probe rate limited");
                  last = now;
              }
              const auto error = client_.probe_dial(target, peer.id);
              Log::debug("dial-back probe for peer=" + to_string(peer.id).substr(0, 12) +
                         " endpoint=" + target.host + ":" + std::to_string(target.port) +
                         " reachable=" + (error.empty() ? "true" : "false") +
                         (error.empty() ? "" : " error=" + error));
              Writer writer;
              writer.u8(error.empty() ? 1 : 0);
              writer.string(error);
              return {MessageType::dial_back_probe_reply, writer.take()};
          });
    route(MessageType::members,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              auto nodes = members_.all();
              Writer writer;
              writer.u32(nodes.size());
              for (const auto& node : nodes)
                  encode_node_info(writer, node);
              return {MessageType::members_reply, writer.take()};
          });
    route(MessageType::telemetry,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              if (!request.payload.empty()) {
                  try {
                      for (auto& value : decode_telemetry_set(request.payload))
                          telemetry_.observe(std::move(value));
                  } catch (const DecodeError&) {
                      // Also accept telemetry sent as a request.
                      telemetry_.observe(decode_node_telemetry(request.payload), true);
                  }
              }
              const auto gossip_ttl =
                  std::max(cfg_.dead_after * 2, std::chrono::milliseconds(60000));
              return {MessageType::telemetry_reply,
                      encode_telemetry_set(telemetry_.recent(gossip_ttl, 64))};
          });
    route(MessageType::identity_resets,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              if (!request.payload.empty())
                  for (const auto& reset : decode_identity_resets(request.payload))
                      apply_identity_reset(reset);
              return {MessageType::identity_resets_reply,
                      encode_identity_resets(identity_resets())};
          });
    route(MessageType::user_sync,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              if (!request.payload.empty())
                  (void)users_.apply_all(decode_users(request.payload));
              return {MessageType::user_sync_reply, encode_users(users_.all())};
          });
    route(MessageType::session_sync,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              if (!request.payload.empty())
                  for (const auto& session : decode_sessions(request.payload))
                      apply_session(session);
              const auto gossip_ttl =
                  std::max(cfg_.dead_after * 2, std::chrono::milliseconds(60000));
              return {MessageType::session_sync_reply,
                      encode_sessions(sessions_.recent(gossip_ttl, 64))};
          });
}

// DATA and CONTROL objects: presence, reads, writes, durability and retention.
void NodeRuntime::bind_storage_routes() {
    route(MessageType::have_object,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              Reader reader(request.payload);
              ObjectId id{reader.fixed<32>()};
              reader.finish();
              Writer writer;
              auto resource = data_resources_.try_acquire(
                  DataWorkContext(frame_type, cfg_.extent_size), cfg_.extent_size);
              if (!resource)
                  return error_reply("DATA resource admission busy or stopping");
              // Repair/rebalance trust this "present" without re-verifying, so
              // authenticate, decrypt and hash: a corrupt replica never counts as
              // healthy placement. (have_objects and retain_objects check presence.)
              writer.u8(local_store().valid(id));
              return {MessageType::bool_reply, writer.take()};
          });
    route(MessageType::have_objects,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
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
              // Used only by retain_data()'s candidate scan
              // (select_present_batched), never by repair. "Present" only makes
              // a node a candidate; retain_objects then claims on index
              // presence. One admission for the whole batch: no per-object I/O.
              auto resource = data_resources_.try_acquire(
                  DataWorkContext(frame_type, cfg_.extent_size), cfg_.extent_size);
              if (!resource)
                  return error_reply("DATA resource admission busy or stopping");
              Writer writer;
              writer.u32(count);
              for (const auto& id : ids)
                  writer.u8(local_store().has(id));
              return {MessageType::have_objects_reply, writer.take()};
          });
    route(MessageType::have_valid_objects,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              Reader reader(request.payload);
              const auto count = reader.u32();
              if (!count || count > have_valid_objects_max)
                  return error_reply("invalid validated presence batch count");
              std::vector<ObjectId> ids;
              ids.reserve(count);
              for (uint32_t i = 0; i < count; ++i) {
                  ObjectId id;
                  id.bytes = reader.fixed<32>();
                  ids.push_back(id);
              }
              reader.finish();
              // Repair trusts "present" as a healthy copy, so each id is read,
              // decrypted and hashed like have_object; one DATA admission per id.
              Writer writer;
              writer.u32(count);
              for (const auto& id : ids) {
                  auto resource = data_resources_.try_acquire(
                      DataWorkContext(frame_type, cfg_.extent_size), cfg_.extent_size);
                  if (!resource)
                      return error_reply("DATA resource admission busy or stopping");
                  writer.u8(local_store().valid(id));
              }
              return {MessageType::have_valid_objects_reply, writer.take()};
          });
    route(MessageType::have_control_objects,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              // The CONTROL counterpart: answers from control_store() and takes
              // no DATA admission. Law 1: control never queues behind or runs
              // inline with bulk data work.
              Reader reader(request.payload);
              const auto count = reader.u32();
              if (!count || count > 200000)
                  return error_reply("invalid control presence batch count");
              std::vector<ObjectId> ids;
              ids.reserve(count);
              for (uint32_t i = 0; i < count; ++i) {
                  ObjectId id;
                  id.bytes = reader.fixed<32>();
                  ids.push_back(id);
              }
              reader.finish();
              Writer writer;
              writer.u32(count);
              for (const auto& id : ids)
                  writer.u8(control_store().has(id));
              return {MessageType::have_control_objects_reply, writer.take()};
          });
    route(MessageType::get_object,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              Reader reader(request.payload);
              ObjectId id{reader.fixed<32>()};
              reader.finish();
              auto resource = data_resources_.acquire(DataWorkContext(frame_type, cfg_.extent_size),
                                                      cfg_.extent_size);
              if (!resource)
                  return error_reply("DATA resource admission stopping");
              auto data = local_store().get(id);
              if (!data)
                  return error_reply("object not found");
              activity_.note(frame_type, data->size());
              Writer writer;
              writer.fixed(id.bytes);
              writer.bytes(*data);
              return {MessageType::object_reply, writer.take()};
          });
    route(MessageType::get_control_object,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
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
          });
    {
        const MessageRoutes::Handler handler =
            [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                   [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
            Reader reader(request.payload);
            ObjectId id{reader.fixed<32>()};
            auto data = reader.bytes(128 * 1024 * 1024);
            reader.finish();
            if (data.size() > cfg_.extent_size)
                return error_reply("DATA object exceeds configured extent size");
            auto resource =
                data_resources_.acquire(DataWorkContext(frame_type, data.size()), data.size());
            if (!resource)
                return error_reply("DATA resource admission stopping");
            activity_.note(frame_type, data.size());
            if (!local_store().has(id))
                events_.notify(NodeEvent::storage);
            if (request.type == MessageType::put_object_deferred) {
                const auto generation = local_store().put_deferred(id, data);
                if (!generation)
                    return error_reply("storage limit reached");
                advertise_storage(local_store().used(), local_store().limit());
                // Bind provisional placement to this process lifetime and
                // node-wide mutation generation. A later barrier for a covered
                // generation is a no-op even with newer writes dirty here.
                Writer reply;
                reply.fixed(identity_.durability_epoch.bytes);
                reply.u64(generation->domain);
                reply.u64(generation->generation);
                reply.u64(generation->backend_instance);
                return {MessageType::ok, reply.take()};
            }
            if (!local_store().put(id, data))
                return error_reply("storage limit reached");
            advertise_storage(local_store().used(), local_store().limit());
            return {MessageType::ok, {}};
        };
        route(MessageType::put_object, handler);
        route(MessageType::put_object_deferred, handler);
    }
    route(MessageType::put_control_object,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              Reader reader(request.payload);
              ObjectId id{reader.fixed<32>()};
              auto data = reader.bytes(128 * 1024 * 1024);
              reader.finish();
              if (!control_store().put(id, data))
                  return error_reply("control storage limit reached");
              return {MessageType::ok, {}};
          });
    route(MessageType::object_durability_barrier,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              Reader reader(request.payload);
              NodeId expected_epoch{reader.fixed<16>()};
              const auto domain = reader.u64();
              const auto required_generation = reader.u64();
              const auto backend_instance = reader.u64();
              // An optional trailing id list turns a refusal into a probe.
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
              if (expected_epoch != identity_.durability_epoch) {
                  // The requester's placement token is from a previous process
                  // incarnation and cannot become true again, but the objects may
                  // be on disk. With ids, answer from disk with fresh tokens
                  // (discipline 1: re-derive, don't assert); without, refuse.
                  if (probe_ids.empty()) {
                      Log::debug("object durability barrier refused: epoch changed expected=" +
                                 to_string(expected_epoch).substr(0, 8) +
                                 " current=" + to_string(identity_.durability_epoch).substr(0, 8) +
                                 " domain=" + std::to_string(domain) +
                                 " generation=" + std::to_string(required_generation));
                      return error_reply("storage durability epoch changed");
                  }
                  Writer reply;
                  reply.fixed(identity_.durability_epoch.bytes);
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
                            std::to_string(probe_ids.size()) +
                            " expected=" + to_string(expected_epoch).substr(0, 8) +
                            " current=" + to_string(identity_.durability_epoch).substr(0, 8));
                  advertise_storage(local_store().used(), local_store().limit());
                  return {MessageType::ok, reply.take()};
              }
              try {
                  local_store().durability_barrier({domain, required_generation, backend_instance},
                                                   DurabilityUrgency::batchable);
                  advertise_storage(local_store().used(), local_store().limit());
                  return {MessageType::ok, {}};
              } catch (const std::exception& error) {
                  return error_reply(std::string("storage durability barrier failed: ") +
                                     error.what());
              }
          });
    route(MessageType::retain_objects,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
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
              // A retention claim says "this node holds the object": index
              // presence, not a re-read, since a quantum commit re-claims every
              // extent of its file inside the writer's metadata mutation. The
              // bytes were verified when put and on every read; scrub finds later
              // corruption. No DATA admission: there is no read buffer.
              for (const auto& id : ids) {
                  const bool present = object_class == RetentionClass::data
                                           ? local_store().has(id)
                                           : control_store().has(id);
                  if (!present)
                      return error_reply("retention object is not durably present");
              }
              claims().retain_batch(object_class, ids, dot);
              return {MessageType::ok, {}};
          });
    route(MessageType::delete_object,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              Reader reader(request.payload);
              ObjectId id{reader.fixed<32>()};
              reader.finish();
              if (claims().retained(RetentionClass::data, id))
                  return error_reply("object has an active retention claim");
              auto resource = data_resources_.try_acquire(
                  DataWorkContext(frame_type, cfg_.extent_size), cfg_.extent_size);
              if (!resource)
                  return error_reply("DATA resource admission busy or stopping");
              (void)local_store().remove(id);
              if (ready(ready_cache))
                  (void)block_cache().remove(id);
              advertise_storage(local_store().used(), local_store().limit());
              return {MessageType::ok, {}};
          });
}


void NodeRuntime::route(MessageType type, MessageRoutes::Handler handler) {
    routes_.bind(type, std::move(handler));
    bound_routes_.push_back(type);
}

void NodeRuntime::unbind_routes() {
    for (const auto type : bound_routes_)
        routes_.unbind(type);
    bound_routes_.clear();
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
            previous->metadata_write_replicas_required != node.metadata_write_replicas_required ||
            previous->flags != node.flags;
        if (node.id != identity_.id)
            client_.note_peer(node);
        members_.observe(std::move(node));
    }
    reader.finish();
    remote_metadata_generation_.store(newest_metadata);

    // Membership exchange is a heartbeat: identical gossip must not wake
    // maintenance (or keep restarting its GC quiet window). Wake only for
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
        events_.notify(topology_changed ? NodeEvent::topology : NodeEvent::metadata);
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
    // Telemetry runs during recovery: unready planes report zero capacity and
    // usage rather than the node vanishing from the cluster.
    auto info = members_.self();
    info.used = advertised_storage_used_.load(std::memory_order_relaxed);
    info.capacity = advertised_storage_capacity_.load(std::memory_order_relaxed);
    info.metadata_generation = advertised_metadata_generation_.load(std::memory_order_relaxed);
    const auto cache_capacity = advertised_cache_capacity_.load(std::memory_order_relaxed);
    const auto cache_used = advertised_cache_used_.load(std::memory_order_relaxed);
    const auto storage_backends_online =
        advertised_backends_online_.load(std::memory_order_relaxed);
    CacheActivity cache_activity;
    cache_activity.hits = advertised_cache_hits_.load(std::memory_order_relaxed);
    cache_activity.misses = advertised_cache_misses_.load(std::memory_order_relaxed);
    cache_activity.evictions = advertised_cache_evictions_.load(std::memory_order_relaxed);
    const auto peers_known = telemetry_peers_known_.load(std::memory_order_relaxed);
    const auto peers_active = telemetry_peers_active_.load(std::memory_order_relaxed);

    // Same vocabulary as root.startup.phase (ClusterStatusService), so a peer
    // can tell a real measurement (ready) from recovery-zeroed capacity. A
    // failed node reports "recovering": there is no wire state for it, and
    // its own Status root has the detail.
    const auto local_readiness = readiness();
    const auto phase = local_readiness.failed         ? NodePhase::recovering
                        : local_readiness.local_state_ready ? NodePhase::ready
                        : local_readiness.control_plane_online ? NodePhase::recovering
                                                                : NodePhase::starting;

    // Client API endpoint (Status nodes[].api_endpoint); empty when no
    // catalogue API runs. A configured endpoint is used as given: behind a TLS
    // proxy the outer scheme and port differ from the bind. The default uses
    // the resolved RPC advertise host, not catalogue.api.listen, which is
    // usually an undialable wildcard. IPv6 literals are bracketed for the URL.
    std::string api_endpoint;
    if (cfg_.catalogue.api.enabled) {
        if (!cfg_.catalogue.api.advertised_endpoint.empty()) {
            api_endpoint = cfg_.catalogue.api.advertised_endpoint;
        } else {
            auto host = info.host;
            if (host.find(':') != std::string::npos && host.front() != '[')
                host = "[" + host + "]";
            api_endpoint = "http://" + host + ":" + std::to_string(cfg_.catalogue.api.port);
        }
    }

    // The playback budgets this node enforces. A node serving no playback
    // reports zeros ("cannot say") rather than figures it would not honour.
    PlaybackBudgets playback;
    if (cfg_.streaming.enabled) {
        playback.startup_timeout_ms =
            static_cast<uint32_t>(std::max<int64_t>(0, cfg_.streaming.startup_timeout.count()));
        playback.segment_timeout_ms =
            static_cast<uint32_t>(std::max<int64_t>(0, cfg_.streaming.segment_timeout.count()));
        playback.pipeline_idle_ms =
            static_cast<uint32_t>(std::max<int64_t>(0, cfg_.streaming.pipeline_idle.count()));
        playback.session_idle_ms =
            static_cast<uint32_t>(std::max<int64_t>(0, cfg_.streaming.session_idle.count()));
        playback.max_sessions_per_account =
            static_cast<uint32_t>(cfg_.streaming.max_sessions_per_account);
        playback.max_transcodes_per_account =
            static_cast<uint32_t>(cfg_.streaming.max_transcodes_per_account);
        playback.max_sessions = static_cast<uint32_t>(cfg_.streaming.max_sessions);
        playback.transcode_entitlement_idle_ms = static_cast<uint32_t>(
            std::max<int64_t>(0, cfg_.streaming.transcode_entitlement_idle.count()));
        playback.startup_no_progress_ms = static_cast<uint32_t>(
            std::max<int64_t>(0, cfg_.streaming.startup_no_progress.count()));
        playback.start_wait_max_ms =
            static_cast<uint32_t>(std::max<int64_t>(0, cfg_.streaming.start_wait_max.count()));
        playback.start_failed_retention_ms = static_cast<uint32_t>(
            std::max<int64_t>(0, cfg_.streaming.start_failed_retention.count()));
        playback.transcode_rates = transcode_rates_.summary();
    }
    telemetry_.set_node_name(cfg_.node_name);
    telemetry_.refresh_local(info, std::string(kServerVersion), cache_capacity, cache_used,
                             storage_backends_online, peers_known, peers_active, 0, 0,
                             peers_active > 0 ? peers_active - 1 : 0, phase,
                             std::move(api_endpoint), playback, cache_activity,
                             traffic_totals());
}

void NodeRuntime::signal_telemetry_refresh() {
    telemetry_demand_.fetch_add(1, std::memory_order_release);
    telemetry_wait_cv_.notify_all();
}

void NodeRuntime::telemetry_loop(std::stop_token stop) {
    ThreadCpuReporter cpu_reporter("macha-telemetry", std::chrono::seconds(5), true);
    // `network.telemetry_interval_ms`, floored so a mis-set value cannot spin.
    const auto interval = std::max(cfg_.telemetry_interval, std::chrono::milliseconds(250));
    // Wakes follow telemetry demand, which peer churn can raise arbitrarily.
    // Local sampling runs on every wake so phase changes publish promptly, but
    // broadcast is limited to once a second and never faster than the cadence.
    const auto min_gossip_interval = std::min(interval, std::chrono::milliseconds(1000));
    auto last_gossip = Clock::time_point{};
    const auto gossip_ttl = std::max(cfg_.dead_after * 2, std::chrono::milliseconds(60000));
    uint64_t handled_demand = 0;
    while (!stop.stop_requested()) {
        const auto demand = telemetry_demand_.load(std::memory_order_acquire);
        try {
            refresh_telemetry();
            // Gossip is not suppressed by foreground work or a busy writer: a
            // node must stay visible while busy or in trouble. SPECULATIVE
            // keeps it out of the control memory reserve and off the control
            // workers, so it cannot delay operational RPC. A set is ~200
            // bytes per entry, at most 64 entries, so every tick is cheap.
            if (const auto now = Clock::now(); now - last_gossip >= min_gossip_interval) {
                last_gossip = now;
                auto values = telemetry_.recent(gossip_ttl, 64);
                if (!values.empty()) {
                    // A no-dial notification on established routes; never blocks.
                    (void)client_.broadcast_best_effort(
                        {MessageType::telemetry, encode_telemetry_set(values)},
                        FrameType::speculative);
                }
            }
        } catch (const std::exception& error) {
            Log::debug("telemetry refresh skipped: " + std::string(error.what()));
        }
        // Sessions are pushed on mutation (propagate_session); this is the
        // backstop for a peer unreachable at the time, on the existing tick.
        try {
            sessions_.prune_expired(unix_ms());
            auto values = sessions_.recent(gossip_ttl, 64);
            if (!values.empty()) {
                // As for the user table: each notification costs every peer a
                // bounded queue admission, so an unchanged set is not resent.
                auto payload = encode_sessions(values);
                const auto digest = sha256(payload);
                std::set<NodeId> peers;
                for (const auto& peer : members_.active())
                    if (peer.id != identity_.id)
                        peers.insert(peer.id);
                const auto now = Clock::now();
                const bool sessions_due =
                    digest != gossiped_sessions_ || peers != gossiped_session_peers_ ||
                    now - gossiped_sessions_at_ >= gossip_reannounce_interval;
                if (sessions_due && now >= gossip_sessions_retry_after_) {
                    const auto reached = client_.broadcast_best_effort(
                        {MessageType::session_sync, std::move(payload)}, FrameType::control);
                    // Record it only once it reached everyone: a best-effort
                    // notify queues nothing for a busy or not-yet-usable peer
                    // (as when a peer rejoins), and recording anyway would
                    // retire the retry before it ran.
                    if (reached >= peers.size()) {
                        gossiped_sessions_ = digest;
                        gossiped_session_peers_ = std::move(peers);
                        gossiped_sessions_at_ = now;
                    } else {
                        // Retry on a floor, not every tick: an unreached peer
                        // is usually busy, and gossip must not add load to it.
                        gossip_sessions_retry_after_ = Clock::now() + gossip_retry_floor;
                    }
                }
            }
        } catch (const std::exception& error) {
            Log::debug("session gossip skipped: " + std::string(error.what()));
        }
        // The whole user table, tombstones included, so a node that missed a
        // deletion learns the tombstone rather than resurrecting the account.
        gossip_users_if_changed();
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
    // Idempotent: align every consumer even if one learned the tombstone first.
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
        if (peer.id == identity_.id)
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
    // Notify, never call: a synchronous call would stall every login on an
    // unreachable-but-not-dead peer, exactly when logging in matters. The
    // local merge is done and the gossip tick is the backstop.
    try {
        (void)client_.broadcast_best_effort({MessageType::session_sync, encode_sessions({session})},
                                            FrameType::control);
    } catch (const std::exception& error) {
        Log::debug("session propagation skipped: " + std::string(error.what()));
    }
}

bool NodeRuntime::apply_user(const UserRecord& user) {
    return users_.apply(user);
}

void NodeRuntime::gossip_users_if_changed() {
    // Every notification costs each peer an admission slot in its bounded
    // request queue, most on a busy node, so an unchanged table sends
    // nothing. A broadcast goes out when the table changed here or a peer
    // appeared that may have missed it (how a node that was down converges:
    // the whole table, tombstones included, goes out once).
    try {
        const auto table = users_.table_hash();
        std::set<NodeId> peers;
        for (const auto& peer : members_.active())
            if (peer.id != identity_.id)
                peers.insert(peer.id);
        // Unchanged, nobody new, re-announce not due: send nothing.
        const auto now = Clock::now();
        if (table == gossiped_user_table_ && peers == gossiped_user_peers_ &&
            now - gossiped_users_at_ < gossip_reannounce_interval)
            return;
        if (now < gossip_users_retry_after_)
            return;

        auto values = users_.all();
        if (values.empty())
            return;
        const auto reached = client_.broadcast_best_effort(
            {MessageType::user_sync, encode_users(values)}, FrameType::control);
        // Commit only when it reached everyone, so a not-yet-usable peer is
        // retried next tick rather than marked told.
        if (reached >= peers.size()) {
            gossiped_user_table_ = table;
            gossiped_user_peers_ = std::move(peers);
            gossiped_users_at_ = now;
        } else {
            gossip_users_retry_after_ = Clock::now() + gossip_retry_floor;
        }
    } catch (const std::exception& error) {
        Log::debug("user gossip skipped: " + std::string(error.what()));
    }
}

void NodeRuntime::propagate_users() {
    // Always the full table: a peer offline longer than the gossip TTL must
    // still converge, and at tens of records it is smaller than the telemetry set.
    try {
        auto values = users_.all();
        if (values.empty())
            return;
        (void)client_.broadcast_best_effort({MessageType::user_sync, encode_users(values)},
                                            FrameType::control);
    } catch (const std::exception& error) {
        Log::debug("user propagation skipped: " + std::string(error.what()));
    }
}

void NodeRuntime::exchange(const Endpoint& endpoint) {
    auto reply = call(endpoint, MessageType::members);
    if (reply.message.type != MessageType::members_reply)
        throw std::runtime_error("membership rejected");
    merge(reply.message.payload);
}

void NodeRuntime::exchange(const NodeInfo& node) {
    // Preserve the authenticated NodeId when selecting the route, so an
    // inbound canonical route is reused rather than a fresh dial made.
    auto reply = call(node, MessageType::members);
    if (reply.message.type != MessageType::members_reply)
        throw std::runtime_error("membership rejected");
    merge(reply.message.payload);
}

void NodeRuntime::loop(std::stop_token stop) {
    ThreadCpuReporter cpu_reporter("macha-node", std::chrono::seconds(5), true);
    while (!stop.stop_requested()) {
        // Readiness is orthogonal to membership: refresh available local
        // planes, then exchange regardless.
        if (ready(ready_data_storage) && local_) {
            const auto refresh_started = Clock::now();
            local_->refresh();
            const auto refresh_ms = elapsed_ms(refresh_started);
            if (refresh_ms >= 100 && Log::enabled(LogLevel::all))
                Log::trace("DIAG node-stage stage=storage-refresh elapsed_ms=" +
                           std::to_string(refresh_ms));
            advertise_storage(local_->used(), local_->limit());
            advertise_storage_backends(static_cast<uint32_t>(local_->online_backends()));
        }
        if (ready(ready_cache) && cache_) {
            const auto stats = cache_->stats();
            advertise_cache(static_cast<uint64_t>(cfg_.cache.max_blocks) * cfg_.extent_size,
                            static_cast<uint64_t>(cache_->blocks()) * cfg_.extent_size,
                            CacheActivity{stats.hits, stats.misses, stats.evictions});
        }
        if (ready(ready_metadata) && meta_)
            advertise_metadata_generation(meta_->generation());

        std::set<std::pair<std::string, uint16_t>> exchanged;
        const auto known_nodes = members_.all();
        telemetry_peers_known_.store(static_cast<uint32_t>(known_nodes.size()),
                                     std::memory_order_relaxed);
        uint32_t active_peers = 1;
        // A peer that accepts no inbound connections is exchanged with only
        // over its session to us; without one there is nothing to dial or log.
        const auto unreachable_by_design = [&](const NodeInfo& node) {
            return !node_inbound_capable(node) &&
                   !client_.has_route(node.id, TransportLane::control);
        };
        for (const auto& endpoint : cfg_.bootstrap) {
            exchanged.emplace(endpoint.host, endpoint.port);
            try {
                auto known =
                    std::find_if(known_nodes.begin(), known_nodes.end(), [&](const NodeInfo& node) {
                        return node.id != identity_.id && node.host == endpoint.host &&
                               node.port == endpoint.port;
                    });
                if (known != known_nodes.end() && unreachable_by_design(*known))
                    continue;
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
            if (node.id == identity_.id)
                continue;
            if (!exchanged.emplace(node.host, node.port).second)
                continue;
            if (unreachable_by_design(node))
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

void NodeRuntime::reconfigure_local(const Config& config) {
    auto updated = normalize_config(config);
    if (!all_local_state_ready())
        throw std::runtime_error("node local state is still recovering");
    local_->reconfigure(updated.storage_backends);
    local_->refresh();
    cache_->reconfigure(updated.cache);
    // Node-local policy, unused by the long-lived network/metadata threads:
    // keep the snapshot in sync with a live reload without changing cluster policy.
    cfg_.storage_backends = updated.storage_backends;
    cfg_.cache = updated.cache;
    cfg_.hydration = updated.hydration;
    cfg_.read_ahead_extents = updated.read_ahead_extents;
    advertise_storage(local_store().used(), local_store().limit());
}
} // namespace macha
