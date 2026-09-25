// SPDX-License-Identifier: GPL-3.0-or-later
#include "distributed_store.hpp"
#include "codec.hpp"
#include "log.hpp"
#include "placement.hpp"
#include "supervised.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <future>
#include <limits>
#include <set>
#include <tuple>

namespace macha {
namespace {
uint64_t elapsed_ms(Clock::time_point since) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count());
}

bool read_aborted(Clock::time_point deadline, const std::atomic_bool* cancelled,
                  const std::function<bool()>& abort = {}) {
    return (cancelled && cancelled->load(std::memory_order_relaxed)) ||
           (abort && abort()) ||
           (deadline != Clock::time_point{} && Clock::now() >= deadline);
}

MemoryClass object_memory_class(FrameType frame_type) {
    switch (frame_type) {
    case FrameType::control: return MemoryClass::control;
    case FrameType::foreground:
    case FrameType::read_ahead: return MemoryClass::viewer;
    case FrameType::loader: return MemoryClass::loader;
    case FrameType::speculative: return MemoryClass::speculative;
    }
    return MemoryClass::speculative;
}

DistributedStore::ObjectData take_object_reply_payload(RpcMessage message,
                                                       const ObjectId& expected) {
    if (message.type != MessageType::object_reply)
        throw std::runtime_error("remote object reply has the wrong message type");

    // The stable wire representation is ObjectId + length + bytes. Validate it
    // in place, then slide the bytes over the small prefix and retain the same
    // allocation. Reader::bytes() would allocate and copy the complete extent.
    Reader reader(message.payload);
    const ObjectId returned{reader.fixed<32>()};
    const auto size = reader.u32();
    if (size > 128ULL * 1024 * 1024 || reader.remaining() != size)
        throw DecodeError("invalid remote object reply size");
    constexpr size_t prefix = 32 + 4;
    const auto data = std::span<const uint8_t>(message.payload).subspan(prefix, size);
    if (returned != expected || object_id(data) != expected)
        throw std::runtime_error("remote integrity failure");
    if (size)
        std::memmove(message.payload.data(), message.payload.data() + prefix, size);
    message.payload.resize(size);
    auto object = std::make_shared<DistributedStore::ObjectBuffer>();
    object->bytes = std::move(message.payload);
    object->retained_memory = std::move(message.retained_memory);
    return object;
}
} // namespace

void DistributedStore::DurabilityBatch::add(DurabilityRequirement next) {
    auto same_replica_keys = [](const DurabilityRequirement& a,
                                const DurabilityRequirement& b) {
        if (a.required != b.required || a.replicas.size() != b.replicas.size())
            return false;
        for (size_t i = 0; i < a.replicas.size(); ++i) {
            const auto& left = a.replicas[i];
            const auto& right = b.replicas[i];
            if (left.id != right.id || left.epoch != right.epoch ||
                left.domain != right.domain ||
                left.backend_instance != right.backend_instance)
                return false;
        }
        return true;
    };
    auto dominates = [&](const DurabilityRequirement& stronger,
                         const DurabilityRequirement& weaker) {
        if (!same_replica_keys(stronger, weaker))
            return false;
        for (size_t i = 0; i < stronger.replicas.size(); ++i)
            if (stronger.replicas[i].generation < weaker.replicas[i].generation)
                return false;
        return true;
    };

    // Durability generations are cumulative within an exact
    // node/epoch/domain/backend incarnation. Keep only the non-dominated
    // frontier for each replica set: the normal sequential publication case
    // therefore retains one compact requirement instead of one per extent.
    for (const auto& existing : requirements)
        if (dominates(existing, next))
            return;
    std::erase_if(requirements,
                  [&](const auto& existing) { return dominates(next, existing); });
    requirements.push_back(std::move(next));
}

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

std::vector<NodeInfo> DistributedStore::hosting_nodes() const {
    // The one choke point for DATA placement (0.42.0): owners, should_own,
    // retention candidates, repair, prompt replication and rebalance all go
    // through ranked(), so filtering here is what keeps an edge node -- or a
    // node that turned out to accept no inbound connections -- out of every
    // owner set and every fallback order. Every peer gossips the same bits,
    // so every peer computes the same answer.
    auto active = n_.membership().active();
    std::erase_if(active, [](const NodeInfo& node) { return !node_hosts_extents(node); });
    return active;
}

