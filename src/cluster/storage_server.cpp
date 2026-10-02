// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/storage_server.hpp"

#include "cluster/cluster.hpp"
#include "codec.hpp"
#include "diagnostics.hpp"
#include "log.hpp"
#include "storage/persistent_cache.hpp"
#include "storage/retention.hpp"
#include "storage/storage_pool.hpp"
#include "supervised.hpp"

namespace macha {

StorageServer::StorageServer(NodeRuntime& node, const NodeIdentity& identity, Stores stores,
                             DataResourceArbiter& data_resources, ActivityClocks& activity,
                             NodeEvents& events, MessageRoutes& routes, size_t extent_size,
                             uint64_t cache_max_blocks, std::chrono::milliseconds refresh_interval)
    : node_(node), identity_(identity), local_(stores.data), control_(stores.control),
      claims_(stores.claims), cache_(stores.cache), data_resources_(data_resources),
      activity_(activity), events_(events), routes_(routes), extent_size_(extent_size),
      cache_max_blocks_(cache_max_blocks), refresh_interval_(refresh_interval) {
    refresh();
    bind_routes();
    refresher_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("storage-refresh", stop, [this, stop] { refresh_loop(stop); });
    });
}

// Routes first, so no request reaches a store while the refresher stops.
StorageServer::~StorageServer() {
    for (const auto type : bound_)
        routes_.unbind(type);
    if (refresher_.joinable()) {
        refresher_.request_stop();
        refresh_cv_.notify_all();
        refresher_.join();
    }
}

void StorageServer::route(MessageType type, MessageRoutes::Handler handler) {
    routes_.bind(type, std::move(handler));
    bound_.push_back(type);
}

void StorageServer::refresh() {
    const auto started = Clock::now();
    local_.refresh();
    const auto refresh_ms = elapsed_ms(started);
    if (refresh_ms >= 100 && Log::enabled(LogLevel::all))
        Log::trace("DIAG node-stage stage=storage-refresh elapsed_ms=" + std::to_string(refresh_ms));
    node_.advertise_storage(local_.used(), local_.limit());
    node_.advertise_storage_backends(static_cast<uint32_t>(local_.online_backends()));
    const auto stats = cache_.stats();
    node_.advertise_cache(cache_max_blocks_ * extent_size_,
                          static_cast<uint64_t>(cache_.blocks()) * extent_size_,
                          CacheActivity{stats.hits, stats.misses, stats.evictions});
}

void StorageServer::refresh_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(refresh_mutex_);
            refresh_cv_.wait_for(lock, stop, refresh_interval_, [] { return false; });
        }
        if (stop.stop_requested())
            return;
        refresh();
    }
}

// DATA and CONTROL objects: presence, reads, writes, durability and retention.
void StorageServer::bind_routes() {
    route(MessageType::have_object,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              Reader reader(request.payload);
              ObjectId id{reader.fixed<32>()};
              reader.finish();
              Writer writer;
              auto resource = data_resources_.try_acquire(
                  DataWorkContext(frame_type, extent_size_), extent_size_);
              if (!resource)
                  return error_reply("DATA resource admission busy or stopping");
              // Repair/rebalance trust this "present" without re-verifying, so
              // authenticate, decrypt and hash: a corrupt replica never counts as
              // healthy placement. (have_objects and retain_objects check presence.)
              writer.u8(local_.valid(id));
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
                  DataWorkContext(frame_type, extent_size_), extent_size_);
              if (!resource)
                  return error_reply("DATA resource admission busy or stopping");
              Writer writer;
              writer.u32(count);
              for (const auto& id : ids)
                  writer.u8(local_.has(id));
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
                      DataWorkContext(frame_type, extent_size_), extent_size_);
                  if (!resource)
                      return error_reply("DATA resource admission busy or stopping");
                  writer.u8(local_.valid(id));
              }
              return {MessageType::have_valid_objects_reply, writer.take()};
          });
    route(MessageType::have_control_objects,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              // The CONTROL counterpart: answers from control_ and takes
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
                  writer.u8(control_.has(id));
              return {MessageType::have_control_objects_reply, writer.take()};
          });
    route(MessageType::get_object,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              Reader reader(request.payload);
              ObjectId id{reader.fixed<32>()};
              reader.finish();
              auto resource = data_resources_.acquire(DataWorkContext(frame_type, extent_size_),
                                                      extent_size_);
              if (!resource)
                  return error_reply("DATA resource admission stopping");
              auto data = local_.get(id);
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
              auto data = control_.get(id);
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
            if (data.size() > extent_size_)
                return error_reply("DATA object exceeds configured extent size");
            auto resource =
                data_resources_.acquire(DataWorkContext(frame_type, data.size()), data.size());
            if (!resource)
                return error_reply("DATA resource admission stopping");
            activity_.note(frame_type, data.size());
            if (!local_.has(id))
                events_.notify(NodeEvent::storage);
            if (request.type == MessageType::put_object_deferred) {
                const auto generation = local_.put_deferred(id, data);
                if (!generation)
                    return error_reply("storage limit reached");
                node_.advertise_storage(local_.used(), local_.limit());
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
            if (!local_.put(id, data))
                return error_reply("storage limit reached");
            node_.advertise_storage(local_.used(), local_.limit());
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
              if (!control_.put(id, data))
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
                      if (auto token = local_.reassert_durable(id))
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
                  node_.advertise_storage(local_.used(), local_.limit());
                  return {MessageType::ok, reply.take()};
              }
              try {
                  local_.durability_barrier({domain, required_generation, backend_instance},
                                                   DurabilityUrgency::batchable);
                  node_.advertise_storage(local_.used(), local_.limit());
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
                                           ? local_.has(id)
                                           : control_.has(id);
                  if (!present)
                      return error_reply("retention object is not durably present");
              }
              claims_.retain_batch(object_class, ids, dot);
              return {MessageType::ok, {}};
          });
    route(MessageType::delete_object,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              Reader reader(request.payload);
              ObjectId id{reader.fixed<32>()};
              reader.finish();
              if (claims_.retained(RetentionClass::data, id))
                  return error_reply("object has an active retention claim");
              auto resource = data_resources_.try_acquire(
                  DataWorkContext(frame_type, extent_size_), extent_size_);
              if (!resource)
                  return error_reply("DATA resource admission busy or stopping");
              (void)local_.remove(id);
              (void)cache_.remove(id);
              node_.advertise_storage(local_.used(), local_.limit());
              return {MessageType::ok, {}};
          });
}

} // namespace macha
