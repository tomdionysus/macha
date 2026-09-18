// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster.hpp"
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
// How long to wait before re-attempting a gossip broadcast that did not reach
// every peer. Long enough that repair traffic cannot become a load source of
// its own, short enough that a rejoining node converges promptly.
constexpr auto gossip_retry_floor = std::chrono::seconds(1);
// Re-announce this often even when nothing has changed and every peer is
// believed told. broadcast_best_effort() reports how many frames it QUEUED,
// not how many were delivered and applied, so "reached >= peers" can mark a
// peer told that never received anything -- a peer whose inbound route is not
// usable yet, which is precisely the state a peer is in while it restarts.
// Without this, such a peer waits for the next change to the table or to the
// membership set, which may never come: observed live on 2026-09-12, where an
// upgraded node sat with an empty user table refusing every request until the
// sender happened to restart. Announcing on change is an optimisation; this is
// the guarantee underneath it.
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

RpcMessage error_reply(const std::string& text) {
    Writer writer;
    writer.string(text);
    return {MessageType::error, writer.take()};
}

// storage.hosts_extents resolved against what the node knows about itself:
// `auto` is "yes if I have somewhere to put them and peers can fetch them".
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
// final; `auto` starts from the persisted resolution when there is one (so a
// restart does not look like a join/leave to placement) and otherwise
// behaves as capable -- dial and accept -- until a dial-back says otherwise.
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
            // Persisted evidence is a convenience; the probe will decide again.
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
      data_resources_(cfg_.data_inflight_bytes, cfg_.data_viewer_reserve_bytes,
                      cfg_.maintenance.background_concurrency
                          ? cfg_.maintenance.background_concurrency
                          : std::max<size_t>(1, std::thread::hardware_concurrency() / 2),
                      cfg_.data_credit_no_progress_deadline),
      retained_memory_(cfg_.runtime.retained_memory_bytes,
                       cfg_.runtime.control_memory_reserve_bytes,
                       cfg_.runtime.viewer_memory_reserve_bytes,
                       cfg_.runtime.loader_memory_reserve_bytes,
                       cfg_.runtime.reassembly_memory_reserve_bytes),
      inbound_(initial_inbound_resolution(cfg_)),
      members_(self_info(cfg_, id_, 0, 0, 0,
                         node_flags_for(inbound_.inbound_capable, inbound_.hosts_extents)),
               cfg_.dead_after, cfg_.state_path / "membership" / "known-nodes.bin"),
      public_connectivity_(cfg_, id_, Endpoint{members_.self().host, members_.self().port}),
      telemetry_(id_, cfg_.state_path / "telemetry" / "last-known.bin"),
      sessions_(cfg_.session.anonymous_ttl, cfg_.session.max_sessions,
               cfg_.state_path / "sessions" / "sessions.bin"),
      users_(cfg_.session.max_users, cfg_.state_path / "users" / "users.bin",
             hkdf_sha256(keys_.master, {},
                         std::span<const uint8_t>(
                             reinterpret_cast<const uint8_t*>("macha/users/v1"), 14))),
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
    // While this node accepts no inbound connections it keeps both lanes
    // dialled to every capable peer itself (see RpcClient::open_requested_lanes);
    // membership is what says who those peers are.
    client_.set_maintained_peers([this] {
        std::vector<NodeInfo> out;
        for (auto& node : members_.active())
            if (node.id != id_ && node_inbound_capable(node))
                out.push_back(std::move(node));
        return out;
    });
    // The roster may already name peers that cannot be dialled; the transport
    // must know before the first exchange, not after the first refused dial.
    for (const auto& node : members_.all())
        if (node.id != id_)
            client_.note_peer(node);
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
    note_startup_progress();
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
        // An edge node runs an empty pool rather than no pool: every caller
        // of local_store() sees "not present" / "no space" and needs no
        // special case. Say so, or capacity=0 reads like a missing disk.
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
    // The one shape that is not legal at all (a cluster nobody could ever
    // connect to) is refused here, before anything listens or is marked
    // started, rather than left to half-work.
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

    // Bring the control plane online before any potentially expensive local
    // backend recovery. Peers can authenticate this node immediately and Status
    // can distinguish reachability from readiness.
    server_.start();
    mark_ready(ready_control_plane);
    Log::info("node " + to_string(id_).substr(0, 12) + " listening on " +
              std::to_string(server_.bound_port()) + " domain=" + members_.self().failure_domain +
              " state=recovering");

    // A node with no configured bootstrap peers is founding the cluster rather
    // than joining one -- the same test MetadataReplica uses to decide whether
    // its genesis record is authority (see the accept_pristine_genesis_authority
    // argument below). That is the one moment an account can be created without
    // an account already existing to authorise it, so it is the only moment
    // this is allowed to happen.
    // An existing cluster upgrading into the accounts system reaches here with
    // an empty table and bootstrap peers configured, so the branch below does
    // not fire and nothing can authenticate: no anonymous account means the
    // session mint refuses, and every other route needs a session. That is a
    // total outage whose symptom (403 everywhere) says nothing about its
    // cause, so it must announce itself rather than be discovered.
    if (!cfg_.bootstrap.empty() && users_.all().empty()) {
        Log::warn("accounts: this node holds no user accounts, so nothing can sign in and "
                  "every API route will refuse with 403");
        Log::warn("accounts: if this cluster has just been upgraded, stop one node and run: "
                  "macha-users " + cfg_.state_path.string() + " <cluster.key> init");
        Log::warn("accounts: if it has not, this node has simply not received the user table "
                  "yet and will converge shortly");
    }

    if (cfg_.bootstrap.empty()) {
        if (auto initial = create_initial_accounts(users_, keys_, cfg_.state_path, id_)) {
            // The password is in the file, not in this line: a log is shipped,
            // rotated and read by more people than a 0600 file in the state
            // directory is.
            Log::warn("accounts: created the '" + initial->root.username + "' and '" +
                      initial->anonymous.username + "' accounts for this new cluster");
            Log::warn("accounts: the generated " + initial->root.username + " password is in " +
                      initial->path.string() + " -- sign in, change it, delete that file");
            propagate_users();
        }
    }

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
        run_supervised("cluster-connectivity", [this, stop] { connectivity_loop(stop); });
    });
}

