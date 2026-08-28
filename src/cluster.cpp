// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster.hpp"
#include "diagnostics.hpp"
#include "durable_file.hpp"

#include "codec.hpp"
#include "log.hpp"
#include "macha_version.hpp"

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
            throw std::runtime_error("incompatible Macha storage layout; 0.18 requires a fresh namespace");
    } else {
        // 0.18 intentionally has no live migration path. Refuse to reinterpret an
        // older namespace/backend layout as the new storage contract. StorageLock
        // has already created .macha.lock, which is the only allowed pre-existing
        // entry for a fresh state directory.
        for (const auto& entry : std::filesystem::directory_iterator(state)) {
            if (entry.path().filename() == ".macha.lock") continue;
            throw std::runtime_error(
                "existing unversioned Macha state detected; 0.18 requires a fresh namespace");
        }
        durable_replace_file(marker, std::string(expected) + "\n");
    }
    return load_or_create_node_id(state);
}
} // namespace

NodeRuntime::NodeRuntime(Config config, ClusterKeys keys)
    : cfg_(normalize_config(std::move(config))), keys_(keys), state_lock_(cfg_.state_path),
      id_(load_v18_node_id(cfg_.state_path)),
      durability_epoch_(random_node_id()),
      local_(cfg_.state_path, id_, cfg_.storage_backends, keys_.storage,
             std::chrono::milliseconds(500), cfg_.storage_packing),
      control_(cfg_.metadata_store.path,
               LocalStoreOptions{cfg_.metadata_store.limit, 0,
                                 cfg_.metadata_store.packing.threshold,
                                 cfg_.metadata_store.packing.target_size},
               keys_.storage),
      cache_(cfg_.cache, keys_.storage),
      retention_(cfg_.state_path, keys_.storage),
      meta_(cfg_.state_path, keys_.storage, cache_.metadata()),
      members_(self_info(cfg_, id_, local_.used(), local_.limit(), meta_.committed().generation),
               cfg_.dead_after),
      public_connectivity_(cfg_, id_,
                           Endpoint{members_.self().host, members_.self().port}),
      telemetry_(id_, cfg_.state_path / "telemetry" / "last-known.bin"),
      client_(
          keys_, [this] { return members_.self(); },
          [this](const NodeInfo& peer) {
              members_.observe(peer, true);
              remote_metadata_generation_.store(
                  std::max(remote_metadata_generation_.load(), peer.metadata_generation));
          },
          [this](uint64_t generation) {
              remote_metadata_epoch_.fetch_add(1, std::memory_order_acq_rel);
              auto current = remote_metadata_generation_.load();
              while (current < generation && !remote_metadata_generation_.compare_exchange_weak(
                                                 current, generation)) {
              }
          },
          cfg_.connect_timeout, cfg_.heartbeat, cfg_.dead_after, cfg_.max_frame_size),
      server_(
          cfg_.listen_host, cfg_.port, keys_, members_.self(),
          [this](const NodeInfo& peer, FrameType frame_type, const RpcMessage& request) { return handle(peer, frame_type, request); },
          [this](const NodeInfo& peer) {
              members_.observe(peer, true);
              auto current = remote_metadata_generation_.load();
              while (current < peer.metadata_generation &&
                     !remote_metadata_generation_.compare_exchange_weak(
                         current, peer.metadata_generation)) {
              }
          },
          cfg_.max_frame_size) {
    server_.attach_client(client_);

    // Resolve the effective public endpoint before any peer exchange. UPnP and
    // the optional AWS external-IP fallback only change how this node is
    // advertised; the local listener remains cfg_.listen_host:cfg_.port.
    (void)refresh_public_connectivity(false);

    // Identity-reset tombstones must be active before the first peer exchange.
    // This prevents stale membership from being reintroduced during startup.
    try {
        const auto committed_snapshot = decode_snapshot(meta_.committed().payload);
        for (const auto& [_, reset] : committed_snapshot.identity_resets)
            apply_identity_reset(reset);
    } catch (const std::exception& error) {
        Log::debug("cannot preload identity reset tombstones: " + std::string(error.what()));
    }

    // The persistent metadata cache is a read/recovery aid only. It must never
    // become metadata authority merely because it is newer than the primary
    // state directory: only a durable acceptance certificate may make a 0.19+
    // commit authoritative. MetadataReplica may use this cache as a quarantined
    // recovery seed, but recovery remains explicitly unaccepted until peers
    // supply accepted-head evidence.
    cache_.remember_metadata(meta_.committed());
    const auto storage_used = local_.used();
    const auto storage_capacity = local_.limit();
    members_.storage(storage_used, storage_capacity);
    members_.metadata_generation(meta_.committed().generation);
    telemetry_storage_used_.store(storage_used, std::memory_order_relaxed);
    telemetry_storage_capacity_.store(storage_capacity, std::memory_order_relaxed);
    telemetry_metadata_generation_.store(meta_.committed().generation, std::memory_order_relaxed);
    refresh_telemetry();
}

