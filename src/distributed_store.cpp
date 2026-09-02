// SPDX-License-Identifier: GPL-3.0-or-later
#include "distributed_store.hpp"
#include "codec.hpp"
#include "log.hpp"
#include "placement.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <tuple>

namespace macha {
namespace {
bool read_aborted(Clock::time_point deadline, const std::atomic_bool* cancelled,
                  const std::function<bool()>& abort = {}) {
    return (cancelled && cancelled->load(std::memory_order_relaxed)) ||
           (abort && abort()) ||
           (deadline != Clock::time_point{} && Clock::now() >= deadline);
}
} // namespace

void DistributedStore::note_foreground(uint64_t bytes) {
    n_.note_activity(FrameType::foreground, bytes);
}

void DistributedStore::note_network(uint64_t bytes, Clock::duration duration) {
    auto seconds = std::chrono::duration<double>(duration).count();
    if (!bytes || seconds <= 0.0)
        return;
    double sample = static_cast<double>(bytes) / seconds;
    double old = network_bps_.load(std::memory_order_relaxed);
    double next = old > 0.0 ? old * 0.80 + sample * 0.20 : sample;
    network_bps_.store(next, std::memory_order_relaxed);
}

std::chrono::milliseconds DistributedStore::foreground_idle_for() const {
    return n_.activity_idle_for(FrameType::foreground);
}

std::vector<NodeInfo> DistributedStore::ranked(const ObjectId& id) const {
    auto active = n_.membership().active();
    return capacity_placement_nodes(id.bytes, active, n_.config().replication);
}

std::vector<NodeInfo> DistributedStore::owners(const ObjectId& id) const {
    auto nodes = ranked(id);
    if (nodes.size() > n_.config().replication)
        nodes.resize(n_.config().replication);
    return nodes;
}

bool DistributedStore::should_own(const ObjectId& id) const {
    auto preferred = owners(id);
    return std::any_of(preferred.begin(), preferred.end(),
                       [&](const NodeInfo& node) { return node.id == n_.node_id(); });
}

ObjectId DistributedStore::put(std::span<const uint8_t> data, std::atomic_bool* cancelled) {
    return put(data, FrameType::loader, cancelled);
}

ObjectId DistributedStore::put(std::span<const uint8_t> data, FrameType frame_type,
                               std::atomic_bool* cancelled) {
    auto started = Clock::now();
    auto id = object_id(data);
    if (!put_impl(id, data, frame_type, cancelled, nullptr)) {
        if (cancelled && cancelled->load(std::memory_order_relaxed))
            throw std::runtime_error("object replication cancelled");
        throw std::runtime_error("object replication quorum unavailable");
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    if (Log::enabled(LogLevel::all))
        Log::trace("DIAG object-put id=" + to_string(id) +
               " bytes=" + std::to_string(data.size()) +
               " ms=" + std::to_string(elapsed.count()));
    return id;
}

bool DistributedStore::put(const ObjectId& id, std::span<const uint8_t> data,
                           std::atomic_bool* cancelled) {
    return put(id, data, FrameType::loader, cancelled);
}

bool DistributedStore::put(const ObjectId& id, std::span<const uint8_t> data,
                           FrameType frame_type, std::atomic_bool* cancelled) {
    return put_impl(id, data, frame_type, cancelled, nullptr);
}

ObjectId DistributedStore::put_deferred(std::span<const uint8_t> data, DurabilityBatch& batch,
                                        std::atomic_bool* cancelled) {
    return put_deferred(data, batch, FrameType::loader, cancelled);
}

ObjectId DistributedStore::put_deferred(std::span<const uint8_t> data, DurabilityBatch& batch,
                                        FrameType frame_type, std::atomic_bool* cancelled) {
    auto id = object_id(data);
    if (!put_impl(id, data, frame_type, cancelled, &batch)) {
        if (cancelled && cancelled->load(std::memory_order_relaxed))
            throw std::runtime_error("object replication cancelled");
        throw std::runtime_error("object replication quorum unavailable");
    }
    return id;
}

bool DistributedStore::put_deferred(const ObjectId& id, std::span<const uint8_t> data,
                                    DurabilityBatch& batch, std::atomic_bool* cancelled) {
    return put_deferred(id, data, batch, FrameType::loader, cancelled);
}

bool DistributedStore::put_deferred(const ObjectId& id, std::span<const uint8_t> data,
                                    DurabilityBatch& batch, FrameType frame_type,
                                    std::atomic_bool* cancelled) {
    return put_impl(id, data, frame_type, cancelled, &batch);
}

bool DistributedStore::put_impl(const ObjectId& id, std::span<const uint8_t> data,
                                FrameType frame_type, std::atomic_bool* cancelled,
                                DurabilityBatch* batch) {
    if (object_id(data) != id)
        throw std::runtime_error("object hash mismatch");
    if (data.size() > n_.config().extent_size)
        throw std::runtime_error("DATA object exceeds configured extent size");
    n_.note_activity(frame_type, data.size());
    const auto operation_started = Clock::now();

    auto nodes = ranked(id);
    if (nodes.empty())
        return false;
    const size_t target = std::min(n_.config().replication, nodes.size());
    const size_t floor = n_.config().min_write_replicas;
    // 0.18 makes foreground durability explicit: min_write_replicas is the
    // publication contract. Desired replication is convergence work performed
    // by repair, not a latency/quorum rule that changes with current membership.
    const size_t need = floor;
    if (nodes.size() < floor)
        return false;

    Writer writer;
    writer.fixed(id.bytes);
    writer.bytes(data);
    const auto payload = writer.take();

    struct PendingPut {
        NodeInfo owner;
        std::optional<AsyncRpc> rpc;
        std::optional<DataResourceArbiter::Lease> resource;
        Clock::time_point started{};
        bool done{};
        bool spilled{};
    };

    std::vector<PendingPut> pending;
    pending.reserve(nodes.size());
    size_t success = 0;
    std::vector<DurableReplica> successful_replicas;
    size_t replacement_needed = 0;
    const size_t initial = std::min(floor, nodes.size());
    size_t next_fallback = initial;
    std::chrono::milliseconds local_store_time{};
    std::chrono::milliseconds remote_max_time{};

    auto finish = [&](bool ok, size_t required) {
        if (ok && batch) {
            std::sort(successful_replicas.begin(), successful_replicas.end(),
                      [](const DurableReplica& a, const DurableReplica& b) {
                          if (a.id != b.id)
                              return a.id < b.id;
                          if (a.epoch != b.epoch)
                              return a.epoch < b.epoch;
                          if (a.domain != b.domain)
                              return a.domain < b.domain;
                          if (a.backend_instance != b.backend_instance)
                              return a.backend_instance < b.backend_instance;
                          return a.generation < b.generation;
                      });
            std::vector<DurableReplica> coalesced;
            for (const auto& replica : successful_replicas) {
                if (!coalesced.empty() && coalesced.back().id == replica.id &&
                    coalesced.back().epoch == replica.epoch &&
                    coalesced.back().domain == replica.domain &&
                    coalesced.back().backend_instance == replica.backend_instance) {
                    coalesced.back().generation =
                        std::max(coalesced.back().generation, replica.generation);
                } else {
                    coalesced.push_back(replica);
                }
            }
            successful_replicas = std::move(coalesced);
            batch->requirements.push_back({id, required, successful_replicas});
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - operation_started);
        if (elapsed >= std::chrono::milliseconds(500) && Log::enabled(LogLevel::debug)) {
            Log::debug("object write quorum id=" + to_string(id) +
                       " bytes=" + std::to_string(data.size()) +
                       " replicas=" + std::to_string(target) +
                       " required=" + std::to_string(need) +
                       " minimum=" + std::to_string(floor) +
                       " success=" + std::to_string(success) +
                       " local_ms=" + std::to_string(local_store_time.count()) +
                       " remote_max_ms=" + std::to_string(remote_max_time.count()) +
                       " total_ms=" + std::to_string(elapsed.count()) +
                       " result=" + std::to_string(ok ? 1 : 0));
        }
        return ok;
    };

    auto launch = [&](const NodeInfo& owner) {
        auto resource = n_.data_resources().acquire(
            DataWorkContext(frame_type, data.size(), {}, cancelled), data.size());
        if (!resource) {
            ++replacement_needed;
            return;
        }
        if (owner.id == n_.node_id()) {
            const auto started = Clock::now();
            if (!n_.local_store().has(id))
                n_.notify_storage_mutation();
            if (batch) {
                if (const auto token = n_.local_store().put_deferred(id, data)) {
                    ++success;
                    successful_replicas.push_back(
                        {owner.id, n_.durability_epoch(), token->domain, token->generation,
                         token->backend_instance});
                } else {
                    ++replacement_needed;
                }
            } else if (n_.local_store().put(id, data)) {
                ++success;
            } else {
                ++replacement_needed;
            }
            local_store_time +=
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
            return;
        }
        try {
            PendingPut item;
            item.owner = owner;
            item.started = Clock::now();
            item.resource.emplace(std::move(*resource));
            const auto type = batch ? MessageType::put_object_deferred : MessageType::put_object;
            item.rpc.emplace(n_.call_async(owner, type, payload, frame_type));
            pending.push_back(std::move(item));
        } catch (...) {
            ++replacement_needed;
        }
    };

    for (size_t i = 0; i < initial; ++i)
        launch(nodes[i]);

    if (success >= need)
        return finish(true, need);

    while (true) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
            for (auto& item : pending) {
                if (!item.done && item.rpc)
                    item.rpc->cancel();
            }
            return finish(false, need);
        }
        bool progressed = false;
        for (auto& item : pending) {
            if (item.done || !item.rpc)
                continue;
            if (item.rpc->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                if (!item.spilled && item.rpc->idle_for() >= n_.config().write_stall) {
                    item.spilled = true;
                    ++replacement_needed;
                    progressed = true;
                    if (Log::enabled(LogLevel::debug)) {
                        Log::debug("object write stalled; spilling id=" + to_string(id) +
                                   " peer=" + to_string(item.owner.id).substr(0, 12) +
                                   " no_progress_ms=" +
                                   std::to_string(item.rpc->idle_for().count()));
                    }
                }
                continue;
            }

            item.done = true;
            progressed = true;
            bool ok = false;
            std::optional<NodeId> durability_epoch;
            uint64_t durability_domain = 0;
            uint64_t durability_generation = 0;
            uint64_t durability_backend_instance = 0;
            try {
                auto reply = item.rpc->get();
                ok = reply.message.type == MessageType::ok;
                if (ok && batch) {
                    Reader reader(reply.message.payload);
                    NodeId epoch{reader.fixed<16>()};
                    durability_domain = reader.u64();
                    durability_generation = reader.u64();
                    durability_backend_instance = reader.u64();
                    reader.finish();
                    durability_epoch = epoch;
                }
            } catch (...) {
                ok = false;
            }
            const auto remote_elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - item.started);
            remote_max_time = std::max(remote_max_time, remote_elapsed);
            if (ok) {
                ++success;
                if (batch)
                    successful_replicas.push_back(
                        {item.owner.id, *durability_epoch, durability_domain,
                         durability_generation, durability_backend_instance});
                note_network(data.size(), Clock::now() - item.started);
            } else if (!item.spilled) {
                ++replacement_needed;
            }

            if (success >= need)
                return finish(true, need);
        }

