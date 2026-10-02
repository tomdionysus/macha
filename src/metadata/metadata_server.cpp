// SPDX-License-Identifier: GPL-3.0-or-later
#include "metadata/metadata_server.hpp"

#include "cluster/cluster.hpp"
#include "codec.hpp"
#include "log.hpp"
#include "storage/persistent_cache.hpp"
#include "supervised.hpp"

#include <algorithm>

namespace macha {
namespace {

RpcMessage metadata_identity_reply(const MetadataIdentity& identity) {
    Writer writer;
    writer.u64(identity.generation);
    writer.fixed(identity.hash.bytes);
    return {MessageType::metadata_identity_reply, writer.take()};
}

} // namespace

MetadataServer::MetadataServer(NodeRuntime& node, MetadataReplica& replica,
                               PersistentBlockCache& cache, MessageRoutes& routes,
                               size_t min_write_replicas,
                               std::chrono::milliseconds refresh_interval)
    : node_(node), replica_(replica), cache_(cache), routes_(routes),
      min_write_replicas_(min_write_replicas), refresh_interval_(refresh_interval) {
    // Identity-reset tombstones must be active before metadata exchange.
    try {
        const auto committed_snapshot = decode_snapshot(replica_.committed().payload);
        for (const auto& [_, reset] : committed_snapshot.identity_resets)
            node_.apply_identity_reset(reset);
    } catch (const std::exception& error) {
        Log::debug("cannot preload identity reset tombstones: " + std::string(error.what()));
    }
    bind_routes();
    refresher_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("metadata-refresh", stop, [this, stop] { refresh_loop(stop); });
    });
}

// Routes first, so no request reaches the replica while the refresher stops.
MetadataServer::~MetadataServer() {
    for (const auto type : bound_)
        routes_.unbind(type);
    if (refresher_.joinable()) {
        refresher_.request_stop();
        refresh_cv_.notify_all();
        refresher_.join();
    }
}

void MetadataServer::refresh_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(refresh_mutex_);
            refresh_cv_.wait_for(lock, stop, refresh_interval_, [] { return false; });
        }
        if (stop.stop_requested())
            return;
        node_.advertise_metadata_generation(replica_.generation());
    }
}

uint64_t MetadataServer::known_generation() const {
    const auto local = replica_.generation();
    const auto remote = node_.remote_metadata_generation();
    return local > remote ? local : remote;
}

void MetadataServer::route(MessageType type, MessageRoutes::Handler handler) {
    routes_.bind(type, std::move(handler));
    bound_.push_back(type);
}

bool MetadataServer::accept_commit(const MetadataAcceptance& acceptance) {
    // Accept only branches whose resulting cluster policy matches the
    // configured one. The certificate's own `required` may be stronger during
    // a safe transition (e.g. W=3 -> W=2), so it is not compared directly;
    // MetadataReplica validates it against the commit and parent policies.
    if (acceptance.required) {
        auto materialized = replica_.materialized(acceptance.hash);
        if (!materialized)
            return false;
        if (materialized->snapshot->metadata_write_replicas_required != min_write_replicas_)
            return false;
    }
    const auto before = replica_.committed();
    bool heads_changed = false;
    if (!replica_.accept_commit(acceptance, &heads_changed))
        return false;
    const auto after = replica_.committed();
    node_.advertise_metadata_generation(std::max(after.generation, acceptance.generation));
    // Decided under the replica lock: comparing copies taken around the call
    // would count a concurrent acceptance and announce a commit twice.
    if (!heads_changed)
        return true;
    if (after.hash != before.hash)
        cache_.remember_metadata(after);
    // A same-generation sibling may leave the preferred head unchanged, but
    // peers still need a wake-up so cache validation and reconciliation see
    // the new head set.
    node_.announce_metadata_generation(std::max(after.generation, acceptance.generation));
    return true;
}

std::vector<MetadataAcceptance> MetadataServer::heads() const {
    return replica_.accepted_head_certificates();
}

// Reads, history, commits and acceptance.
void MetadataServer::bind_routes() {
    route(MessageType::get_metadata,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              return {MessageType::metadata_reply,
                      encode_metadata_record(replica_.current())};
          });
    route(MessageType::get_committed_metadata,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              return {MessageType::metadata_reply,
                      encode_metadata_record(replica_.committed())};
          });
    route(MessageType::get_metadata_identity,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              return metadata_identity_reply(replica_.committed_identity());
          });
    route(MessageType::get_metadata_history_entry,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              Reader reader(request.payload);
              Hash256 hash;
              hash.bytes = reader.fixed<32>();
              reader.finish();
              auto entry = replica_.history_entry(hash);
              if (!entry)
                  return error_reply("metadata history entry unavailable");
              return {MessageType::metadata_history_entry_reply,
                      encode_metadata_history_entry(*entry)};
          });
    route(MessageType::get_metadata_history_record,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              // Repair of a peer's unreconstructable accepted head: serve the
              // record as a full body, whatever frame shape is stored (see
              // MetadataManager::repair_unreconstructable_heads()).
              Reader reader(request.payload);
              Hash256 hash;
              hash.bytes = reader.fixed<32>();
              reader.finish();
              auto entry = replica_.full_history_record(hash);
              if (!entry)
                  return error_reply("metadata history record unavailable");
              return {MessageType::metadata_history_entry_reply,
                      encode_metadata_history_entry(*entry)};
          });
    route(MessageType::has_metadata_history_entry,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              Reader reader(request.payload);
              Hash256 hash;
              hash.bytes = reader.fixed<32>();
              reader.finish();
              Writer writer;
              writer.u8(replica_.history_contains(hash));
              return {MessageType::bool_reply, writer.take()};
          });
    route(MessageType::put_metadata_history_entry,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              auto entry = decode_metadata_history_entry(request.payload);
              Writer writer;
              writer.u8(replica_.import_history(entry));
              return {MessageType::bool_reply, writer.take()};
          });
    route(MessageType::get_metadata_heads,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
             [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              return {MessageType::metadata_heads_reply,
                      encode_metadata_acceptance_set(heads())};
          });
    route(MessageType::put_metadata_commit,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
             [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              auto entry = decode_metadata_history_entry(request.payload);
              Writer writer;
              writer.u8(replica_.import_history(entry));
              return {MessageType::bool_reply, writer.take()};
          });
    route(MessageType::accept_metadata_commit,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              auto acceptance = decode_metadata_acceptance(request.payload);
              Writer writer;
              writer.u8(accept_commit(acceptance));
              return {MessageType::bool_reply, writer.take()};
          });
    route(MessageType::propose_history_floor,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              auto proposal = decode_history_checkpoint_proof(request.payload);
              Writer writer;
              writer.u8(replica_.record_checkpoint_ack(proposal));
              return {MessageType::bool_reply, writer.take()};
          });
    route(MessageType::commit_history_floor,
          [this]([[maybe_unused]] const NodeInfo& peer, [[maybe_unused]] FrameType frame_type,
                 [[maybe_unused]] const RpcMessage& request) -> RpcMessage {
              auto commit = decode_history_checkpoint_proof(request.payload);
              Writer writer;
              writer.u8(replica_.record_checkpoint_commit(commit.floor_hash, commit.epoch));
              return {MessageType::bool_reply, writer.take()};
          });
}

} // namespace macha