NodeRuntime::~NodeRuntime() {
    stop();
}

void NodeRuntime::start() {
    if (started_.exchange(true))
        return;
    local_writer_ = std::jthread([this](std::stop_token stop) { local_writer_loop(stop); });
    server_.start();
    if (cfg_.connectivity_check.enabled)
        (void)public_connectivity_.probe(false);
    telemetry_worker_ = std::jthread([this](std::stop_token stop) { telemetry_loop(stop); });
    maintenance_ = std::jthread([this](std::stop_token stop) { loop(stop); });
    Log::info("node " + to_string(id_).substr(0, 12) + " listening on " +
              std::to_string(server_.bound_port()) + " domain=" + members_.self().failure_domain);
}

void NodeRuntime::stop() {
    if (!started_.exchange(false)) {
        Log::debug("shutdown: NodeRuntime::stop already stopped");
        return;
    }
    Log::debug("shutdown: NodeRuntime::stop begin");
    request_stop();
    // Telemetry never waits on transport, so retire it before closing shared
    // RPC state. This also proves shutdown cannot be held behind telemetry.
    if (telemetry_worker_.joinable()) {
        Log::debug("shutdown: telemetry joining");
        telemetry_worker_.join();
        Log::debug("shutdown: telemetry joined");
    }
    // Close transport before joining maintenance. A maintenance iteration may
    // already be waiting on an RPC; closing the client/server first makes that
    // wait fail promptly instead of holding shutdown behind network timeouts.
    Log::debug("shutdown: RpcServer::stop calling");
    server_.stop();
    Log::debug("shutdown: RpcServer::stop returned");
    Log::debug("shutdown: RpcClient::stop calling");
    client_.stop();
    Log::debug("shutdown: RpcClient::stop returned");
    if (maintenance_.joinable()) {
        Log::debug("shutdown: node maintenance joining");
        maintenance_.join();
        Log::debug("shutdown: node maintenance joined");
    }
    if (local_writer_.joinable()) {
        Log::debug("shutdown: local writer joining");
        local_writer_.join();
        Log::debug("shutdown: local writer joined");
    }
    Log::debug("shutdown: NodeRuntime::stop complete");
}

void NodeRuntime::request_stop() {
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
}

std::chrono::milliseconds NodeRuntime::stall_notice_for(MessageType type) const {
    if (type == MessageType::get_object || type == MessageType::put_object ||
        type == MessageType::put_object_deferred ||
        type == MessageType::object_durability_barrier)
        return cfg_.data_stall_notice;
    return cfg_.control_stall_notice;
}

RpcReply NodeRuntime::call(const NodeInfo& node, MessageType type,
                           std::span<const uint8_t> payload) {
    return client_.call(node, type, payload, stall_notice_for(type));
}

RpcReply NodeRuntime::call(const Endpoint& endpoint, MessageType type,
                           std::span<const uint8_t> payload) {
    return client_.call(endpoint, type, payload, stall_notice_for(type));
}

RpcReply NodeRuntime::call(const NodeInfo& node, MessageType type,
                           std::span<const uint8_t> payload, FrameType frame_type) {
    return client_.call(node, type, payload, frame_type, stall_notice_for(type));
}

RpcReply NodeRuntime::call(const Endpoint& endpoint, MessageType type,
                           std::span<const uint8_t> payload, FrameType frame_type) {
    return client_.call(endpoint, type, payload, frame_type, stall_notice_for(type));
}

AsyncRpc NodeRuntime::call_async(const NodeInfo& node, MessageType type,
                                 std::span<const uint8_t> payload) {
    return client_.call_async(node, type, payload);
}

AsyncRpc NodeRuntime::call_async(const Endpoint& endpoint, MessageType type,
                                 std::span<const uint8_t> payload) {
    return client_.call_async(endpoint, type, payload);
}