        while (replacement_needed && next_fallback < nodes.size() && success < need) {
            --replacement_needed;
            launch(nodes[next_fallback++]);
            progressed = true;
            if (success >= need)
                return finish(true, need);
        }

        size_t unfinished = 0;
        for (const auto& item : pending)
            if (!item.done) ++unfinished;

        if (success + unfinished + (nodes.size() - next_fallback) < floor)
            break;

        if (!progressed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return finish(success >= floor, floor);
}

bool DistributedStore::durability_barrier(const DurabilityBatch& batch, FrameType frame_type) {
    if (batch.empty())
        return true;

    // Coalesce by exact process-epoch-bound physical placement. Requests which
    // share a filesystem domain still reach the same DurabilityDomain and are
    // group-committed there; the backend incarnation remains part of the token
    // so a reopened/replaced backend cannot satisfy an old placement.
    using ReplicaKey = std::tuple<NodeId, NodeId, uint64_t, uint64_t>;
    std::map<ReplicaKey, uint64_t> wanted_map;
    for (const auto& requirement : batch.requirements) {
        for (const auto& replica : requirement.replicas) {
            auto& generation = wanted_map[
                {replica.id, replica.epoch, replica.domain, replica.backend_instance}];
            generation = std::max(generation, replica.generation);
        }
    }
    std::vector<DurableReplica> wanted;
    wanted.reserve(wanted_map.size());
    for (const auto& [key, generation] : wanted_map) {
        wanted.push_back({std::get<0>(key), std::get<1>(key), std::get<2>(key), generation,
                          std::get<3>(key)});
    }

    std::map<NodeId, NodeInfo> peers;
    for (const auto& peer : n_.membership().all())
        peers.emplace(peer.id, peer);

    struct PendingBarrier {
        DurableReplica replica{};
        std::optional<AsyncRpc> rpc;
    };
    std::vector<PendingBarrier> pending;

    // Launch remote waits before blocking on the local domain. This aligns the
    // batch windows across replicas, so a publication does not unnecessarily
    // serialize one physical durability cut per node.
    for (const auto& replica : wanted) {
        if (replica.id == n_.node_id())
            continue;
        auto found = peers.find(replica.id);
        if (found == peers.end())
            continue;
        try {
            Writer payload;
            payload.fixed(replica.epoch.bytes);
            payload.u64(replica.domain);
            payload.u64(replica.generation);
            payload.u64(replica.backend_instance);
            PendingBarrier item;
            item.replica = replica;
            item.rpc.emplace(n_.call_async(found->second, MessageType::object_durability_barrier,
                                           payload.data(), frame_type));
            pending.push_back(std::move(item));
        } catch (...) {
        }
    }

    std::map<ReplicaKey, uint64_t> durable;
    for (const auto& replica : wanted) {
        if (replica.id != n_.node_id() || replica.epoch != n_.durability_epoch())
            continue;
        try {
            n_.local_store().durability_barrier(
                {replica.domain, replica.generation, replica.backend_instance},
                DurabilityUrgency::batchable);
            durable[{replica.id, replica.epoch, replica.domain, replica.backend_instance}] =
                replica.generation;
        } catch (const std::exception& error) {
            Log::warn("local storage durability barrier failed: " + std::string(error.what()));
        }
    }

    for (auto& item : pending) {
        try {
            if (item.rpc && item.rpc->get().message.type == MessageType::ok) {
                durable[{item.replica.id, item.replica.epoch, item.replica.domain,
                         item.replica.backend_instance}] = item.replica.generation;
            }
        } catch (...) {
        }
    }

    for (const auto& requirement : batch.requirements) {
        size_t count = 0;
        for (const auto& replica : requirement.replicas) {
            const auto found = durable.find(
                {replica.id, replica.epoch, replica.domain, replica.backend_instance});
            if (found != durable.end() && found->second >= replica.generation)
                ++count;
        }
        if (count < requirement.required) {
            Log::debug("object durability quorum unavailable id=" + to_string(requirement.id) +
                       " required=" + std::to_string(requirement.required) +
                       " durable=" + std::to_string(count));
            return false;
        }
    }
    return true;
}


RpcReply DistributedStore::bounded_control_call(const NodeInfo& target, MessageType type,
                                                std::span<const uint8_t> payload) {
    auto request = n_.call_async(target, type, payload, FrameType::control);
    const auto deadline = std::max(n_.config().dead_after, n_.config().connect_timeout);
    if (request.wait_for(deadline) != std::future_status::ready) {
        // Fast health probes can continue to succeed while an ordinary control
        // worker is wedged. Abort this exact route so a retention-before-commit
        // barrier is bounded and its pending promise/queue ownership is released.
        request.abort();
        throw std::runtime_error("control RPC deadline exceeded peer=" + target.host +
                                 " message=" + std::to_string(static_cast<unsigned>(type)));
    }
    return request.get();
}

bool DistributedStore::retain_on(const NodeInfo& target, RetentionClass object_class,
                                 const std::vector<ObjectId>& input,
                                 const RetentionDot& dot) {
    if (input.empty())
        return true;
    auto ids = input;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (target.id == n_.node_id()) {
        for (const auto& id : ids) {
            const bool present = object_class == RetentionClass::data
                                     ? n_.local_store().valid(id)
                                     : n_.control_store().valid(id);
            if (!present)
                return false;
        }
        n_.retention_store().retain_batch(object_class, ids, dot);
        return true;
    }

    Writer writer;
    writer.u8(static_cast<uint8_t>(object_class));
    writer.fixed(dot.origin.bytes);
    writer.u64(dot.sequence);
    writer.u32(static_cast<uint32_t>(ids.size()));
    for (const auto& id : ids)
        writer.fixed(id.bytes);
    try {
        return bounded_control_call(target, MessageType::retain_objects, writer.data())
                   .message.type == MessageType::ok;
    } catch (const std::exception& error) {
        Log::debug("retention claim peer=" + target.host + " error=" + error.what());
        return false;
    }
}

bool DistributedStore::retain_data(const std::vector<ObjectId>& input,
                                   const RetentionDot& dot) {
    auto ids = input;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids.empty())
        return true;

