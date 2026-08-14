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
    return rendezvous_nodes(id.bytes, active, active.size());
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
    bool ok = n_.call(target, MessageType::put_object, writer.data()).message.type == MessageType::ok;
    if (ok)
        note_network(data.size(), Clock::now() - started);
    if (foreground)
        note_foreground(data.size());
    return ok;
}

std::optional<Bytes> DistributedStore::get_from(const NodeInfo& target, const ObjectId& id,
                                                bool foreground) {
    try {
        if (target.id == n_.node_id()) {
            auto data = n_.local_store().get(id);
            if (data && foreground)
                note_foreground(data->size());
            return data;
        }

        Writer writer;
        writer.fixed(id.bytes);
        auto started = Clock::now();
        auto reply = n_.call(target, MessageType::get_object, writer.data());
        if (reply.message.type != MessageType::object_reply)
            return {};
        Reader reader(reply.message.payload);
        ObjectId returned{reader.fixed<32>()};
        auto data = reader.bytes(128 * 1024 * 1024);
        reader.finish();
        if (returned != id || object_id(data) != id)
            throw std::runtime_error("remote integrity failure");
        note_network(data.size(), Clock::now() - started);
        if (foreground)
            note_foreground(data.size());
        return data;
    } catch (const std::exception& e) {
        Log::debug("object read: " + std::string(e.what()));
        return {};
    }
}

std::optional<Bytes> DistributedStore::get_remote(const ObjectId& id, size_t stripe,
                                                  bool foreground,
                                                  bool opportunistic_persist) {
    auto preferred = owners(id);
    if (!preferred.empty())
        std::rotate(preferred.begin(), preferred.begin() + stripe % preferred.size(),
                    preferred.end());

    std::set<NodeId> seen;
    for (const auto& target : preferred) {
        seen.insert(target.id);
        if (target.id == n_.node_id())
            continue;
        if (auto data = get_from(target, id, foreground)) {
            if (opportunistic_persist)
                n_.enqueue_fetched(id, *data, should_own(id));
            return data;
        }
    }

    // Objects may deliberately live on fallback nodes when a preferred owner is
    // full, and may temporarily remain on old owners during membership changes.
    for (const auto& target : ranked(id)) {
        if (target.id == n_.node_id() || seen.contains(target.id))
            continue;
        if (auto data = get_from(target, id, foreground)) {
            if (opportunistic_persist)
                n_.enqueue_fetched(id, *data, should_own(id));
            return data;
        }
    }
    return {};
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

    auto data = get_remote(id, stripe, foreground, foreground);
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

uint64_t DistributedStore::repair_once(uint64_t byte_budget, const std::set<ObjectId>* live) {
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

            auto nodes = ranked(id);
            if (nodes.empty()) {
                ++processed;
                continue;
            }
            const size_t target = std::min(n_.config().replication, nodes.size());
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

            if (keepers.size() >= target && !keepers.contains(n_.node_id()))
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
            if (!should_own(id) || n_.local_store().has(id)) {
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

            auto data = get_remote(id, 0, false, false);
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