AsyncRpc NodeRuntime::call_async(const NodeInfo& node, MessageType type,
                                 std::span<const uint8_t> payload, FrameType frame_type) {
    return client_.call_async(node, type, payload, frame_type);
}

AsyncRpc NodeRuntime::call_async(const Endpoint& endpoint, MessageType type,
                                 std::span<const uint8_t> payload, FrameType frame_type) {
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
    remote_metadata_epoch_.fetch_add(1, std::memory_order_acq_rel);
    members_.metadata_generation(generation);
    Writer writer;
    writer.u64(generation);
    client_.broadcast({MessageType::metadata_notice, writer.take()});
}

bool NodeRuntime::store_metadata_commit(const MetadataHistoryEntry& entry) {
    return meta_.import_history(entry);
}

bool NodeRuntime::accept_metadata_commit(const MetadataAcceptance& acceptance) {
    // A protocol-20 node accepts only branches whose *resulting* cluster policy
    // matches its configured policy. The certificate's own `required` value may
    // be stronger during a safe policy transition (for example W=3 -> W=2), so
    // comparing it directly with the local configuration would incorrectly
    // reject the transition. MetadataReplica validates the certificate against
    // the commit and its parent policies.
    if (acceptance.required) {
        auto record = meta_.historical(acceptance.hash);
        if (!record)
            return false;
        const auto snapshot = decode_snapshot(record->payload);
        if (snapshot.metadata_write_replicas_required !=
            cfg_.metadata_min_write_replicas)
            return false;
    }
    const auto heads_before = meta_.accepted_head_certificates();
    const auto before = meta_.committed();
    if (!meta_.accept_commit(acceptance))
        return false;
    const auto heads_after = meta_.accepted_head_certificates();
    const auto after = meta_.committed();
    members_.metadata_generation(std::max(after.generation, acceptance.generation));
    if (heads_after == heads_before)
        return true;
    if (after.hash != before.hash)
        cache_.remember_metadata(after);
    // A same-generation sibling may not change the materialised preferred head,
    // but peers still need an ordinary metadata wake-up so foreground cache
    // validation and background reconciliation notice the changed head set.
    announce_metadata_generation(std::max(after.generation, acceptance.generation));
    return true;
}