    const size_t floor = n_.config().min_write_replicas;
    if (!floor)
        return false;

    // Plan claims first, then persist one RetainBatch per selected node. This
    // keeps metadata-only touches of multi-extent files from degenerating into
    // one fsync/RPC per object while preserving a per-object DATA durability
    // floor. Fallback single-object claims below handle a node disappearing
    // between planning and batch persistence.
    std::map<NodeId, NodeInfo> node_info;
    std::map<NodeId, std::vector<ObjectId>> batches;
    std::map<ObjectId, std::vector<NodeInfo>> candidates_by_object;

    for (const auto& id : ids) {
        auto candidates = ranked(id);
        candidates_by_object.emplace(id, candidates);
        std::vector<NodeInfo> selected;
        selected.reserve(floor);
        for (const auto& candidate : candidates) {
            if (selected.size() >= floor)
                break;
            bool present = false;
            try {
                present = has_on(candidate, id);
            } catch (...) {
                present = false;
            }
            if (present)
                selected.push_back(candidate);
        }

        if (selected.size() < floor) {
            // A metadata-only mutation may be the first operation on this object
            // after old placement disappeared. Re-establish the ordinary DATA
            // durability floor before creating the new causal claim.
            auto data = get(id, 0, FrameType::speculative);
            if (!data || !put(id, *data))
                return false;
            candidates = ranked(id);
            candidates_by_object[id] = candidates;
            selected.clear();
            for (const auto& candidate : candidates) {
                if (selected.size() >= floor)
                    break;
                bool present = false;
                try {
                    present = has_on(candidate, id);
                } catch (...) {
                    present = false;
                }
                if (present)
                    selected.push_back(candidate);
            }
        }
        if (selected.size() < floor) {
            Log::debug("DATA retention placement unavailable id=" + to_string(id) +
                       " required=" + std::to_string(floor) +
                       " present=" + std::to_string(selected.size()));
            return false;
        }
        for (const auto& candidate : selected) {
            node_info[candidate.id] = candidate;
            batches[candidate.id].push_back(id);
        }
    }

    std::map<ObjectId, std::set<NodeId>> claimed;
    for (auto& [node_id, batch] : batches) {
        auto found = node_info.find(node_id);
        if (found == node_info.end())
            continue;
        if (!retain_on(found->second, RetentionClass::data, batch, dot))
            continue;
        for (const auto& id : batch)
            claimed[id].insert(node_id);
    }

