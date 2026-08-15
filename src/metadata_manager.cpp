// SPDX-License-Identifier: GPL-3.0-or-later
#include "metadata_manager.hpp"

#include "codec.hpp"
#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <thread>

namespace macha {
namespace {
size_t quorum(size_t n) {
    return n / 2 + 1;
}

std::vector<NodeId> voters_of(const MetadataRecord& record) {
    return decode_snapshot(record.payload).metadata_voters;
}

bool same_voters(std::vector<NodeId> a, std::vector<NodeId> b) {
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

bool newer_than(const MetadataRecord& a, const MetadataRecord& b) {
    return a.generation > b.generation ||
           (a.generation == b.generation && a.hash > b.hash);
}

struct PendingRead {
    NodeInfo owner;
    std::optional<AsyncRpc> rpc;
    bool done{};
};

struct PendingBool {
    NodeInfo owner;
    std::optional<AsyncRpc> rpc;
    bool done{};
};

struct PendingCas {
    NodeInfo owner;
    std::optional<AsyncRpc> rpc;
    bool done{};
};

bool bool_reply(const RpcReply& reply) {
    if (reply.message.type != MessageType::bool_reply)
        return false;
    Reader reader(reply.message.payload);
    bool ok = reader.u8() != 0;
    reader.finish();
    return ok;
}

std::pair<bool, MetadataRecord> cas_reply(const RpcReply& reply) {
    if (reply.message.type != MessageType::cas_reply)
        return {};
    Reader reader(reply.message.payload);
    bool ok = reader.u8() != 0;
    auto record = decode_metadata_record(reader.bytes());
    reader.finish();
    return {ok, record};
}
} // namespace

MetadataManager::MetadataManager(NodeRuntime& node) : node_(node) {
    static constexpr char label[] = "macha/metadata-placement/v1";
    placement_key_ = sha256({reinterpret_cast<const uint8_t*>(label), sizeof(label) - 1});
}

std::optional<NodeInfo> MetadataManager::node_info(const NodeId& id) const {
    for (const auto& node : node_.membership().all()) {
        if (node.id == id)
            return node;
    }
    return {};
}

std::vector<NodeInfo> MetadataManager::voter_nodes(const std::vector<NodeId>& ids) const {
    std::vector<NodeInfo> out;
    out.reserve(ids.size());
    for (const auto& id : ids) {
        if (auto node = node_info(id))
            out.push_back(*node);
    }
    return out;
}

MetadataRecord MetadataManager::latest(const std::vector<MetadataRecord>& records) const {
    if (records.empty())
        throw std::runtime_error("metadata unavailable");
    auto result = records.front();
    for (const auto& record : records) {
        if (newer_than(record, result))
            result = record;
    }
    return result;
}

MetadataRecord MetadataManager::cache_record(const MetadataRecord& record) {
    {
        std::lock_guard lock(cache_mutex_);
        cache_ = record;
        cache_until_ = Clock::now() + node_.config().metadata_cache;
    }
    return record;
}

std::optional<MetadataRecord> MetadataManager::cached_record() {
    std::lock_guard lock(cache_mutex_);
    if (!cache_ || Clock::now() >= cache_until_)
        return {};
    if (node_.metadata_replica().current().generation > cache_->generation ||
        node_.remote_metadata_generation() > cache_->generation)
        return {};
    return cache_;
}

bool MetadataManager::seed_quorum(const std::vector<NodeInfo>& nodes,
                                  const MetadataRecord& record, size_t required) {
    if (!required)
        return true;

    size_t success = 0;
    size_t completed = 0;
    std::vector<PendingBool> pending;
    pending.reserve(nodes.size());
    const auto encoded = encode_metadata_record(record);

    for (const auto& owner : nodes) {
        if (owner.id == node_.node_id()) {
            ++completed;
            if (node_.seed_metadata(record))
                ++success;
            continue;
        }
        try {
            PendingBool item;
            item.owner = owner;
            item.rpc.emplace(node_.call_async(owner, MessageType::seed_metadata, encoded));
            pending.push_back(std::move(item));
        } catch (...) {
            ++completed;
        }
    }

    if (success >= required)
        return true;
    if (success + (nodes.size() - completed) < required)
        return false;

    while (completed < nodes.size()) {
        bool progressed = false;
        for (auto& item : pending) {
            if (item.done || !item.rpc)
                continue;
            if (item.rpc->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                continue;
            item.done = true;
            ++completed;
            progressed = true;
            try {
                if (bool_reply(item.rpc->get()))
                    ++success;
            } catch (...) {
            }
            if (success >= required)
                return true;
            if (success + (nodes.size() - completed) < required)
                return false;
        }
        if (!progressed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return success >= required;
}


bool MetadataManager::checkpoint_quorum(const std::vector<NodeInfo>& nodes,
                                        const MetadataRecord& record, size_t required) {
    if (!required)
        return true;

    size_t success = 0;
    size_t completed = 0;
    std::vector<PendingBool> pending;
    pending.reserve(nodes.size());
    const auto encoded = encode_metadata_record(record);

    for (const auto& owner : nodes) {
        if (owner.id == node_.node_id()) {
            ++completed;
            if (node_.checkpoint_metadata(record))
                ++success;
            continue;
        }
        try {
            PendingBool item;
            item.owner = owner;
            item.rpc.emplace(
                node_.call_async(owner, MessageType::checkpoint_metadata, encoded));
            pending.push_back(std::move(item));
        } catch (...) {
            ++completed;
        }
    }

    if (success >= required)
        return true;
    if (success + (nodes.size() - completed) < required)
        return false;

    while (completed < nodes.size()) {
        bool progressed = false;
        for (auto& item : pending) {
            if (item.done || !item.rpc)
                continue;
            if (item.rpc->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                continue;
            item.done = true;
            ++completed;
            progressed = true;
            try {
                if (bool_reply(item.rpc->get()))
                    ++success;
            } catch (...) {
            }
            if (success >= required)
                return true;
            if (success + (nodes.size() - completed) < required)
                return false;
        }
        if (!progressed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return success >= required;
}

void MetadataManager::seed_all_best_effort(const std::vector<NodeInfo>& nodes,
                                           const MetadataRecord& record) {
    std::vector<PendingBool> pending;
    pending.reserve(nodes.size());
    const auto encoded = encode_metadata_record(record);

    for (const auto& owner : nodes) {
        if (owner.id == node_.node_id()) {
            (void)node_.checkpoint_metadata(record);
            continue;
        }
        try {
            PendingBool item;
            item.owner = owner;
            item.rpc.emplace(
                node_.call_async(owner, MessageType::checkpoint_metadata, encoded));
            pending.push_back(std::move(item));
        } catch (const std::exception& error) {
            Log::debug("metadata checkpoint " + owner.host + ": " + error.what());
        }
    }

    for (;;) {
        bool pending_work = false;
        bool progressed = false;
        for (auto& item : pending) {
            if (item.done || !item.rpc)
                continue;
            pending_work = true;
            if (item.rpc->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                continue;
            item.done = true;
            progressed = true;
            try {
                if (!bool_reply(item.rpc->get()))
                    Log::debug("metadata checkpoint rejected by " + item.owner.host);
            } catch (const std::exception& error) {
                Log::debug("metadata checkpoint " + item.owner.host + ": " + error.what());
            }
        }
        if (!pending_work)
            return;
        if (!progressed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

MetadataManager::CasResult MetadataManager::cas_quorum(const std::vector<NodeInfo>& nodes,
                                                       const MetadataRecord& expected,
                                                       std::span<const uint8_t> payload,
                                                       size_t required) {
    CasResult result;
    if (!required)
        return result;

    Writer writer;
    writer.u64(expected.generation);
    writer.fixed(expected.hash.bytes);
    writer.bytes(payload);
    const auto encoded = writer.take();

    size_t completed = 0;
    std::vector<PendingCas> pending;
    pending.reserve(nodes.size());

    auto observe = [&](bool ok, const MetadataRecord& record) {
        if (ok) {
            ++result.success;
            if (!result.committed || newer_than(record, *result.committed))
                result.committed = record;
        } else if (record.generation > expected.generation ||
                   (record.generation == expected.generation && record.hash != expected.hash)) {
            result.conflict = true;
        }
    };

    for (const auto& owner : nodes) {
        if (owner.id == node_.node_id()) {
            ++completed;
            MetadataRecord record;
            bool ok = node_.cas_metadata(expected.generation, expected.hash, payload, &record);
            observe(ok, record);
            continue;
        }
        try {
            PendingCas item;
            item.owner = owner;
            item.rpc.emplace(node_.call_async(owner, MessageType::cas_metadata, encoded));
            pending.push_back(std::move(item));
        } catch (...) {
            ++completed;
        }
    }

    if (result.success >= required)
        return result;
    if (result.success + (nodes.size() - completed) < required)
        return result;

    while (completed < nodes.size()) {
        bool progressed = false;
        for (auto& item : pending) {
            if (item.done || !item.rpc)
                continue;
            if (item.rpc->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                continue;
            item.done = true;
            ++completed;
            progressed = true;
            try {
                auto [ok, record] = cas_reply(item.rpc->get());
                observe(ok, record);
            } catch (...) {
            }
            if (result.success >= required)
                return result;
            if (result.success + (nodes.size() - completed) < required)
                return result;
        }
        if (!progressed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return result;
}

MetadataRecord MetadataManager::read_group(const std::vector<NodeId>& voters) {
    if (voters.empty())
        throw std::runtime_error("metadata voter set is empty");
    const size_t need = quorum(voters.size());
    auto nodes = voter_nodes(voters);
    if (nodes.size() < need)
        throw std::runtime_error("metadata read quorum unavailable");

    std::vector<MetadataRecord> matching;
    std::vector<MetadataRecord> transitioned;
    size_t responded = 0;
    size_t completed = 0;
    std::vector<PendingRead> pending;
    pending.reserve(nodes.size());

    auto observe = [&](const MetadataRecord& record) {
        ++responded;
        auto record_voters = voters_of(record);
        if (record_voters.empty() || same_voters(record_voters, voters))
            matching.push_back(record);
        else
            transitioned.push_back(record);
    };

    for (const auto& owner : nodes) {
        if (owner.id == node_.node_id()) {
            ++completed;
            observe(node_.metadata_replica().current());
            continue;
        }
        try {
            PendingRead item;
            item.owner = owner;
            item.rpc.emplace(node_.call_async(owner, MessageType::get_metadata));
            pending.push_back(std::move(item));
        } catch (...) {
            ++completed;
        }
    }

    auto decide = [&]() -> std::optional<MetadataRecord> {
        struct TransitionGroup {
            std::vector<NodeId> voters;
            std::vector<MetadataRecord> records;
        };
        std::vector<TransitionGroup> groups;
        for (const auto& record : transitioned) {
            auto next = voters_of(record);
            std::sort(next.begin(), next.end());
            auto found = std::find_if(groups.begin(), groups.end(), [&](const auto& group) {
                return group.voters == next;
            });
            if (found == groups.end())
                groups.push_back({std::move(next), {record}});
            else
                found->records.push_back(record);
        }
        for (const auto& group : groups) {
            if (group.records.size() >= need) {
                auto successor = latest(group.records);
                node_.seed_metadata(successor);
                node_.checkpoint_metadata(successor);

                // An old-group majority has committed the voter transition.
                // Repair the successor onto a new-group quorum before following
                // it. This also completes a transition interrupted after the
                // old quorum committed but before every new voter was seeded.
                auto next_nodes = voter_nodes(group.voters);
                const size_t next_need = quorum(group.voters.size());
                if (next_nodes.size() < next_need ||
                    !seed_quorum(next_nodes, successor, next_need))
                    throw std::runtime_error("metadata voter transition target quorum unavailable");
                checkpoint_quorum(next_nodes, successor, next_need);
                return read_group(group.voters);
            }
        }

        if (responded < need || matching.empty())
            return {};

        auto current = latest(matching);
        size_t identical = 0;
        for (const auto& record : matching) {
            if (record.hash == current.hash)
                ++identical;
        }

        // Do not read-repair a voter transition from a minority. A transition
        // is followed only when an old-group majority reports the same next
        // group above. Ordinary same-group generations may be safely repaired.
        if (identical < need) {
            auto current_voters = voters_of(current);
            if (!same_voters(current_voters, voters))
                return {};
            if (!seed_quorum(nodes, current, need))
                return {};
        }

        auto policy = decode_snapshot(current.payload);
        if (policy.metadata_voters.empty())
            throw std::runtime_error("metadata voter group lost its configuration");
        if (policy.extent_size != node_.config().extent_size) {
            throw std::runtime_error("cluster extent size does not match local configuration");
        }
        node_.seed_metadata(current);
        node_.checkpoint_metadata(current);
        return current;
    };

    if (auto result = decide())
        return *result;

    while (completed < nodes.size()) {
        bool progressed = false;
        for (auto& item : pending) {
            if (item.done || !item.rpc)
                continue;
            if (item.rpc->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                continue;
            item.done = true;
            ++completed;
            progressed = true;
            try {
                auto reply = item.rpc->get();
                if (reply.message.type == MessageType::metadata_reply)
                    observe(decode_metadata_record(reply.message.payload));
            } catch (...) {
            }
            if (auto result = decide())
                return *result;
        }
        if (responded + (nodes.size() - completed) < need)
            break;
        if (!progressed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (responded < need)
        throw std::runtime_error("metadata read quorum unavailable");
    if (matching.empty())
        throw std::runtime_error("metadata voter configuration conflict");
    throw std::runtime_error("metadata repair quorum unavailable");
}

MetadataRecord MetadataManager::maybe_reconfigure(const MetadataRecord& initial) {
    MetadataRecord current = initial;

    for (size_t attempt = 0; attempt < 8; ++attempt) {
        auto snapshot = decode_snapshot(current.payload);
        const auto old_voters = snapshot.metadata_voters;
        if (old_voters.empty())
            throw std::runtime_error("metadata voter group lost its configuration");
        if (snapshot.extent_size != node_.config().extent_size)
            throw std::runtime_error("cluster extent size does not match local configuration");

        // Replica-policy changes are an offline coordinated operation: every
        // node must be restarted with the same desired values. Transport v6
        // does not advertise desired policy, so mixed rolling configurations
        // cannot be safely reconciled here.
        const size_t old_need = quorum(old_voters.size());
        const size_t target = node_.config().metadata_replication;
        const uint32_t old_data_replication = snapshot.data_replication;
        auto active = node_.membership().active();

        std::set<NodeId> active_ids;
        for (const auto& peer : active)
            active_ids.insert(peer.id);

        std::vector<NodeId> surviving;
        std::vector<NodeInfo> surviving_nodes;
        for (const auto& id : old_voters) {
            if (!active_ids.contains(id))
                continue;
            surviving.push_back(id);
            if (auto owner = node_info(id))
                surviving_nodes.push_back(*owner);
        }
        if (surviving.size() < old_need)
            return current;

        std::vector<NodeId> next;
        if (target == old_voters.size() && surviving.size() == old_voters.size()) {
            next = old_voters;
        } else {
            if (active.size() < target) {
                // Ordinary degraded operation is still allowed while the old
                // voter group has quorum. Reconfiguration waits for enough
                // nodes to restore the configured group size. A deliberate
                // size increase, however, cannot take effect until its target
                // nodes are present.
                if (target == old_voters.size())
                    return current;
                throw std::runtime_error("metadata policy change: need " +
                                         std::to_string(target) + " active nodes, have " +
                                         std::to_string(active.size()));
            }

            if (surviving_nodes.size() > target) {
                auto ranked = rendezvous_nodes(placement_key_.bytes, surviving_nodes, target);
                for (const auto& peer : ranked)
                    next.push_back(peer.id);
            } else {
                next = surviving;
                std::set<NodeId> chosen(next.begin(), next.end());
                std::vector<NodeInfo> candidates;
                for (const auto& peer : active) {
                    if (!chosen.contains(peer.id))
                        candidates.push_back(peer);
                }
                auto ranked = rendezvous_nodes(placement_key_.bytes, candidates,
                                               target - next.size());
                for (const auto& peer : ranked)
                    next.push_back(peer.id);
            }
            std::sort(next.begin(), next.end());
            if (next.size() != target)
                throw std::runtime_error("metadata policy change target voter set unavailable");
        }

        const bool voters_changed = !same_voters(next, old_voters);
        const bool data_changed = snapshot.data_replication != node_.config().replication;
        if (!voters_changed && !data_changed)
            return current;

        auto old_nodes = voter_nodes(old_voters);
        if (old_nodes.size() < old_need)
            return current;

        std::vector<NodeInfo> next_nodes;
        size_t next_need = 0;
        if (voters_changed) {
            next_nodes = voter_nodes(next);
            next_need = quorum(next.size());
            if (next_nodes.size() < next_need)
                throw std::runtime_error("metadata policy change target quorum unavailable");

            // Put the last committed old-group record on enough future voters
            // before changing the configuration. Newly-added voters can then
            // accept/repair the successor immediately after the old quorum CAS.
            if (!seed_quorum(next_nodes, current, next_need))
                throw std::runtime_error("metadata policy change target quorum unavailable");
        }

        snapshot.metadata_voters = next;
        snapshot.data_replication = static_cast<uint32_t>(node_.config().replication);
        auto payload = encode_snapshot(snapshot);

        // The old voter majority serialises the configuration change. This is
        // the authority for both resizing/replacing the metadata group and for
        // changing the stored data-replication policy.
        auto result = cas_quorum(old_nodes, current, payload, old_need);
        if (result.success >= old_need && result.committed) {
            auto committed = *result.committed;
            if (voters_changed) {
                if (!seed_quorum(next_nodes, committed, next_need)) {
                    // The old quorum has already committed this transition. Do
                    // not attempt to roll it back; read_group() will finish
                    // seeding the new quorum when connectivity returns.
                    node_.checkpoint_metadata(committed);
                    throw std::runtime_error(
                        "metadata voter transition committed; target quorum unavailable");
                }
                checkpoint_quorum(next_nodes, committed, next_need);
            } else {
                checkpoint_quorum(old_nodes, committed, old_need);
            }
            node_.checkpoint_metadata(committed);
            seed_all_best_effort(active, committed);
            cache_record(committed);
            if (old_data_replication != node_.config().replication ||
                old_voters.size() != next.size()) {
                Log::info("cluster replication policy changed: data " +
                          std::to_string(old_data_replication) + " metadata " +
                          std::to_string(old_voters.size()) + " -> data " +
                          std::to_string(node_.config().replication) + " metadata " +
                          std::to_string(next.size()));
            } else {
                Log::info("metadata voter group replaced unavailable node(s)");
            }
            return committed;
        }
        if (!result.conflict)
            throw std::runtime_error("metadata policy change quorum unavailable");

        current = read_record_base();
    }

    throw std::runtime_error("metadata policy change conflict");
}

std::optional<MetadataRecord> MetadataManager::recover_from_committed_checkpoints(
    const std::vector<NodeInfo>& active) {
    if (active.size() < 2)
        return {};

    struct Checkpoint {
        NodeInfo owner;
        MetadataRecord record;
    };

    std::vector<Checkpoint> checkpoints;
    std::vector<PendingRead> pending;
    pending.reserve(active.size());
    size_t completed = 0;
    size_t failed = 0;

    for (const auto& owner : active) {
        if (owner.id == node_.node_id()) {
            ++completed;
            checkpoints.push_back({owner, node_.metadata_replica().committed()});
            continue;
        }
        try {
            PendingRead item;
            item.owner = owner;
            item.rpc.emplace(node_.call_async(owner, MessageType::get_committed_metadata));
            pending.push_back(std::move(item));
        } catch (...) {
            ++completed;
            ++failed;
        }
    }

    while (completed < active.size()) {
        bool progressed = false;
        for (auto& item : pending) {
            if (item.done || !item.rpc)
                continue;
            if (item.rpc->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                continue;
            item.done = true;
            ++completed;
            progressed = true;
            try {
                auto reply = item.rpc->get();
                if (reply.message.type != MessageType::metadata_reply) {
                    ++failed;
                    continue;
                }
                checkpoints.push_back(
                    {item.owner, decode_metadata_record(reply.message.payload)});
            } catch (...) {
                ++failed;
            }
        }
        if (!progressed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Recovery is a deliberate replacement operation, not a partition escape
    // hatch. Every member still considered active must participate in the
    // checkpoint survey. Stale member records simply delay recovery until
    // dead_after expires instead of allowing a partitioned cohort to promote
    // itself around an apparently-live peer.
    if (failed || checkpoints.size() != active.size()) {
        Log::debug("metadata recovery waiting for active checkpoint witnesses");
        return {};
    }

    std::vector<Checkpoint> durable;
    std::vector<NodeInfo> fresh;
    for (const auto& checkpoint : checkpoints) {
        if (checkpoint.record.generation <= 1)
            fresh.push_back(checkpoint.owner);
        else
            durable.push_back(checkpoint);
    }
    if (durable.empty() || fresh.empty())
        return {};

    uint64_t highest_generation = 0;
    for (const auto& checkpoint : durable)
        highest_generation = std::max(highest_generation, checkpoint.record.generation);

    std::optional<MetadataRecord> base;
    for (const auto& checkpoint : durable) {
        if (checkpoint.record.generation != highest_generation)
            continue;
        if (!base) {
            base = checkpoint.record;
            continue;
        }
        if (checkpoint.record.hash != base->hash) {
            throw std::runtime_error(
                "metadata recovery conflict: committed checkpoints diverge");
        }
    }
    if (!base)
        return {};

    auto snapshot = decode_snapshot(base->payload);
    const size_t target = snapshot.metadata_voters.size();
    if (!target)
        throw std::runtime_error("metadata recovery checkpoint has no voter group");
    if (active.size() < target)
        return {};
    if (snapshot.extent_size != node_.config().extent_size)
        throw std::runtime_error(
            "metadata recovery checkpoint extent size does not match configuration");

    std::set<NodeId> active_ids;
    for (const auto& owner : active)
        active_ids.insert(owner.id);

    std::vector<NodeId> surviving_voters;
    for (const auto& voter : snapshot.metadata_voters) {
        if (active_ids.contains(voter))
            surviving_voters.push_back(voter);
    }

    const size_t old_need = quorum(snapshot.metadata_voters.size());
    if (surviving_voters.size() >= old_need)
        return {}; // Normal quorum recovery/reconfiguration must win when possible.

    const size_t missing = target - surviving_voters.size();
    std::set<NodeId> old_voter_ids(snapshot.metadata_voters.begin(), snapshot.metadata_voters.end());
    std::vector<NodeInfo> replacements;
    for (const auto& owner : fresh) {
        if (!old_voter_ids.contains(owner.id))
            replacements.push_back(owner);
    }
    if (replacements.size() < missing) {
        Log::debug("metadata recovery waiting for " + std::to_string(missing) +
                   " fresh replacement node(s)");
        return {};
    }

    auto ranked = rendezvous_nodes(placement_key_.bytes, replacements, missing);
    if (ranked.size() != missing)
        return {};

    std::vector<NodeId> next_voters = surviving_voters;
    for (const auto& owner : ranked)
        next_voters.push_back(owner.id);
    std::sort(next_voters.begin(), next_voters.end());

    snapshot.metadata_voters = next_voters;
    auto payload = encode_snapshot(snapshot);
    MetadataRecord recovery;
    recovery.generation = base->generation + 1;
    recovery.previous = base->hash;
    recovery.payload = std::move(payload);
    recovery.hash = metadata_hash(recovery.generation, recovery.previous, recovery.payload);

    std::vector<NodeInfo> next_nodes;
    next_nodes.reserve(next_voters.size());
    for (const auto& id : next_voters) {
        auto found = std::find_if(active.begin(), active.end(),
                                  [&](const auto& owner) { return owner.id == id; });
        if (found == active.end())
            return {};
        next_nodes.push_back(*found);
    }

    // First install the deterministic successor on a new-group quorum without
    // marking it as a committed recovery checkpoint. Only after that quorum is
    // present do we checkpoint the successor. This keeps an interrupted
    // recovery attempt out of the durable witness set used by the next retry.
    if (!seed_quorum(next_nodes, recovery, quorum(next_nodes.size())))
        return {};
    if (!checkpoint_quorum(next_nodes, recovery, quorum(next_nodes.size())))
        return {};

    (void)node_.checkpoint_metadata(recovery);
    seed_all_best_effort(active, recovery);
    Log::info("metadata voter group recovered from committed checkpoint generation " +
              std::to_string(base->generation) + " using " + std::to_string(missing) +
              " fresh replacement node(s)");
    return recovery;
}

MetadataRecord MetadataManager::discover_or_form() {
    auto active = node_.membership().active();

    // Discovery is parallel: a slow/dead peer cannot hold startup behind its
    // socket timeout when another reachable voter already knows the cluster.
    std::vector<MetadataRecord> discovered;
    std::vector<PendingRead> pending;
    size_t completed = 0;
    for (const auto& peer : active) {
        if (peer.id == node_.node_id()) {
            ++completed;
            auto record = node_.metadata_replica().current();
            if (record.generation > 1)
                discovered.push_back(std::move(record));
            continue;
        }
        try {
            PendingRead item;
            item.owner = peer;
            item.rpc.emplace(node_.call_async(peer, MessageType::get_metadata));
            pending.push_back(std::move(item));
        } catch (...) {
            ++completed;
        }
    }

    while (completed < active.size()) {
        bool progressed = false;
        for (auto& item : pending) {
            if (item.done || !item.rpc)
                continue;
            if (item.rpc->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                continue;
            item.done = true;
            ++completed;
            progressed = true;
            try {
                auto reply = item.rpc->get();
                if (reply.message.type == MessageType::metadata_reply) {
                    auto record = decode_metadata_record(reply.message.payload);
                    if (record.generation > 1)
                        discovered.push_back(std::move(record));
                }
            } catch (...) {
            }
        }
        if (!discovered.empty())
            break;
        if (!progressed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!discovered.empty()) {
        std::sort(discovered.begin(), discovered.end(), [](const auto& a, const auto& b) {
            return newer_than(a, b);
        });
        std::reverse(discovered.begin(), discovered.end());

        std::set<std::vector<NodeId>> tried;
        for (const auto& candidate : discovered) {
            auto voters = voters_of(candidate);
            auto sorted = voters;
            std::sort(sorted.begin(), sorted.end());
            if (!tried.insert(sorted).second)
                continue;
            try {
                return cache_record(read_group(voters));
            } catch (const std::exception& error) {
                Log::debug("metadata discovery candidate: " + std::string(error.what()));
            }
        }
        if (auto recovered = recover_from_committed_checkpoints(active))
            return cache_record(*recovered);
        throw std::runtime_error(
            "no discovered metadata voter group has quorum; waiting for replacement recovery");
    }

    if (auto recovered = recover_from_committed_checkpoints(active))
        return cache_record(*recovered);

    const size_t target = node_.config().metadata_replication;
    if (active.size() < target) {
        throw std::runtime_error("metadata group forming: need " + std::to_string(target) +
                                 " active nodes, have " + std::to_string(active.size()));
    }
    // A configured joiner must not invent a new namespace while none of its
    // bootstrap peers are reachable. Once at least one peer is known, genesis
    // can be formed deterministically by the configured voter set; requiring a
    // special bootstrap-less founder would make symmetric bootstrap impossible.
    if (!node_.config().bootstrap.empty() && active.size() == 1) {
        throw std::runtime_error("metadata group forming: waiting for bootstrap peer");
    }

    auto selected = rendezvous_nodes(placement_key_.bytes, active, target);
    std::vector<NodeId> voter_ids;
    voter_ids.reserve(selected.size());
    for (const auto& peer : selected)
        voter_ids.push_back(peer.id);
    std::sort(voter_ids.begin(), voter_ids.end());

    auto genesis = genesis_metadata();
    auto snapshot = decode_snapshot(genesis.payload);
    auto root = snapshot.entries.find("/");
    if (root == snapshot.entries.end())
        throw std::runtime_error("genesis metadata has no filesystem root");
    root->second.uid = node_.config().filesystem.root_uid;
    root->second.gid = node_.config().filesystem.root_gid;
    root->second.mode = node_.config().filesystem.root_mode;
    root->second.ctime_ns = root->second.mtime_ns = wall_time_ns();
    snapshot.metadata_voters = voter_ids;
    snapshot.data_replication = static_cast<uint32_t>(node_.config().replication);
    snapshot.extent_size = node_.config().extent_size;
    auto payload = encode_snapshot(snapshot);

    auto result = cas_quorum(selected, genesis, payload, quorum(target));
    if (result.success >= quorum(target) && result.committed) {
        seed_quorum(selected, *result.committed, quorum(target));
        checkpoint_quorum(selected, *result.committed, quorum(target));
        node_.checkpoint_metadata(*result.committed);
        Log::info("metadata voter group formed with " + std::to_string(target) + " nodes");
        return cache_record(*result.committed);
    }

    if (result.conflict) {
        for (int attempt = 0; attempt < 5; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            auto local = node_.metadata_replica().current();
            auto voters = voters_of(local);
            if (voters.size() == target)
                return cache_record(read_group(voters));
        }
    }

    throw std::runtime_error("metadata voter group formation quorum unavailable");
}

MetadataRecord MetadataManager::read_record_base() {
    auto local = node_.metadata_replica().current();
    auto voters = voters_of(local);
    if (voters.empty())
        return discover_or_form();
    return read_group(voters);
}

MetadataRecord MetadataManager::read_record_uncached() {
    return maybe_reconfigure(read_record_base());
}

MetadataRecord MetadataManager::read_record() {
    if (auto cached = cached_record())
        return *cached;
    try {
        return cache_record(read_record_uncached());
    } catch (const std::exception& error) {
        // Reads may continue from the last durably persisted snapshot when the
        // node is completely isolated. Mutations deliberately do not use this
        // fallback: mutate() calls read_record_uncached() and still requires a
        // metadata quorum, preserving split-brain safety.
        auto local = node_.metadata_replica().current();
        auto voters = voters_of(local);
        if (local.generation > 1 && !voters.empty()) {
            Log::debug("metadata quorum unavailable; using persisted read-only snapshot: " +
                       std::string(error.what()));
            return cache_record(local);
        }
        throw;
    }
}

MetadataSnapshot MetadataManager::snapshot() {
    return decode_snapshot(read_record().payload);
}

MetadataRecord MetadataManager::mutate(const std::function<void(MetadataSnapshot&)>& mutate,
                                       size_t retries) {
    std::lock_guard lock(mutation_mutex_);

    for (size_t attempt = 0; attempt < retries; ++attempt) {
        // Mutations never trust the read cache. They start from a fresh quorum
        // read so a missed invalidation cannot become a conflicting write base.
        auto current = read_record_uncached();
        auto snapshot = decode_snapshot(current.payload);
        auto voters = snapshot.metadata_voters;
        auto data_replication = snapshot.data_replication;
        auto extent_size = snapshot.extent_size;
        mutate(snapshot);
        if (!same_voters(snapshot.metadata_voters, voters))
            throw std::runtime_error("filesystem mutation attempted to change metadata voters");
        if (snapshot.data_replication != data_replication || snapshot.extent_size != extent_size)
            throw std::runtime_error("filesystem mutation attempted to change cluster policy");

        auto payload = encode_snapshot(snapshot);
        if (payload == current.payload)
            return cache_record(current);

        auto nodes = voter_nodes(voters);
        auto result = cas_quorum(nodes, current, payload, quorum(voters.size()));
        if (result.success >= quorum(voters.size()) && result.committed) {
            // A write is durable at quorum now. Remaining replicas are seeded
            // best-effort; foreground mutation latency does not wait for all of
            // them, and read repair/maintenance will converge stragglers.
            seed_quorum(nodes, *result.committed, quorum(voters.size()));
            checkpoint_quorum(nodes, *result.committed, quorum(voters.size()));
            node_.checkpoint_metadata(*result.committed);
            return cache_record(*result.committed);
        }
        if (!result.conflict)
            throw std::runtime_error("metadata write quorum unavailable");
    }

    throw std::runtime_error("metadata mutation conflict");
}

void MetadataManager::repair_once() {
    auto record = read_record_uncached();
    auto voters = voters_of(record);
    auto voter_replicas = voter_nodes(voters);
    seed_quorum(voter_replicas, record, quorum(voters.size()));
    checkpoint_quorum(voter_replicas, record, quorum(voters.size()));

    // Namespace checkpoints are tiny compared with media extents and are
    // intentionally durable on every active node, not only metadata voters.
    // They are recovery witnesses, not extra consensus votes. A replacement
    // node can therefore reconstruct the namespace before the object repair
    // loop walks the recovered live set and repopulates its assigned blocks.
    seed_all_best_effort(node_.membership().active(), record);
    cache_record(record);
}
} // namespace macha