std::vector<MetadataAcceptance> NodeRuntime::metadata_heads() const {
    return meta_.accepted_head_certificates();
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
        case MessageType::have_object: {
            Reader reader(request.payload);
            ObjectId id{reader.fixed<32>()};
            reader.finish();
            Writer writer;
            // Replica-presence RPCs are durability decisions, not directory
            // existence probes. Authenticate/decrypt/hash the object before
            // allowing repair or write-floor logic to count this replica.
            writer.u8(local_.valid(id));
            return {MessageType::bool_reply, writer.take()};
        }
        case MessageType::get_object: {
            Reader reader(request.payload);
            ObjectId id{reader.fixed<32>()};
            reader.finish();
            auto data = local_.get(id);
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
            auto data = control_.get(id);
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
            note_activity(frame_type, data.size());
            if (request.type == MessageType::put_object_deferred) {
                const auto generation = local_.put_deferred(id, data);
                if (!generation)
                    return error_reply("storage limit reached");
                members_.storage(local_.used(), local_.limit());
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
            if (!local_.put(id, data))
                return error_reply("storage limit reached");
            members_.storage(local_.used(), local_.limit());
            return {MessageType::ok, {}};
        }
        case MessageType::put_control_object: {
            Reader reader(request.payload);
            ObjectId id{reader.fixed<32>()};
            auto data = reader.bytes(128 * 1024 * 1024);
            reader.finish();
            if (!control_.put(id, data))
                return error_reply("control storage limit reached");
            return {MessageType::ok, {}};
        }
        case MessageType::object_durability_barrier: {
            Reader reader(request.payload);
            NodeId expected_epoch{reader.fixed<16>()};
            const auto domain = reader.u64();
            const auto required_generation = reader.u64();
            const auto backend_instance = reader.u64();
            reader.finish();
            if (expected_epoch != durability_epoch_)
                return error_reply("storage durability epoch changed");
            try {
                local_.durability_barrier({domain, required_generation, backend_instance},
                                          DurabilityUrgency::batchable);
                members_.storage(local_.used(), local_.limit());
                return {MessageType::ok, {}};
            } catch (const std::exception& error) {
                return error_reply(std::string("storage durability barrier failed: ") + error.what());
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
                const bool present = object_class == RetentionClass::data
                                         ? local_.valid(id)
                                         : control_.valid(id);
                if (!present)
                    return error_reply("retention object is not durably present");
            }
            retention_.retain_batch(object_class, ids, dot);
            return {MessageType::ok, {}};
        }
        case MessageType::delete_object: {
            Reader reader(request.payload);
            ObjectId id{reader.fixed<32>()};
            reader.finish();
            if (retention_.retained(RetentionClass::data, id))
                return error_reply("object has an active retention claim");
            (void)local_.remove(id);
            (void)cache_.remove(id);
            members_.storage(local_.used(), local_.limit());
            return {MessageType::ok, {}};
        }
        case MessageType::get_metadata:
            return {MessageType::metadata_reply, encode_metadata_record(meta_.current())};
        case MessageType::get_committed_metadata:
            return {MessageType::metadata_reply, encode_metadata_record(meta_.committed())};
        case MessageType::get_metadata_identity:
            return metadata_identity_reply(meta_.committed_identity());
        case MessageType::get_metadata_history_entry: {
            Reader reader(request.payload);
            Hash256 hash;
            hash.bytes = reader.fixed<32>();
            reader.finish();
            auto entry = meta_.history_entry(hash);
            if (!entry)
                return error_reply("metadata history entry unavailable");
            return {MessageType::metadata_history_entry_reply,
                    encode_metadata_history_entry(*entry)};
        }
        case MessageType::has_metadata_history_entry: {
            Reader reader(request.payload);
            Hash256 hash;
            hash.bytes = reader.fixed<32>();
            reader.finish();
            Writer writer;
            writer.u8(meta_.history_contains(hash));
            return {MessageType::bool_reply, writer.take()};
        }
        case MessageType::put_metadata_history_entry: {
            auto entry = decode_metadata_history_entry(request.payload);
            Writer writer;
            writer.u8(meta_.import_history(entry));
            return {MessageType::bool_reply, writer.take()};
        }
        case MessageType::get_metadata_heads:
            return {MessageType::metadata_heads_reply,
                    encode_metadata_acceptance_set(metadata_heads())};
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
    Reader reader(payload);
    auto count = reader.u32();
    if (count > 100000)
        throw DecodeError("member list too large");
    uint64_t newest_metadata = remote_metadata_generation_.load();
    for (uint32_t i = 0; i < count; ++i) {
        auto node = decode_node_info(reader);
        newest_metadata = std::max(newest_metadata, node.metadata_generation);
        members_.observe(std::move(node));
    }
    reader.finish();
    remote_metadata_generation_.store(newest_metadata);
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
                  std::to_string(status.advertised.port) + " source=" +
                  status.advertised_source);
    }
    return status;
}

void NodeRuntime::refresh_telemetry() {
    // Public reachability may change the advertised host/port at runtime. Use
    // the authoritative current self identity instead of a stale construction
    // snapshot; this lock is taken only once per telemetry refresh interval.
    auto info = members_.self();
    info.used = telemetry_storage_used_.load(std::memory_order_relaxed);
    info.capacity = telemetry_storage_capacity_.load(std::memory_order_relaxed);
    info.metadata_generation = telemetry_metadata_generation_.load(std::memory_order_relaxed);
    const uint64_t cache_capacity = static_cast<uint64_t>(cfg_.cache.max_blocks) * cfg_.extent_size;
    const uint64_t cache_used = static_cast<uint64_t>(cache_.blocks()) * cfg_.extent_size;
    const auto peers_known = telemetry_peers_known_.load(std::memory_order_relaxed);
    const auto peers_active = telemetry_peers_active_.load(std::memory_order_relaxed);
    telemetry_.refresh_local(
        info, std::string(kServerVersion), cache_capacity, cache_used,
        static_cast<uint32_t>(local_.online_backends()), peers_known, peers_active,
        0, 0, peers_active > 0 ? peers_active - 1 : 0);
}