    for (const auto& id : ids) {
        auto& successful = claimed[id];
        if (successful.size() >= floor)
            continue;
        const auto candidates = candidates_by_object.find(id);
        if (candidates == candidates_by_object.end())
            return false;
        for (const auto& candidate : candidates->second) {
            if (successful.size() >= floor)
                break;
            if (successful.contains(candidate.id))
                continue;
            bool present = false;
            try {
                present = has_on(candidate, id);
            } catch (...) {
                present = false;
            }
            if (present && retain_on(candidate, RetentionClass::data, {id}, dot))
                successful.insert(candidate.id);
        }
        if (successful.size() < floor) {
            Log::debug("DATA retention floor unavailable id=" + to_string(id) +
                       " required=" + std::to_string(floor) +
                       " retained=" + std::to_string(successful.size()));
            return false;
        }
    }
    return true;
}

bool DistributedStore::retain_control(const std::vector<ObjectId>& input,
                                      const RetentionDot& dot, size_t required) {
    auto ids = input;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids.empty())
        return true;
    if (!required)
        return false;

    auto active = n_.membership().active();
    active.erase(std::remove_if(active.begin(), active.end(), [&](const NodeInfo& peer) {
        return peer.metadata_write_replicas_required != required;
    }), active.end());
    if (active.size() < required)
        return false;
    std::stable_sort(active.begin(), active.end(), [&](const NodeInfo& a, const NodeInfo& b) {
        return a.id == n_.node_id() && b.id != n_.node_id();
    });

    std::vector<std::pair<ObjectId, Bytes>> graph;
    graph.reserve(ids.size());
    for (const auto& id : ids) {
        if (!ensure_control_local(id))
            return false;
        auto bytes = n_.control_store().get(id);
        if (!bytes)
            return false;
        graph.emplace_back(id, std::move(*bytes));
    }

    auto put_control_on = [&](const NodeInfo& target, const ObjectId& id,
                              std::span<const uint8_t> bytes) {
        if (target.id == n_.node_id())
            return n_.control_store().put(id, bytes);
        Writer writer;
        writer.fixed(id.bytes);
        writer.bytes(bytes);
        try {
            auto started = Clock::now();
            const auto reply = n_.call(target, MessageType::put_control_object,
                                       writer.data(), FrameType::control);
            const bool ok = reply.message.type == MessageType::ok;
            if (ok)
                note_network(bytes.size(), Clock::now() - started);
            return ok;
        } catch (const std::exception& error) {
            Log::debug("CONTROL retention object store peer=" + target.host +
                       " error=" + error.what());
            return false;
        }
    };

    // Critical-path CONTROL publication scales with the configured metadata
    // write floor, not cluster membership. Background control repair may later
    // fan the immutable graph out to every node.
    size_t retained_count = 0;
    for (const auto& candidate : active) {
        bool graph_present = true;
        for (const auto& [id, bytes] : graph) {
            if (!put_control_on(candidate, id, bytes)) {
                graph_present = false;
                break;
            }
        }
        if (!graph_present)
            continue;
        if (retain_on(candidate, RetentionClass::control, ids, dot))
            ++retained_count;
        if (retained_count >= required)
            break;
    }
    if (retained_count < required) {
        Log::debug("CONTROL retention floor unavailable objects=" + std::to_string(ids.size()) +
                   " required=" + std::to_string(required) +
                   " retained=" + std::to_string(retained_count));
        return false;
    }
    return true;
}

bool DistributedStore::put_on(const NodeInfo& target, const ObjectId& id,
                              std::span<const uint8_t> data, bool foreground) {
    const auto frame_type = foreground ? FrameType::foreground : FrameType::speculative;
    auto resource = n_.data_resources().acquire(
        DataWorkContext(frame_type, data.size()), data.size());
    if (!resource)
        return false;
    if (target.id == n_.node_id()) {
        if (!n_.local_store().has(id))
            n_.notify_storage_mutation();
        return n_.local_store().put(id, data);
    }
    Writer writer;
    writer.fixed(id.bytes);
    writer.bytes(data);
    auto started = Clock::now();
    bool ok = n_.call(target, MessageType::put_object, writer.data(), frame_type).message.type ==
              MessageType::ok;
    if (ok)
        note_network(data.size(), Clock::now() - started);
    if (foreground)
        note_foreground(data.size());
    return ok;
}

std::optional<Bytes> DistributedStore::get_from(const NodeInfo& target, const ObjectId& id,
                                                FrameType frame_type,
                                                const std::shared_ptr<SharedFetch>& shared,
                                                Clock::time_point deadline,
                                                std::atomic_bool* cancelled,
                                                const std::function<bool()>& abort) {
    try {
        auto resource = n_.data_resources().acquire(
            DataWorkContext(frame_type, n_.config().extent_size, deadline, cancelled),
            n_.config().extent_size);
        if (!resource)
            return {};
        if (target.id == n_.node_id())
            return n_.local_store().get(id);

        Writer writer;
        writer.fixed(id.bytes);
        auto started = Clock::now();
        auto rpc = n_.call_async(target, MessageType::get_object, writer.data(), frame_type);
        if (shared) {
            FrameType effective;
            {
                std::lock_guard lock(shared->mutex);
                shared->promote_network = rpc.promotion_callback();
                effective = shared->frame_type;
            }
            if (frame_type_priority(effective) < frame_type_priority(frame_type))
                rpc.promote(effective);
        }
        while (rpc.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready) {
            if (read_aborted(deadline, cancelled, abort)) {
                // A shared fetch may have acquired other waiters after this
                // caller became its leader.  Cancellation can abort the wire
                // RPC only while nobody else depends on it; otherwise finish
                // the shared transfer and let the cancelled caller discard it.
                const bool may_cancel = !shared ||
                    shared->waiters.load(std::memory_order_relaxed) == 0;
                if (!may_cancel)
                    continue;
                rpc.cancel();
                if (shared) {
                    std::lock_guard lock(shared->mutex);
                    shared->promote_network = {};
                }
                return {};
            }
        }
        auto reply = rpc.get();
        if (shared) {
            std::lock_guard lock(shared->mutex);
            shared->promote_network = {};
        }
        if (reply.message.type != MessageType::object_reply)
            return {};
        Reader reader(reply.message.payload);
        ObjectId returned{reader.fixed<32>()};
        auto data = reader.bytes(128 * 1024 * 1024);
        reader.finish();
        if (returned != id || object_id(data) != id)
            throw std::runtime_error("remote integrity failure");
        note_network(data.size(), Clock::now() - started);
        return data;
    } catch (const std::exception& e) {
        if (shared) {
            std::lock_guard lock(shared->mutex);
            shared->promote_network = {};
        }
        Log::debug("object read: " + std::string(e.what()));
        return {};
    }
}

