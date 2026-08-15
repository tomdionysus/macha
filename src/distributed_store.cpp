// SPDX-License-Identifier: GPL-3.0-or-later
#include "distributed_store.hpp"
#include "codec.hpp"
#include "log.hpp"
#include "placement.hpp"
#include <algorithm>
#include <chrono>
#include <set>

namespace macha {
namespace {
size_t quorum(size_t n) {
    return n / 2 + 1;
}

int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
        .count();
}
} // namespace

void DistributedStore::note_foreground(uint64_t bytes) {
    foreground_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    last_foreground_ms_.store(steady_ms(), std::memory_order_relaxed);
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
    auto last = last_foreground_ms_.load(std::memory_order_relaxed);
    if (!last)
        return std::chrono::hours(24);
    auto now = steady_ms();
    return std::chrono::milliseconds(std::max<int64_t>(0, now - last));
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

ObjectId DistributedStore::put(std::span<const uint8_t> data) {
    auto started = Clock::now();
    auto id = object_id(data);
    if (!put(id, data))
        throw std::runtime_error("object replication quorum unavailable");
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    Log::debug("DIAG object-put id=" + to_string(id) +
               " bytes=" + std::to_string(data.size()) +
               " ms=" + std::to_string(elapsed.count()));
    return id;
}

bool DistributedStore::put(const ObjectId& id, std::span<const uint8_t> data) {
    if (object_id(data) != id)
        throw std::runtime_error("object hash mismatch");
    note_foreground(data.size());

    auto nodes = ranked(id);
    if (nodes.empty())
        return false;
    const size_t target = std::min(n_.config().replication, nodes.size());

    Writer writer;
    writer.fixed(id.bytes);
    writer.bytes(data);
    const auto payload = writer.take();

    struct PendingPut {
        NodeInfo owner;
        std::optional<AsyncRpc> rpc;
        Clock::time_point started{};
        bool done{};
    };

    std::vector<PendingPut> pending;
    pending.reserve(nodes.size());
    size_t success = 0;
    size_t completed = 0;
    size_t next_fallback = target;

    auto launch = [&](const NodeInfo& owner) {
        if (owner.id == n_.node_id()) {
            ++completed;
            if (n_.local_store().put(id, data))
                ++success;
            return;
        }
        try {
            PendingPut item;
            item.owner = owner;
            item.started = Clock::now();
            item.rpc.emplace(n_.call_async(owner, MessageType::put_object, payload));
            pending.push_back(std::move(item));
        } catch (...) {
            ++completed;
        }
    };

    for (size_t i = 0; i < target; ++i)
        launch(nodes[i]);

    const size_t need = quorum(target);
    if (success >= need)
        return true;

    while (true) {
        bool progressed = false;
        size_t failures = 0;
        for (auto& item : pending) {
            if (item.done || !item.rpc)
                continue;
            if (item.rpc->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                continue;

            item.done = true;
            ++completed;
            progressed = true;
            bool ok = false;
            try {
                ok = item.rpc->get().message.type == MessageType::ok;
            } catch (...) {
            }
            if (ok) {
                ++success;
                note_network(data.size(), Clock::now() - item.started);
            } else {
                ++failures;
            }

            if (success >= need)
                return true;
        }

        while (failures && next_fallback < nodes.size() && success < need) {
            launch(nodes[next_fallback++]);
            --failures;
        }

        size_t unfinished = 0;
        for (const auto& item : pending) {
            if (!item.done)
                ++unfinished;
        }
        if (success + unfinished + (nodes.size() - next_fallback) < need)
            break;

        if (!progressed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return success >= need;
}

bool DistributedStore::put_on(const NodeInfo& target, const ObjectId& id,
                              std::span<const uint8_t> data, bool foreground) {
    if (target.id == n_.node_id())
        return n_.local_store().put(id, data);
    Writer writer;
    writer.fixed(id.bytes);
    writer.bytes(data);
    auto started = Clock::now();
    const auto frame_type = foreground ? FrameType::foreground : FrameType::speculative;
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
                                                const std::shared_ptr<SharedFetch>& shared) {
    try {
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
                                                  bool opportunistic_persist) {
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

        shared->cv.wait(lock, [&] { return shared->done; });
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
        while (!candidates.empty()) {
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
            auto data = get_from(target, id, transfer_type, shared);
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

std::optional<Bytes> DistributedStore::get(const ObjectId& id, size_t stripe, bool foreground) {
    auto started = Clock::now();
    if (auto data = n_.local_store().get(id)) {
        if (foreground)
            note_foreground(data->size());
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        Log::debug("DIAG object-get id=" + to_string(id) +
                   " source=owned bytes=" + std::to_string(data->size()) +
                   " ms=" + std::to_string(elapsed.count()));
        return data;
    }

    if (auto cached = n_.block_cache().get(id)) {
        if (foreground)
            note_foreground(cached->size());
        if (should_own(id))
            n_.enqueue_fetched(id, *cached, true);
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        Log::debug("DIAG object-get id=" + to_string(id) +
                   " source=cache bytes=" + std::to_string(cached->size()) +
                   " ms=" + std::to_string(elapsed.count()));
        return cached;
    }

    auto data = get_remote(id, stripe,
                           foreground ? FrameType::foreground : FrameType::speculative,
                           foreground, foreground);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    Log::debug("DIAG object-get id=" + to_string(id) +
               " source=remote result=" + std::to_string(data ? 1 : 0) +
               " bytes=" + std::to_string(data ? data->size() : 0) +
               " ms=" + std::to_string(elapsed.count()));
    return data;
}

bool DistributedStore::has_on(const NodeInfo& target, const ObjectId& id) {
    if (target.id == n_.node_id())
        return n_.local_store().has(id);
    Writer writer;
    writer.fixed(id.bytes);
    auto reply = n_.call(target, MessageType::have_object, writer.data());
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

bool DistributedStore::locally_available(const ObjectId& id) const {
    return n_.local_store().has(id) || n_.block_cache().has(id);
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
    if (n_.local_store().has(id))
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

void DistributedStore::erase_all(const ObjectId& id) {
    Writer writer;
    writer.fixed(id.bytes);
    for (const auto& target : n_.membership().active()) {
        try {
            if (target.id == n_.node_id()) {
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
    auto ids = n_.local_store().list();
    if (ids.empty()) {
        scrub_offset_ = 0;
        return 0;
    }
    scrub_offset_ %= ids.size();
    std::rotate(ids.begin(), ids.begin() + scrub_offset_, ids.end());

    uint64_t checked = 0;
    size_t processed = 0;
    for (const auto& id : ids) {
        try {
            auto data = n_.local_store().get(id);
            if (data)
                checked += data->size();
        } catch (const std::exception& e) {
            Log::warn("removing corrupt local object " + to_string(id) + ": " + e.what());
            n_.local_store().remove(id);
        }
        ++processed;
        if (byte_budget && checked >= byte_budget)
            break;
    }
    scrub_offset_ = (scrub_offset_ + processed) % ids.size();
    return checked;
}

uint64_t DistributedStore::repair_once(uint64_t byte_budget, const std::set<ObjectId>* live,
                                       const std::set<ObjectId>* universal) {
    uint64_t transferred = 0;

    // First, existing local replicas push toward the current deterministic owner
    // set. Deletion happens only after the target number of good replicas has
    // been confirmed, preserving availability during membership churn.
    auto ids = n_.local_store().list();
    if (live)
        std::erase_if(ids, [&](const ObjectId& id) { return !live->contains(id); });
    if (!ids.empty()) {
        repair_offset_ %= ids.size();
        std::rotate(ids.begin(), ids.begin() + repair_offset_, ids.end());
        size_t processed = 0;
        for (const auto& id : ids) {
            if (byte_budget && transferred >= byte_budget)
                break;
            if (!n_.local_store().has(id)) {
                ++processed;
                continue;
            }

            const bool everywhere = universal && universal->contains(id);
            auto nodes = everywhere ? n_.membership().active() : ranked(id);
            if (nodes.empty()) {
                ++processed;
                continue;
            }
            const size_t target = everywhere ? nodes.size()
                                             : std::min(n_.config().replication, nodes.size());
            std::set<NodeId> keepers;
            std::optional<Bytes> source;

            for (const auto& peer : nodes) {
                if (keepers.size() >= target)
                    break;
                bool present = false;
                try {
                    present = has_on(peer, id);
                } catch (...) {
                }
                if (!present) {
                    if (!source) {
                        source = n_.local_store().get(id);
                        if (!source)
                            break;
                    }
                    if (byte_budget && transferred && transferred + source->size() > byte_budget)
                        break;
                    try {
                        present = put_on(peer, id, *source, false);
                        if (present && peer.id != n_.node_id())
                            transferred += source->size();
                    } catch (...) {
                    }
                }
                if (present)
                    keepers.insert(peer.id);
            }

            if (!everywhere && keepers.size() >= target && !keepers.contains(n_.node_id()))
                n_.local_store().remove(id);
            ++processed;
        }
        repair_offset_ = (repair_offset_ + processed) % ids.size();
    } else {
        repair_offset_ = 0;
    }

    // A newly joined node has no local objects to scan, so push-only repair can
    // never populate it. Walk the committed live set and proactively pull any
    // object for which this node is now a preferred owner. This makes joining a
    // node converge automatically without requiring an old owner to notice it
    // first.
    if (live && !live->empty() && (!byte_budget || transferred < byte_budget)) {
        std::vector<ObjectId> live_ids(live->begin(), live->end());
        pull_offset_ %= live_ids.size();
        std::rotate(live_ids.begin(), live_ids.begin() + pull_offset_, live_ids.end());
        size_t processed = 0;
        for (const auto& id : live_ids) {
            if (byte_budget && transferred >= byte_budget)
                break;
            const bool everywhere = universal && universal->contains(id);
            if ((!everywhere && !should_own(id)) || n_.local_store().has(id)) {
                ++processed;
                continue;
            }

            // Playback may already have fetched this exact object into the
            // persistent non-DHT cache. Promote that copy locally before doing
            // any network I/O, so playback-assisted convergence never requires
            // a second download.
            if (auto cached = n_.block_cache().get(id)) {
                (void)n_.local_store().put(id, *cached);
                ++processed;
                continue;
            }

            auto data = get_remote(id, 0, FrameType::speculative, false, false);
            if (data) {
                if (n_.local_store().put(id, *data))
                    transferred += data->size();
            }
            ++processed;
        }
        pull_offset_ = (pull_offset_ + processed) % live_ids.size();
    } else if (!live || live->empty()) {
        pull_offset_ = 0;
    }

    return transferred;
}
} // namespace macha
