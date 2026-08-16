// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster.hpp"
#include "diagnostics.hpp"

#include "codec.hpp"
#include "log.hpp"

#include <algorithm>
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
    return node;
}

RpcMessage error_reply(const std::string& text) {
    Writer writer;
    writer.string(text);
    return {MessageType::error, writer.take()};
}

int64_t activity_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
        .count();
}
} // namespace

NodeRuntime::NodeRuntime(Config config, ClusterKeys keys)
    : cfg_(normalize_config(std::move(config))), keys_(keys), state_lock_(cfg_.state_path),
      id_(load_or_create_node_id(cfg_.state_path)),
      local_(cfg_.state_path, id_, cfg_.storage_backends, keys_.storage),
      cache_(cfg_.cache, keys_.storage),
      meta_(cfg_.state_path, keys_.storage),
      members_(self_info(cfg_, id_, local_.used(), local_.limit(), meta_.committed().generation),
               cfg_.dead_after),
      client_(
          keys_, [this] { return members_.self(); },
          [this](const NodeInfo& peer) {
              members_.observe(peer, true);
              remote_metadata_generation_.store(
                  std::max(remote_metadata_generation_.load(), peer.metadata_generation));
          },
          [this](uint64_t generation) {
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

    // The metadata cache is deliberately independent of node state. If node
    // state was restored from an older backup but the SSD cache survived, a
    // newer valid snapshot can improve read-only/offline startup. Normal quorum
    // reads still arbitrate before any mutation.
    if (auto cached = cache_.metadata()) {
        if (cached->generation > meta_.committed().generation) {
            (void)meta_.seed(*cached);
            (void)meta_.remember_committed(*cached);
        }
    }
    cache_.remember_metadata(meta_.committed());
    members_.storage(local_.used(), local_.limit());
    members_.metadata_generation(meta_.committed().generation);
}

NodeRuntime::~NodeRuntime() {
    stop();
}

void NodeRuntime::start() {
    if (started_.exchange(true))
        return;
    local_writer_ = std::jthread([this](std::stop_token stop) { local_writer_loop(stop); });
    server_.start();
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
    if (maintenance_.joinable()) {
        Log::debug("shutdown: node maintenance request_stop");
        maintenance_.request_stop();
        Log::debug("shutdown: node maintenance joining");
        maintenance_.join();
        Log::debug("shutdown: node maintenance joined");
    }
    Log::debug("shutdown: RpcServer::stop calling");
    server_.stop();
    Log::debug("shutdown: RpcServer::stop returned");
    Log::debug("shutdown: RpcClient::stop calling");
    client_.stop();
    Log::debug("shutdown: RpcClient::stop returned");
    if (local_writer_.joinable()) {
        Log::debug("shutdown: local writer request_stop");
        local_writer_.request_stop();
        local_copy_cv_.notify_all();
        Log::debug("shutdown: local writer joining");
        local_writer_.join();
        Log::debug("shutdown: local writer joined");
    }
    Log::debug("shutdown: NodeRuntime::stop complete");
}

std::chrono::milliseconds NodeRuntime::stall_notice_for(MessageType type) const {
    if (type == MessageType::get_object || type == MessageType::put_object)
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
    members_.metadata_generation(generation);
    Writer writer;
    writer.u64(generation);
    client_.broadcast({MessageType::metadata_notice, writer.take()});
}

bool NodeRuntime::seed_metadata(const MetadataRecord& record) {
    // Seeding repairs/stages a replica but does not make the record a durable
    // recovery witness. Only checkpoint_metadata() is called after a quorum is
    // known to have committed the record.
    return meta_.seed(record);
}

bool NodeRuntime::checkpoint_metadata(const MetadataRecord& record) {
    auto before = meta_.committed();
    (void)meta_.seed(record);
    bool checkpointed = meta_.remember_committed(record);
    auto committed = meta_.committed();
    members_.metadata_generation(committed.generation);
    if (checkpointed)
        cache_.remember_metadata(committed);
    if (checkpointed && committed.hash != before.hash)
        announce_metadata_generation(committed.generation);
    return checkpointed;
}

bool NodeRuntime::cas_metadata(uint64_t generation, const Hash256& hash,
                               std::span<const uint8_t> payload, MetadataRecord* out) {
    // A successful per-voter CAS is only a proposal until MetadataManager has
    // observed a quorum. It must not advance the advertised generation or the
    // durable committed checkpoint on its own.
    return meta_.cas(generation, hash, payload, out);
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
        case MessageType::have_object: {
            Reader reader(request.payload);
            ObjectId id{reader.fixed<32>()};
            reader.finish();
            Writer writer;
            writer.u8(local_.has(id));
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
        case MessageType::put_object: {
            Reader reader(request.payload);
            ObjectId id{reader.fixed<32>()};
            auto data = reader.bytes(128 * 1024 * 1024);
            reader.finish();
            note_activity(frame_type, data.size());
            if (!local_.put(id, data))
                return error_reply("storage limit reached");
            members_.storage(local_.used(), local_.limit());
            return {MessageType::ok, {}};
        }
        case MessageType::delete_object: {
            Reader reader(request.payload);
            ObjectId id{reader.fixed<32>()};
            reader.finish();
            (void)local_.remove(id);
            (void)cache_.remove(id);
            members_.storage(local_.used(), local_.limit());
            return {MessageType::ok, {}};
        }
        case MessageType::get_metadata:
            return {MessageType::metadata_reply, encode_metadata_record(meta_.current())};
        case MessageType::get_committed_metadata:
            return {MessageType::metadata_reply, encode_metadata_record(meta_.committed())};
        case MessageType::seed_metadata: {
            auto metadata = decode_metadata_record(request.payload);
            Writer writer;
            writer.u8(seed_metadata(metadata));
            return {MessageType::bool_reply, writer.take()};
        }
        case MessageType::checkpoint_metadata: {
            auto metadata = decode_metadata_record(request.payload);
            Writer writer;
            writer.u8(checkpoint_metadata(metadata));
            return {MessageType::bool_reply, writer.take()};
        }
        case MessageType::cas_metadata: {
            Reader reader(request.payload);
            auto generation = reader.u64();
            Hash256 hash{reader.fixed<32>()};
            auto payload = reader.bytes();
            reader.finish();
            MetadataRecord out;
            bool ok = cas_metadata(generation, hash, payload, &out);
            Writer writer;
            writer.u8(ok);
            writer.bytes(encode_metadata_record(out));
            return {MessageType::cas_reply, writer.take()};
        }
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
        if (refresh_ms >= 100 && Log::enabled(LogLevel::debug))
            Log::debug("DIAG node-stage stage=storage-refresh elapsed_ms=" +
                       std::to_string(refresh_ms));
        members_.storage(local_.used(), local_.limit());
        members_.metadata_generation(meta_.current().generation);
        std::set<std::pair<std::string, uint16_t>> exchanged;
        const auto known_nodes = members_.all();
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
            } catch (const std::exception& error) {
                Log::debug("peer " + node.host + ": " + error.what());
            }
        }
        cpu_reporter.tick();
        auto until = Clock::now() + cfg_.heartbeat;
        while (!stop.stop_requested() && Clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