std::vector<NodeInfo> DistributedStore::ranked(const ObjectId& id) const {
    return capacity_placement_nodes(id.bytes, hosting_nodes(), n_.config().replication);
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
                                        FrameType frame_type, std::atomic_bool* cancelled,
                                        const DataWorkContext* work) {
    auto id = object_id(data);
    if (!put_impl(id, data, frame_type, cancelled, &batch, work)) {
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
                                    std::atomic_bool* cancelled, const DataWorkContext* work) {
    return put_impl(id, data, frame_type, cancelled, &batch, work);
}

bool DistributedStore::put_impl(const ObjectId& id, std::span<const uint8_t> data,
                                FrameType frame_type, std::atomic_bool* cancelled,
                                DurabilityBatch* batch, const DataWorkContext* work) {
    if (object_id(data) != id)
        throw std::runtime_error("object hash mismatch");
    if (data.size() > n_.config().extent_size)
        throw std::runtime_error("DATA object exceeds configured extent size");
    n_.note_activity(frame_type, data.size());
    const auto operation_started = Clock::now();

    auto nodes = ranked(id);
    if (nodes.empty())
        return false;
    // The writer keeps a copy of what it writes: the local store is the
    // first replica tried, then placement order. Until 0.32.13 the order was
    // placement alone, so an offsite writer's first (and, at a floor of 1,
    // only) copy could be an upload across the WAN, its own viewers read
    // it back across the WAN, and its publication rate was the link's
    // (es-1, 2026-09-07). Repair still converges the copies onto the
    // placement owners afterwards.
    if (auto self = std::find_if(nodes.begin(), nodes.end(),
                                 [&](const NodeInfo& node) { return node.id == n_.node_id(); });
        self != nodes.end() && self != nodes.begin()) {
        std::rotate(nodes.begin(), self, self + 1);
    }
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
            batch->add({id, required, successful_replicas});
        }
        if (ok && success < target)
            queue_prompt_replication(id);
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

    // No-progress budget (only when the caller's context carries one). A put
    // is not failed for being slow: any completion, any fallback launch, any
    // pending transfer still moving bytes, or progress anywhere else in the
    // pipeline (the shared counter) re-arms the window. Only a put where
    // nothing at all advances for the whole budget fails -- and it fails
    // retryably, so publication backs off and eventually parks instead of
    // sitting in drain_one_extent() forever. Before this, a spilled put stayed
    // "unfinished" and kept the exit test below from ever firing, so a silent
    // peer with no replacement replica was an infinite 1 ms spin.
    const auto budget = work ? work->no_progress_budget() : std::chrono::milliseconds{};
    const auto* shared_progress = work ? work->progress() : nullptr;
    uint64_t seen = shared_progress ? shared_progress->load(std::memory_order_relaxed) : 0;
    auto window_started = Clock::now();

    while (true) {
        if (budget.count() > 0) {
            const auto now = Clock::now();
            if (shared_progress) {
                const auto now_seen = shared_progress->load(std::memory_order_relaxed);
                if (now_seen != seen) {
                    seen = now_seen;
                    window_started = now;
                }
            }
            for (const auto& item : pending)
                if (!item.done && item.rpc && item.rpc->idle_for() < budget)
                    window_started = now;
            if (work->cancelled() || work->expired() || now - window_started >= budget) {
                for (auto& item : pending)
                    if (!item.done && item.rpc)
                        item.rpc->cancel();
                Log::debug("object write made no progress within budget id=" + to_string(id) +
                           " budget_ms=" + std::to_string(budget.count()) +
                           " success=" + std::to_string(success) +
                           " required=" + std::to_string(need));
                return finish(false, need);
            }
        }
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
            window_started = Clock::now();
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
            window_started = Clock::now();
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

namespace {

// error_reply() encodes a single string; anything else is reported by type.
std::string reply_error_text(const RpcReply& reply) {
    if (reply.message.type == MessageType::error) {
        try {
            Reader reader(reply.message.payload);
            return reader.string();
        } catch (...) {
        }
    }
    return "reply type " + std::to_string(static_cast<int>(reply.message.type));
}

} // namespace

bool DistributedStore::durability_barrier(DurabilityBatch& batch, FrameType frame_type,
                                          std::vector<ObjectId>* unsatisfiable) {
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

    // Why each replica did or did not count, for the failure line below. A
    // bare "required=1 durable=0" spun gbni-1 for half an hour on 2026-09-06
    // with nothing saying which peer, or whether it was the epoch, the
    // barrier, or the transport that said no.
    std::map<ReplicaKey, std::string> outcome;

    // Launch remote waits before blocking on the local domain. This aligns the
    // batch windows across replicas, so a publication does not unnecessarily
    // serialize one physical durability cut per node.
    for (const auto& replica : wanted) {
        if (replica.id == n_.node_id())
            continue;
        auto found = peers.find(replica.id);
        if (found == peers.end())
            continue;
        const ReplicaKey key{replica.id, replica.epoch, replica.domain, replica.backend_instance};
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
        } catch (const std::exception& error) {
            // Still transient -- a request that was never sent is "ask again" --
            // but say why, so "could not send" and "did not try" stay distinct.
            outcome[key] = std::string("remote-launch-failed: ") + error.what();
        } catch (...) {
            outcome[key] = "remote-launch-failed: unknown";
        }
    }

    std::map<ReplicaKey, uint64_t> durable;
    for (const auto& replica : wanted) {
        if (replica.id != n_.node_id())
            continue;
        const ReplicaKey key{replica.id, replica.epoch, replica.domain, replica.backend_instance};
        if (replica.epoch != n_.durability_epoch()) {
            outcome[key] = "local-epoch-changed";
            continue;
        }
        try {
            n_.local_store().durability_barrier(
                {replica.domain, replica.generation, replica.backend_instance},
                DurabilityUrgency::batchable);
            durable[key] = replica.generation;
            outcome[key] = "local-durable";
        } catch (const std::exception& error) {
            Log::warn("local storage durability barrier failed: " + std::string(error.what()));
            outcome[key] = std::string("local-barrier-failed: ") + error.what();
        }
    }

    for (auto& item : pending) {
        const ReplicaKey key{item.replica.id, item.replica.epoch, item.replica.domain,
                             item.replica.backend_instance};
        try {
            if (!item.rpc) {
                outcome[key] = "remote-not-sent";
                continue;
            }
            auto reply = item.rpc->get();
            if (reply.message.type == MessageType::ok) {
                durable[key] = item.replica.generation;
                outcome[key] = "remote-durable";
            } else {
                outcome[key] = "remote-refused: " + reply_error_text(reply);
            }
        } catch (const std::exception& error) {
            outcome[key] = std::string("remote-transport: ") + error.what();
        } catch (...) {
            outcome[key] = "remote-transport: unknown";
        }
    }
    for (const auto& replica : wanted) {
        if (replica.id == n_.node_id())
            continue;
        const ReplicaKey key{replica.id, replica.epoch, replica.domain, replica.backend_instance};
        if (!outcome.contains(key))
            outcome[key] = peers.contains(replica.id) ? "remote-not-sent" : "peer-unknown";
    }

    // Discipline 1 of the self-healing plan: a refusal because a peer's
    // process or backend incarnation changed means "your token is dead", not
    // "your bytes are gone". Ask again with the object ids; the peer answers
    // from its disk with fresh tokens, and the batch is re-stamped in place so
    // the next barrier is ordinary. Restarting es-1 on 2026-09-06 otherwise
    // left gbni-1 asking the same dead question about a 13.9 GB file forever.
    const auto key_of = [](const DurableReplica& replica) {
        return ReplicaKey{replica.id, replica.epoch, replica.domain, replica.backend_instance};
    };
    std::set<ReplicaKey> stale_remote;
    std::set<ReplicaKey> stale_local;
    for (const auto& [key, text] : outcome) {
        if (text == "remote-refused: storage durability epoch changed")
            stale_remote.insert(key);
        else if (text == "local-epoch-changed" || text.starts_with("local-barrier-failed"))
            stale_local.insert(key);
    }
    if (!stale_remote.empty() || !stale_local.empty()) {
        size_t reasserted = 0;
        size_t absent = 0;
        // Local: the backend was reopened under us. Same answer, no RPC.
        for (auto& requirement : batch.requirements) {
            for (auto& replica : requirement.replicas) {
                if (!stale_local.contains(key_of(replica)))
                    continue;
                if (auto token = n_.local_store().reassert_durable(requirement.id)) {
                    replica = {n_.node_id(), n_.durability_epoch(), token->domain,
                               token->generation, token->backend_instance};
                    durable[key_of(replica)] = replica.generation;
                    outcome[key_of(replica)] = "local-reasserted";
                    ++reasserted;
                } else {
                    ++absent;
                }
            }
        }
        // Remote: one probe per peer carrying every id that named a dead token.
        std::map<NodeId, std::vector<ObjectId>> probe_ids;
        std::map<NodeId, DurableReplica> probe_token;
        for (const auto& requirement : batch.requirements) {
            for (const auto& replica : requirement.replicas) {
                if (!stale_remote.contains(key_of(replica)))
                    continue;
                probe_ids[replica.id].push_back(requirement.id);
                probe_token.emplace(replica.id, replica);
            }
        }
        for (auto& [peer_id, ids] : probe_ids) {
            const auto found = peers.find(peer_id);
            if (found == peers.end())
                continue;
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            std::map<ObjectId, DurableReplica> fresh;
            std::string probe_outcome;
            try {
                for (size_t offset = 0; offset < ids.size(); offset += 4096) {
                    const auto& token = probe_token.at(peer_id);
                    Writer payload;
                    payload.fixed(token.epoch.bytes);
                    payload.u64(token.domain);
                    payload.u64(token.generation);
                    payload.u64(token.backend_instance);
                    const auto end = std::min(ids.size(), offset + 4096);
                    payload.u32(static_cast<uint32_t>(end - offset));
                    for (size_t i = offset; i < end; ++i)
                        payload.fixed(ids[i].bytes);
                    auto reply = n_.call(found->second, MessageType::object_durability_barrier,
                                         payload.data(), frame_type);
                    if (reply.message.type != MessageType::ok) {
                        probe_outcome = "probe-refused: " + reply_error_text(reply);
                        break;
                    }
                    Reader reader(reply.message.payload);
                    NodeId fresh_epoch{reader.fixed<16>()};
                    const auto count = reader.u32();
                    if (count > 4096)
                        throw std::runtime_error("too many reasserted ids");
                    for (uint32_t i = 0; i < count; ++i) {
                        ObjectId id{reader.fixed<32>()};
                        const auto domain = reader.u64();
                        const auto generation = reader.u64();
                        const auto instance = reader.u64();
                        fresh[id] = {peer_id, fresh_epoch, domain, generation, instance};
                    }
                    reader.finish();
                }
            } catch (const std::exception& error) {
                probe_outcome = std::string("probe-transport: ") + error.what();
            }
            for (auto& requirement : batch.requirements) {
                for (auto& replica : requirement.replicas) {
                    if (replica.id != peer_id || !stale_remote.contains(key_of(replica)))
                        continue;
                    if (!probe_outcome.empty()) {
                        outcome[key_of(replica)] = probe_outcome;
                        continue;
                    }
                    const auto it = fresh.find(requirement.id);
                    if (it == fresh.end()) {
                        outcome[key_of(replica)] =
                            "remote-refused: epoch changed; object absent on peer after probe";
                        ++absent;
                        continue;
                    }
                    replica = it->second;
                    durable[key_of(replica)] = replica.generation;
                    outcome[key_of(replica)] = "remote-reasserted";
                    ++reasserted;
                }
            }
        }
        Log::info("object durability re-derived after incarnation change reasserted=" +
                  std::to_string(reasserted) + " absent=" + std::to_string(absent) +
                  " peers=" + std::to_string(probe_ids.size()));
    }

    // A requirement is reported as unsatisfiable -- "re-put this object" --
    // only when every replica that failed did so definitively (the peer says
    // it no longer holds the object, or is unknown). A transport failure, a
    // peer mid-restart, or a pre-0.29 peer that cannot be probed is "ask
    // again", never "re-send": on the first live run a Broken pipe at the
    // instant of a restart re-put extents the peer still had.
    const auto definitive = [](const std::string& text) {
        return text.starts_with("remote-refused: epoch changed; object absent") ||
               text == "peer-unknown" || text == "local-epoch-changed" ||
               text.starts_with("local-barrier-failed");
    };
    bool all_durable = true;
    for (const auto& requirement : batch.requirements) {
        size_t count = 0;
        bool transient = false;
        std::string detail;
        for (const auto& replica : requirement.replicas) {
            const ReplicaKey key{replica.id, replica.epoch, replica.domain,
                                 replica.backend_instance};
            const auto found = durable.find(key);
            const auto seen = outcome.find(key);
            if (found != durable.end() && found->second >= replica.generation)
                ++count;
            else if (seen == outcome.end() || !definitive(seen->second))
                transient = true;
            if (Log::enabled(LogLevel::debug)) {
                detail += " replica=" + to_string(replica.id).substr(0, 12) +
                          " epoch=" + to_string(replica.epoch).substr(0, 8) +
                          " gen=" + std::to_string(replica.generation) + " outcome=\"" +
                          (seen == outcome.end() ? std::string("unexamined") : seen->second) +
                          "\"";
            }
        }
        if (count < requirement.required) {
            Log::debug("object durability quorum unavailable id=" + to_string(requirement.id) +
                       " required=" + std::to_string(requirement.required) +
                       " durable=" + std::to_string(count) +
                       (transient ? " transient=yes" : " transient=no") + detail);
            all_durable = false;
            if (!unsatisfiable || transient)
                return false;
            unsatisfiable->push_back(requirement.id);
        }
    }
    return all_durable;
}


RpcReply DistributedStore::bounded_control_call(const NodeInfo& target, MessageType type,
                                                std::span<const uint8_t> payload,
                                                FrameType frame_type) {
    auto request = n_.call_async(target, type, payload, frame_type);
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
            // A retention claim says "this node holds the object". Until
            // 0.32.3 it re-read, decrypted and hashed every extent here (the
            // full valid() path) inside the metadata mutation: minutes per
            // publication on a busy node, with the store mutex held for the
            // duration so every HTTP request -- status and playback included
            // -- queued behind it (es-1, 2026-09-07: 16 HTTP workers blocked
            // in LocalStore::has, status unavailable for 40 s+). Presence in
            // the index is the same contract discipline 1 wrote down for
            // durability ("present after a restart is durable"): the bytes
            // were verified when this node put them, and the scrub, not the
            // retention claim, is where later corruption is found. (No DATA
            // admission for an index lookup; the remote handler matches
            // since 0.32.7.)
            const bool present = object_class == RetentionClass::data
                                     ? n_.local_store().has(id)
                                     : n_.control_store().has(id);
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
        return bounded_control_call(target, MessageType::retain_objects, writer.data(),
                                    FrameType::loader)
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
    const auto started = Clock::now();
    std::map<NodeId, NodeInfo> node_info;
    std::map<NodeId, std::vector<ObjectId>> batches;
    std::map<ObjectId, std::vector<NodeInfo>> candidates_by_object;
    for (const auto& id : ids)
        candidates_by_object.emplace(id, ranked(id));

    // Batched, concurrent candidate-presence scan (the O(N) serial
    // has_on()-per-candidate loop this exists to remove). Preference order and
    // the per-object floor requirement are identical to the serial form.
    auto selected_by_object = select_present_batched(candidates_by_object, floor);
    const auto scan_ms = elapsed_ms(started);

    std::vector<ObjectId> short_ids;
    for (const auto& id : ids)
        if (selected_by_object[id].size() < floor)
            short_ids.push_back(id);
    const auto short_count = short_ids.size();
    size_t fallback_claims = 0;
    // Where the writer's retention barrier spends its time, when it is slow
    // enough to matter (1-15 s per quantum commit on the live cluster,
    // 2026-09-07): the presence scan, re-replication of short objects, the
    // per-node claims, per-object fallbacks.
    const auto report = [&](bool ok) {
        const auto total = elapsed_ms(started);
        if (total >= 250 && Log::enabled(LogLevel::debug))
            Log::debug("DATA retention barrier ids=" + std::to_string(ids.size()) +
                       " nodes=" + std::to_string(batches.size()) + " total_ms=" +
                       std::to_string(total) + " scan_ms=" + std::to_string(scan_ms) +
                       " short=" + std::to_string(short_count) +
                       " fallback_claims=" + std::to_string(fallback_claims) +
                       " ok=" + (ok ? "yes" : "no"));
    };

    if (!short_ids.empty()) {
        // A metadata-only mutation may be the first operation on an object
        // after its old placement disappeared. Re-establish the ordinary DATA
        // durability floor before creating the new causal claim. This only
        // touches objects the batched scan above already found short, so it
        // stays a rare, per-object serial path rather than the common one.
        std::map<ObjectId, std::vector<NodeInfo>> rescan_candidates;
        for (const auto& id : short_ids) {
            auto data = get(id, 0, FrameType::speculative);
            if (!data || !put(id, *data)) {
                report(false);
                return false;
            }
            auto candidates = ranked(id);
            candidates_by_object[id] = candidates;
            rescan_candidates.emplace(id, std::move(candidates));
        }
        auto rescanned = select_present_batched(rescan_candidates, floor);
        for (auto& [id, selected] : rescanned)
            selected_by_object[id] = std::move(selected);
    }

    for (const auto& id : ids) {
        auto& selected = selected_by_object[id];
        if (selected.size() < floor) {
            Log::debug("DATA retention placement unavailable id=" + to_string(id) +
                       " required=" + std::to_string(floor) +
                       " present=" + std::to_string(selected.size()));
            report(false);
            return false;
        }
        for (const auto& candidate : selected) {
            node_info[candidate.id] = candidate;
            batches[candidate.id].push_back(id);
        }
    }

    // One claim RPC per selected node, in parallel: the claims are
    // independent, and serially each one cost a full round trip (60-200 ms
    // to the remote and the wireless nodes on the live cluster) inside the
    // writer's metadata mutation, for every quantum commit.
    std::map<ObjectId, std::set<NodeId>> claimed;
    {
        std::vector<std::pair<NodeId, std::future<bool>>> claims;
        claims.reserve(batches.size());
        for (auto& [node_id, batch] : batches) {
            auto found = node_info.find(node_id);
            if (found == node_info.end())
                continue;
            const NodeInfo target = found->second;
            const std::vector<ObjectId>* ids_ptr = &batch;
            claims.emplace_back(node_id, std::async(std::launch::async, [this, target, ids_ptr, dot] {
                return retain_on(target, RetentionClass::data, *ids_ptr, dot);
            }));
        }
        for (auto& [node_id, result] : claims) {
            bool ok = false;
            try {
                ok = result.get();
            } catch (...) {
                ok = false;
            }
            if (!ok)
                continue;
            for (const auto& id : batches[node_id])
                claimed[id].insert(node_id);
        }
    }

    for (const auto& id : ids) {
        auto& successful = claimed[id];
        if (successful.size() >= floor)
            continue;
        const auto candidates = candidates_by_object.find(id);
        if (candidates == candidates_by_object.end()) {
            report(false);
            return false;
        }
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
            ++fallback_claims;
            if (present && retain_on(candidate, RetentionClass::data, {id}, dot))
                successful.insert(candidate.id);
        }
        if (successful.size() < floor) {
            Log::debug("DATA retention floor unavailable id=" + to_string(id) +
                       " required=" + std::to_string(floor) +
                       " retained=" + std::to_string(successful.size()));
            report(false);
            return false;
        }
    }
    report(true);
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

    const auto started = Clock::now();
    auto active = n_.membership().active();
    active.erase(std::remove_if(active.begin(), active.end(), [&](const NodeInfo& peer) {
        return peer.metadata_write_replicas_required != required;
    }), active.end());
    if (active.size() < required)
        return false;
    // Local first, then the nearest measured peer: this runs inside the
    // writer's metadata mutation, and NodeId order sent gbni-1's every
    // catalogue claim across the WAN (2026-09-07).
    active = order_commit_replicas(active, n_.node_id(), [&](const NodeId& peer) {
        return n_.peer_latency(peer);
    });

    // A retention claim asserts this node holds every object, and the peer
    // copies are derived from ours, so local presence is a precondition
    // whatever any peer turns out to need.
    for (const auto& id : ids)
        if (!ensure_control_local(id))
            return false;

    // Ask the peer what it is missing before sending it anything.
    //
    // Until 0.53.1 this uploaded the whole referenced control graph to every
    // candidate on every commit, with no presence check anywhere on the path.
    // That was tolerable while a graph was 65 catalogue shards. Once the
    // namespace became tree-backed a commit referenced its whole spine, and
    // the cost stopped scaling with what changed and started scaling with how
    // large the library had grown: measured on gbni-1 on 2026-09-22, three
    // minutes of importing produced 25 commits, pushed 5,469 control objects
    // to peers, and grew the control store by *zero* objects. Every byte of it
    // was an object both ends already had. It is the same defect 0.51.0 fixed
    // on the replication path -- "a node already present is not
    // re-replicated" -- which was never applied here.
    //
    // A peer too old to answer, or one that fails the probe, falls back to
    // being sent everything, which is exactly what it got before.
    auto missing_on = [&](const NodeInfo& target) {
        std::vector<ObjectId> missing;
        constexpr size_t probe_batch = 4096;
        for (size_t offset = 0; offset < ids.size(); offset += probe_batch) {
            const auto end = std::min(ids.size(), offset + probe_batch);
            Writer writer;
            writer.u32(static_cast<uint32_t>(end - offset));
            for (size_t i = offset; i < end; ++i)
                writer.fixed(ids[i].bytes);
            try {
                auto reply = bounded_control_call(target, MessageType::have_control_objects,
                                                  writer.data());
                if (reply.message.type != MessageType::have_control_objects_reply)
                    throw std::runtime_error(reply_error_text(reply));
                Reader reader(reply.message.payload);
                const auto count = reader.u32();
                if (count != end - offset)
                    throw std::runtime_error("control presence reply count mismatch");
                for (size_t i = offset; i < end; ++i)
                    if (!reader.u8())
                        missing.push_back(ids[i]);
                reader.finish();
            } catch (const std::exception& error) {
                Log::debug("CONTROL presence probe peer=" + target.host +
                           " error=" + error.what() + "; sending the whole graph");
                return ids;
            }
        }
        return missing;
    };

    // Store the graph on one candidate with the puts pipelined rather than one
    // round trip after another, which cost 65 x 65 ms = 4.2 s per mutation on
    // the live cluster (`control_ms=4247 control_objects=65`).
    //
    // Pipelined, but bounded. Until 0.53.1 this fired the entire graph at once,
    // which was correct while a graph was 65 catalogue shards and became a
    // cluster-wide failure when the namespace went tree-backed and a commit
    // started touching 838 control objects: the puts overran the connection's
    // outbound queue, the catch below cancelled the whole batch, and every
    // peer was skipped in turn. gbni-1 then had retained=1 against required=2
    // and killed the operator's ingest job -- on every commit over the line,
    // for hours, while reporting a dead network link (2026-09-22).
    //
    // Bound it by what the connection will actually hold, not by a number of
    // our own choosing. A put occupies a writer queue slot until it is sent
    // and a pending-reply slot until it is answered, so the binding limit is
    // the smaller of the two. A quarter of it, because this graph is one
    // caller among heartbeats, metadata commits and status traffic on the same
    // lane, and taking more than a share of a shared budget is precisely how
    // this broke. That still pipelines ~64 deep -- about thirteen round trips
    // for a graph of 838 against the fifty it would take serially -- and it no
    // longer has any relationship to how large the namespace has grown.
    const size_t connection_budget =
        std::min(max_pending_rpc_requests, max_peer_outbound_messages);
    const size_t put_window = std::max<size_t>(1, connection_budget / 4);
    auto put_graph_on = [&](const NodeInfo& target) {
        if (target.id == n_.node_id()) {
            // Local puts are reaffirmations as well as writes: LocalStore::put
            // refreshes an existing object's physical age, which is what keeps
            // reachability GC from racing a commit that reuses an old orphan.
            // So the local candidate is not presence-filtered.
            for (const auto& id : ids) {
                auto bytes = n_.control_store().get(id);
                if (!bytes || !n_.control_store().put(id, *bytes))
                    return false;
            }
            return true;
        }
        const auto missing = missing_on(target);
        if (missing.size() != ids.size() && Log::enabled(LogLevel::debug))
            Log::debug("CONTROL graph peer=" + target.host +
                       " referenced=" + std::to_string(ids.size()) +
                       " missing=" + std::to_string(missing.size()));
        if (missing.empty())
            return true;
        std::vector<std::pair<AsyncRpc, size_t>> in_flight;
        in_flight.reserve(std::min(put_window, missing.size()));
        const auto put_started = Clock::now();
        bool ok = true;
        size_t bytes_sent = 0;
        auto drain = [&] {
            for (auto& [rpc, size] : in_flight) {
                try {
                    if (rpc.get().message.type == MessageType::ok)
                        bytes_sent += size;
                    else
                        ok = false;
                } catch (const std::exception& error) {
                    Log::debug("CONTROL retention object store peer=" + target.host +
                               " error=" + error.what());
                    ok = false;
                }
            }
            in_flight.clear();
        };
        for (const auto& id : missing) {
            if (in_flight.size() >= put_window)
                drain();
            // Read the bytes only for what is actually being sent. The old
            // form materialised every referenced object up front, so a commit
            // decrypted its whole spine out of the control store to discover
            // the peer wanted none of it.
            auto bytes = n_.control_store().get(id);
            if (!bytes) {
                for (auto& [rpc, _] : in_flight)
                    rpc.cancel();
                return false;
            }
            Writer writer;
            writer.fixed(id.bytes);
            writer.bytes(*bytes);
            try {
                in_flight.emplace_back(n_.call_async(target, MessageType::put_control_object,
                                                     writer.data(), FrameType::control),
                                       bytes->size());
            } catch (const std::exception& error) {
                Log::debug("CONTROL retention object store peer=" + target.host +
                           " error=" + error.what());
                for (auto& [rpc, _] : in_flight)
                    rpc.cancel();
                return false;
            }
        }
        drain();
        if (bytes_sent)
            note_network(bytes_sent, Clock::now() - put_started);
        return ok;
    };

    // Critical-path CONTROL publication scales with the configured metadata
    // write floor, not cluster membership. Background control repair may later
    // fan the immutable graph out to every node.
    size_t retained_count = 0;
    size_t tried = 0;
    for (const auto& candidate : active) {
        ++tried;
        if (!put_graph_on(candidate))
            continue;
        if (retain_on(candidate, RetentionClass::control, ids, dot))
            ++retained_count;
        if (retained_count >= required)
            break;
    }
    const auto total_ms = elapsed_ms(started);
    if (total_ms >= 250 && Log::enabled(LogLevel::debug))
        Log::debug("CONTROL retention claim objects=" + std::to_string(ids.size()) +
                   " required=" + std::to_string(required) + " tried=" + std::to_string(tried) +
                   " retained=" + std::to_string(retained_count) +
                   " total_ms=" + std::to_string(total_ms));
    if (retained_count < required) {
        Log::debug("CONTROL retention floor unavailable objects=" + std::to_string(ids.size()) +
                   " required=" + std::to_string(required) +
                   " retained=" + std::to_string(retained_count));
        return false;
    }
    return true;
}