std::optional<Bytes> DistributedStore::get_remote(const ObjectId& id, size_t stripe,
                                                  FrameType frame_type, bool foreground,
                                                  bool opportunistic_persist,
                                                  Clock::time_point deadline,
                                                  std::atomic_bool* cancelled,
                                                  const std::function<bool()>& abort) {
    // A hard wall-clock deadline belongs to one caller, not to an ObjectId-wide
    // shared fetch. Probe reads therefore use a private transfer so expiry can
    // abort the underlying RPC. Cancellation-only playback reads retain normal
    // shared-fetch deduplication/promotion; a stopped caller may abandon its wait
    // but does not cancel an ObjectId transfer other readers may still need.
    if (deadline != Clock::time_point{}) {
        auto try_private = [&](std::vector<NodeInfo> candidates) -> std::optional<Bytes> {
            std::erase_if(candidates, [&](const NodeInfo& node) { return node.id == n_.node_id(); });
            size_t attempt = 0;
            const auto work = foreground ? ReplicaWorkClass::foreground
                                         : ReplicaWorkClass::speculative;
            while (!candidates.empty() && !read_aborted(deadline, cancelled, abort)) {
                auto ordered = replica_selector_.order(candidates, stripe + attempt, work);
                if (ordered.empty()) break;
                const auto target = ordered.front();
                auto found = std::find_if(candidates.begin(), candidates.end(), [&](const NodeInfo& node) {
                    return node.id == target.id;
                });
                if (found != candidates.end()) candidates.erase(found);

                auto started = Clock::now();
                replica_selector_.started(target, work);
                auto data = get_from(target, id, frame_type, nullptr, deadline, cancelled, abort);
                replica_selector_.finished(target, work, data ? data->size() : 0,
                                           Clock::now() - started, data.has_value());
                if (data) return data;
                ++attempt;
            }
            return {};
        };

        auto preferred = owners(id);
        std::set<NodeId> preferred_ids;
        for (const auto& node : preferred) preferred_ids.insert(node.id);
        auto data = try_private(std::move(preferred));
        if (!data && !read_aborted(deadline, cancelled, abort)) {
            auto fallback = ranked(id);
            std::erase_if(fallback, [&](const NodeInfo& node) {
                return preferred_ids.contains(node.id);
            });
            data = try_private(std::move(fallback));
        }
        if (data) {
            if (foreground) note_foreground(data->size());
            if (opportunistic_persist) n_.enqueue_fetched(id, *data, should_own(id));
        }
        return data;
    }

    std::shared_ptr<SharedFetch> shared;
    bool leader = false;
    {
        std::lock_guard lock(fetch_mutex_);
        if (auto it = fetches_.find(id); it != fetches_.end()) {
            shared = it->second.lock();
            if (shared)
                shared->waiters.fetch_add(1, std::memory_order_relaxed);
        }
        if (!shared) {
            shared = std::make_shared<SharedFetch>();
            shared->foreground = foreground;
            shared->frame_type = frame_type;
            shared->opportunistic_persist = opportunistic_persist;
            fetches_[id] = shared;
            leader = true;
        }
    }

    if (!leader) {
        std::function<void(FrameType)> promote_network;
        std::unique_lock lock(shared->mutex);
        if (frame_type_priority(frame_type) < frame_type_priority(shared->frame_type)) {
            shared->frame_type = frame_type;
            promote_network = shared->promote_network;
        }
        if (foreground && !shared->foreground) {
            shared->foreground = true;
            if (shared->active_peer &&
                shared->active_class == ReplicaWorkClass::speculative) {
                replica_selector_.promoted(*shared->active_peer);
                shared->active_class = ReplicaWorkClass::foreground;
            }
        }
        if (opportunistic_persist)
            shared->opportunistic_persist = true;
        if (promote_network) {
            lock.unlock();
            promote_network(frame_type);
            lock.lock();
        }

        while (!shared->done && !read_aborted(deadline, cancelled, abort))
            shared->cv.wait_for(lock, std::chrono::milliseconds(25));
        shared->waiters.fetch_sub(1, std::memory_order_relaxed);
        if (!shared->done)
            return {};
        if (shared->result) {
            if (foreground && !shared->foreground_accounted) {
                note_foreground(shared->result->size());
                shared->foreground_accounted = true;
            }
            if (opportunistic_persist && !shared->persist_queued) {
                n_.enqueue_fetched(id, *shared->result, should_own(id));
                shared->persist_queued = true;
            }
        }
        return shared->result;
    }

    auto finish = [&](std::optional<Bytes> result) {
        bool account_foreground = false;
        bool queue_persist = false;
        {
            // Keep the fetch-table lock until the shared result is published.
            // A caller that found this transfer before completion is therefore
            // guaranteed to become a waiter; a caller arriving afterwards can
            // start a new fetch only after this network transfer is complete.
            std::lock_guard fetch_lock(fetch_mutex_);
            std::lock_guard shared_lock(shared->mutex);
            const bool has_waiters = shared->waiters.load(std::memory_order_relaxed) != 0;
            if (has_waiters)
                shared->result = result;
            if (result) {
                if (shared->foreground && !shared->foreground_accounted) {
                    shared->foreground_accounted = true;
                    account_foreground = true;
                }
                if (shared->opportunistic_persist && !shared->persist_queued) {
                    shared->persist_queued = true;
                    queue_persist = true;
                }
            }
            shared->done = true;

            auto it = fetches_.find(id);
            if (it != fetches_.end() && it->second.lock() == shared)
                fetches_.erase(it);
        }

        if (result && account_foreground)
            note_foreground(result->size());
        if (result && queue_persist)
            n_.enqueue_fetched(id, *result, should_own(id));
        shared->cv.notify_all();
        return result;
    };

    auto try_candidates = [&](std::vector<NodeInfo> candidates) -> std::optional<Bytes> {
        std::erase_if(candidates, [&](const NodeInfo& node) { return node.id == n_.node_id(); });
        size_t attempt = 0;
        while (!candidates.empty() && !read_aborted(deadline, cancelled, abort)) {
            ReplicaWorkClass work;
            {
                std::lock_guard lock(shared->mutex);
                work = shared->foreground ? ReplicaWorkClass::foreground
                                          : ReplicaWorkClass::speculative;
            }

            auto ordered = replica_selector_.order(candidates, stripe + attempt, work);
            if (ordered.empty())
                break;
            const auto target = ordered.front();
            auto found = std::find_if(candidates.begin(), candidates.end(), [&](const NodeInfo& n) {
                return n.id == target.id;
            });
            if (found != candidates.end())
                candidates.erase(found);

            Clock::time_point started;
            {
                std::lock_guard lock(shared->mutex);
                shared->active_peer = target;
                shared->active_class = shared->foreground ? ReplicaWorkClass::foreground
                                                          : ReplicaWorkClass::speculative;
                replica_selector_.started(target, shared->active_class);
                started = Clock::now();
            }

            FrameType transfer_type;
            {
                std::lock_guard lock(shared->mutex);
                transfer_type = shared->frame_type;
            }
            // A shared transfer belongs to the ObjectId, not to whichever
            // caller happened to become its leader.  A cancelled leader may
            // abort the wire RPC while it has no other waiters; once another
            // reader has joined, the shared transfer is allowed to complete.
            auto data = get_from(target, id, transfer_type, shared, {}, cancelled, abort);
            {
                std::lock_guard lock(shared->mutex);
                replica_selector_.finished(target, shared->active_class,
                                           data ? data->size() : 0, Clock::now() - started,
                                           data.has_value());
                shared->active_peer.reset();
            }
            if (data)
                return data;
            ++attempt;
        }
        return {};
    };

    try {
        auto preferred = owners(id);
        std::set<NodeId> preferred_ids;
        for (const auto& node : preferred)
            preferred_ids.insert(node.id);
        if (auto data = try_candidates(std::move(preferred)))
            return finish(std::move(data));

        // Objects may deliberately live on fallback nodes when a preferred owner is
        // full, and may temporarily remain on old owners during membership changes.
        auto fallback = ranked(id);
        std::erase_if(fallback,
                      [&](const NodeInfo& node) { return preferred_ids.contains(node.id); });
        if (auto data = try_candidates(std::move(fallback)))
            return finish(std::move(data));
    } catch (const std::exception& error) {
        Log::debug("remote object scheduling " + to_string(id) + ": " + error.what());
    } catch (...) {
        Log::debug("remote object scheduling " + to_string(id) + ": unknown error");
    }
    return finish({});
}