void NodeRuntime::telemetry_loop(std::stop_token stop) {
    ThreadCpuReporter cpu_reporter("macha-telemetry", std::chrono::seconds(5), true);
    const auto interval = std::chrono::seconds(5);
    const auto idle_before_gossip = std::chrono::seconds(2);
    const auto gossip_ttl = std::max(cfg_.dead_after * 2, std::chrono::milliseconds(60000));
    while (!stop.stop_requested()) {
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
        cpu_reporter.tick();
        std::unique_lock lock(telemetry_wait_mutex_);
        telemetry_wait_cv_.wait_for(lock, stop, interval, [] { return false; });
    }
}

bool NodeRuntime::apply_identity_reset(const IdentityAssociationReset& reset) {
    const bool changed = members_.apply_identity_reset(reset);
    // Keep all consumers idempotently aligned even if one of them learned the
    // tombstone first through a different path.
    telemetry_.apply_identity_reset(reset);
    client_.invalidate_identity_association(reset);
    if (changed) {
        Log::info("node identity association reset scope=" +
                  identity_reset_key(reset.host, reset.port) + " stale_node_id=" +
                  (reset.stale_node_id == NodeId{} ? std::string("<any>") : to_string(reset.stale_node_id)) +
                  " epoch=" + std::to_string(reset.epoch) +
                  " reset_by=" + to_string(reset.reset_by) +
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
        const auto refresh_started = Clock::now();
        local_.refresh();
        const auto refresh_ms = elapsed_ms(refresh_started);
        if (refresh_ms >= 100 && Log::enabled(LogLevel::all))
            Log::trace("DIAG node-stage stage=storage-refresh elapsed_ms=" +
                       std::to_string(refresh_ms));
        const auto storage_used = local_.used();
        const auto storage_capacity = local_.limit();
        const auto metadata_generation = meta_.generation();
        members_.storage(storage_used, storage_capacity);
        members_.metadata_generation(metadata_generation);
        telemetry_storage_used_.store(storage_used, std::memory_order_relaxed);
        telemetry_storage_capacity_.store(storage_capacity, std::memory_order_relaxed);
        telemetry_metadata_generation_.store(metadata_generation, std::memory_order_relaxed);
        std::set<std::pair<std::string, uint16_t>> exchanged;
        const auto known_nodes = members_.all();
        telemetry_peers_known_.store(static_cast<uint32_t>(known_nodes.size()),
                                     std::memory_order_relaxed);
        uint32_t active_peers = 1;
        for (const auto& endpoint : cfg_.bootstrap) {
            exchanged.emplace(endpoint.host, endpoint.port);
            try {
                // Bootstrap is only identity-less before first authentication.
                // Once the advertised endpoint belongs to a known NodeId, use
                // the identity-aware route so a reconnect/backoff state cannot
                // force us back into endpoint-dial behaviour.
                auto known = std::find_if(known_nodes.begin(), known_nodes.end(),
                                          [&](const NodeInfo& node) {
                                              return node.id != id_ &&
                                                     node.host == endpoint.host &&
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
    const bool cache = cache_.enabled();
    if (!cache && !promote)
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
            local_copy_cv_.wait(lock, [&] {
                return stop.stop_requested() || !local_copies_.empty();
            });
            if (stop.stop_requested() && local_copies_.empty())
                return;
            job = std::move(local_copies_.front());
            local_copies_.pop_front();
            local_copy_bytes_ -= job.data.size();
        }
        bool cached = false;
        if (job.cache)
            cached = cache_.put(job.id, job.data);
        // With a persistent cache, foreground fetches are made durable on the
        // cache device first and authoritative HDD promotion is left to idle
        // maintenance. If the cache write fails (or cache is disabled), retain
        // the already-fetched bytes by promoting here rather than forcing a
        // second network transfer later.
        if (job.promote && (!job.cache || !cached)) {
            (void)local_.put(job.id, job.data);
            members_.storage(local_.used(), local_.limit());
        }
        cpu_reporter.tick();
    }
}

void NodeRuntime::reconfigure_local(const Config& config) {
    auto updated = normalize_config(config);
    local_.reconfigure(updated.storage_backends);
    local_.refresh();
    cache_.reconfigure(updated.cache);
    // These fields are node-local policy only and are not consumed by the
    // long-lived networking/metadata threads, so keep the public snapshot in
    // sync with a successful live reload without changing cluster policy.
    cfg_.storage_backends = updated.storage_backends;
    cfg_.cache = updated.cache;
    cfg_.hydration = updated.hydration;
    cfg_.read_ahead_extents = updated.read_ahead_extents;
    members_.storage(local_.used(), local_.limit());
}
} // namespace macha