void NodeRuntime::refuse_impossible_cluster() const {
    if (cfg_.inbound_capable != Tristate::no)
        return;
    if (cfg_.bootstrap.empty())
        throw std::runtime_error(
            "network.inbound_capable is false and no bootstrap peers are configured: a founding "
            "node must accept inbound connections, or nothing could ever join this cluster");
    // Only a bootstrap peer this node has met before can be known to be
    // incapable; an unknown one is given the benefit of the doubt.
    const auto known = members_.all();
    for (const auto& endpoint : cfg_.bootstrap) {
        const auto found =
            std::find_if(known.begin(), known.end(), [&](const NodeInfo& node) {
                return node.id != id_ && node.host == endpoint.host && node.port == endpoint.port;
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
    // The flags travel with every handshake and members reply from here on;
    // placement moves exactly as it would for a join or a leave.
    if (members_.set_flags(inbound_capable, hosts)) {
        server_.set_local(members_.self());
        signal_service_event(ServiceEvent::topology);
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

    // `auto` resolution. Evidence is a peer that could be asked (a CONTROL
    // session exists) reporting whether a fresh TCP connection to our
    // advertised endpoint completed a handshake. The resolution is sticky:
    // capable -> incapable needs two consecutive failures, incapable ->
    // capable needs one success (someone demonstrably connected). A peer
    // that could not be asked at all is no evidence either way.
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
            if (node.id == id_ || !node_inbound_capable(node))
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
            // One failure is not a verdict; confirm it on the next round.
            next_probe = Clock::now() + cfg_.heartbeat;
        } else {
            apply_inbound_resolution(false, "probe:" + to_string(peer->id));
            next_probe = Clock::now() + cfg_.inbound_reprobe_while_incapable;
        }
    }
}

void NodeRuntime::request_stop() {
    data_resources_.stop();
    retained_memory_.stop();
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

RpcMessage NodeRuntime::handle(const NodeInfo& peer, FrameType frame_type,
                               const RpcMessage& request) {
    try {
        // Health/control must never depend on storage I/O. Capacity is refreshed
        // by the node maintenance loop and after successful mutations below.
        switch (request.type) {
        case MessageType::ping:
            return {MessageType::ok, {}};
        case MessageType::dial_request: {
            // A peer that cannot dial us wants a lane it does not have. The
            // handshake behind `peer` is what authenticates the request; the
            // health thread does the dialling, under its ordinary backoff.
            Reader reader(request.payload);
            const auto lane = static_cast<TransportLane>(reader.u8());
            reader.finish();
            if (lane != TransportLane::control && lane != TransportLane::data)
                return error_reply("invalid transport lane");
            Log::debug("dial request received peer=" + to_string(peer.id).substr(0, 12) +
                       " lane=" + transport_lane_name(lane));
            client_.request_lane(peer, lane);
            return {MessageType::ok, {}};
        }
        case MessageType::dial_back_probe: {
            // "Can you connect to me at this address?" Answered with one fresh
            // TCP connection and a handshake that must authenticate as the
            // asker, never with an existing route. Rate limited per peer so
            // the probe cannot be used to make this node hammer an address.
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
        }
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
        case MessageType::user_sync: {
            if (!request.payload.empty())
                (void)users_.apply_all(decode_users(request.payload));
            return {MessageType::user_sync_reply, encode_users(users_.all())};
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
            // "yes, already present". Authenticate/decrypt/hash here so a
            // corrupt remote replica is never counted as healthy placement.
            // (The batched have_objects below and retain_objects are
            // presence checks since 0.32.7; see the note there.)
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
            // then persists the claim against index presence (0.32.7), the
            // same contract as the local claim path: a claim is not a
            // re-read, the scrub is. One admission charge for the whole batch, not one per
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
            // A retention claim says "this node holds the object". Until
            // 0.32.7 this handler re-read, decrypted and hashed every id in
            // the batch (valid()), serially, inside the writer's metadata
            // mutation: a quantum commit re-claims every extent of its file,
            // so a replica re-read gigabytes per 32 MB quantum (gbni-2:
            // 12.1 s per batch; es-1 with its disk saturated: the 195-284 s
            // mutations of 2026-09-07). The local side of retain_on() moved
            // to index presence in 0.32.3 for the same reason; the bytes were
            // verified when this node put them, every read authenticates
            // them again, and the scrub campaign is where later corruption is
            // found. No DATA admission either: there is no read buffer.
            for (const auto& id : ids) {
                const bool present = object_class == RetentionClass::data
                                         ? local_store().has(id)
                                         : control_store().has(id);
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
            previous->metadata_write_replicas_required != node.metadata_write_replicas_required ||
            previous->flags != node.flags;
        if (node.id != id_)
            client_.note_peer(node);
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

    // Advertised API endpoint for clients (Status nodes[].api_endpoint);
    // empty when this node runs no catalogue API, letting a consumer treat it
    // as unreported rather than guess. A configured endpoint is used as
    // given -- it describes the outer address, which behind a TLS-terminating
    // proxy differs from the bind in both scheme and port.
    //
    // The default is built from `info.host` -- this node's already-resolved
    // RPC advertise address -- rather than catalogue.api.listen: the API, like
    // RPC, conventionally binds a wildcard (0.0.0.0), which is not itself
    // dialable, so falling back to the raw listen address would readvertise
    // that wildcard instead of a real endpoint. A bare IPv6 literal is
    // bracketed, since an unbracketed one cannot be parsed back out of a URL.
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

    // The playback budgets this node enforces, so a client can bound its own
    // attempt against them instead of guessing. A node that serves no playback
    // reports none rather than a figure it would not honour: zero reads as
    // "cannot say", which is the honest answer from a node with streaming off.
    PlaybackBudgets playback;
    if (cfg_.streaming.enabled) {
        playback.startup_timeout_ms =
            static_cast<uint32_t>(std::max<int64_t>(0, cfg_.streaming.startup_timeout.count()));
        playback.segment_timeout_ms =
            static_cast<uint32_t>(std::max<int64_t>(0, cfg_.streaming.segment_timeout.count()));
    }
    telemetry_.refresh_local(info, std::string(kServerVersion), cache_capacity, cache_used,
                             storage_backends_online, peers_known, peers_active, 0, 0,
                             peers_active > 0 ? peers_active - 1 : 0, phase,
                             std::move(api_endpoint), playback);
}

void NodeRuntime::signal_telemetry_refresh() {
    telemetry_demand_.fetch_add(1, std::memory_order_release);
    telemetry_wait_cv_.notify_all();
}

void NodeRuntime::telemetry_loop(std::stop_token stop) {
    ThreadCpuReporter cpu_reporter("macha-telemetry", std::chrono::seconds(5), true);
    // `network.telemetry_interval_ms`, 10s by default. A floor keeps a
    // mis-set value from turning this into a spin loop.
    const auto interval = std::max(cfg_.telemetry_interval, std::chrono::milliseconds(250));
    // The wait below returns early whenever telemetry demand changes, and
    // demand is bumped on every peer observation and every readiness
    // transition -- a reconnecting or flapping peer can raise that rate
    // arbitrarily. Local sampling is cheap and still runs on every wake, so a
    // phase change is published promptly, but the network broadcast keeps its
    // own floor: however often this loop is woken, it cannot gossip more than
    // once a second, and never faster than the configured cadence itself.
    const auto min_gossip_interval = std::min(interval, std::chrono::milliseconds(1000));
    auto last_gossip = Clock::time_point{};
    const auto gossip_ttl = std::max(cfg_.dead_after * 2, std::chrono::milliseconds(60000));
    uint64_t handled_demand = 0;
    while (!stop.stop_requested()) {
        const auto demand = telemetry_demand_.load(std::memory_order_acquire);
        try {
            refresh_telemetry();
            // Sampling is always local. Gossip used to be suppressed whenever
            // the node had recent foreground/read-ahead work and then admitted
            // only onto an idle writer, which inverted what an operator needs:
            // a node went invisible exactly while it was busy or in trouble,
            // and on 2026-09-09 every WAN pair in the cluster reported peers
            // with an empty runtime block and a ~5 minute old sample. Removing
            // those two gates is what makes it timely; the frame class stays
            // SPECULATIVE deliberately, so gossip keeps out of the control
            // memory reserve and off the two control workers, and still cannot
            // delay operational RPC. A telemetry set is ~200 bytes per entry,
            // capped at 64 entries, so sending it every tick is cheap.
            if (const auto now = Clock::now(); now - last_gossip >= min_gossip_interval) {
                last_gossip = now;
                auto values = telemetry_.recent(gossip_ttl, 64);
                if (!values.empty()) {
                    // Still a no-dial notification: it rides established routes
                    // and never blocks. What it no longer does is give up the
                    // moment the writer has anything else in flight.
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
            if (!values.empty()) {
                // Same rule as the user table below, and for the same reason:
                // a peer admits every notification through its bounded server
                // request queue, so re-sending an unchanged set every tick
                // spends real RPC admission on every peer forever. Sessions do
                // change often, but between mints this is still silent.
                auto payload = encode_sessions(values);
                const auto digest = sha256(payload);
                std::set<NodeId> peers;
                for (const auto& peer : members_.active())
                    if (peer.id != id_)
                        peers.insert(peer.id);
                const auto now = Clock::now();
                const bool sessions_due =
                    digest != gossiped_sessions_ || peers != gossiped_session_peers_ ||
                    now - gossiped_sessions_at_ >= gossip_reannounce_interval;
                if (sessions_due && now >= gossip_sessions_retry_after_) {
                    const auto reached = client_.broadcast_best_effort(
                        {MessageType::session_sync, std::move(payload)}, FrameType::control);
                    // Record having announced this only once it actually went
                    // to everyone. A best-effort notify queues nothing when the
                    // writer is busy or a peer is not usable yet -- which is
                    // exactly the case at the moment a peer rejoins -- and
                    // recording it anyway would retire the retry before it ran.
                    if (reached >= peers.size()) {
                        gossiped_sessions_ = digest;
                        gossiped_session_peers_ = std::move(peers);
                        gossiped_sessions_at_ = now;
                    } else {
                        // Retry, but on a floor rather than on every tick. An
                        // unreached peer is usually a busy writer, and hammering
                        // a busy node with repair traffic is how this became a
                        // problem in the first place.
                        gossip_sessions_retry_after_ = Clock::now() + gossip_retry_floor;
                    }
                }
            }
        } catch (const std::exception& error) {
            Log::debug("session gossip skipped: " + std::string(error.what()));
        }
        // The user table rides the same tick. Unlike sessions this is the
        // whole table including tombstones, so a node that missed a deletion
        // while it was down learns the tombstone rather than resurrecting the
        // account from its own stale replica.
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
    // Notify, never call. The serial call() this replaced ran to
    // control_no_progress_deadline (30 s) against every peer that membership
    // still called active, so one unreachable-but-not-yet-dead peer stalled
    // every login by that long -- exactly when metadata is degraded and peers
    // are unreachable is exactly when you need to log in. The local merge has
    // already happened above and the gossip tick is the documented backstop,
    // so there was never anything to wait for.
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
    // Every inbound notification is admitted through the peer's bounded server
    // request queue (RpcServer::enqueue_notification), so unconditional
    // periodic gossip spends a real RPC admission slot on every peer, every
    // tick, forever -- and spends most on a busy node, which is where it can
    // least be afforded. A table that has not changed must therefore cost
    // nothing at all.
    //
    // Two things make a broadcast worth spending: the table changed here, or a
    // peer appeared that may have missed the change that produced it. The
    // second is what makes a node that was down converge: it joins, the active
    // count rises, and the whole table (tombstones included) goes out once.
    try {
        const auto table = users_.table_hash();
        std::set<NodeId> peers;
        for (const auto& peer : members_.active())
            if (peer.id != id_)
                peers.insert(peer.id);
        // Nothing changed here, nobody new has arrived, and the periodic
        // re-announce is not due: say nothing at all.
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
        // As with sessions: commit only when it reached everyone, so a peer
        // that was not yet usable is retried on the next tick instead of being
        // marked told. This is what makes a node that was down converge.
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
    // Always the full table, never a window: a peer that was offline longer
    // than the gossip TTL must still converge, and at tens of records this is
    // smaller than the telemetry set already broadcast on the same tick.
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
        // A peer that accepts no inbound connections is exchanged with only
        // over the session it opened to us; when there is none there is
        // nothing to dial and nothing to log about it.
        const auto unreachable_by_design = [&](const NodeInfo& node) {
            return !node_inbound_capable(node) &&
                   !client_.has_route(node.id, TransportLane::control);
        };
        for (const auto& endpoint : cfg_.bootstrap) {
            exchanged.emplace(endpoint.host, endpoint.port);
            try {
                auto known =
                    std::find_if(known_nodes.begin(), known_nodes.end(), [&](const NodeInfo& node) {
                        return node.id != id_ && node.host == endpoint.host &&
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
            if (node.id == id_)
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

void NodeRuntime::enqueue_fetched(const ObjectId& id, std::span<const uint8_t> data, bool promote) {
    const bool cache = ready(ready_cache) && cache_ && cache_->enabled();
    if (promote && (!ready(ready_data_storage) || !local_))
        promote = false;
    if (!cache && !promote)
        return;

    auto memory = retained_memory_.try_acquire(MemoryClass::speculative,
                                               MemoryOwner::object_payload, data.size());
    if (!memory) {
        // Dropping is the design (see below), but a dropped opportunity has
        // to be visible: a cache that "did not fill" with nothing in the log
        // is indistinguishable from a cache that is broken.
        Log::debug("opportunistic persistence skipped object=" + to_string(id) +
                   " reason=retained_memory bytes=" + std::to_string(data.size()) +
                   " cache=" + (cache ? "1" : "0") + " promote=" + (promote ? "1" : "0"));
        return;
    }

    // Do not let opportunistic persistence become back-pressure on playback.
    // If the bounded memory queue is full we simply drop this opportunity; the
    // normal repair loop will converge authoritative replicas later.
    constexpr size_t max_queued_bytes = 256ULL * 1024 * 1024;
    std::lock_guard lock(local_copy_mutex_);
    if (data.size() > max_queued_bytes || local_copy_bytes_ + data.size() > max_queued_bytes) {
        Log::debug("opportunistic persistence skipped object=" + to_string(id) +
                   " reason=queue_full queued_bytes=" + std::to_string(local_copy_bytes_));
        return;
    }
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
            Log::debug("opportunistic persistence skipped object=" + to_string(job.id) +
                       " reason=data_credit");
            continue;
        }
        bool cached = false;
        if (job.cache && ready(ready_cache) && cache_) {
            cached = cache_->put(job.id, job.data);
            if (!cached)
                Log::debug("opportunistic persistence skipped object=" + to_string(job.id) +
                           " reason=cache_put_failed");
        }
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