std::optional<Bytes> DistributedStore::get(const ObjectId& id, size_t stripe, bool foreground,
                                           Clock::time_point deadline,
                                           std::atomic_bool* cancelled) {
    return get(id, stripe, foreground ? FrameType::foreground : FrameType::speculative,
               deadline, cancelled);
}

std::optional<Bytes> DistributedStore::get(const ObjectId& id, size_t stripe, FrameType frame_type,
                                           Clock::time_point deadline,
                                           std::atomic_bool* cancelled) {
    const bool foreground = frame_type == FrameType::foreground;
    const bool interactive = frame_type == FrameType::foreground || frame_type == FrameType::read_ahead;
    // Record foreground demand before touching local/cache/network storage.
    // Completion-time accounting alone is too late to suppress lower-priority
    // publication when the very first playback read is the one being delayed.
    if (foreground)
        note_foreground(0);
    auto started = Clock::now();
    auto log_playback_read = [&](std::string_view source, size_t bytes, bool ok) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        if (foreground && elapsed >= std::chrono::milliseconds(250) &&
            Log::enabled(LogLevel::debug)) {
            Log::debug("playback object read source=" + std::string(source) +
                       " stripe=" + std::to_string(stripe) +
                       " id=" + to_string(id) +
                       " result=" + std::to_string(ok ? 1 : 0) +
                       " bytes=" + std::to_string(bytes) +
                       " elapsed_ms=" + std::to_string(elapsed.count()));
        }
        return elapsed;
    };
    auto local_resource = n_.data_resources().acquire(
        DataWorkContext(frame_type, n_.config().extent_size, deadline, cancelled),
        n_.config().extent_size);
    if (!local_resource)
        return {};
    auto local_data = n_.local_store().get(id);
    local_resource.reset();
    if (auto data = std::move(local_data)) {
        if (interactive)
            n_.note_activity(frame_type, data->size());
        auto elapsed = log_playback_read("owned", data->size(), true);
        if (Log::enabled(LogLevel::all))
            Log::trace("DIAG object-get id=" + to_string(id) +
                   " source=owned bytes=" + std::to_string(data->size()) +
                   " ms=" + std::to_string(elapsed.count()));
        return data;
    }

    auto cache_resource = n_.data_resources().acquire(
        DataWorkContext(frame_type, n_.config().extent_size, deadline, cancelled),
        n_.config().extent_size);
    if (!cache_resource)
        return {};
    auto cache_data = n_.block_cache().get(id);
    cache_resource.reset();
    if (auto cached = std::move(cache_data)) {
        if (interactive)
            n_.note_activity(frame_type, cached->size());
        if (should_own(id))
            n_.enqueue_fetched(id, *cached, true);
        auto elapsed = log_playback_read("cache", cached->size(), true);
        if (Log::enabled(LogLevel::all))
            Log::trace("DIAG object-get id=" + to_string(id) +
                   " source=cache bytes=" + std::to_string(cached->size()) +
                   " ms=" + std::to_string(elapsed.count()));
        return cached;
    }

    if (read_aborted(deadline, cancelled))
        return {};
    auto data = get_remote(id, stripe, frame_type,
                           foreground, interactive, deadline, cancelled);
    if (data && frame_type == FrameType::read_ahead)
        n_.note_activity(frame_type, data->size());
    auto elapsed = log_playback_read("remote", data ? data->size() : 0, data.has_value());
    if (Log::enabled(LogLevel::all))
        Log::trace("DIAG object-get id=" + to_string(id) +
               " source=remote result=" + std::to_string(data ? 1 : 0) +
               " bytes=" + std::to_string(data ? data->size() : 0) +
               " ms=" + std::to_string(elapsed.count()));
    return data;
}

bool DistributedStore::has_on(const NodeInfo& target, const ObjectId& id) {
    if (target.id == n_.node_id())
        return n_.local_store().valid(id);
    Writer writer;
    writer.fixed(id.bytes);
    auto reply = bounded_control_call(target, MessageType::have_object, writer.data());
    if (reply.message.type != MessageType::bool_reply)
        return false;
    Reader reader(reply.message.payload);
    bool present = reader.u8() != 0;
    reader.finish();
    return present;
}

size_t DistributedStore::replicate_all(const ObjectId& id, std::span<const uint8_t> data,
                                       bool foreground) {
    size_t success = 0;
    for (const auto& target : n_.membership().active()) {
        try {
            if (put_on(target, id, data, foreground))
                ++success;
        } catch (const std::exception& e) {
            Log::debug("universal object write " + target.host + ": " + e.what());
        }
    }
    return success;
}