DistributedStore::DistributedStore(NodeRuntime& n) : n_(n) {
    prompt_thread_ = std::jthread([this](std::stop_token stop) {
        run_supervised("prompt-replication", [this, stop] { prompt_replication_loop(stop); });
    });
}

DistributedStore::~DistributedStore() {
    if (prompt_thread_.joinable()) {
        prompt_thread_.request_stop();
        prompt_cv_.notify_all();
        prompt_thread_.join();
    }
}

DistributedStore::PromptReplicationStats DistributedStore::prompt_replication_stats() const {
    PromptReplicationStats out;
    {
        std::lock_guard lock(const_cast<std::mutex&>(prompt_mutex_));
        out.queued = prompt_queue_.size();
    }
    out.copies = prompt_copies_.load(std::memory_order_relaxed);
    out.failures = prompt_failures_.load(std::memory_order_relaxed);
    return out;
}

void DistributedStore::queue_prompt_replication(const ObjectId& id) {
    {
        std::lock_guard lock(prompt_mutex_);
        if (!prompt_queued_.insert(id).second)
            return;
        prompt_queue_.push_back(id);
    }
    prompt_cv_.notify_one();
}

void DistributedStore::prompt_replication_loop(std::stop_token stop) {
    // One object at a time: the local copy is read and pushed to the first
    // placement owner that lacks it, as speculative DATA work (behind viewers
    // and loaders in the arbiter, and counted against the background effort
    // ceiling). Reaching `replication` beyond the second copy stays with the
    // repair pass; this loop exists to close the single-copy window quickly.
    std::deque<std::pair<ObjectId, Clock::time_point>> retry;
    while (!stop.stop_requested()) {
        std::optional<ObjectId> id;
        {
            std::unique_lock lock(prompt_mutex_);
            prompt_cv_.wait(lock, stop, [&] {
                return !prompt_queue_.empty() ||
                       (!retry.empty() && retry.front().second <= Clock::now());
            });
            if (stop.stop_requested()) return;
            if (!prompt_queue_.empty()) {
                id = prompt_queue_.front();
                prompt_queue_.pop_front();
                prompt_queued_.erase(*id);
            }
        }
        if (!id) {
            if (!retry.empty() && retry.front().second <= Clock::now()) {
                id = retry.front().first;
                retry.pop_front();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
        }
        try {
            if (!n_.local_store().has(*id)) continue; // gone (deleted, or never local)
            auto nodes = ranked(*id);
            const size_t target = std::min(n_.config().replication, nodes.size());
            if (target < 2) continue;
            size_t holders = 1; // the local copy
            std::optional<NodeInfo> destination;
            for (const auto& node : nodes) {
                if (node.id == n_.node_id()) continue;
                bool present = false;
                try {
                    present = has_on(node, *id);
                } catch (...) {
                    present = false;
                }
                if (present) {
                    if (++holders >= 2) break;
                } else if (!destination) {
                    destination = node;
                }
            }
            if (holders >= 2 || !destination) continue;
            auto data = n_.local_store().get(*id);
            if (!data) continue;
            if (put_on(*destination, *id, *data, false)) {
                prompt_copies_.fetch_add(1, std::memory_order_relaxed);
                n_.notify_storage_mutation();
            } else {
                prompt_failures_.fetch_add(1, std::memory_order_relaxed);
                retry.emplace_back(*id, Clock::now() + std::chrono::seconds(30));
            }
        } catch (const std::exception& error) {
            prompt_failures_.fetch_add(1, std::memory_order_relaxed);
            Log::debug("prompt replication id=" + to_string(*id) + " error=" + error.what());
            retry.emplace_back(*id, Clock::now() + std::chrono::seconds(30));
        }
        if (retry.size() > 4096) retry.pop_front();
    }
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

DistributedStore::ObjectData
DistributedStore::get_from(const NodeInfo& target, const ObjectId& id, FrameType frame_type,
                           const std::shared_ptr<SharedFetch>& shared,
                           Clock::time_point deadline, std::atomic_bool* cancelled,
                           const std::function<bool()>& abort) {
    try {
        auto resource = n_.data_resources().acquire(
            DataWorkContext(frame_type, n_.config().extent_size, deadline, cancelled),
            n_.config().extent_size);
        if (!resource)
            return {};
        if (target.id == n_.node_id()) {
            auto memory = n_.retained_memory().acquire(
                object_memory_class(frame_type), MemoryOwner::object_payload,
                n_.config().extent_size, deadline, cancelled);
            if (!memory)
                return {};
            auto bytes = n_.local_store().get(id);
            if (!bytes)
                return {};
            auto object = std::make_shared<ObjectBuffer>();
            object->bytes = std::move(*bytes);
            object->retained_memory =
                std::make_shared<std::vector<RetainedMemoryLedger::Lease>>();
            object->retained_memory->push_back(std::move(*memory));
            return object;
        }

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
        auto data = take_object_reply_payload(std::move(reply.message), id);
        note_network(data->bytes.size(), Clock::now() - started);
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

DistributedStore::ObjectData
DistributedStore::get_remote(const ObjectId& id, size_t stripe, FrameType frame_type,
                             bool foreground, bool opportunistic_persist,
                             Clock::time_point deadline, std::atomic_bool* cancelled,
                             const std::function<bool()>& abort) {
    // A hard wall-clock deadline belongs to one caller, not to an ObjectId-wide
    // shared fetch. Probe reads therefore use a private transfer so expiry can
    // abort the underlying RPC. Cancellation-only playback reads retain normal
    // shared-fetch deduplication/promotion; a stopped caller may abandon its wait
    // but does not cancel an ObjectId transfer other readers may still need.
    if (deadline != Clock::time_point{}) {
        auto try_private = [&](std::vector<NodeInfo> candidates) -> ObjectData {
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
                replica_selector_.finished(target, work, data ? data->bytes.size() : 0,
                                           Clock::now() - started, static_cast<bool>(data));
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
            if (foreground) note_foreground(data->bytes.size());
            if (opportunistic_persist)
                n_.enqueue_fetched(id, data->bytes, should_own(id));
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
                note_foreground(shared->result->bytes.size());
                shared->foreground_accounted = true;
            }
            if (opportunistic_persist && !shared->persist_queued) {
                n_.enqueue_fetched(id, shared->result->bytes, should_own(id));
                shared->persist_queued = true;
            }
        }
        return shared->result;
    }

    auto finish = [&](ObjectData result) {
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
            note_foreground(result->bytes.size());
        if (result && queue_persist)
            n_.enqueue_fetched(id, result->bytes, should_own(id));
        shared->cv.notify_all();
        return result;
    };

    auto try_candidates = [&](std::vector<NodeInfo> candidates) -> ObjectData {
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
                                           data ? data->bytes.size() : 0,
                                           Clock::now() - started, static_cast<bool>(data));
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
    auto data = get_shared(id, stripe, frame_type, deadline, cancelled);
    if (!data)
        return {};
    return data->bytes;
}

DistributedStore::ObjectData
DistributedStore::get_shared(const ObjectId& id, size_t stripe, FrameType frame_type,
                             Clock::time_point deadline, std::atomic_bool* cancelled) {
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
    auto local_memory = n_.retained_memory().acquire(
        object_memory_class(frame_type), MemoryOwner::object_payload,
        n_.config().extent_size, deadline, cancelled);
    if (!local_memory)
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
        auto object = std::make_shared<ObjectBuffer>();
        object->bytes = std::move(*data);
        object->retained_memory =
            std::make_shared<std::vector<RetainedMemoryLedger::Lease>>();
        object->retained_memory->push_back(std::move(*local_memory));
        return object;
    }
    local_memory.reset();

    auto cache_resource = n_.data_resources().acquire(
        DataWorkContext(frame_type, n_.config().extent_size, deadline, cancelled),
        n_.config().extent_size);
    if (!cache_resource)
        return {};
    auto cache_memory = n_.retained_memory().acquire(
        object_memory_class(frame_type), MemoryOwner::object_payload,
        n_.config().extent_size, deadline, cancelled);
    if (!cache_memory)
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
        auto object = std::make_shared<ObjectBuffer>();
        object->bytes = std::move(*cached);
        object->retained_memory =
            std::make_shared<std::vector<RetainedMemoryLedger::Lease>>();
        object->retained_memory->push_back(std::move(*cache_memory));
        return object;
    }
    cache_memory.reset();

    if (read_aborted(deadline, cancelled))
        return {};
    auto data = get_remote(id, stripe, frame_type,
                           foreground, interactive, deadline, cancelled);
    if (data && frame_type == FrameType::read_ahead)
        n_.note_activity(frame_type, data->bytes.size());
    auto elapsed = log_playback_read("remote", data ? data->bytes.size() : 0,
                                     static_cast<bool>(data));
    if (Log::enabled(LogLevel::all))
        Log::trace("DIAG object-get id=" + to_string(id) +
               " source=remote result=" + std::to_string(data ? 1 : 0) +
               " bytes=" + std::to_string(data ? data->bytes.size() : 0) +
               " ms=" + std::to_string(elapsed.count()));
    return data;
}

std::map<NodeId, std::map<ObjectId, bool>> DistributedStore::batched_have_objects(
    const std::map<NodeId, NodeInfo>& node_info,
    const std::map<NodeId, std::vector<ObjectId>>& ids_by_node) {
    std::map<NodeId, std::map<ObjectId, bool>> results;

    // Local node: cheap presence check, no RPC, no chunking/concurrency needed.
    // No DATA admission either: an index lookup owns no buffer, and taking a
    // 4 MB loader lease per id (thousands per quantum commit) queued the
    // writer's retention barrier behind its own publications.
    if (auto self = ids_by_node.find(n_.node_id()); self != ids_by_node.end()) {
        auto& out = results[n_.node_id()];
        for (const auto& id : self->second)
            out[id] = n_.local_store().has(id);
    }

    const size_t batch_size = std::max<size_t>(1, n_.config().retention_check_batch_size);
    const size_t max_concurrency = std::max<size_t>(1, n_.config().retention_check_concurrency);

    struct Chunk {
        NodeId node;
        std::vector<ObjectId> ids;
    };
    std::deque<Chunk> chunks;
    for (const auto& [node_id, ids] : ids_by_node) {
        if (node_id == n_.node_id())
            continue;
        for (size_t offset = 0; offset < ids.size(); offset += batch_size) {
            const size_t end = std::min(ids.size(), offset + batch_size);
            chunks.push_back({node_id, std::vector<ObjectId>(ids.begin() + static_cast<long>(offset),
                                                              ids.begin() + static_cast<long>(end))});
        }
    }

    struct InFlight {
        NodeId node;
        std::vector<ObjectId> ids;
        AsyncRpc rpc;
    };
    std::vector<InFlight> in_flight;
    in_flight.reserve(max_concurrency);

    auto fail_chunk = [&](const NodeId& node, const std::vector<ObjectId>& ids) {
        auto& out = results[node];
        for (const auto& id : ids)
            out.emplace(id, false);
    };

    auto launch = [&](Chunk chunk) {
        auto found = node_info.find(chunk.node);
        if (found == node_info.end()) {
            fail_chunk(chunk.node, chunk.ids);
            return;
        }
        Writer writer;
        writer.u32(static_cast<uint32_t>(chunk.ids.size()));
        for (const auto& id : chunk.ids)
            writer.fixed(id.bytes);
        try {
            auto rpc = n_.call_async(found->second, MessageType::have_objects, writer.data(),
                                     FrameType::loader);
            in_flight.push_back({chunk.node, std::move(chunk.ids), std::move(rpc)});
        } catch (const std::exception& error) {
            Log::debug("retention presence batch peer=" + found->second.host +
                       " error=" + error.what());
            fail_chunk(chunk.node, chunk.ids);
        }
    };

    const auto deadline = std::max(n_.config().dead_after, n_.config().connect_timeout);
    while (!chunks.empty() || !in_flight.empty()) {
        while (!chunks.empty() && in_flight.size() < max_concurrency) {
            launch(std::move(chunks.front()));
            chunks.pop_front();
        }
        if (in_flight.empty())
            break;
        auto& item = in_flight.front();
        bool ok = false;
        std::vector<bool> present(item.ids.size(), false);
        try {
            if (item.rpc.wait_for(deadline) != std::future_status::ready) {
                item.rpc.abort();
            } else {
                auto reply = item.rpc.get();
                if (reply.message.type == MessageType::have_objects_reply) {
                    Reader reader(reply.message.payload);
                    if (reader.u32() == item.ids.size()) {
                        for (size_t i = 0; i < item.ids.size(); ++i)
                            present[i] = reader.u8() != 0;
                        reader.finish();
                        ok = true;
                    }
                }
            }
        } catch (const std::exception& error) {
            Log::debug("retention presence batch peer=" +
                       (node_info.contains(item.node) ? node_info.at(item.node).host : "") +
                       " error=" + error.what());
        }
        auto& out = results[item.node];
        for (size_t i = 0; i < item.ids.size(); ++i)
            out[item.ids[i]] = ok && present[i];
        in_flight.erase(in_flight.begin());
    }
    return results;
}

std::map<ObjectId, std::vector<NodeInfo>> DistributedStore::select_present_batched(
    const std::map<ObjectId, std::vector<NodeInfo>>& candidates_by_object, size_t floor) {
    std::map<ObjectId, std::vector<NodeInfo>> selected;
    std::map<ObjectId, size_t> next_index;
    std::vector<ObjectId> pending;
    pending.reserve(candidates_by_object.size());
    for (const auto& [id, candidates] : candidates_by_object) {
        selected[id];
        next_index[id] = 0;
        if (!candidates.empty())
            pending.push_back(id);
    }

    while (!pending.empty()) {
        std::map<NodeId, NodeInfo> node_info;
        std::map<NodeId, std::vector<ObjectId>> ids_by_node;
        std::vector<ObjectId> next_pending;
        for (const auto& id : pending) {
            auto& sel = selected[id];
            if (sel.size() >= floor)
                continue;
            auto& idx = next_index[id];
            const auto& candidates = candidates_by_object.at(id);
            if (idx >= candidates.size())
                continue;
            const auto& candidate = candidates[idx];
            ++idx;
            node_info[candidate.id] = candidate;
            ids_by_node[candidate.id].push_back(id);
            next_pending.push_back(id);
        }
        if (ids_by_node.empty())
            break;

        auto results = batched_have_objects(node_info, ids_by_node);
        for (const auto& [node_id, ids] : ids_by_node) {
            const auto& node = node_info.at(node_id);
            const auto found = results.find(node_id);
            for (const auto& id : ids) {
                const bool present =
                    found != results.end() && found->second.contains(id) && found->second.at(id);
                if (present)
                    selected[id].push_back(node);
            }
        }

        pending = std::move(next_pending);
        std::erase_if(pending, [&](const ObjectId& id) {
            return selected[id].size() >= floor ||
                   next_index[id] >= candidates_by_object.at(id).size();
        });
    }
    return selected;
}

bool DistributedStore::has_on(const NodeInfo& target, const ObjectId& id) {
    if (target.id == n_.node_id()) {
        // Presence-only: this is a candidate-selection probe, not the retention
        // commit. A full decrypt here is exactly the scaling cliff this exists
        // to remove. No DATA admission for an index lookup.
        return n_.local_store().has(id);
    }
    Writer writer;
    writer.fixed(id.bytes);
    auto reply = bounded_control_call(target, MessageType::have_object, writer.data(),
                                      FrameType::speculative);
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
    for (const auto& target : hosting_nodes()) {
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
        auto resource = n_.data_resources().acquire(
            DataWorkContext(FrameType::speculative, data.size()), data.size());
        if (!resource)
            return false;
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
    if (!data)
        return false;
    auto resource = n_.data_resources().acquire(
        DataWorkContext(frame_type, data->bytes.size()), data->bytes.size());
    return resource && n_.block_cache().put(id, data->bytes);
}

bool DistributedStore::ensure_local(const ObjectId& id, bool foreground) {
    const auto local_class = foreground ? FrameType::foreground : FrameType::speculative;
    {
        auto resource = n_.data_resources().acquire(
            DataWorkContext(local_class, n_.config().extent_size), n_.config().extent_size);
        if (!resource)
            return false;
        if (n_.local_store().valid(id))
            return true;
    }
    if (auto cached = n_.block_cache().get(id)) {
        auto resource = n_.data_resources().acquire(
            DataWorkContext(local_class, cached->size()), cached->size());
        if (resource && n_.local_store().put(id, *cached))
            return true;
    }
    auto data = get_remote(id, 0,
                           foreground ? FrameType::foreground : FrameType::speculative,
                           foreground, false);
    if (!data)
        return false;
    auto resource = n_.data_resources().acquire(
        DataWorkContext(local_class, data->bytes.size()), data->bytes.size());
    return resource && n_.local_store().put(id, data->bytes);
}

bool DistributedStore::ensure_control_local(const ObjectId& id) {
    // No DATA credit: the control store is not on the DATA device, and no
    // other control-store access takes one. This took a 4 MiB speculative
    // credit to check an 18 KB object, and when the wait was abandoned under
    // DATA pressure the commit that asked failed with it
    // (TODO/2026-09-23-torrent-writes-starve-publication-incident.md).
    if (n_.control_store().valid(id))
        return true;

    Writer writer;
    writer.fixed(id.bytes);
    const auto payload = writer.take();

    // Control objects are metadata-replica data rather than DHT DATA
    // replicas. Search every currently active peer and keep the transfer on the
    // CONTROL transport while using speculative worker priority so it cannot
    // block health/quorum traffic or require a DATA session to exist.
    size_t asked = 0;
    std::string last_failure = "no active peer";
    for (const auto& target : n_.membership().active()) {
        if (target.id == n_.node_id())
            continue;
        ++asked;
        try {
            auto started = Clock::now();
            auto reply = n_.call(target, MessageType::get_control_object, payload,
                                 FrameType::speculative);
            if (reply.message.type != MessageType::control_object_reply) {
                last_failure = target.host + ": " + message_type_name(reply.message.type);
                continue;
            }

            Reader reader(reply.message.payload);
            ObjectId returned{reader.fixed<32>()};
            auto data = reader.bytes(128 * 1024 * 1024);
            reader.finish();
            if (returned != id || object_id(data) != id) {
                Log::debug("control object read " + target.host + ": integrity failure");
                last_failure = target.host + ": integrity failure";
                continue;
            }
            note_network(data.size(), Clock::now() - started);
            if (n_.control_store().put(id, data))
                return true;
            last_failure = "local control store put failed";
        } catch (const std::exception& e) {
            Log::debug("control object read " + target.host + ": " + e.what());
            last_failure = target.host + ": " + e.what();
        }
    }
    Log::warn("control object unavailable id=" + hex(id.bytes) + " peers_asked=" + std::to_string(asked) +
              " last_failure=" + last_failure);
    return false;
}

void DistributedStore::erase_all(const ObjectId& id) {
    Writer writer;
    writer.fixed(id.bytes);
    for (const auto& target : n_.membership().active()) {
        try {
            if (target.id == n_.node_id()) {
                auto resource = n_.data_resources().acquire(
                    DataWorkContext(FrameType::speculative, n_.config().extent_size),
                    n_.config().extent_size);
                if (resource && !n_.retention_store().retained(RetentionClass::data, id))
                    (void)n_.local_store().remove(id);
                (void)n_.block_cache().remove(id);
            } else {
                (void)n_.call(target, MessageType::delete_object, writer.data(),
                              FrameType::speculative);
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

namespace {
// One line a minute at most, per kind. A cluster that has genuinely lost a
// node can be missing a great many objects at once, and a warning per object
// would bury the journal exactly when someone needs to read it. The cumulative
// counter in Status is the measure; the log line is the prompt to go and look.
constexpr auto repair_warning_interval = std::chrono::minutes(1);
constexpr size_t repair_sample_size = 32;
} // namespace

void DistributedStore::note_repair_unsourceable(const ObjectId& id) {
    const auto total = repair_pull_unsourceable_.fetch_add(1, std::memory_order_relaxed) + 1;
    bool warn = false;
    {
        std::lock_guard lock(repair_sample_mutex_);
        if (std::find(repair_unsourceable_sample_.begin(), repair_unsourceable_sample_.end(), id) ==
            repair_unsourceable_sample_.end()) {
            repair_unsourceable_sample_.push_back(id);
            while (repair_unsourceable_sample_.size() > repair_sample_size)
                repair_unsourceable_sample_.pop_front();
        }
        const auto now = Clock::now();
        if (repair_unsourceable_last_log_ == Clock::time_point{} ||
            now - repair_unsourceable_last_log_ >= repair_warning_interval) {
            repair_unsourceable_last_log_ = now;
            warn = true;
        }
    }
    if (warn)
        Log::warn("repair cannot source an object this node should own object=" + to_string(id) +
                  " unsourceable_total=" + std::to_string(total) +
                  "; the live namespace references it and no peer would supply it");
}

void DistributedStore::note_repair_local_unreadable(const ObjectId& id) {
    const auto total = repair_local_unreadable_.fetch_add(1, std::memory_order_relaxed) + 1;
    bool warn = false;
    {
        std::lock_guard lock(repair_sample_mutex_);
        const auto now = Clock::now();
        if (repair_unreadable_last_log_ == Clock::time_point{} ||
            now - repair_unreadable_last_log_ >= repair_warning_interval) {
            repair_unreadable_last_log_ = now;
            warn = true;
        }
    }
    if (warn)
        Log::warn("repair found a local object it cannot read back object=" + to_string(id) +
                  " local_unreadable_total=" + std::to_string(total) +
                  "; the store listed it but the read failed");
}

DistributedStore::RepairDiagnostics DistributedStore::repair_diagnostics() const {
    RepairDiagnostics out;
    out.pull_unsourceable = repair_pull_unsourceable_.load(std::memory_order_relaxed);
    out.local_unreadable = repair_local_unreadable_.load(std::memory_order_relaxed);
    std::lock_guard lock(repair_sample_mutex_);
    out.unsourceable_sample.assign(repair_unsourceable_sample_.begin(),
                                   repair_unsourceable_sample_.end());
    return out;
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
        if (target.id == n_.node_id()) {
            auto resource = n_.data_resources().acquire(
                DataWorkContext(FrameType::speculative, n_.config().extent_size),
                n_.config().extent_size);
            if (!resource)
                return std::nullopt;
            // Unlike has_on()'s local branch, this decides whether a *repair
            // push* target already holds a healthy copy, with no downstream
            // re-verification step -- a corrupt-but-present local object must
            // not be counted as satisfying placement, or it never gets healed.
            return n_.local_store().valid(id);
        }
        if (!reserve_operation() || yielded())
            return std::nullopt;

        Writer writer;
        writer.fixed(id.bytes);
        try {
            // Yield is taken between operations, never inside one: a probe
            // already sent completes (one bounded control round trip).
            auto rpc = n_.call_async(target, MessageType::have_object, writer.data(), FrameType::speculative);
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
        if (target.id == n_.node_id()) {
            auto resource = n_.data_resources().acquire(
                DataWorkContext(FrameType::speculative, data.size()), data.size());
            if (!resource)
                return std::nullopt;
            return n_.local_store().put(id, data);
        }
        if (!reserve_operation() || yielded())
            return std::nullopt;

        Writer writer;
        writer.fixed(id.bytes);
        writer.bytes(data);
        auto started = Clock::now();
        try {
            // One extent in flight is the bounded non-pre-emptible work the
            // laws allow a lower class. Cancelling it on every yield meant a
            // WAN transfer longer than the slice never completed at all.
            auto rpc = n_.call_async(target, MessageType::put_object, writer.data(),
                                     FrameType::speculative);
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
            // "Everywhere" means every node that hosts extents; a non-hosting
            // node fetches a universal object on demand into its cache like
            // anything else.
            auto nodes = everywhere ? hosting_nodes() : ranked(id);
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
                        auto resource = n_.data_resources().acquire(
                            DataWorkContext(FrameType::speculative, n_.config().extent_size),
                            n_.config().extent_size);
                        if (!resource) {
                            retry = true;
                            break;
                        }
                        source = n_.local_store().get(id);
                        if (!source) {
                            note_repair_local_unreadable(id);
                            break;
                        }
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
                !n_.retention_store().retained(RetentionClass::data, id)) {
                auto resource = n_.data_resources().acquire(
                    DataWorkContext(FrameType::speculative, n_.config().extent_size),
                    n_.config().extent_size);
                if (resource)
                    n_.local_store().remove(id);
            }

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
            const bool everywhere = universal && n_.hosts_extents() &&
                                    std::binary_search(universal->begin(), universal->end(), id);
            bool local_valid = false;
            if (everywhere || should_own(id)) {
                auto resource = n_.data_resources().acquire(
                    DataWorkContext(FrameType::speculative, n_.config().extent_size),
                    n_.config().extent_size);
                if (!resource)
                    break;
                // No downstream re-verification follows this decision (unlike
                // has_on()'s callers): a corrupt local copy must not be
                // treated as "already own it, skip the pull", or a locally
                // rotted object this node should own is never re-fetched.
                local_valid = n_.local_store().valid(id);
            }
            if ((!everywhere && !should_own(id)) || local_valid) {
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
                auto resource = n_.data_resources().acquire(
                    DataWorkContext(FrameType::speculative, cached->size()), cached->size());
                if (!resource)
                    break;
                (void)n_.local_store().put(id, *cached);
                repair_pull_after_ = id;
                ++it;
                ++scanned_total;
                ++result.pull_examined;
                continue;
            }

            if (!reserve_operation())
                break;
            // The fetch runs to completion and is kept: until 0.59.0 a yield
            // during it abandoned the transfer, or discarded the bytes once
            // they had arrived, so a node that was never quiet never pulled.
            auto data = get_remote(id, 0, FrameType::speculative, false, false, {}, nullptr);
            if (data) {
                auto resource = n_.data_resources().acquire(
                    DataWorkContext(FrameType::speculative, data->bytes.size()),
                    data->bytes.size());
                if (!resource)
                    break;
                if (n_.local_store().put(id, data->bytes))
                    transferred += data->bytes.size();
            } else {
                // This node should own it, does not have it, the block cache
                // did not have it, and no peer answered with it. After a node
                // leaves the cluster this is how a genuinely unavailable
                // extent presents itself -- and it presented itself silently
                // until 0.40.0. A transient peer or RPC failure lands here
                // too, so this counts rather than concludes.
                note_repair_unsourceable(id);
                ++result.pull_unsourceable;
            }

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
