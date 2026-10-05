// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/distributed_store.hpp"
#include "diagnostics.hpp"
#include "codec.hpp"
#include "durable_file.hpp"
#include "log.hpp"
#include "observation.hpp"
#include "cluster/placement.hpp"
#include "supervised.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <limits>
#include <set>
#include <tuple>

namespace macha {
namespace {
uint64_t since_ms(Clock::time_point since) {
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

    // Wire form: ObjectId + length + bytes. Validate in place, then slide the
    // bytes over the prefix to keep the allocation (Reader::bytes() would copy).
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
    // node/epoch/domain/backend incarnation, so keep only the non-dominated
    // frontier per replica set: sequential publication keeps one requirement.
    for (const auto& existing : requirements)
        if (dominates(existing, next))
            return;
    std::erase_if(requirements,
                  [&](const auto& existing) { return dominates(next, existing); });
    requirements.push_back(std::move(next));
}

void DistributedStore::note_foreground(uint64_t bytes) {
    activity_.note(FrameType::foreground, bytes);
}

void DistributedStore::note_network(uint64_t bytes, Clock::duration duration) {
    auto seconds = std::chrono::duration<double>(duration).count();
    // Background credit is spent an extent at a time, so the estimate must
    // reflect an extent's cost. A transfer under half an extent is dominated
    // by round trip and remote write, not bandwidth, and would understate it.
    if (!bytes || seconds <= 0.0 || bytes < n_.config().extent_size / 2)
        return;
    double sample = static_cast<double>(bytes) / seconds;
    double old = network_bps_.load(std::memory_order_relaxed);
    double next = old > 0.0 ? old * 0.80 + sample * 0.20 : sample;
    network_bps_.store(next, std::memory_order_relaxed);
}

std::chrono::milliseconds DistributedStore::foreground_idle_for() const {
    return activity_.idle_for(FrameType::foreground);
}

std::vector<NodeInfo> DistributedStore::hosting_nodes() const {
    // The one choke point for DATA placement: owners, should_own, retention
    // candidates, repair, prompt replication and rebalance all use ranked(),
    // so filtering here keeps edge and inbound-incapable nodes out of every
    // owner set and fallback order. All peers gossip the same bits, so all
    // compute the same answer.
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
    activity_.note(frame_type, data.size());
    const auto operation_started = Clock::now();

    auto nodes = ranked(id);
    if (nodes.empty())
        return false;
    // The local store is the first replica tried, then placement order, so an
    // offsite writer's first copy, its viewers' reads and its publication
    // rate do not depend on the WAN. Repair converges copies onto the
    // placement owners afterwards.
    if (auto self = std::find_if(nodes.begin(), nodes.end(),
                                 [&](const NodeInfo& node) { return node.id == n_.node_id(); });
        self != nodes.end() && self != nodes.begin()) {
        std::rotate(nodes.begin(), self, self + 1);
    }
    const size_t target = std::min(n_.config().replication, nodes.size());
    // Copies sought before the write returns. One copy is a write; the rest
    // are sought from the nodes present and otherwise left to repair.
    const size_t floor =
        std::max<size_t>(1, std::min(n_.config().write_copies, nodes.size()));
    const size_t need = floor;

    Writer writer;
    writer.fixed(id.bytes);
    writer.bytes(data);
    const auto payload = writer.take();

    struct PendingPut {
        NodeInfo owner;
        std::optional<AsyncRpc> rpc;
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

    // A DATA lease covers work on this node's device. A put to a peer takes
    // none here: the peer admits it against its own device, and a sender
    // holding a slot while it waited would leave two nodes that replicate to
    // each other waiting on each other's slots.
    auto launch = [&](const NodeInfo& owner) {
        if (owner.id == n_.node_id()) {
            auto resource = data_resources_.acquire(
                DataWorkContext(frame_type, data.size(), {}, cancelled), data.size());
            if (!resource) {
                ++replacement_needed;
                return;
            }
            const auto started = Clock::now();
            if (!local_.data().has(id))
                events_.notify(NodeEvent::storage);
            if (batch) {
                if (const auto token = local_.data().put_deferred(id, data)) {
                    ++success;
                    successful_replicas.push_back(
                        {owner.id, n_.durability_epoch(), token->domain, token->generation,
                         token->backend_instance});
                } else {
                    ++replacement_needed;
                }
            } else if (local_.data().put(id, data)) {
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

    // No-progress budget (only when the caller's context carries one). A slow
    // put is not failed: any completion, fallback launch, pending transfer
    // moving bytes, or progress elsewhere in the pipeline (the shared counter)
    // re-arms the window. Only a put where nothing advances for the whole
    // budget fails, retryably, so publication backs off and parks instead of
    // spinning on a silent peer.
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
                           " sought=" + std::to_string(need));
                return finish(success > 0 && !work->cancelled(), success);
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
        size_t stalled = 0;
        for (const auto& item : pending) {
            if (item.done)
                continue;
            ++unfinished;
            if (item.spilled)
                ++stalled;
        }

        // No further copy can be had promptly: what landed is the write. A
        // stalled peer is waited for only while nothing has landed.
        const size_t untried = nodes.size() - next_fallback;
        if (success > 0 && success + (unfinished - stalled) + untried < floor)
            break;
        if (!success && !unfinished && !untried)
            break;

        if (!progressed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return finish(success > 0, success);
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

    // Coalesce by exact process-epoch-bound physical placement. Requests that
    // share a filesystem domain are still group-committed there; the backend
    // incarnation is part of the token so a reopened or replaced backend
    // cannot satisfy an old placement.
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

    // Why each replica did or did not count (peer, epoch, barrier or
    // transport), for the failure line below.
    std::map<ReplicaKey, std::string> outcome;

    // Launch remote waits before blocking on the local domain, aligning batch
    // windows so a publication does not serialise one durability cut per node.
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
            // Still transient (an unsent request is "ask again"), but the
            // reason keeps "could not send" distinct from "did not try".
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
            local_.data().durability_barrier(
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

    // A refusal because a peer's process or backend incarnation changed means
    // "your token is dead", not "your bytes are gone" (discipline 1). Ask
    // again with the object ids; the peer answers from disk with fresh tokens
    // and the batch is re-stamped in place, so the next barrier is ordinary.
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
                if (auto token = local_.data().reassert_durable(requirement.id)) {
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
        // Remote: one probe per peer with every id that named a dead token.
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

    // Unsatisfiable ("re-put this object") only when every failed replica
    // failed definitively (the peer no longer holds the object, or is
    // unknown). A transport failure or a peer mid-restart is "ask again",
    // never "re-send".
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
        // Fast health probes can succeed while a control worker is wedged.
        // Abort this exact route so a retention-before-commit barrier is
        // bounded and releases its pending promise and queue ownership.
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
            // A retention claim says "this node holds the object": index
            // presence, not a read-decrypt-hash, which inside the metadata
            // mutation would hold the store mutex and stall every HTTP request.
            // The bytes were verified when put; scrub finds later corruption.
            // No DATA admission for an index lookup; the remote handler matches.
            const bool present = object_class == RetentionClass::data
                                     ? local_.data().has(id)
                                     : local_.control().has(id);
            if (!present)
                return false;
        }
        local_.retention().retain_batch(object_class, ids, dot);
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

std::vector<ObjectId> DistributedStore::retain_data(const std::vector<ObjectId>& input,
                                                    const RetentionDot& dot) {
    auto ids = input;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::vector<ObjectId> unheld;
    if (ids.empty())
        return unheld;

    // Claims sought per object. A holder that is away is not claimed on: its
    // own head and the deletion grace protect its copy until it returns.
    const size_t floor = std::max<size_t>(1, n_.config().write_copies);

    // Plan claims first, then persist one RetainBatch per selected node, so a
    // multi-extent file is not one fsync/RPC per object. Single-object
    // fallback claims below cover a node vanishing between planning and
    // persistence.
    const auto started = Clock::now();
    std::map<NodeId, NodeInfo> node_info;
    std::map<NodeId, std::vector<ObjectId>> batches;
    std::map<ObjectId, std::vector<NodeInfo>> candidates_by_object;
    for (const auto& id : ids)
        candidates_by_object.emplace(id, ranked(id));

    // Batched, concurrent candidate-presence scan; same preference order as a
    // serial has_on() walk.
    auto selected_by_object = select_present_batched(candidates_by_object, floor);
    const auto scan_ms = since_ms(started);

    // An object the scan found on no node present has nothing to copy from
    // and nobody to claim on: it is unheld, and nothing more is asked about
    // it. One found on fewer nodes than sought is re-placed below.
    std::set<ObjectId> absent;
    std::vector<ObjectId> short_ids;
    for (const auto& id : ids) {
        const auto present = selected_by_object[id].size();
        if (!present)
            absent.insert(id);
        else if (present < floor)
            short_ids.push_back(id);
    }
    const auto short_count = short_ids.size();
    size_t fallback_claims = 0;
    // Where a slow retention barrier spends its time: presence scan,
    // re-replication of short objects, per-node claims, per-object fallbacks.
    size_t under_claimed = 0;
    const auto report = [&] {
        static auto& barrier = observations().histogram("claim.data_barrier_us");
        static auto& barrier_ids = observations().counter("claim.data_barrier.ids");
        static auto& barrier_short = observations().counter("claim.data_barrier.under_claimed");
        barrier.record(elapsed_us(started));
        barrier_ids.fetch_add(ids.size(), std::memory_order_relaxed);
        barrier_short.fetch_add(under_claimed, std::memory_order_relaxed);
        const auto total = since_ms(started);
        if (total >= 250 && Log::enabled(LogLevel::debug))
            Log::debug("DATA retention barrier ids=" + std::to_string(ids.size()) +
                       " nodes=" + std::to_string(batches.size()) + " total_ms=" +
                       std::to_string(total) + " scan_ms=" + std::to_string(scan_ms) +
                       " short=" + std::to_string(short_count) +
                       " fallback_claims=" + std::to_string(fallback_claims) +
                       " under_claimed=" + std::to_string(under_claimed));
    };

    if (!short_ids.empty()) {
        // A metadata-only mutation may be the first touch after an object's
        // old placement disappeared: re-place it where possible before the
        // new claim. Only objects the scan found short take this rare serial
        // path; one that cannot be read now is left to repair.
        std::map<ObjectId, std::vector<NodeInfo>> rescan_candidates;
        for (const auto& id : short_ids) {
            auto data = get(id, 0, FrameType::speculative);
            if (!data || !put(id, *data))
                continue;
            auto candidates = ranked(id);
            candidates_by_object[id] = candidates;
            rescan_candidates.emplace(id, std::move(candidates));
        }
        auto rescanned = select_present_batched(rescan_candidates, floor);
        for (auto& [id, selected] : rescanned)
            selected_by_object[id] = std::move(selected);
    }

    for (const auto& id : ids) {
        for (const auto& candidate : selected_by_object[id]) {
            node_info[candidate.id] = candidate;
            batches[candidate.id].push_back(id);
        }
    }

    // One claim RPC per selected node, in parallel: claims are independent,
    // and each serial round trip would land inside the writer's metadata
    // mutation for every quantum commit.
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
        if (absent.contains(id)) {
            ++under_claimed;
            unheld.push_back(id);
            continue;
        }
        const auto candidates = candidates_by_object.find(id);
        if (candidates == candidates_by_object.end()) {
            ++under_claimed;
            unheld.push_back(id);
            continue;
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
        if (successful.size() < floor)
            ++under_claimed;
        if (successful.empty())
            unheld.push_back(id);
    }
    report();
    return unheld;
}

bool DistributedStore::retain_control(const std::vector<ObjectId>& input,
                                      const RetentionDot& dot) {
    auto ids = input;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids.empty())
        return true;

    const size_t sought = std::max<size_t>(1, n_.config().metadata_write_copies);
    const auto started = Clock::now();
    auto active = n_.membership().active();
    // Local first, then the nearest measured peer: this runs inside the
    // writer's metadata mutation.
    active = order_commit_replicas(active, n_.node_id(), [&](const NodeId& peer) {
        return n_.peer_latency(peer);
    });

    // A retention claim asserts this node holds the object and peer copies
    // derive from ours. An object that cannot be made local now is not
    // claimed here; its holder's own head protects it.
    std::erase_if(ids, [&](const ObjectId& id) { return !ensure_control_local(id); });
    if (ids.empty())
        return true;

    // Ask the peer what it is missing before sending anything, so a commit's
    // cost scales with what changed, not with the namespace's size. A peer
    // that cannot answer is sent everything.
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

    // Puts to a candidate are pipelined, bounded by what the connection holds:
    // a put occupies a writer queue slot until sent and a pending-reply slot
    // until answered, so the limit is the smaller of the two. A quarter of it,
    // because this graph shares the lane with heartbeats, commits and status
    // traffic; taking more than a share of a shared budget overruns it.
    const size_t connection_budget =
        std::min(max_pending_rpc_requests, max_peer_outbound_messages);
    const size_t put_window = std::max<size_t>(1, connection_budget / 4);
    auto put_graph_on = [&](const NodeInfo& target) {
        if (target.id == n_.node_id()) {
            // Not presence-filtered: LocalStore::put refreshes an existing
            // object's physical age, so reachability GC cannot race a commit
            // that reuses an old orphan.
            for (const auto& id : ids) {
                auto bytes = local_.control().get(id);
                if (!bytes || !local_.control().put(id, *bytes))
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
            // Read bytes only for what is sent.
            auto bytes = local_.control().get(id);
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

    // Critical-path CONTROL publication claims on a few nodes, not on
    // membership; background control repair fans out later. The local claim
    // is the one that must hold.
    size_t retained_count = 0;
    size_t tried = 0;
    bool local_retained = false;
    for (const auto& candidate : active) {
        ++tried;
        if (!put_graph_on(candidate))
            continue;
        if (retain_on(candidate, RetentionClass::control, ids, dot)) {
            ++retained_count;
            local_retained = local_retained || candidate.id == n_.node_id();
        }
        if (retained_count >= sought)
            break;
    }
    const auto total_ms = since_ms(started);
    if (total_ms >= 250 && Log::enabled(LogLevel::debug))
        Log::debug("CONTROL retention claim objects=" + std::to_string(ids.size()) +
                   " sought=" + std::to_string(sought) + " tried=" + std::to_string(tried) +
                   " retained=" + std::to_string(retained_count) +
                   " total_ms=" + std::to_string(total_ms));
    return local_retained;
}

DistributedStore::DistributedStore(ClusterNode& n, LocalState& local, ActivityClocks& activity,
                                   DataResourceArbiter& data_resources,
                                   RetainedMemoryLedger& retained_memory, NodeEvents& events,
                                   DistributedStoreOptions options)
    : n_(n), local_(local), activity_(activity), data_resources_(data_resources),
      retained_memory_(retained_memory), events_(events), repair_trace_(std::move(options.repair_trace)) {
    if (options.repair_position) {
        repair_position_path_ = std::move(*options.repair_position);
        std::ifstream in(repair_position_path_);
        uint64_t settled = 0;
        if (in >> settled && settled)
            repair_push_resume_ = settled;
    }
    prompt_thread_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("prompt-replication", stop, [this, stop] { prompt_replication_loop(stop); });
    });
    local_writer_ = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("local-copy-writer", stop, [this, stop] { local_writer_loop(stop); });
    });
}


void DistributedStore::save_repair_position(bool force) {
    if (repair_position_path_.empty() || repair_push_settled_ == repair_position_saved_)
        return;
    const auto now = Clock::now();
    if (!force && now - repair_position_saved_at_ < std::chrono::seconds(30))
        return;
    try {
        std::filesystem::create_directories(repair_position_path_.parent_path());
        durable_replace_file(repair_position_path_, std::to_string(repair_push_settled_));
        repair_position_saved_ = repair_push_settled_;
        repair_position_saved_at_ = now;
    } catch (const std::exception& error) {
        // A lost position costs one pass of re-checking after a restart.
        Log::debug("repair position not saved: " + std::string(error.what()));
    }
}

DistributedStore::~DistributedStore() {
    if (local_writer_.joinable()) {
        local_copy_cancelled_.store(true, std::memory_order_release);
        local_writer_.request_stop();
        local_copy_cv_.notify_all();
        local_writer_.join();
    }
    save_repair_position(true);
    if (prompt_thread_.joinable()) {
        prompt_thread_.request_stop();
        prompt_cv_.notify_all();
        prompt_thread_.join();
    }
}

DistributedStore::PromptReplicationStats DistributedStore::prompt_replication_stats() const {
    PromptReplicationStats out;
    {
        Lock lock(prompt_mutex_);
        out.queued = prompt_queue_.size();
    }
    out.copies = prompt_copies_.load(std::memory_order_relaxed);
    out.failures = prompt_failures_.load(std::memory_order_relaxed);
    return out;
}

void DistributedStore::queue_prompt_replication(const ObjectId& id) {
    {
        Lock lock(prompt_mutex_);
        if (!prompt_queued_.insert(id).second)
            return;
        prompt_queue_.push_back(id);
    }
    prompt_cv_.notify_one();
}

void DistributedStore::prompt_replication_loop(std::stop_token stop) {
    // One object at a time: the local copy is pushed to the first placement
    // owner that lacks it, as speculative DATA work (behind viewers and
    // loaders, counted against the background ceiling). Copies beyond the
    // second are repair's; this loop only closes the single-copy window.
    // Repair is the guarantee, so this sends only where there is room and
    // gives up on an object after a few refusals.
    struct Retry {
        ObjectId id;
        Clock::time_point due;
        unsigned attempts{};
    };
    constexpr unsigned max_attempts = 5;
    std::deque<Retry> retry;
    while (!stop.stop_requested()) {
        std::optional<ObjectId> id;
        unsigned attempts = 0;
        {
            Lock lock(prompt_mutex_);
            prompt_cv_.wait(lock.native(), stop, [&]() MACHA_REQUIRES(prompt_mutex_) {
                return !prompt_queue_.empty() ||
                       (!retry.empty() && retry.front().due <= Clock::now());
            });
            if (stop.stop_requested()) return;
            if (!prompt_queue_.empty()) {
                id = prompt_queue_.front();
                prompt_queue_.pop_front();
                prompt_queued_.erase(*id);
            }
        }
        if (!id) {
            if (!retry.empty() && retry.front().due <= Clock::now()) {
                id = retry.front().id;
                attempts = retry.front().attempts;
                retry.pop_front();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
        }
        const auto retry_later = [&] {
            prompt_failures_.fetch_add(1, std::memory_order_relaxed);
            if (attempts + 1 >= max_attempts) {
                prompt_dropped_.fetch_add(1, std::memory_order_relaxed);
                return; // repair's to finish
            }
            // 30 s, 60 s, 120 s, 240 s.
            const auto delay = std::chrono::seconds(30) * (1u << attempts);
            retry.push_back({*id, Clock::now() + delay, attempts + 1});
            std::sort(retry.begin(), retry.end(),
                      [](const Retry& a, const Retry& b) { return a.due < b.due; });
        };
        try {
            if (!local_.data().has(*id)) continue; // gone (deleted, or never local)
            auto nodes = ranked(*id);
            const size_t target = std::min(n_.config().replication, nodes.size());
            if (target < 2) continue;
            const uint64_t room_needed = std::max<uint64_t>(n_.config().extent_size, 1);
            size_t holders = 1; // the local copy
            bool skipped_full = false;
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
                    continue;
                }
                // Room as the node gossips it; a refused put wastes the link.
                if (node.capacity && node.used + room_needed > node.capacity) {
                    skipped_full = true;
                    continue;
                }
                if (!destination) destination = node;
            }
            if (holders >= 2) continue;
            if (!destination) {
                if (skipped_full) {
                    prompt_skipped_no_room_.fetch_add(1, std::memory_order_relaxed);
                    prompt_dropped_.fetch_add(1, std::memory_order_relaxed);
                }
                continue;
            }
            auto data = local_.data().get(*id);
            if (!data) continue;
            if (put_on(*destination, *id, *data, false)) {
                prompt_copies_.fetch_add(1, std::memory_order_relaxed);
                events_.notify(NodeEvent::storage);
            } else {
                retry_later();
            }
        } catch (const std::exception& error) {
            Log::debug("prompt replication id=" + to_string(*id) + " error=" + error.what());
            retry_later();
        }
        if (retry.size() > 4096) {
            retry.pop_front();
            prompt_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool DistributedStore::put_on(const NodeInfo& target, const ObjectId& id,
                              std::span<const uint8_t> data, bool foreground) {
    const auto frame_type = foreground ? FrameType::foreground : FrameType::speculative;
    if (target.id == n_.node_id()) {
        auto resource = data_resources_.acquire(
            DataWorkContext(frame_type, data.size()), data.size());
        if (!resource)
            return false;
        if (!local_.data().has(id))
            events_.notify(NodeEvent::storage);
        return local_.data().put(id, data);
    }
    // No local lease across the peer's admission (see replicate()).
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
        if (target.id == n_.node_id()) {
            // A lease for this node's device; a fetch from a peer takes none
            // here, since the peer admits the read against its own.
            auto resource = data_resources_.acquire(
                DataWorkContext(frame_type, n_.config().extent_size, deadline, cancelled),
                n_.config().extent_size);
            if (!resource)
                return {};
            auto memory = retained_memory_.acquire(
                object_memory_class(frame_type), MemoryOwner::object_payload,
                n_.config().extent_size, deadline, cancelled);
            if (!memory)
                return {};
            auto bytes = local_.data().get(id);
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
                Lock lock(shared->mutex);
                shared->promote_network = rpc.promotion_callback();
                effective = shared->frame_type;
            }
            if (frame_type_priority(effective) < frame_type_priority(frame_type))
                rpc.promote(effective);
        }
        while (rpc.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready) {
            if (read_aborted(deadline, cancelled, abort)) {
                // Cancellation aborts the RPC only while no other waiter
                // depends on the shared fetch; otherwise the cancelled caller
                // discards the result.
                const bool may_cancel = !shared ||
                    shared->waiters.load(std::memory_order_relaxed) == 0;
                if (!may_cancel)
                    continue;
                rpc.cancel();
                if (shared) {
                    Lock lock(shared->mutex);
                    shared->promote_network = {};
                }
                return {};
            }
        }
        auto reply = rpc.get();
        if (shared) {
            Lock lock(shared->mutex);
            shared->promote_network = {};
        }
        if (reply.message.type != MessageType::object_reply)
            return {};
        auto data = take_object_reply_payload(std::move(reply.message), id);
        note_network(data->bytes.size(), Clock::now() - started);
        return data;
    } catch (const std::exception& e) {
        if (shared) {
            Lock lock(shared->mutex);
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
    // A wall-clock deadline belongs to one caller, so deadline reads use a
    // private transfer whose expiry can abort the RPC. Cancellation-only
    // reads share and dedupe; a stopped caller abandons its wait but does
    // not cancel a transfer others may need.
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
                enqueue_local_copy(id, data->bytes, should_own(id));
        }
        return data;
    }

    std::shared_ptr<SharedFetch> shared;
    bool leader = false;
    {
        Lock lock(fetch_mutex_);
        if (auto it = fetches_.find(id); it != fetches_.end()) {
            shared = it->second.lock();
            if (shared)
                shared->waiters.fetch_add(1, std::memory_order_relaxed);
        }
        if (!shared) {
            shared = std::make_shared<SharedFetch>(foreground, frame_type, opportunistic_persist);
            fetches_[id] = shared;
            leader = true;
        }
    }

    if (!leader) {
        std::function<void(FrameType)> promote_network;
        {
            Lock lock(shared->mutex);
            if (frame_type_priority(frame_type) < frame_type_priority(shared->frame_type)) {
                shared->frame_type = frame_type;
                promote_network = shared->promote_network;
            }
            if (foreground && !shared->foreground) {
                shared->foreground = true;
                if (shared->active_peer && shared->active_class == ReplicaWorkClass::speculative) {
                    replica_selector_.promoted(*shared->active_peer);
                    shared->active_class = ReplicaWorkClass::foreground;
                }
            }
            if (opportunistic_persist)
                shared->opportunistic_persist = true;
        }
        if (promote_network)
            promote_network(frame_type);

        Lock lock(shared->mutex);
        while (!shared->done && !read_aborted(deadline, cancelled, abort))
            shared->cv.wait_for(lock.native(), std::chrono::milliseconds(25));
        shared->waiters.fetch_sub(1, std::memory_order_relaxed);
        if (!shared->done)
            return {};
        if (shared->result) {
            if (foreground && !shared->foreground_accounted) {
                note_foreground(shared->result->bytes.size());
                shared->foreground_accounted = true;
            }
            if (opportunistic_persist && !shared->persist_queued) {
                enqueue_local_copy(id, shared->result->bytes, should_own(id));
                shared->persist_queued = true;
            }
        }
        return shared->result;
    }

    auto finish = [&](ObjectData result) {
        bool account_foreground = false;
        bool queue_persist = false;
        {
            // Hold the fetch-table lock until the result is published: an
            // earlier caller is guaranteed to be a waiter, a later one starts a
            // new fetch only after this transfer completes.
            Lock fetch_lock(fetch_mutex_);
            Lock shared_lock(shared->mutex);
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
            enqueue_local_copy(id, result->bytes, should_own(id));
        shared->cv.notify_all();
        return result;
    };

    auto try_candidates = [&](std::vector<NodeInfo> candidates) -> ObjectData {
        std::erase_if(candidates, [&](const NodeInfo& node) { return node.id == n_.node_id(); });
        size_t attempt = 0;
        while (!candidates.empty() && !read_aborted(deadline, cancelled, abort)) {
            ReplicaWorkClass work;
            {
                Lock lock(shared->mutex);
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
                Lock lock(shared->mutex);
                shared->active_peer = target;
                shared->active_class = shared->foreground ? ReplicaWorkClass::foreground
                                                          : ReplicaWorkClass::speculative;
                replica_selector_.started(target, shared->active_class);
                started = Clock::now();
            }

            FrameType transfer_type;
            {
                Lock lock(shared->mutex);
                transfer_type = shared->frame_type;
            }
            // The transfer belongs to the ObjectId, not its leader: a cancelled
            // leader may abort the RPC only while no other reader has joined.
            auto data = get_from(target, id, transfer_type, shared, {}, cancelled, abort);
            {
                Lock lock(shared->mutex);
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

        // Objects may live on fallback nodes when an owner is full, and on old
        // owners during membership changes.
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
    // Record foreground demand before any storage access, so the first
    // playback read already suppresses lower-priority publication.
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
    auto local_resource = data_resources_.acquire(
        DataWorkContext(frame_type, n_.config().extent_size, deadline, cancelled),
        n_.config().extent_size);
    if (!local_resource)
        return {};
    auto local_memory = retained_memory_.acquire(
        object_memory_class(frame_type), MemoryOwner::object_payload,
        n_.config().extent_size, deadline, cancelled);
    if (!local_memory)
        return {};
    auto local_data = local_.data().get(id);
    local_resource.reset();
    if (auto data = std::move(local_data)) {
        if (interactive)
            activity_.note(frame_type, data->size());
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

    auto cache_resource = data_resources_.acquire(
        DataWorkContext(frame_type, n_.config().extent_size, deadline, cancelled),
        n_.config().extent_size);
    if (!cache_resource)
        return {};
    auto cache_memory = retained_memory_.acquire(
        object_memory_class(frame_type), MemoryOwner::object_payload,
        n_.config().extent_size, deadline, cancelled);
    if (!cache_memory)
        return {};
    auto cache_data = local_.cache().get(id);
    cache_resource.reset();
    if (auto cached = std::move(cache_data)) {
        if (interactive)
            activity_.note(frame_type, cached->size());
        if (should_own(id))
            enqueue_local_copy(id, *cached, true);
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
        activity_.note(frame_type, data->bytes.size());
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
    const std::map<NodeId, std::vector<ObjectId>>& ids_by_node, FrameType frame_type) {
    std::map<NodeId, std::map<ObjectId, bool>> results;

    // Local node: index lookup, no RPC. No DATA admission: it owns no buffer,
    // and a loader lease per id would queue the retention barrier behind the
    // writer's own publications.
    if (auto self = ids_by_node.find(n_.node_id()); self != ids_by_node.end()) {
        auto& out = results[n_.node_id()];
        for (const auto& id : self->second)
            out[id] = local_.data().has(id);
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
                                     frame_type);
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

std::map<NodeId, std::map<ObjectId, bool>> DistributedStore::validated_presence(
    const std::map<NodeId, NodeInfo>& node_info,
    const std::map<NodeId, std::vector<ObjectId>>& ids_by_node) {
    std::map<NodeId, std::map<ObjectId, bool>> results;
    struct InFlight {
        const NodeInfo* node;
        std::vector<ObjectId> ids;
        std::optional<AsyncRpc> rpc;
    };
    std::vector<InFlight> requests;
    for (const auto& [node_id, ids] : ids_by_node) {
        const auto found = node_info.find(node_id);
        if (found == node_info.end())
            continue;
        for (size_t offset = 0; offset < ids.size(); offset += have_valid_objects_max) {
            const auto end = std::min(ids.size(), offset + have_valid_objects_max);
            InFlight request{&found->second,
                             {ids.begin() + static_cast<long>(offset),
                              ids.begin() + static_cast<long>(end)},
                             {}};
            Writer writer;
            writer.u32(static_cast<uint32_t>(request.ids.size()));
            for (const auto& id : request.ids)
                writer.fixed(id.bytes);
            try {
                request.rpc = n_.call_async(found->second, MessageType::have_valid_objects,
                                            writer.data(), FrameType::speculative);
            } catch (const std::exception& error) {
                Log::debug("repair presence batch peer=" + found->second.host +
                           " error=" + error.what());
            }
            requests.push_back(std::move(request));
        }
    }

    const auto one_at_a_time = [&](const NodeInfo& node, const ObjectId& id) {
        Writer writer;
        writer.fixed(id.bytes);
        try {
            auto reply =
                n_.call_async(node, MessageType::have_object, writer.data(), FrameType::speculative)
                    .get();
            if (reply.message.type != MessageType::bool_reply)
                return false;
            Reader reader(reply.message.payload);
            const bool present = reader.u8() != 0;
            reader.finish();
            return present;
        } catch (...) {
            return false;
        }
    };

    const auto deadline = std::max(n_.config().dead_after, n_.config().connect_timeout);
    for (auto& request : requests) {
        auto& out = results[request.node->id];
        bool answered = false;
        bool unknown_message = false;
        if (request.rpc) {
            try {
                if (request.rpc->wait_for(deadline) != std::future_status::ready) {
                    request.rpc->abort();
                } else {
                    auto reply = request.rpc->get();
                    if (reply.message.type == MessageType::have_valid_objects_reply) {
                        Reader reader(reply.message.payload);
                        if (reader.u32() == request.ids.size()) {
                            for (const auto& id : request.ids)
                                out[id] = reader.u8() != 0;
                            reader.finish();
                            answered = true;
                        }
                    } else if (reply.message.type == MessageType::error) {
                        unknown_message = true;
                    }
                }
            } catch (const std::exception& error) {
                Log::debug("repair presence batch peer=" + request.node->host +
                           " error=" + error.what());
            }
        }
        if (answered)
            continue;
        for (const auto& id : request.ids)
            out[id] = unknown_message ? one_at_a_time(*request.node, id) : false;
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
        // Presence-only candidate probe, not the retention commit; no decrypt
        // and no DATA admission for an index lookup.
        return local_.data().has(id);
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

    // Every active node is a metadata/control replica; publication policy
    // decides how many durable acknowledgements are required.
    for (const auto& target : n_.membership().active()) {
        try {
            if (target.id == n_.node_id()) {
                if (local_.control().put(id, data))
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

uint64_t DistributedStore::reachability_epoch() const noexcept {
    return (events_.count(NodeEvent::topology) << 8) + local_.data().online_backends();
}

bool DistributedStore::locally_available(const ObjectId& id) const {
    return local_.data().has(id) || local_.cache().has(id);
}

bool DistributedStore::cache_local(const ObjectId& id, std::span<const uint8_t> data) {
    if (!local_.cache().enabled())
        return false;
    try {
        auto resource = data_resources_.acquire(
            DataWorkContext(FrameType::speculative, data.size()), data.size());
        if (!resource)
            return false;
        return local_.cache().put(id, data);
    } catch (const std::exception& e) {
        Log::debug("cache write-through skipped object=" + to_string(id) + " error=" + e.what());
        return false;
    }
}

bool DistributedStore::hydration_available() const {
    return local_.cache().enabled();
}

bool DistributedStore::hydrate(const ObjectId& id, size_t stripe, FrameType frame_type) {
    if (local_.data().has(id) || local_.cache().has(id))
        return true;
    if (!local_.cache().enabled())
        return false;
    auto data = get_remote(id, stripe, frame_type, false, false);
    if (!data)
        return false;
    auto resource = data_resources_.acquire(
        DataWorkContext(frame_type, data->bytes.size()), data->bytes.size());
    return resource && local_.cache().put(id, data->bytes);
}

bool DistributedStore::ensure_local(const ObjectId& id, bool foreground) {
    const auto local_class = foreground ? FrameType::foreground : FrameType::speculative;
    {
        auto resource = data_resources_.acquire(
            DataWorkContext(local_class, n_.config().extent_size), n_.config().extent_size);
        if (!resource)
            return false;
        if (local_.data().valid(id))
            return true;
    }
    if (auto cached = local_.cache().get(id)) {
        auto resource = data_resources_.acquire(
            DataWorkContext(local_class, cached->size()), cached->size());
        if (resource && local_.data().put(id, *cached))
            return true;
    }
    auto data = get_remote(id, 0,
                           foreground ? FrameType::foreground : FrameType::speculative,
                           foreground, false);
    if (!data)
        return false;
    auto resource = data_resources_.acquire(
        DataWorkContext(local_class, data->bytes.size()), data->bytes.size());
    return resource && local_.data().put(id, data->bytes);
}

bool DistributedStore::ensure_control_local(const ObjectId& id) {
    return local_.control_fetch().pull(
        id, [this](uint64_t bytes, Clock::duration elapsed) { note_network(bytes, elapsed); });
}

void DistributedStore::erase_all(const ObjectId& id) {
    Writer writer;
    writer.fixed(id.bytes);
    for (const auto& target : n_.membership().active()) {
        try {
            if (target.id == n_.node_id()) {
                auto resource = data_resources_.acquire(
                    DataWorkContext(FrameType::speculative, n_.config().extent_size),
                    n_.config().extent_size);
                if (resource && !local_.retention().retained(RetentionClass::data, id))
                    (void)local_.data().remove(id);
                (void)local_.cache().remove(id);
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
        auto step = local_.data().scrub_step(byte_budget ? byte_budget - checked : 0, 256);
        checked += step.bytes;
        if (step.complete || step.yielded)
            break;
    }
    return checked;
}

uint64_t DistributedStore::repair_once(uint64_t byte_budget,
                                      std::optional<std::span<const ObjectId>> live) {
    return repair_step(byte_budget ? byte_budget : std::numeric_limits<uint64_t>::max(), 0, live)
        .bytes_transferred;
}

namespace {
// At most one warning a minute per kind: a lost node can leave many objects
// missing at once. Status's cumulative counter is the measure.
constexpr auto repair_warning_interval = std::chrono::minutes(1);
constexpr size_t repair_sample_size = 32;
} // namespace

void DistributedStore::note_repair_unsourceable(const ObjectId& id) {
    const auto total = repair_pull_unsourceable_.fetch_add(1, std::memory_order_relaxed) + 1;
    bool warn = false;
    {
        Lock lock(repair_sample_mutex_);
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
        Lock lock(repair_sample_mutex_);
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
    out.push_examined = repair_push_examined_total_.load(std::memory_order_relaxed);
    out.pull_examined = repair_pull_examined_total_.load(std::memory_order_relaxed);
    out.bytes_transferred = repair_bytes_total_.load(std::memory_order_relaxed);
    out.passes_completed = repair_passes_completed_.load(std::memory_order_relaxed);
    out.push_phase_complete = repair_push_phase_complete_.load(std::memory_order_relaxed);
    out.gate_ran = repair_gate_ran_.load(std::memory_order_relaxed);
    out.gate_share = repair_gate_share_.load(std::memory_order_relaxed);
    out.gate_quiescent = repair_gate_quiescent_.load(std::memory_order_relaxed);
    out.gate_credit = repair_gate_credit_.load(std::memory_order_relaxed);
    out.last_credit_bytes = repair_last_credit_.load(std::memory_order_relaxed);
    if (const auto last = repair_last_gate_.load(std::memory_order_relaxed); last & 0x100U) {
        out.last_gate = static_cast<RepairGate>((last >> 4) & 0xFU);
        out.paced_by = static_cast<uint8_t>(last & 0xFU);
    }
    const auto prompt = prompt_replication_stats();
    out.prompt_queued = prompt.queued;
    out.prompt_copies = prompt.copies;
    out.prompt_failures = prompt.failures;
    out.prompt_skipped_no_room = prompt_skipped_no_room_.load(std::memory_order_relaxed);
    out.prompt_dropped = prompt_dropped_.load(std::memory_order_relaxed);
    Lock lock(repair_sample_mutex_);
    out.unsourceable_sample.assign(repair_unsourceable_sample_.begin(),
                                   repair_unsourceable_sample_.end());
    return out;
}

DistributedStore::RepairResult
DistributedStore::repair_step(uint64_t byte_budget, size_t operation_budget,
                              std::optional<std::span<const ObjectId>> live,
                              const std::function<bool()>& should_yield,
                              uint64_t live_generation,
                              const std::function<bool(const ObjectId&)>& unavailable) {
    RepairResult result;
    result.complete = false;
    auto& transferred = result.bytes_transferred;
    size_t operations = 0;

    // Cursor-based: never materialises the store's object list or copies the
    // live set per slice, which would make idle repair O(store).
    const std::optional<const ObjectId*> live_identity =
        live ? std::optional<const ObjectId*>(live->data()) : std::nullopt;
    const bool generation_changed =
        live_generation ? repair_live_generation_ != live_generation
                        : repair_live_identity_ != live_identity;
    const auto reset_push_window = [&] {
        repair_push_window_.clear();
        repair_push_presence_.clear();
        repair_push_probed_.clear();
        repair_push_cursor_exhausted_ = false;
    };
    // A new live set does not restart the pass: positions are kept, and a
    // pass the live set changed under is followed at once by another, which
    // covers anything that became live behind a cursor.
    if (generation_changed &&
        (repair_push_settled_ || repair_pull_after_ || !repair_push_window_.empty()))
        repair_pass_spans_change_ = true;
    repair_live_identity_ = live_identity;
    repair_live_generation_ = live_generation;
    if (!live || live->empty())
        repair_pull_complete_ = true;

    auto reset_completed_pass = [&] {
        repair_push_cursor_ = {};
        reset_push_window();
        repair_push_settled_ = 0;
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

    // Bound local/live-set examinations separately from remote operations: a
    // settled object needs no RPC, so the RPC budget alone would not bound a tick.
    const size_t scan_budget = operation_budget
                                   ? std::clamp<size_t>(operation_budget * 4, 16, 64)
                                   : std::numeric_limits<size_t>::max();
    size_t scanned_total = 0;

    // A peer whose advertised free space cannot take an extent is neither
    // probed nor sent one, so a full peer cannot spend the whole operation
    // budget. The copy stays under-replicated, which placement reports.
    const auto has_room = [&](const NodeInfo& peer) {
        return peer.id == n_.node_id() || !peer.capacity ||
               peer.used + n_.config().extent_size <= peer.capacity;
    };

    // Push local replicas toward the current owner set.
    if (!repair_push_complete_) {
        // After a restart, skip the saved number of objects by index (no
        // reads). The store's order can shift, so the pass counts as spanning
        // a change and another follows.
        if (repair_push_resume_) {
            uint64_t skipped = 0;
            while (skipped < *repair_push_resume_) {
                bool pass_complete = false;
                if (!local_.data().next_object(repair_push_cursor_, pass_complete)) {
                    if (pass_complete)
                        repair_push_cursor_exhausted_ = true;
                    break;
                }
                ++skipped;
            }
            repair_push_settled_ = skipped;
            repair_position_saved_ = skipped;
            repair_pass_spans_change_ = true;
            repair_push_resume_.reset();
        }
        while (repair_push_window_.size() < scan_budget && !repair_push_cursor_exhausted_) {
            bool pass_complete = false;
            auto next = local_.data().next_object(repair_push_cursor_, pass_complete);
            if (!next) {
                if (pass_complete)
                    repair_push_cursor_exhausted_ = true;
                break;
            }
            repair_push_window_.push_back(*next);
        }
        if (repair_push_window_.empty() && repair_push_cursor_exhausted_)
            repair_push_complete_ = true;

        // One presence round for every unprobed object in the window,
        // have_valid_objects_max per request, all requests concurrent.
        std::map<NodeId, NodeInfo> probe_nodes;
        std::map<NodeId, std::vector<ObjectId>> probe_ids;
        for (const auto& id : repair_push_window_) {
            if (repair_push_probed_.contains(id))
                continue;
            if (live && !std::binary_search(live->begin(), live->end(), id))
                continue;
            for (const auto& peer : ranked(id)) {
                if (peer.id == n_.node_id() || !has_room(peer))
                    continue;
                probe_nodes[peer.id] = peer;
                probe_ids[peer.id].push_back(id);
            }
        }
        bool probed = true;
        if (!probe_ids.empty()) {
            for (const auto& [_, ids] : probe_ids) {
                const auto requests =
                    (ids.size() + have_valid_objects_max - 1) / have_valid_objects_max;
                for (size_t request = 0; request < requests && probed; ++request)
                    probed = reserve_operation();
                if (!probed)
                    break;
            }
            if (probed && !yielded()) {
                for (const auto& [node, ids] : validated_presence(probe_nodes, probe_ids))
                    for (const auto& [id, present] : ids)
                        repair_push_presence_[{node, id}] = present;
            } else {
                probed = false;
            }
        }
        if (probed) {
            for (const auto& id : repair_push_window_)
                repair_push_probed_.insert(id);
        }

        // Sends go out in batches, all of a batch in flight at once; each
        // object settles when its own sends answer. A batch holds at most
        // repair_sends_in_flight objects and two extents of payload (or one
        // larger object), bounding what an arriving viewer waits behind.
        struct Send {
            ObjectId id;
            NodeInfo peer;
            std::shared_ptr<const Bytes> data;
            std::optional<AsyncRpc> rpc;
        };
        struct Plan {
            ObjectId id;
            bool live{true};
            size_t target{};
            std::set<NodeId> keepers;
        };
        const auto settle = [&](const ObjectId& id) {
            if (auto at = std::find(repair_push_window_.begin(), repair_push_window_.end(), id);
                at != repair_push_window_.end())
                repair_push_window_.erase(at);
            repair_push_probed_.erase(id);
            std::erase_if(repair_push_presence_,
                          [&](const auto& entry) { return entry.first.second == id; });
            ++repair_push_settled_;
            ++scanned_total;
            ++result.push_examined;
        };
        const uint64_t batch_bytes_cap = 2 * std::max<uint64_t>(1, n_.config().extent_size);

        bool push_stopped = !probed;
        while (!push_stopped && !repair_push_window_.empty()) {
            if (yielded())
                break;
            std::vector<Plan> plans;
            std::vector<Send> sends;
            uint64_t batch_bytes = 0;
            const std::vector<ObjectId> window(repair_push_window_.begin(),
                                               repair_push_window_.end());
            for (const auto& id : window) {
                if (sends.size() >= repair_sends_in_flight ||
                    (!sends.empty() && batch_bytes >= batch_bytes_cap))
                    break;
                Plan plan;
                plan.id = id;
                // The physical cursor also sees non-live objects; GC handles them.
                if (live && !std::binary_search(live->begin(), live->end(), id)) {
                    plan.live = false;
                    plans.push_back(std::move(plan));
                    continue;
                }
                const auto nodes = ranked(id);
                plan.target = std::min(n_.config().replication, nodes.size());
                std::shared_ptr<const Bytes> source;
                std::vector<Send> object_sends;
                uint64_t object_bytes = 0;
                bool stop = false;
                bool unreadable = false;
                for (const auto& peer : nodes) {
                    if (plan.keepers.size() + object_sends.size() >= plan.target)
                        break;
                    if (!has_room(peer))
                        continue;
                    bool present = false;
                    if (peer.id == n_.node_id()) {
                        // Index presence: corrupt local copies are found and
                        // removed by scrub and reads, after which repair sees
                        // them absent.
                        present = local_.data().has(id);
                    } else if (auto found = repair_push_presence_.find({peer.id, id});
                               found != repair_push_presence_.end()) {
                        present = found->second;
                    }
                    if (present) {
                        plan.keepers.insert(peer.id);
                        continue;
                    }
                    if (!source) {
                        auto resource = data_resources_.acquire(
                            DataWorkContext(FrameType::speculative, n_.config().extent_size),
                            n_.config().extent_size);
                        if (!resource) {
                            stop = true;
                            break;
                        }
                        auto read = local_.data().get(id);
                        if (!read) {
                            note_repair_local_unreadable(id);
                            unreadable = true;
                            break;
                        }
                        source = std::make_shared<const Bytes>(std::move(*read));
                    }
                    if (peer.id == n_.node_id()) {
                        auto resource = data_resources_.acquire(
                            DataWorkContext(FrameType::speculative, source->size()),
                            source->size());
                        if (!resource) {
                            stop = true;
                            break;
                        }
                        if (local_.data().put(id, *source))
                            plan.keepers.insert(peer.id);
                        continue;
                    }
                    // Credit pays for bytes put on the wire, never for finding
                    // out which are needed.
                    if (transferred + batch_bytes + object_bytes + source->size() > byte_budget) {
                        result.credit_limited = true;
                        stop = true;
                        break;
                    }
                    if (!reserve_operation()) {
                        stop = true;
                        break;
                    }
                    object_sends.push_back(Send{id, peer, source, {}});
                    object_bytes += source->size();
                }
                if (stop) {
                    push_stopped = true;
                    break;
                }
                if (unreadable) {
                    plans.push_back(std::move(plan));
                    continue;
                }
                batch_bytes += object_bytes;
                for (auto& send : object_sends)
                    sends.push_back(std::move(send));
                plans.push_back(std::move(plan));
            }

            // All sends go out before any answer is awaited; each completes and
            // is kept even if repair's turn ends meanwhile.
            const auto dispatched = Clock::now();
            for (auto& send : sends) {
                Writer writer;
                writer.fixed(send.id.bytes);
                writer.bytes(*send.data);
                try {
                    send.rpc = n_.call_async(send.peer, MessageType::put_object, writer.data(),
                                             FrameType::speculative);
                } catch (const std::exception& error) {
                    Log::debug("repair push peer=" + send.peer.host + " error=" + error.what());
                }
            }
            std::set<ObjectId> failed;
            uint64_t delivered = 0;
            for (auto& send : sends) {
                bool ok = false;
                if (send.rpc) {
                    try {
                        ok = send.rpc->get().message.type == MessageType::ok;
                    } catch (...) {
                    }
                }
                trace_repair("push " + to_string(send.id) + " to " + to_string(send.peer.id) +
                             (ok ? " delivered" : " failed"));
                if (!ok) {
                    failed.insert(send.id);
                    continue;
                }
                delivered += send.data->size();
                repair_push_presence_[{send.peer.id, send.id}] = true;
                for (auto& plan : plans)
                    if (plan.id == send.id)
                        plan.keepers.insert(send.peer.id);
            }
            if (delivered) {
                transferred += delivered;
                // One sample for the batch: its sends shared the link.
                note_network(delivered, Clock::now() - dispatched);
            }

            size_t settled = 0;
            for (const auto& plan : plans) {
                if (failed.contains(plan.id))
                    continue; // retried from the same place next step
                if (plan.live && plan.keepers.size() >= plan.target &&
                    !plan.keepers.contains(n_.node_id()) &&
                    !local_.retention().retained(RetentionClass::data, plan.id)) {
                    auto resource = data_resources_.acquire(
                        DataWorkContext(FrameType::speculative, n_.config().extent_size),
                        n_.config().extent_size);
                    if (resource) {
                        local_.data().remove(plan.id);
                        trace_repair("drop-local " + to_string(plan.id));
                    }
                }
                settle(plan.id);
                ++settled;
            }
            if (!failed.empty() || !settled)
                break;
        }
        save_repair_position(false);
        if (repair_push_window_.empty() && repair_push_cursor_exhausted_)
            repair_push_complete_ = true;
    }

    // Pull objects this node should own, walking the ordered live set in place
    // with upper_bound().
    if (!repair_pull_complete_ && live && !live->empty() && !result.credit_limited) {
        auto it = repair_pull_after_
                      ? std::upper_bound(live->begin(), live->end(), *repair_pull_after_)
                      : live->begin();
        while (it != live->end() && scanned_total < scan_budget) {
            if (yielded())
                break;

            const ObjectId id = *it;
            bool local_valid = false;
            if (should_own(id)) {
                // Index lookup, as in the push: reading every held extent would
                // never reach the missing ones. Scrub and reads own corruption.
                local_valid = local_.data().has(id);
            }
            if (!should_own(id) || local_valid) {
                repair_pull_after_ = id;
                ++it;
                ++scanned_total;
                ++result.pull_examined;
                continue;
            }

            // Promote a copy playback already cached before any network I/O,
            // so convergence never downloads twice.
            if (auto cached = local_.cache().get(id)) {
                auto resource = data_resources_.acquire(
                    DataWorkContext(FrameType::speculative, cached->size()), cached->size());
                if (!resource)
                    break;
                (void)local_.data().put(id, *cached);
                trace_repair("pull " + to_string(id) + " from-cache");
                repair_pull_after_ = id;
                ++it;
                ++scanned_total;
                ++result.pull_examined;
                continue;
            }

            if (unavailable && unavailable(id)) {
                static auto& skipped =
                    observations().counter("maintenance.repair.pull_unavailable");
                skipped.fetch_add(1, std::memory_order_relaxed);
                trace_repair("pull " + to_string(id) + " unavailable");
                repair_pull_after_ = id;
                ++it;
                ++scanned_total;
                ++result.pull_examined;
                continue;
            }

            if (transferred + n_.config().extent_size > byte_budget) {
                result.credit_limited = true;
                break;
            }

            if (!reserve_operation())
                break;
            // The fetch runs to completion and is kept even if repair yields,
            // so a node that is never quiet still pulls.
            auto data = get_remote(id, 0, FrameType::speculative, false, false, {}, nullptr);
            if (data) {
                auto resource = data_resources_.acquire(
                    DataWorkContext(FrameType::speculative, data->bytes.size()),
                    data->bytes.size());
                if (!resource)
                    break;
                if (local_.data().put(id, data->bytes))
                    transferred += data->bytes.size();
                trace_repair("pull " + to_string(id) + " fetched");
            } else {
                // Should own it, not here, not cached, and no peer supplied it:
                // possibly an unavailable extent, or a transient peer/RPC
                // failure, so this counts rather than concludes.
                note_repair_unsourceable(id);
                ++result.pull_unsourceable;
                trace_repair("pull " + to_string(id) + " unsourceable");
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
    repair_push_examined_total_.fetch_add(result.push_examined, std::memory_order_relaxed);
    repair_pull_examined_total_.fetch_add(result.pull_examined, std::memory_order_relaxed);
    repair_bytes_total_.fetch_add(result.bytes_transferred, std::memory_order_relaxed);
    repair_push_phase_complete_.store(repair_push_complete_, std::memory_order_relaxed);
    if (repair_push_complete_ && repair_pull_complete_) {
        // A pass the live set changed under may have passed objects before
        // they became live: run another rather than report settled.
        result.complete = !repair_pass_spans_change_;
        repair_pass_spans_change_ = false;
        repair_passes_completed_.fetch_add(1, std::memory_order_relaxed);
        reset_completed_pass();
        save_repair_position(true);
    }
    return result;
}
void DistributedStore::enqueue_local_copy(const ObjectId& id, std::span<const uint8_t> data,
                                          bool promote) {
    const bool cache = local_.cache().enabled();
    if (!cache && !promote)
        return;

    auto memory = retained_memory_.try_acquire(MemoryClass::speculative,
                                               MemoryOwner::object_payload, data.size());
    if (!memory) {
        // Log the drop, so an unfilled cache is distinguishable from a broken one.
        Log::debug("opportunistic persistence skipped object=" + to_string(id) +
                   " reason=retained_memory bytes=" + std::to_string(data.size()) +
                   " cache=" + (cache ? "1" : "0") + " promote=" + (promote ? "1" : "0"));
        return;
    }

    // Opportunistic persistence must not back-pressure playback: when the
    // bounded queue is full the copy is dropped and repair converges later.
    constexpr size_t max_queued_bytes = 256ULL * 1024 * 1024;
    Lock lock(local_copy_mutex_);
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

void DistributedStore::wait_local_copies_settled() {
    Lock lock(local_copy_mutex_);
    local_copy_settled_cv_.wait(lock.native(), [&]() MACHA_REQUIRES(local_copy_mutex_) {
        return local_copies_.empty() && !local_copy_writing_;
    });
}

void DistributedStore::local_writer_loop(std::stop_token stop) {
    ThreadCpuReporter cpu_reporter("macha-local-wr");
    while (true) {
        LocalCopyJob job;
        {
            Lock lock(local_copy_mutex_);
            local_copy_cv_.wait(lock.native(), [&]() MACHA_REQUIRES(local_copy_mutex_) {
                return stop.stop_requested() || !local_copies_.empty();
            });
            if (stop.stop_requested() && local_copies_.empty())
                return;
            job = std::move(local_copies_.front());
            local_copies_.pop_front();
            local_copy_bytes_ -= job.data.size();
            local_copy_writing_ = true;
        }
        struct Settled {
            DistributedStore& store;
            ~Settled() {
                {
                    Lock lock(store.local_copy_mutex_);
                    store.local_copy_writing_ = false;
                }
                store.local_copy_settled_cv_.notify_all();
            }
        } settled{*this};
        auto resource = data_resources_.acquire(
            DataWorkContext(FrameType::speculative, job.data.size(), {}, &local_copy_cancelled_),
            job.data.size());
        if (!resource) {
            if (stop.stop_requested())
                return;
            Log::debug("opportunistic persistence skipped object=" + to_string(job.id) +
                       " reason=data_credit");
            continue;
        }
        bool cached = false;
        if (job.cache) {
            cached = local_.cache().put(job.id, job.data);
            if (!cached)
                Log::debug("opportunistic persistence skipped object=" + to_string(job.id) +
                           " reason=cache_put_failed");
        }
        // With a persistent cache, foreground fetches land on the cache device
        // and HDD promotion is left to idle maintenance. If the cache write
        // fails (or there is no cache), promote here rather than fetch again.
        if (job.promote && (!job.cache || !cached)) {
            auto& local = local_.data();
            (void)local.put(job.id, job.data);
            n_.advertise_storage(local.used(), local.limit());
        }
        cpu_reporter.tick();
    }
}

} // namespace macha