size_t DistributedStore::replicate_control(const ObjectId& id,
                                             std::span<const uint8_t> data) {
    size_t success = 0;
    Writer writer;
    writer.fixed(id.bytes);
    writer.bytes(data);
    const auto payload = writer.take();

    // Every active node is a metadata/control replica in 0.19. Publication
    // policy decides how many durable acknowledgements are required; there is
    // no privileged metadata replica subset.
    for (const auto& target : n_.membership().active()) {
        try {
            if (target.id == n_.node_id()) {
                if (n_.control_store().put(id, data))
                    ++success;
                continue;
            }

            auto started = Clock::now();
            auto reply = n_.call(target, MessageType::put_control_object, payload,
                                 FrameType::speculative);
            if (reply.message.type == MessageType::ok) {
                ++success;
                note_network(data.size(), Clock::now() - started);
            }
        } catch (const std::exception& e) {
            Log::debug("control object write " + target.host + ": " + e.what());
        }
    }
    return success;
}

bool DistributedStore::locally_available(const ObjectId& id) const {
    return n_.local_store().has(id) || n_.block_cache().has(id);
}

bool DistributedStore::cache_local(const ObjectId& id, std::span<const uint8_t> data) {
    if (!n_.block_cache().enabled())
        return false;
    try {
        return n_.block_cache().put(id, data);
    } catch (const std::exception& e) {
        Log::debug("cache write-through skipped object=" + to_string(id) + " error=" + e.what());
        return false;
    }
}

bool DistributedStore::hydration_available() const {
    return n_.block_cache().enabled();
}

bool DistributedStore::hydrate(const ObjectId& id, size_t stripe, FrameType frame_type) {
    if (n_.local_store().has(id) || n_.block_cache().has(id))
        return true;
    if (!n_.block_cache().enabled())
        return false;
    auto data = get_remote(id, stripe, frame_type, false, false);
    return data && n_.block_cache().put(id, *data);
}

bool DistributedStore::ensure_local(const ObjectId& id, bool foreground) {
    if (n_.local_store().valid(id))
        return true;
    if (auto cached = n_.block_cache().get(id)) {
        if (n_.local_store().put(id, *cached))
            return true;
    }
    auto data = get_remote(id, 0,
                           foreground ? FrameType::foreground : FrameType::speculative,
                           foreground, false);
    return data && n_.local_store().put(id, *data);
}

bool DistributedStore::ensure_control_local(const ObjectId& id) {
    if (n_.control_store().valid(id))
        return true;

    Writer writer;
    writer.fixed(id.bytes);
    const auto payload = writer.take();

    // Control objects are metadata-replica data rather than DHT DATA
    // replicas. Search every currently active peer and keep the transfer on the
    // CONTROL transport while using speculative worker priority so it cannot
    // block health/quorum traffic or require a DATA session to exist.
    for (const auto& target : n_.membership().active()) {
        if (target.id == n_.node_id())
            continue;
        try {
            auto started = Clock::now();
            auto reply = n_.call(target, MessageType::get_control_object, payload,
                                 FrameType::speculative);
            if (reply.message.type != MessageType::control_object_reply)
                continue;

            Reader reader(reply.message.payload);
            ObjectId returned{reader.fixed<32>()};
            auto data = reader.bytes(128 * 1024 * 1024);
            reader.finish();
            if (returned != id || object_id(data) != id) {
                Log::debug("control object read " + target.host + ": integrity failure");
                continue;
            }
            note_network(data.size(), Clock::now() - started);
            if (n_.control_store().put(id, data))
                return true;
        } catch (const std::exception& e) {
            Log::debug("control object read " + target.host + ": " + e.what());
        }
    }
    return false;
}

void DistributedStore::erase_all(const ObjectId& id) {
    Writer writer;
    writer.fixed(id.bytes);
    for (const auto& target : n_.membership().active()) {
        try {
            if (target.id == n_.node_id()) {
                if (!n_.retention_store().retained(RetentionClass::data, id))
                    (void)n_.local_store().remove(id);
                (void)n_.block_cache().remove(id);
            } else {
                (void)n_.call(target, MessageType::delete_object, writer.data());
            }
        } catch (const std::exception& e) {
            Log::debug("object delete " + target.host + ": " + e.what());
        }
    }
}

uint64_t DistributedStore::scrub_once(uint64_t byte_budget) {
    uint64_t checked = 0;
    while (!byte_budget || checked < byte_budget) {
        auto step = n_.local_store().scrub_step(byte_budget ? byte_budget - checked : 0, 256);
        checked += step.bytes;
        if (step.complete || step.yielded)
            break;
    }
    return checked;
}

uint64_t DistributedStore::repair_once(uint64_t byte_budget, const std::vector<ObjectId>* live,
                                       const std::vector<ObjectId>* universal) {
    return repair_step(byte_budget, 0, live, universal).bytes_transferred;
}

DistributedStore::RepairResult
DistributedStore::repair_step(uint64_t byte_budget, size_t operation_budget,
                              const std::vector<ObjectId>* live,
                              const std::vector<ObjectId>* universal,
                              const std::function<bool()>& should_yield,
                              uint64_t live_generation) {
    RepairResult result;
    result.complete = false;
    auto& transferred = result.bytes_transferred;
    size_t operations = 0;

    // Repair is deliberately cursor-based. The old implementation rebuilt a
    // complete vector of every locally stored object and copied the complete
    // live-object set on every scheduler slice, then usually examined only a
    // handful of objects before the RPC operation budget was exhausted. On a
    // media-sized store that made idle repair itself an O(store) hot loop.
    const bool generation_changed =
        live_generation ? repair_live_generation_ != live_generation
                        : repair_live_identity_ != live;
    if (generation_changed) {
        repair_push_cursor_ = {};
        repair_push_pending_.reset();
        repair_pull_after_.reset();
        repair_push_complete_ = false;
        repair_pull_complete_ = false;
    }
    repair_live_identity_ = live;
    repair_live_generation_ = live_generation;
    if (!live || live->empty())
        repair_pull_complete_ = true;

    auto reset_completed_pass = [&] {
        repair_push_cursor_ = {};
        repair_push_pending_.reset();
        repair_pull_after_.reset();
        repair_push_complete_ = false;
        repair_pull_complete_ = !live || live->empty();
    };

    auto yielded = [&] {
        if (should_yield && should_yield()) {
            result.complete = false;
            result.yielded = true;
            return true;
        }
        return false;
    };
    auto reserve_operation = [&] {
        if (operation_budget && operations >= operation_budget) {
            result.complete = false;
            return false;
        }
        ++operations;
        result.remote_operations = operations;
        return true;
    };

    auto maintenance_has_on = [&](const NodeInfo& target,
                                  const ObjectId& id) -> std::optional<bool> {
        if (target.id == n_.node_id())
            return n_.local_store().valid(id);
        if (!reserve_operation() || yielded())
            return std::nullopt;

        Writer writer;
        writer.fixed(id.bytes);
        try {
            auto rpc = n_.call_async(target, MessageType::have_object, writer.data(), FrameType::speculative);
            while (rpc.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready) {
                if (yielded()) {
                    rpc.cancel();
                    return std::nullopt;
                }
            }
            auto reply = rpc.get();
            if (reply.message.type != MessageType::bool_reply)
                return false;
            Reader reader(reply.message.payload);
            bool present = reader.u8() != 0;
            reader.finish();
            return present;
        } catch (...) {
            return false;
        }
    };

    auto maintenance_put_on = [&](const NodeInfo& target, const ObjectId& id,
                                  std::span<const uint8_t> data) -> std::optional<bool> {
        if (target.id == n_.node_id())
            return n_.local_store().put(id, data);
        if (!reserve_operation() || yielded())
            return std::nullopt;

        Writer writer;
        writer.fixed(id.bytes);
        writer.bytes(data);
        auto started = Clock::now();
        try {
            auto rpc = n_.call_async(target, MessageType::put_object, writer.data(),
                                     FrameType::speculative);
            while (rpc.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready) {
                if (yielded()) {
                    rpc.cancel();
                    return std::nullopt;
                }
            }
            bool ok = rpc.get().message.type == MessageType::ok;
            if (ok)
                note_network(data.size(), Clock::now() - started);
            return ok;
        } catch (...) {
            return false;
        }
    };

    // Limit cheap local/live-set examinations independently of remote RPCs. A
    // settled object may require no network operation at all; without a scan
    // budget a single maintenance tick could still walk millions of objects.
    const size_t scan_budget = operation_budget
                                   ? std::clamp<size_t>(operation_budget * 4, 16, 64)
                                   : std::numeric_limits<size_t>::max();
    size_t scanned_total = 0;

    // Push existing local replicas toward the current deterministic owner set.
    if (!repair_push_complete_) {
        while (scanned_total < scan_budget && !repair_push_complete_) {
            if (yielded())
                break;
            if (byte_budget && transferred >= byte_budget)
                break;
            if (operation_budget && operations >= operation_budget)
                break;

            ObjectId id;
            if (repair_push_pending_) {
                id = *repair_push_pending_;
            } else {
                bool pass_complete = false;
                auto next = n_.local_store().next_object(repair_push_cursor_, pass_complete);
                if (!next) {
                    if (pass_complete)
                        repair_push_complete_ = true;
                    break;
                }
                id = *next;
                repair_push_pending_ = id;
            }

            // Physical cursors can see stale/non-live local objects. Garbage is
            // handled separately; replica repair simply skips them.
            if (live && !std::binary_search(live->begin(), live->end(), id)) {
                repair_push_pending_.reset();
                ++scanned_total;
                ++result.push_examined;
                continue;
            }

            const bool everywhere = universal &&
                                    std::binary_search(universal->begin(), universal->end(), id);
            auto nodes = everywhere ? n_.membership().active() : ranked(id);
            if (nodes.empty()) {
                repair_push_pending_.reset();
                ++scanned_total;
                ++result.push_examined;
                continue;
            }
            const size_t target = everywhere ? nodes.size()
                                             : std::min(n_.config().replication, nodes.size());
            std::set<NodeId> keepers;
            std::optional<Bytes> source;
            bool retry = false;

            for (const auto& peer : nodes) {
                if (keepers.size() >= target)
                    break;
                auto present_result = maintenance_has_on(peer, id);
                if (!present_result) {
                    retry = true;
                    break;
                }
                bool present = *present_result;
                if (!present) {
                    if (!source) {
                        source = n_.local_store().get(id);
                        if (!source)
                            break;
                    }
                    if (byte_budget && transferred && transferred + source->size() > byte_budget) {
                        retry = true;
                        break;
                    }
                    auto put_result = maintenance_put_on(peer, id, *source);
                    if (!put_result) {
                        retry = true;
                        break;
                    }
                    present = *put_result;
                    if (present && peer.id != n_.node_id())
                        transferred += source->size();
                }
                if (present)
                    keepers.insert(peer.id);
            }

            if (retry)
                break;

            if (!everywhere && keepers.size() >= target && !keepers.contains(n_.node_id()) &&
                !n_.retention_store().retained(RetentionClass::data, id))
                n_.local_store().remove(id);

            repair_push_pending_.reset();
            ++scanned_total;
            ++result.push_examined;
        }
    }

    // Pull objects this node should own. Iterate the immutable ordered live set
    // directly using upper_bound() rather than copying N object IDs into a new
    // vector on every bounded repair slice.
    if (!repair_pull_complete_ && live && !live->empty() &&
        (!byte_budget || transferred < byte_budget)) {
        auto it = repair_pull_after_
                      ? std::upper_bound(live->begin(), live->end(), *repair_pull_after_)
                      : live->begin();
        while (it != live->end() && scanned_total < scan_budget) {
            if (yielded())
                break;
            if (byte_budget && transferred >= byte_budget)
                break;

            const ObjectId id = *it;
            const bool everywhere = universal &&
                                    std::binary_search(universal->begin(), universal->end(), id);
            if ((!everywhere && !should_own(id)) || n_.local_store().valid(id)) {
                repair_pull_after_ = id;
                ++it;
                ++scanned_total;
                ++result.pull_examined;
                continue;
            }

            // Playback may already have fetched this exact object into the
            // persistent non-DHT cache. Promote that copy locally before doing
            // any network I/O, so playback-assisted convergence never requires
            // a second download.
            if (auto cached = n_.block_cache().get(id)) {
                (void)n_.local_store().put(id, *cached);
                repair_pull_after_ = id;
                ++it;
                ++scanned_total;
                ++result.pull_examined;
                continue;
            }

            if (!reserve_operation())
                break;
            auto data = get_remote(id, 0, FrameType::speculative, false, false, {}, nullptr,
                                   should_yield);
            if (yielded())
                break;
            if (data && n_.local_store().put(id, *data))
                transferred += data->size();

            repair_pull_after_ = id;
            ++it;
            ++scanned_total;
            ++result.pull_examined;
        }
        if (it == live->end()) {
            repair_pull_complete_ = true;
            repair_pull_after_.reset();
        }
    }

    result.remote_operations = operations;
    if (repair_push_complete_ && repair_pull_complete_) {
        result.complete = true;
        reset_completed_pass();
    }
    return result;
}
} // namespace macha
