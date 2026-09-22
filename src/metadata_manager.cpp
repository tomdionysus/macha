// SPDX-License-Identifier: GPL-3.0-or-later
#include "metadata_manager.hpp"
#include "diagnostics.hpp"
#include "placement.hpp"
#include "namespace_control_store.hpp"

#include "codec.hpp"
#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <thread>

namespace macha {
namespace {
bool same_legacy_metadata_voters(std::vector<NodeId> a, std::vector<NodeId> b) {
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

bool bool_reply(const RpcReply& reply) {
    if (reply.message.type != MessageType::bool_reply)
        return false;
    Reader reader(reply.message.payload);
    bool ok = reader.u8() != 0;
    reader.finish();
    return ok;
}

} // namespace

MetadataManager::MetadataManager(NodeRuntime& node) : node_(node) {}

const char* metadata_availability_name(MetadataAvailability availability) noexcept {
    switch (availability) {
    case MetadataAvailability::unavailable:
        return "unavailable";
    case MetadataAvailability::read_only:
        return "read-only";
    case MetadataAvailability::writable:
        return "writable";
    }
    return "unavailable";
}

MetadataClusterStatus MetadataManager::cluster_status() const noexcept {
    MetadataClusterStatus out;
    out.generation = replica_generation_.load(std::memory_order_acquire);
    out.observed_unix_ms = replica_observed_unix_ms_.load(std::memory_order_acquire);
    out.replicas = metadata_replicas_.load(std::memory_order_acquire);
    out.replicas_online = metadata_replicas_online_.load(std::memory_order_acquire);
    out.write_replicas_required = metadata_write_replicas_required_.load(std::memory_order_acquire);
    out.availability = metadata_availability_.load(std::memory_order_acquire);
    out.stable = metadata_replica_set_stable_.load(std::memory_order_acquire);
    out.write_available = metadata_write_available_.load(std::memory_order_acquire);
    return out;
}

void MetadataManager::publish_replica_state(bool validated, std::string_view reason) {
    // Every known node is metadata-capable. The write policy is a durability
    // floor, not a fixed voter set: any metadata_min_write_replicas currently
    // reachable replicas may accept a mutation.
    auto view = available_snapshot_view();
    const auto membership = node_.membership().snapshot();
    const size_t required = node_.config().metadata_min_write_replicas;
    const auto expected_policy = static_cast<uint32_t>(required);
    const size_t known = membership.all.size();
    const bool peer_policy_mismatch = std::any_of(
        membership.active.begin(), membership.active.end(), [&](const NodeInfo& peer) {
            return peer.metadata_write_replicas_required != expected_policy;
        });
    const bool local_policy_mismatch =
        view && view->snapshot->metadata_write_replicas_required &&
        view->snapshot->metadata_write_replicas_required != expected_policy;
    const size_t online = static_cast<size_t>(std::count_if(
        membership.active.begin(), membership.active.end(), [&](const NodeInfo& peer) {
            return peer.metadata_write_replicas_required == expected_policy;
        }));
    const uint64_t generation = view ? view->generation : 0;

    MetadataAvailability next = MetadataAvailability::unavailable;
    if (view) {
        // Replica-set validation describes convergence/stability, not write
        // authority.  A locally committed branch plus the configured number of
        // reachable metadata replicas is enough to attempt a mutation; any
        // divergent peer is reconciled by the mutation/read path itself.
        next = !local_policy_mismatch && online >= required ? MetadataAvailability::writable
                                                            : MetadataAvailability::read_only;
    }

    replica_generation_.store(generation, std::memory_order_release);
    metadata_replicas_.store(static_cast<uint32_t>(known), std::memory_order_release);
    metadata_replicas_online_.store(static_cast<uint32_t>(online), std::memory_order_release);
    metadata_write_replicas_required_.store(static_cast<uint32_t>(required),
                                            std::memory_order_release);
    replica_observed_unix_ms_.store(unix_ms(), std::memory_order_release);
    metadata_write_available_.store(next == MetadataAvailability::writable,
                                    std::memory_order_release);
    metadata_replica_set_stable_.store(validated && !peer_policy_mismatch && !local_policy_mismatch,
                                       std::memory_order_release);

    const auto previous = metadata_availability_.exchange(next, std::memory_order_acq_rel);
    if (previous == next)
        return;

    std::string transition_reason;
    if (next == MetadataAvailability::writable) {
        transition_reason = validated ? "metadata write durability floor available"
                                      : "metadata write durability floor available; reconciliation pending";
    } else if (next == MetadataAvailability::read_only) {
        if (local_policy_mismatch)
            transition_reason = "local metadata write-floor policy mismatch";
        else if (peer_policy_mismatch && online < required)
            transition_reason = "metadata write-floor policy mismatch leaves too few compatible replicas";
        else if (previous == MetadataAvailability::writable)
            transition_reason = "metadata write durability floor lost";
        else if (!reason.empty())
            transition_reason.assign(reason);
        else
            transition_reason = "local metadata state ready; metadata write durability floor unavailable";
    } else if (!reason.empty()) {
        transition_reason.assign(reason);
    } else {
        transition_reason = "coherent metadata unavailable";
    }

    std::string message =
        "metadata availability changed state=" + std::string(metadata_availability_name(next)) +
        " previous=" + metadata_availability_name(previous) +
        " reason=\"" + transition_reason + "\"";
    if (generation) {
        message += " generation=" + std::to_string(generation) +
                   " replicas=" + std::to_string(online) + "/" +
                   std::to_string(known) +
                   " required=" + std::to_string(required);
    }

    if (next == MetadataAvailability::writable ||
        (previous == MetadataAvailability::unavailable &&
         next == MetadataAvailability::read_only)) {
        Log::info(message);
    } else {
        Log::warn(message);
    }
}

std::optional<NodeInfo> MetadataManager::node_info(const NodeId& id) const {
    for (const auto& node : node_.membership().all()) {
        if (node.id == id)
            return node;
    }
    return {};
}

std::vector<NodeInfo> MetadataManager::replica_nodes(const std::vector<NodeId>& ids) const {
    std::vector<NodeInfo> out;
    out.reserve(ids.size());
    for (const auto& id : ids) {
        if (auto node = node_info(id))
            out.push_back(*node);
    }
    return out;
}

std::vector<NodeInfo> MetadataManager::compatible_replicas(
    const std::vector<NodeInfo>& nodes) const {
    const auto required = static_cast<uint32_t>(node_.config().metadata_min_write_replicas);
    std::vector<NodeInfo> out;
    out.reserve(nodes.size());
    for (const auto& peer : nodes) {
        if (peer.metadata_write_replicas_required == required)
            out.push_back(peer);
    }
    return out;
}

void MetadataManager::require_metadata_policy_match(const std::vector<NodeInfo>& nodes) const {
    const auto required = static_cast<uint32_t>(node_.config().metadata_min_write_replicas);
    for (const auto& peer : nodes) {
        if (peer.metadata_write_replicas_required == required)
            continue;
        throw MetadataNotReady(
            "metadata write-floor policy mismatch peer=" + to_string(peer.id) +
            " local=" + std::to_string(required) +
            " peer_required=" + std::to_string(peer.metadata_write_replicas_required));
    }
}

MetadataRecord MetadataManager::cache_record(
    const MetadataRecord& record, std::shared_ptr<const MetadataSnapshot> decoded) {
    // Durable management tombstones are operational constraints as soon as a
    // committed metadata generation is decoded, not merely data for the UI.
    for (const auto& [_, reset] : decoded->identity_resets)
        node_.apply_identity_reset(reset);

    std::lock_guard lock(cache_mutex_);
    // Concurrent replica/local reads can complete out of order. Never let an
    // older completion move the process cache backwards after a newer immutable
    // record has already been observed.
    if (cache_ && newer_than(*cache_, record))
        return *cache_;
    cache_ = record;
    cache_until_ = Clock::now() + node_.config().metadata_cache;
    cache_remote_epoch_ = node_.remote_metadata_epoch();
    if (!decoded_cache_ || decoded_generation_ != record.generation || decoded_hash_ != record.hash) {
        const bool namespace_changed = !decoded_cache_ || decoded_cache_->entries != decoded->entries;
        decoded_cache_ = std::move(decoded);
        decoded_generation_ = record.generation;
        decoded_hash_ = record.hash;
        if (namespace_changed)
            ++decoded_namespace_revision_;
        available_generation_.store(decoded_generation_, std::memory_order_release);
        available_namespace_revision_.store(decoded_namespace_revision_, std::memory_order_release);
    }
    return record;
}

MetadataRecord MetadataManager::cache_record(const MetadataRecord& record) {
    {
        std::lock_guard lock(cache_mutex_);
        if (cache_ && newer_than(*cache_, record))
            return *cache_;
        cache_ = record;
        cache_until_ = Clock::now() + node_.config().metadata_cache;
        cache_remote_epoch_ = node_.remote_metadata_epoch();
        if (decoded_cache_ && decoded_generation_ == record.generation && decoded_hash_ == record.hash)
            return record;
    }

    // Decode only when the canonical record actually changes. Metadata reads can
    // refresh their short metadata cache frequently; rebuilding tens of thousands
    // of FsEntry/extent objects on every getattr was the dominant namespace cost.
    if (auto materialized = node_.metadata_replica().materialized(record.hash);
        materialized && materialized->record.generation == record.generation &&
        materialized->record.payload == record.payload)
        return cache_record(record, materialized->snapshot);
    auto decoded = std::make_shared<const MetadataSnapshot>(decode_snapshot(record.payload));
    return cache_record(record, std::move(decoded));
}

std::optional<MetadataRecord> MetadataManager::cached_record() {
    std::lock_guard lock(cache_mutex_);
    if (!cache_ || Clock::now() >= cache_until_ ||
        cache_remote_epoch_ != node_.remote_metadata_epoch())
        return {};
    if (node_.metadata_replica().committed_generation() > cache_->generation ||
        node_.remote_metadata_generation() > cache_->generation)
        return {};
    return cache_;
}

void MetadataManager::set_namespace_store(DistributedStore* store) {
    namespace_store_ = store;
    if (!store) {
        node_.metadata_replica().set_namespace_delta_applier({});
        return;
    }
    // Replay writes nodes locally and replicates nothing: a history entry
    // being materialised is a commit that already reached the floor when it
    // was made, and re-establishing that here would make rebuilding a local
    // head depend on peers being up. That is the shape of outage this codebase
    // has already had once.
    node_.metadata_replica().set_namespace_delta_applier(
        [this](const ObjectId& root, const MetadataDelta& delta) {
            auto nodes = ControlNamespaceNodeStore::for_replay(node_, *namespace_store_);
            return apply_delta_to_namespace_tree(root, nodes, delta);
        });
}

void require_coherent_namespace(const MetadataSnapshot& snapshot) {
    if (snapshot.namespace_root && !snapshot.entries.empty())
        throw MetadataNotReady("metadata snapshot carries both a namespace root and an entry map; "
                               "the two could disagree about what the namespace is");
}

namespace {
// The view is where the rest of the system gets a namespace, so it is where
// the invariant above is enforced. See require_coherent_namespace.
MetadataSnapshotView coherent(MetadataSnapshotView view) {
    if (view.snapshot)
        require_coherent_namespace(*view.snapshot);
    return view;
}
} // namespace

std::optional<MetadataSnapshotView> MetadataManager::cached_snapshot_view() {
    std::lock_guard lock(cache_mutex_);
    // The decoded snapshot is reusable indefinitely for a specific immutable
    // metadata record, but it is not evidence that the record is still current.
    // Honour the same short TTL as cached_record() so missed generation notices
    // eventually force a replica validation instead of making metadata stale forever.
    if (!cache_ || !decoded_cache_ || Clock::now() >= cache_until_ ||
        cache_remote_epoch_ != node_.remote_metadata_epoch())
        return {};
    if (cache_->generation != decoded_generation_ || cache_->hash != decoded_hash_)
        return {};
    if (node_.metadata_replica().committed_generation() > decoded_generation_ ||
        node_.remote_metadata_generation() > decoded_generation_)
        return {};
    return coherent(MetadataSnapshotView{decoded_generation_, decoded_namespace_revision_,
                                             decoded_hash_, decoded_cache_});
}

bool MetadataManager::import_history_from_peer(const NodeInfo& owner, const Hash256& target,
                                               FrameType frame_type) {
    auto& local = node_.metadata_replica();
    if (local.history_contains(target)) {
        // A prior peer may have supplied a full checkpoint while lacking an
        // older part of its authenticated ancestry. Looking only at the head's
        // direct parent misses a hole below an otherwise continuous suffix and
        // can make a real ancestor appear rootless. Heal only while the accepted
        // heads actually lack a common ancestor, and stop as soon as that proof
        // becomes possible. Traversal uses the resident link index: reading the
        // multi-megabyte checkpoint payload for every historical record caused
        // severe CPU/RSS amplification during the original recovery.
        auto remains_rootless = [&] {
            const auto certificates = local.accepted_head_certificates();
            for (size_t left = 0; left < certificates.size(); ++left) {
                for (size_t right = left + 1; right < certificates.size(); ++right) {
                    if (!local.history_common_ancestor(certificates[left].hash,
                                                       certificates[right].hash))
                        return true;
                }
            }
            return false;
        };
        if (remains_rootless() && owner.id != node_.node_id()) {
            std::vector<Hash256> pending{target};
            std::set<Hash256> inspected;
            while (!pending.empty()) {
                const auto hash = pending.back();
                pending.pop_back();
                if (!inspected.insert(hash).second)
                    continue;
                const auto entry = local.history_links(hash);
                if (!entry)
                    continue;

                std::vector<Hash256> parents = entry->merge_parents;
                if (entry->previous != Hash256{})
                    parents.push_back(entry->previous);
                for (const auto& parent : parents) {
                    if (!local.history_contains(parent)) {
                        (void)import_history_from_peer(owner, parent, frame_type);
                        if (!remains_rootless())
                            return true;
                    }
                    if (local.history_contains(parent))
                        pending.push_back(parent);
                }
            }
        }
        return true;
    }
    if (owner.id == node_.node_id())
        return false;

    struct Task {
        Hash256 hash{};
        bool required{};
        bool loaded{};
        MetadataHistoryEntry entry;
    };

    std::vector<Task> stack;
    std::set<Hash256> active;
    stack.push_back({target, true, false, {}});
    active.insert(target);

    while (!stack.empty()) {
        auto& task = stack.back();
        if (local.history_contains(task.hash)) {
            active.erase(task.hash);
            stack.pop_back();
            continue;
        }

        if (!task.loaded) {
            try {
                Writer request;
                request.fixed(task.hash.bytes);
                auto reply = node_.call(owner, MessageType::get_metadata_history_entry,
                                        request.take(), frame_type);
                if (reply.message.type != MessageType::metadata_history_entry_reply)
                    throw std::runtime_error("metadata history entry unavailable");
                task.entry = decode_metadata_history_entry(reply.message.payload);
                if (task.entry.hash != task.hash)
                    throw std::runtime_error("metadata history reply identity mismatch");
                task.loaded = true;
            } catch (...) {
                const bool required = task.required;
                active.erase(task.hash);
                stack.pop_back();
                if (required)
                    return false;
                continue;
            }

            std::vector<std::pair<Hash256, bool>> dependencies;
            // A full checkpoint is independently materializable. Its previous
            // and merge-parent links are useful ancestry, not transfer
            // prerequisites; recursively pulling them copied gigabytes of
            // unrelated retained history for every newly observed head. A
            // delta alone requires its primary predecessor, so follow exactly
            // that chain until the nearest full checkpoint.
            for (const auto& dependency :
                 metadata_history_materialization_dependencies(task.entry))
                dependencies.emplace_back(dependency, true);

            bool pushed = false;
            for (auto it = dependencies.rbegin(); it != dependencies.rend(); ++it) {
                if (local.history_contains(it->first))
                    continue;
                if (!active.insert(it->first).second) {
                    if (it->second)
                        return false;
                    continue;
                }
                stack.push_back({it->first, it->second, false, {}});
                pushed = true;
            }
            if (pushed)
                continue;
        }

        const bool required = task.required;
        const auto hash = task.hash;
        const auto entry = task.entry;
        active.erase(hash);
        stack.pop_back();
        if (!local.import_history(entry) && required)
            return false;
    }

    return local.history_contains(target);
}

size_t MetadataManager::repair_unreconstructable_heads(FrameType frame_type) {
    auto& local = node_.metadata_replica();
    const auto broken = local.unreconstructable_heads();
    if (broken.empty())
        return 0;
    std::vector<NodeInfo> peers;
    for (auto& peer : compatible_replicas(node_.membership().active()))
        if (peer.id != node_.node_id())
            peers.push_back(std::move(peer));
    if (peers.empty()) {
        Log::debug("metadata head repair: " + std::to_string(broken.size()) +
                   " unreconstructable head(s) flagged but no peer is reachable");
        return 0;
    }

    size_t repaired = 0;
    for (const auto& hash : broken) {
        bool done = false;
        for (const auto& owner : peers) {
            try {
                Writer request;
                request.fixed(hash.bytes);
                auto reply = node_.call(owner, MessageType::get_metadata_history_record,
                                        request.take(), frame_type);
                if (reply.message.type != MessageType::metadata_history_entry_reply) {
                    Log::debug("metadata head repair: " + owner.host + " cannot serve hash=" +
                               hex(hash.bytes));
                    continue;
                }
                auto entry = decode_metadata_history_entry(reply.message.payload);
                if (entry.hash != hash || entry.body != MetadataHistoryEntry::Body::full) {
                    Log::warn("metadata head repair: " + owner.host +
                              " returned a mismatched record for hash=" + hex(hash.bytes));
                    continue;
                }
                if (!local.reanchor_history(entry)) {
                    Log::warn("metadata head repair: record from " + owner.host +
                              " rejected locally hash=" + hex(hash.bytes));
                    continue;
                }
                Log::info("metadata accepted head repaired from peer " + owner.host +
                          " hash=" + hex(hash.bytes) + " generation=" +
                          std::to_string(entry.generation));
                done = true;
                break;
            } catch (const std::exception& error) {
                Log::debug("metadata head repair: " + owner.host + ": " + error.what());
            }
        }
        if (done)
            ++repaired;
    }
    return repaired;
}

bool MetadataManager::push_history_to_peer(const NodeInfo& owner, const Hash256& target,
                                            FrameType frame_type) {
    auto& local = node_.metadata_replica();
    if (owner.id == node_.node_id())
        return local.history_contains(target);
    if (!local.history_contains(target))
        return false;

    std::map<Hash256, bool> remote_presence;
    auto remote_has = [&](const Hash256& hash) {
        if (auto found = remote_presence.find(hash); found != remote_presence.end())
            return found->second;
        bool present = false;
        try {
            Writer request;
            request.fixed(hash.bytes);
            auto reply = node_.call(owner, MessageType::has_metadata_history_entry,
                                    request.take(), frame_type);
            present = bool_reply(reply);
        } catch (...) {
        }
        remote_presence.emplace(hash, present);
        return present;
    };

    struct Task {
        Hash256 hash{};
        bool required{};
        bool expanded{};
        MetadataHistoryEntry entry;
    };

    std::vector<Task> stack;
    std::set<Hash256> active;
    std::set<Hash256> planned;
    std::vector<std::pair<MetadataHistoryEntry, bool>> transfer;
    stack.push_back({target, true, false, {}});
    active.insert(target);

    while (!stack.empty()) {
        auto& task = stack.back();
        if (planned.contains(task.hash) || remote_has(task.hash)) {
            active.erase(task.hash);
            stack.pop_back();
            continue;
        }

        if (!task.expanded) {
            auto entry = local.history_entry(task.hash);
            if (!entry) {
                const bool required = task.required;
                active.erase(task.hash);
                stack.pop_back();
                if (required)
                    return false;
                continue;
            }
            task.entry = *entry;
            task.expanded = true;

            std::vector<std::pair<Hash256, bool>> dependencies;
            // Mirror the pull-side rule: only a delta's primary predecessor is
            // required to materialize it. Full checkpoints are self-contained;
            // their previous and merge-parent links preserve ancestry but are
            // not transfer prerequisites. Walking those optional links here
            // caused peers which already shared the current head to exchange
            // their entire retained history during convergence.
            for (const auto& dependency :
                 metadata_history_materialization_dependencies(*entry))
                dependencies.emplace_back(dependency, true);

            bool pushed = false;
            for (auto it = dependencies.rbegin(); it != dependencies.rend(); ++it) {
                if (planned.contains(it->first) || remote_has(it->first))
                    continue;
                if (!active.insert(it->first).second) {
                    if (it->second)
                        return false;
                    continue;
                }
                stack.push_back({it->first, it->second, false, {}});
                pushed = true;
            }
            if (pushed)
                continue;
        }

        const bool required = task.required;
        const auto hash = task.hash;
        transfer.emplace_back(std::move(task.entry), required);
        planned.insert(hash);
        active.erase(hash);
        stack.pop_back();
    }

    // Requests on one peer retain FIFO execution order at the receiver. Keep a
    // small window in flight so network latency and durable history appends can
    // overlap without allowing an unbounded recovery chain into RPC memory.
    constexpr size_t transfer_window = 8;
    struct PendingTransfer {
        bool required{};
        AsyncRpc rpc;
    };
    std::deque<PendingTransfer> pending;
    history_transfers_.fetch_add(1, std::memory_order_relaxed);

    auto finish_oldest = [&] {
        auto item = std::move(pending.front());
        pending.pop_front();
        try {
            return bool_reply(item.rpc.get()) || !item.required;
        } catch (...) {
            return !item.required;
        }
    };
    auto note_depth = [&] {
        auto peak = history_peak_in_flight_.load(std::memory_order_relaxed);
        while (peak < pending.size() &&
               !history_peak_in_flight_.compare_exchange_weak(
                   peak, pending.size(), std::memory_order_relaxed)) {
        }
    };

    for (auto& [entry, required] : transfer) {
        try {
            auto encoded = encode_metadata_history_entry(entry);
            pending.push_back({required,
                node_.call_async(owner, MessageType::put_metadata_history_entry,
                                 encoded, frame_type)});
            history_entries_submitted_.fetch_add(1, std::memory_order_relaxed);
            note_depth();
        } catch (...) {
            if (required)
                return false;
        }
        if (pending.size() == transfer_window && !finish_oldest())
            return false;
    }
    while (!pending.empty())
        if (!finish_oldest())
            return false;
    return true;
}

MetadataHistoryEntry MetadataManager::commit_history_entry(
    const MetadataRecord& record, std::span<const uint8_t> delta) const {
    if (!valid_metadata_record(record))
        throw std::runtime_error("cannot publish invalid metadata commit");
    MetadataHistoryEntry entry;
    entry.generation = record.generation;
    entry.previous = record.previous;
    entry.hash = record.hash;
    entry.previous_known = record.generation > 1 && record.previous != Hash256{};
    entry.merge_parents = decode_snapshot(record.payload).merge_parents;
    if (!delta.empty()) {
        entry.body = MetadataHistoryEntry::Body::delta;
        entry.payload.assign(delta.begin(), delta.end());
    } else {
        entry.body = MetadataHistoryEntry::Body::full;
        entry.payload.assign(record.payload.begin(), record.payload.end());
    }
    return entry;
}

bool MetadataManager::store_commit_on(const NodeInfo& owner,
                                      const MetadataHistoryEntry& compact,
    const MetadataRecord& record,
                                      FrameType frame_type) {
    if (owner.id == node_.node_id()) {
        if (compact.body != MetadataHistoryEntry::Body::delta)
            return node_.metadata_replica().store_commit(record);
        if (node_.metadata_replica().store_commit(record, compact.payload))
            return true;

        // The immutable full record is already present in the publication
        // proposal. A compact body can be rejected because its parent is absent
        // or because an exact caller supplied a non-reconstructing delta; neither
        // condition should make the local path weaker than a remote replica,
        // which already retries the full body below. This is a single bounded
        // fallback, not a retry loop.
        Log::warn("local metadata delta rejected; retrying full record generation=" +
                  std::to_string(record.generation));
        return node_.metadata_replica().store_commit(record);
    }

    try {
        auto encoded = encode_metadata_history_entry(compact);
        auto reply = node_.call(owner, MessageType::put_metadata_commit, encoded, frame_type);
        if (bool_reply(reply))
            return true;

        // A compact delta may arrive at a perfectly valid replica which simply
        // has not imported its parent yet. Supply that immutable dependency and
        // retry the compact body before considering a full fallback. Otherwise
        // ordinary replica lag turns every reconciliation into another complete
        // namespace snapshot on the lagging node.
        if (compact.body == MetadataHistoryEntry::Body::delta) {
            if (push_history_to_peer(owner, compact.previous, frame_type)) {
                encoded = encode_metadata_history_entry(compact);
                if (bool_reply(
                        node_.call(owner, MessageType::put_metadata_commit, encoded, frame_type)))
                    return true;
            }
            auto full = commit_history_entry(record);
            encoded = encode_metadata_history_entry(full);
            return bool_reply(
                node_.call(owner, MessageType::put_metadata_commit, encoded, frame_type));
        }
    } catch (const std::exception& error) {
        Log::debug("metadata commit store " + owner.host + ": " + error.what());
    }
    return false;
}

bool MetadataManager::accept_commit_on(const NodeInfo& owner,
                                       const MetadataAcceptance& acceptance,
                                       FrameType frame_type) {
    // NodeRuntime owns accepted-head change detection and notification for the
    // local path. Do not add a second publication-level announcement after
    // this returns: repeated evidence is intentionally a no-op there.
    if (owner.id == node_.node_id())
        return node_.accept_metadata_commit(acceptance);
    try {
        const auto encoded = encode_metadata_acceptance(acceptance);
        return bool_reply(
            node_.call(owner, MessageType::accept_metadata_commit, encoded, frame_type));
    } catch (const std::exception& error) {
        Log::debug("metadata acceptance " + owner.host + ": " + error.what());
        return false;
    }
}

size_t MetadataManager::acceptance_floor_for(const MetadataRecord& record) const {
    const auto snapshot_floor = [](const MetadataSnapshot& snapshot) -> size_t {
        if (snapshot.metadata_write_replicas_required)
            return snapshot.metadata_write_replicas_required;
        if (!snapshot.metadata_voters.empty())
            return snapshot.metadata_voters.size() / 2 + 1;
        return 0;
    };

    const auto current = decode_snapshot(record.payload);
    size_t required = snapshot_floor(current);
    if (!required)
        throw std::runtime_error("protocol-20 metadata commit has no write-floor policy");

    // Policy transitions are certified at the strongest policy visible on any
    // parent edge. This matters for lowering the floor and for merge commits
    // which reconcile a policy-transition branch with an older sibling.
    for (const auto& parent_hash : metadata_record_parents(record)) {
        auto parent = node_.metadata_replica().materialized(parent_hash);
        if (!parent)
            throw MetadataNotReady("metadata commit parent unavailable for policy validation");
        required = std::max(required, snapshot_floor(*parent->snapshot));
    }
    return required;
}

MetadataManager::PublishedCommit MetadataManager::publish_commit(
    const std::vector<NodeInfo>& nodes, const MetadataRecord& record,
    std::span<const uint8_t> delta, FrameType frame_type) {
    const size_t required = acceptance_floor_for(record);
    auto compatible = compatible_replicas(nodes);
    if (compatible.size() < required)
        throw MetadataNotReady("metadata write durability floor unavailable: too few policy-compatible replicas");

    const auto compact = commit_history_entry(record, delta);
    PublishedCommit out;
    out.record = record;

    auto ordered = order_commit_replicas(compatible, node_.node_id(), [&](const NodeId& peer) {
        return node_.peer_latency(peer);
    });

    // Store the immutable commit independently on the nearest available
    // registered replicas until the configured durability floor is reached. A
    // receiver never compares it with its current head; it validates the commit
    // and durably appends it to the DAG. The caller's local replica is required
    // to participate so the operation can immediately continue from the commit.
    const bool trace = Log::enabled(LogLevel::debug);
    for (const auto& owner : ordered) {
        const auto started = Clock::now();
        const bool stored = store_commit_on(owner, compact, record, frame_type);
        if (stored)
            out.stored_on.push_back(owner);
        if (trace && owner.id != node_.node_id()) {
            const auto ms = elapsed_ms(started);
            if (ms >= 250 || !stored)
                Log::debug("metadata commit store replica=" + owner.host +
                           " stored=" + (stored ? "yes" : "no") + " ms=" + std::to_string(ms) +
                           " generation=" + std::to_string(record.generation));
        }
        if (out.stored_on.size() >= required)
            break;
    }
    if (out.stored_on.size() < required)
        throw MetadataNotReady("metadata commit durability floor unavailable");
    if (std::none_of(out.stored_on.begin(), out.stored_on.end(), [&](const NodeInfo& owner) {
            return owner.id == node_.node_id();
        }))
        throw std::runtime_error("local metadata commit store failed");

    out.acceptance.generation = record.generation;
    out.acceptance.hash = record.hash;
    out.acceptance.required = static_cast<uint32_t>(required);
    out.acceptance.replicas.reserve(out.stored_on.size());
    for (const auto& owner : out.stored_on)
        out.acceptance.replicas.push_back(owner.id);
    std::sort(out.acceptance.replicas.begin(), out.acceptance.replicas.end());
    out.acceptance.replicas.erase(
        std::unique(out.acceptance.replicas.begin(), out.acceptance.replicas.end()),
        out.acceptance.replicas.end());

    // Acceptance is evidence about the already-completed immutable stores, not
    // a second consensus decision. Before installing the certificate, make the
    // commit's parent policy/history available on each holder. This is required
    // to validate a lowering transition and means every certificate holder can
    // later reconstruct the branch rather than possessing an opaque head only.
    const auto parents = metadata_record_parents(record);
    size_t accepted = 0;
    bool local_accepted = false;
    for (const auto& owner : out.stored_on) {
        bool ancestry_ready = true;
        if (owner.id != node_.node_id()) {
            for (const auto& parent : parents) {
                if (!push_history_to_peer(owner, parent, frame_type)) {
                    ancestry_ready = false;
                    break;
                }
            }
        }
        const auto accept_started = Clock::now();
        const bool accepted_here =
            ancestry_ready && accept_commit_on(owner, out.acceptance, frame_type);
        if (accepted_here) {
            ++accepted;
            local_accepted = local_accepted || owner.id == node_.node_id();
        }
        if (trace && owner.id != node_.node_id()) {
            const auto ms = elapsed_ms(accept_started);
            if (ms >= 250 || !accepted_here)
                Log::debug("metadata commit accept replica=" + owner.host +
                           " accepted=" + (accepted_here ? "yes" : "no") +
                           " ancestry_ready=" + (ancestry_ready ? "yes" : "no") +
                           " ms=" + std::to_string(ms) +
                           " generation=" + std::to_string(record.generation));
        }
    }
    if (!local_accepted)
        throw std::runtime_error("local metadata acceptance persistence failed");
    if (accepted < required)
        throw MetadataNotReady("metadata acceptance certificate durability floor unavailable");

    return out;
}

std::vector<std::pair<NodeInfo, MetadataAcceptance>> MetadataManager::discover_accepted_heads(
    const std::vector<NodeInfo>& nodes, FrameType frame_type) {
    std::vector<std::pair<NodeInfo, MetadataAcceptance>> out;
    for (const auto& owner : nodes) {
        try {
            std::vector<MetadataAcceptance> heads;
            if (owner.id == node_.node_id()) {
                heads = node_.metadata_heads();
            } else {
                auto reply = node_.call(owner, MessageType::get_metadata_heads, {}, frame_type);
                if (reply.message.type != MessageType::metadata_heads_reply)
                    continue;
                heads = decode_metadata_acceptance_set(reply.message.payload);
            }
            for (auto& head : heads)
                out.emplace_back(owner, std::move(head));
        } catch (const std::exception& error) {
            Log::debug("metadata head survey " + owner.host + ": " + error.what());
        }
    }
    return out;
}

bool MetadataManager::replicate_accepted_head(const NodeInfo& owner,
                                              const MetadataRecord& record,
                                              const MetadataAcceptance& acceptance,
                                              FrameType frame_type) {
    if (owner.id == node_.node_id()) {
        if (!node_.metadata_replica().store_commit(record))
            return false;
        return node_.accept_metadata_commit(acceptance);
    }
    if (!push_history_to_peer(owner, record.hash, frame_type)) {
        // The local history may have been compactly rooted at this accepted
        // record. A full immutable commit is sufficient for an arbitrary fresh
        // replica even when earlier ancestry is not locally materialised.
        if (!store_commit_on(owner, commit_history_entry(record), record, frame_type))
            return false;
    }
    return accept_commit_on(owner, acceptance, frame_type);
}

void MetadataManager::ensure_accepted_head_durable(
    const std::vector<NodeInfo>& nodes, const MetadataRecord& record, size_t required,
    FrameType frame_type) {
    auto acceptance = node_.metadata_replica().acceptance(record.hash);
    if (!acceptance)
        throw MetadataNotReady("metadata mutation is visible locally without acceptance certificate");
    if (acceptance->required != acceptance_floor_for(record))
        throw MetadataNotReady("metadata acceptance certificate policy mismatch");

    const auto compatible = compatible_replicas(nodes);
    if (compatible.size() < required)
        throw MetadataNotReady("metadata acceptance certificate durability floor unavailable");

    size_t durable = 0;
    for (const auto& owner : compatible) {
        if (replicate_accepted_head(owner, record, *acceptance, frame_type))
            ++durable;
        if (durable >= required)
            break;
    }
    if (durable < required)
        throw MetadataNotReady("metadata acceptance certificate durability floor unavailable");
}

std::optional<std::vector<std::pair<NodeInfo, MetadataAcceptance>>>
MetadataManager::discover_accepted_heads_required(const std::vector<NodeInfo>& nodes,
                                                   FrameType frame_type) {
    std::vector<std::pair<NodeInfo, MetadataAcceptance>> out;
    for (const auto& owner : nodes) {
        try {
            std::vector<MetadataAcceptance> heads;
            if (owner.id == node_.node_id()) {
                heads = node_.metadata_heads();
            } else {
                auto reply = node_.call(owner, MessageType::get_metadata_heads, {}, frame_type);
                if (reply.message.type != MessageType::metadata_heads_reply)
                    return std::nullopt;
                heads = decode_metadata_acceptance_set(reply.message.payload);
            }
            for (auto& head : heads)
                out.emplace_back(owner, std::move(head));
        } catch (const std::exception& error) {
            Log::debug("metadata head survey (required) " + owner.host + ": " + error.what());
            return std::nullopt;
        }
    }
    return out;
}

bool MetadataManager::propose_history_floor_on(const NodeInfo& owner,
                                               const HistoryCheckpointProof& proposal,
                                               FrameType frame_type) {
    if (owner.id == node_.node_id())
        return node_.metadata_replica().record_checkpoint_ack(proposal);
    try {
        const auto encoded = encode_history_checkpoint_proof(proposal);
        return bool_reply(
            node_.call(owner, MessageType::propose_history_floor, encoded, frame_type));
    } catch (const std::exception& error) {
        Log::debug("history checkpoint proposal " + owner.host + ": " + error.what());
        return false;
    }
}

bool MetadataManager::commit_history_floor_on(const NodeInfo& owner, const Hash256& floor_hash,
                                              const Hash256& epoch, FrameType frame_type) {
    if (owner.id == node_.node_id())
        return node_.metadata_replica().record_checkpoint_commit(floor_hash, epoch);
    try {
        HistoryCheckpointProof commit_message;
        commit_message.floor_hash = floor_hash;
        commit_message.epoch = epoch;
        commit_message.status = HistoryCheckpointProof::Status::committed;
        const auto encoded = encode_history_checkpoint_proof(commit_message);
        return bool_reply(
            node_.call(owner, MessageType::commit_history_floor, encoded, frame_type));
    } catch (const std::exception& error) {
        Log::debug("history checkpoint commit " + owner.host + ": " + error.what());
        return false;
    }
}

void MetadataManager::attempt_history_checkpoint(size_t record_threshold,
                                                 uint64_t byte_threshold) {
    // The round reads and depends on accepted-head/committed state exactly
    // like read_group()/repair_once() do, and repair_once() already holds
    // mutation_mutex_ across its own full network round trip. Match that
    // shape: serialise against a concurrent foreground mutation for the
    // whole round rather than just the final local compaction step.
    std::unique_lock mutation_lock(mutation_mutex_);

    const auto diagnostics = node_.metadata_replica().diagnostics();
    if (diagnostics.history_records < record_threshold &&
        diagnostics.history_file_bytes < byte_threshold)
        return;
    if (node_.metadata_replica().recovery_required())
        return;

    // Gate: only ever attempt this with exactly one local accepted head, and
    // only once every durably-known participant is currently, directly
    // reachable -- mirrors the one other irreversible, cluster-wide-consensus
    // decision this codebase already makes this way (destructive object GC).
    auto local_heads = node_.metadata_replica().accepted_heads();
    if (local_heads.size() != 1)
        return;
    if (!node_.membership().all_known_reachable())
        return;
    const auto floor_hash_candidate = local_heads.front().hash;

    auto participants = node_.membership().all();
    if (participants.empty())
        return;
    std::sort(participants.begin(), participants.end(),
              [](const NodeInfo& a, const NodeInfo& b) { return a.id < b.id; });

    // epoch fingerprints exactly this participant set. A membership change
    // mid-round changes the epoch on the next attempt, which invalidates any
    // in-flight proposal automatically -- no separate membership-version
    // bookkeeping needed.
    Writer epoch_writer;
    for (const auto& participant : participants)
        epoch_writer.fixed(participant.id.bytes);
    const auto epoch = sha256(epoch_writer.take());

    // Survey: require every participant to answer, and every participant to
    // report exactly one accepted head, identical to this node's own.
    auto surveyed = discover_accepted_heads_required(participants, FrameType::control);
    if (!surveyed)
        return;
    std::map<NodeId, std::vector<MetadataAcceptance>> by_node;
    for (auto& [owner, acceptance] : *surveyed)
        by_node[owner.id].push_back(std::move(acceptance));
    if (by_node.size() != participants.size())
        return; // A participant reported no heads at all -- not settled yet.

    std::optional<Hash256> floor_hash;
    uint64_t floor_generation = 0;
    for (const auto& [id, heads] : by_node) {
        if (heads.size() != 1)
            return; // That participant has not itself converged to one head.
        if (!floor_hash) {
            floor_hash = heads.front().hash;
            floor_generation = heads.front().generation;
        } else if (heads.front().hash != *floor_hash) {
            return; // Participants disagree -- not settled yet.
        }
    }
    if (!floor_hash || *floor_hash != floor_hash_candidate)
        return;

    HistoryCheckpointProof proposal;
    proposal.floor_hash = *floor_hash;
    proposal.floor_generation = floor_generation;
    proposal.epoch = epoch;
    proposal.participants.reserve(participants.size());
    for (const auto& participant : participants)
        proposal.participants.push_back(participant.id);
    proposal.status = HistoryCheckpointProof::Status::acked;

    // Propose + durable ack: every participant must ack before anyone
    // commits. Abort outright on the first failure; the next maintenance
    // cycle re-proposes the identical (floor_hash, epoch), which is cheap to
    // re-ack since the content is unchanged.
    for (const auto& owner : participants) {
        if (!propose_history_floor_on(owner, proposal, FrameType::control))
            return;
    }

    // Commit locally first. If even the local commit is refused (a stale
    // proposal already superseded it), do not broadcast a commit no one
    // durably acked.
    if (!node_.metadata_replica().record_checkpoint_commit(*floor_hash, epoch))
        return;

    for (const auto& owner : participants) {
        if (owner.id == node_.node_id())
            continue;
        // Best-effort from here: a participant that misses this broadcast
        // simply remains at `acked` and adopts the commit on the next
        // maintenance cycle, via an identical re-proposal -- see the class
        // comment on HistoryCheckpointProof.
        (void)commit_history_floor_on(owner, *floor_hash, epoch, FrameType::control);
    }

    // Only ever compact once *this* replica's own proof is committed.
    // compact_history_if_safe() re-validates its own preconditions
    // independently -- this protocol is an added prerequisite gate, not a
    // replacement for them.
    if (node_.metadata_replica().compact_history_if_safe(record_threshold, byte_threshold))
        Log::info("metadata history checkpoint committed and compacted floor_generation=" +
                  std::to_string(floor_generation) + " floor_hash=" + to_string(*floor_hash) +
                  " participants=" + std::to_string(participants.size()));
}

MetadataRecord MetadataManager::read_group(const std::vector<NodeId>& replicas,
                                           FrameType frame_type) {
    auto nodes = compatible_replicas(replica_nodes(replicas));
    if (nodes.empty())
        throw MetadataNotReady("metadata replicas unavailable");

    struct ObservedHead {
        std::vector<MetadataAcceptance> certificates;
        std::vector<NodeInfo> owners;
    };
    std::map<Hash256, ObservedHead> observed;
    for (auto& [owner, acceptance] : discover_accepted_heads(nodes, frame_type)) {
        auto& head = observed[acceptance.hash];
        if (std::none_of(head.owners.begin(), head.owners.end(),
                         [&](const NodeInfo& value) { return value.id == owner.id; }))
            head.owners.push_back(owner);
        if (std::find(head.certificates.begin(), head.certificates.end(), acceptance) ==
            head.certificates.end())
            head.certificates.push_back(std::move(acceptance));
    }
    if (observed.empty())
        throw MetadataNotReady("no accepted metadata heads available");

    // Import every accepted head independently. A receiver is not asked to
    // replace its current head; it merely stores the immutable DAG material and
    // the acceptance certificate. This is the central 0.19 semantic boundary.
    for (const auto& [hash, head] : observed) {
        // See unacceptable_head_retry_at_'s declaration: a hash this replica
        // recently confirmed it cannot accept is skipped outright -- no
        // repeated import RPCs, no repeated rejection -- rather than
        // re-attempted on every single read_group() call.
        {
            std::lock_guard lock(unacceptable_head_mutex_);
            auto found = unacceptable_head_retry_at_.find(hash);
            if (found != unacceptable_head_retry_at_.end() && Clock::now() < found->second)
                continue;
        }
        auto& local = node_.metadata_replica();
        std::vector<NodeInfo> history_sources = head.owners;
        for (const auto& certificate : head.certificates) {
            for (const auto& witness : certificate.replicas) {
                auto found = std::find_if(nodes.begin(), nodes.end(), [&](const NodeInfo& peer) {
                    return peer.id == witness;
                });
                if (found != nodes.end() &&
                    std::none_of(history_sources.begin(), history_sources.end(),
                                 [&](const NodeInfo& peer) { return peer.id == found->id; }))
                    history_sources.push_back(*found);
            }
        }

        bool imported = local.history_contains(hash);
        for (const auto& owner : history_sources) {
            if (owner.id == node_.node_id())
                continue;
            imported = import_history_from_peer(owner, hash, frame_type) || imported;
        }
        if (!imported)
            continue;
        bool accepted = false;
        for (const auto& certificate : head.certificates)
            accepted = node_.accept_metadata_commit(certificate) || accepted;
        if (!accepted) {
            constexpr auto retry_cooldown = std::chrono::seconds(30);
            {
                std::lock_guard lock(unacceptable_head_mutex_);
                unacceptable_head_retry_at_[hash] = Clock::now() + retry_cooldown;
            }
            Log::warn("ignoring metadata head without a valid acceptance certificate hash=" +
                      to_string(hash));
        } else {
            {
                std::lock_guard lock(unacceptable_head_mutex_);
                unacceptable_head_retry_at_.erase(hash);
            }
            // Installing this certificate may have exposed a second head whose
            // common ancestry crosses a compacted boundary. Revisit the now-
            // local head so the lightweight, rootless-only healer can request
            // just the missing proof records in this same convergence event.
            for (const auto& owner : history_sources) {
                if (owner.id != node_.node_id())
                    (void)import_history_from_peer(owner, hash, frame_type);
            }
        }
    }

    const size_t need = node_.config().metadata_min_write_replicas;
    // Only the merge-and-publish branch below needs serializing: concurrent
    // foreground reads that all observe a divergence must not each mint their
    // own reconciliation commit for it. Acquired lazily on first observing
    // more than one head, and re-checked immediately after acquiring it, since
    // another caller may have already reconciled while this one waited.
    std::optional<std::unique_lock<std::mutex>> reconciliation_lock;
    for (;;) {
        auto heads = node_.metadata_replica().accepted_heads();
        if (heads.empty())
            throw MetadataNotReady("metadata accepted-head set is empty");
        std::sort(heads.begin(), heads.end(), [](const MetadataRecord& a,
                                                 const MetadataRecord& b) {
            if (a.hash != b.hash)
                return a.hash < b.hash;
            return a.generation < b.generation;
        });

        if (heads.size() == 1) {
            auto selected = heads.front();
            auto materialized = node_.metadata_replica().materialized(selected.hash);
            if (!materialized)
                throw MetadataNotReady("selected metadata head cannot be materialized");
            if (materialized->snapshot->extent_size &&
                materialized->snapshot->extent_size != node_.config().extent_size)
                throw std::runtime_error("cluster extent size does not match local configuration");
            // A whole-cluster configuration change may legitimately leave the
            // accepted branch carrying the previous write floor. Return the
            // accepted head here; maybe_reconfigure() performs the explicit
            // transition commit at max(old_floor, new_floor) before any write.
            return cache_record(selected, materialized->snapshot);
        }

        if (!reconciliation_lock) {
            reconciliation_lock.emplace(reconciliation_mutex_);
            continue; // re-read heads now that we hold the lock; may already be resolved
        }

        if (compatible_replicas(nodes).size() < need)
            throw MetadataNotReady(
                "divergent metadata heads await reconciliation; write durability floor unavailable");

        // Deterministically fold the maximal accepted-head set two branches at a
        // time. Each merge commit explicitly names both parents and applies
        // three-way conflict-presence semantics so explicit resolutions survive.
        // Three or more partitions therefore
        // converge as a sequence of immutable two-parent merges rather than by
        // inventing a winner or blocking on an N-way special case.
        auto left = heads[0];
        auto right = heads[1];
        const auto common =
            node_.metadata_replica().history_common_ancestor(left.hash, right.hash);
        if (!common)
            throw MetadataNotReady("divergent metadata heads have no known common ancestor");
        auto base_materialized = node_.metadata_replica().materialized(*common);
        if (!base_materialized)
            throw MetadataNotReady("metadata common ancestor cannot be reconstructed");
        auto left_materialized = node_.metadata_replica().materialized(left.hash);
        auto right_materialized = node_.metadata_replica().materialized(right.hash);
        if (!left_materialized || !right_materialized)
            throw MetadataNotReady("metadata merge head cannot be materialized");
        // Reconciliation is the one path that still wants whole namespaces:
        // the three-way merge is path-wise, so a tree-backed branch is
        // materialised for it and the result is re-rooted afterwards. A merge
        // therefore costs what it costs today -- it is the operation this work
        // has not yet made cheaper -- but it is correct, which an empty map
        // would not be: it would merge to an empty namespace and call that
        // agreement.
        auto namespace_nodes =
            namespace_store_ ? std::optional<ControlNamespaceNodeStore>(
                                   ControlNamespaceNodeStore::for_reading(node_, *namespace_store_))
                             : std::nullopt;
        const auto materialise = [&](const MetadataSnapshot& snapshot) {
            if (!snapshot.namespace_root)
                return snapshot;
            if (!namespace_nodes)
                throw MetadataNotReady("no namespace node store is configured");
            return attach_namespace(snapshot, *namespace_nodes);
        };
        const auto tree_backed = left_materialized->snapshot->namespace_root.has_value() ||
                                 right_materialized->snapshot->namespace_root.has_value();
        auto merged = merge_metadata_snapshots(
            materialise(*base_materialized->snapshot), materialise(*left_materialized->snapshot),
            materialise(*right_materialized->snapshot), left.hash, right.hash);
        if (merged.snapshot.extent_size &&
            merged.snapshot.extent_size != node_.config().extent_size)
            throw std::runtime_error("cluster extent size does not match local configuration");

        // Use the lower hash as the primary parent so every reconciler which
        // observes the same maximal head pair produces the same immutable merge
        // commit. Reconciliation is a pure join of already-authored histories,
        // not a new user mutation: the merged vector clock already causally
        // covers both parents and every live object in the result was retained
        // by one of those authored parent mutations. Adding a fresh local
        // origin/sequence here makes simultaneous reconcilers manufacture
        // equivalent sibling merges forever (A+B -> M1/M2 -> M3/M4 ...).
        if (right.hash < left.hash) {
            std::swap(left, right);
        }
        merged.snapshot.metadata_voters.clear();
        merged.snapshot.merge_parents = {right.hash};

        // Back into a tree if the branches were trees, which also means the
        // merge result is written and replicated as tree nodes before the
        // record naming its root is published.
        if (tree_backed) {
            if (!namespace_store_)
                throw MetadataNotReady("no namespace node store is configured");
            auto commit_nodes = ControlNamespaceNodeStore::for_commit(
                node_, *namespace_store_, merged.snapshot.metadata_write_replicas_required);
            merged.snapshot = detach_namespace(std::move(merged.snapshot), commit_nodes);
        }

        MetadataRecord reconciliation;
        reconciliation.generation = std::max(left.generation, right.generation) + 1;
        reconciliation.previous = left.hash;
        reconciliation.payload = tree_backed ? encode_snapshot_v14(merged.snapshot)
                                             : encode_snapshot(merged.snapshot);
        reconciliation.hash = metadata_hash(reconciliation.generation,
                                            reconciliation.previous,
                                            reconciliation.payload);

        Bytes reconciliation_delta;
        const auto* primary_snapshot =
            left_materialized->record.hash == reconciliation.previous
                ? left_materialized->snapshot.get()
                : right_materialized->snapshot.get();
        // metadata_delta diffs two entry maps. The merged snapshot is a tree
        // again by now, so there is nothing to diff against and the merge is
        // published as a full record -- which is what it was before 0.28.2
        // anyway. A tree-native merge would carry its own change set and make
        // this a delta again.
        if (auto delta = tree_backed ? std::nullopt
                                     : metadata_delta(*primary_snapshot, merged.snapshot)) {
            auto encoded = encode_metadata_delta(*delta);
            if (encoded.size() < reconciliation.payload.size())
                reconciliation_delta = std::move(encoded);
        }
        (void)publish_commit(nodes, reconciliation, reconciliation_delta, frame_type);
        if (merged.conflicts_superseded)
            conflicts_superseded_.fetch_add(merged.conflicts_superseded, std::memory_order_relaxed);
        Log::info("metadata histories reconciled generation=" +
                  std::to_string(reconciliation.generation) +
                  " history_body=" +
                  std::string(reconciliation_delta.empty() ? "full" : "delta") +
                  " history_bytes=" +
                  std::to_string(reconciliation_delta.empty()
                                     ? reconciliation.payload.size()
                                     : reconciliation_delta.size()) +
                  " conflicts=" + std::to_string(merged.conflicts_created) +
                  " superseded=" + std::to_string(merged.conflicts_superseded) +
                  " standing=" + std::to_string(merged.snapshot.conflicts.size()) +
                  " remaining_heads=" + std::to_string(heads.size() - 1));
        // accept_commit() removes accepted ancestors from the local head set. The
        // loop therefore naturally folds any remaining divergent heads into the
        // newly accepted reconciliation commit.
    }
}

MetadataRecord MetadataManager::maybe_reconfigure(const MetadataRecord& initial) {
    auto snapshot = decode_snapshot(initial.payload);
    if (snapshot.extent_size && snapshot.extent_size != node_.config().extent_size)
        throw std::runtime_error("cluster extent size does not match local configuration");

    const size_t configured = node_.config().metadata_min_write_replicas;
    const size_t persisted = snapshot.metadata_write_replicas_required
                                 ? snapshot.metadata_write_replicas_required
                                 : (!snapshot.metadata_voters.empty()
                                        ? snapshot.metadata_voters.size() / 2 + 1
                                        : 0);
    const bool policy_transition = persisted != configured;
    const bool clear_legacy_metadata_voters = !snapshot.metadata_voters.empty();
    const bool data_policy_change = snapshot.data_replication != node_.config().replication;

    const auto active = node_.membership().active();
    // Before a durable protocol-20 policy exists, every active peer must agree
    // on the configured write floor. Filtering mismatched peers first can let
    // two incompatible cohorts independently establish authority from the same
    // legacy/genesis state. Established protocol-20 clusters continue to ignore
    // mismatched peers while the persisted floor remains satisfiable.
    if (!snapshot.metadata_write_replicas_required)
        require_metadata_policy_match(active);
    const auto compatible = compatible_replicas(active);

    // `metadata_participants` was introduced during the protocol-20 bring-up as
    // a migration roster. It is not authority: every authenticated, policy-
    // compatible node is metadata-capable. Preserve/reconstruct the roster only
    // long enough to establish the one-time legacy retention baseline, then
    // clear it permanently.
    auto participants = snapshot.metadata_participants;
    bool migration_roster_change = false;
    if (!snapshot.retention_baseline_complete && participants.empty()) {
        for (const auto& legacy : snapshot.metadata_voters)
            if (legacy != NodeId{})
                participants.insert(legacy);
        for (const auto& [id, _] : snapshot.node_status)
            if (id != NodeId{})
                participants.insert(id);
        for (const auto& [id, _] : snapshot.mutation_sequences)
            if (id != NodeId{})
                participants.insert(id);
        for (const auto& peer : active)
            if (peer.id != NodeId{})
                participants.insert(peer.id);
        migration_roster_change = participants != snapshot.metadata_participants;
    }

    const bool clear_migration_roster =
        snapshot.retention_baseline_complete && !snapshot.metadata_participants.empty();
    if (!policy_transition && !clear_legacy_metadata_voters && !data_policy_change &&
        !migration_roster_change && !clear_migration_roster)
        return initial;

    // Policy transitions are ordinary immutable commits certified at the
    // stronger of old/new floors. No node-seat roster participates.
    const size_t transition_floor = std::max(configured, persisted);
    if (compatible.size() < transition_floor)
        return initial;

    snapshot.metadata_voters.clear();
    snapshot.metadata_write_replicas_required = static_cast<uint32_t>(configured);
    snapshot.metadata_participants = snapshot.retention_baseline_complete
                                         ? std::set<NodeId>{}
                                         : std::move(participants);
    snapshot.merge_parents.clear();
    snapshot.data_replication = static_cast<uint32_t>(node_.config().replication);
    MetadataRecord proposed;
    proposed.generation = initial.generation + 1;
    proposed.previous = initial.hash;
    proposed.payload = encode_snapshot(snapshot);
    proposed.hash = metadata_hash(proposed.generation, proposed.previous, proposed.payload);
    (void)publish_commit(compatible, proposed, {}, FrameType::control);
    Log::info("metadata policy/migration transition committed generation=" +
              std::to_string(proposed.generation) +
              " old_write_floor=" + std::to_string(persisted) +
              " new_write_floor=" + std::to_string(configured) +
              " acceptance_floor=" + std::to_string(transition_floor) +
              " participants=" + std::to_string(snapshot.metadata_participants.size()));
    return cache_record(proposed,
                        std::make_shared<MetadataSnapshot>(std::move(snapshot)));
}

MetadataManager::RecoverySurvey MetadataManager::recover_from_committed_checkpoints(
    const std::vector<NodeInfo>& active) {
    RecoverySurvey survey;
    if (active.empty())
        return survey;

    std::vector<PendingRead> pending;
    size_t completed = 0;
    size_t failed = 0;
    bool durable_history = false;

    for (const auto& owner : active) {
        if (owner.id == node_.node_id()) {
            ++completed;
            durable_history = durable_history ||
                              node_.metadata_replica().committed().generation > 1;
            continue;
        }
        try {
            PendingRead item;
            item.owner = owner;
            item.rpc.emplace(node_.call_async(owner, MessageType::get_committed_metadata,
                                              {}, FrameType::control));
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
                durable_history = durable_history ||
                                  decode_metadata_record(reply.message.payload).generation > 1;
            } catch (...) {
                ++failed;
            }
        }
        if (!progressed)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // This survey has one job in 0.19: a configured joiner may form a virgin
    // namespace only after every currently-active bootstrap peer has positively
    // demonstrated that no durable post-genesis history exists. It does not
    // elect/replace authorities.
    survey.complete = failed == 0;
    survey.durable_history = durable_history;
    return survey;
}

MetadataRecord MetadataManager::discover_or_form() {
    const auto active = node_.membership().active();
    const size_t need = node_.config().metadata_min_write_replicas;
    const auto local_snapshot = decode_snapshot(node_.metadata_replica().committed().payload);
    if (local_snapshot.metadata_write_replicas_required &&
        local_snapshot.metadata_write_replicas_required != need)
        throw MetadataNotReady("local metadata write-floor policy does not match configuration");
    const bool established_policy = local_snapshot.metadata_write_replicas_required != 0;
    if (!established_policy)
        require_metadata_policy_match(active);
    const auto write_active = compatible_replicas(active);
    if (write_active.size() < need) {
        throw MetadataNotReady("metadata replica set forming: need " + std::to_string(need) +
                               " policy-compatible active nodes, have " +
                               std::to_string(write_active.size()));
    }
    if (!node_.config().bootstrap.empty() && active.size() == 1)
        throw MetadataNotReady("metadata replica set forming: waiting for bootstrap peer");

    std::vector<NodeId> ids;
    ids.reserve(active.size());
    for (const auto& peer : active)
        ids.push_back(peer.id);

    // First discover accepted heads. Unlike the old protocol there is no
    // genesis-election race: if any post-genesis accepted commit exists, import
    // it (and any siblings) through the ordinary branch path.
    bool any_post_genesis = false;
    for (const auto& [_, acceptance] : discover_accepted_heads(active, FrameType::control)) {
        if (acceptance.generation > 1) {
            any_post_genesis = true;
            break;
        }
    }
    if (any_post_genesis)
        return read_group(ids, FrameType::control);

    if (!node_.config().bootstrap.empty()) {
        auto survey = recover_from_committed_checkpoints(active);
        if (!survey.complete)
            throw MetadataNotReady(
                "metadata replica set forming: waiting for bootstrap checkpoint survey");
        if (survey.durable_history)
            return read_group(ids, FrameType::control);
    }

    auto base = genesis_metadata();
    auto snapshot = decode_snapshot(base.payload);
    auto root = snapshot.entries.find("/");
    if (root == snapshot.entries.end())
        throw std::runtime_error("genesis metadata has no filesystem root");
    root->second.uid = node_.config().filesystem.root_uid;
    root->second.gid = node_.config().filesystem.root_gid;
    root->second.mode = node_.config().filesystem.root_mode;

    // Virgin founders must construct byte-identical generation 2 without a
    // coordinator. Root timestamps therefore use a deterministic non-zero
    // protocol sentinel; subsequent filesystem timestamps are ordinary wall
    // time. This removes startup leadership from the metadata model entirely.
    root->second.ctime_ns = root->second.mtime_ns = 1;
    snapshot.metadata_voters.clear();
    snapshot.data_replication = static_cast<uint32_t>(node_.config().replication);
    snapshot.extent_size = node_.config().extent_size;
    snapshot.metadata_write_replicas_required = static_cast<uint32_t>(need);
    snapshot.metadata_participants.clear();
    // Genesis itself is the first accepted branch point; there is no older
    // protocol-20 causal horizon yet. Background convergence advances this to
    // generation 2 once every founding participant has durably accepted it.
    snapshot.metadata_branch_floor = {};
    snapshot.retention_baseline_complete = true; // virgin namespace has no inherited objects

    MetadataRecord formed;
    formed.generation = base.generation + 1;
    formed.previous = base.hash;
    formed.payload = encode_snapshot(snapshot);
    formed.hash = metadata_hash(formed.generation, formed.previous, formed.payload);
    (void)publish_commit(write_active, formed, {}, FrameType::control);

    Log::info("metadata replica set formed active=" + std::to_string(write_active.size()) +
              " required=" + std::to_string(need));
    return cache_record(formed,
                        std::make_shared<MetadataSnapshot>(std::move(snapshot)));
}

MetadataRecord MetadataManager::read_record_base() {
    auto active = node_.membership().active();
    std::vector<NodeId> ids;
    ids.reserve(active.size());
    for (const auto& peer : active)
        ids.push_back(peer.id);
    if (node_.metadata_replica().committed().generation <= 1)
        return discover_or_form();
    return read_group(ids, FrameType::control);
}

MetadataRecord MetadataManager::read_record_uncached() {
    auto record = maybe_reconfigure(read_record_base());
    if (node_.metadata_replica().recovery_required())
        node_.metadata_replica().mark_recovered();
    return record;
}

MetadataRecord MetadataManager::read_record() {
    if (auto cached = cached_record())
        return *cached;
    try {
        return cache_record(read_record_uncached());
    } catch (const std::exception& error) {
        // Reads may continue from the last durably persisted local snapshot
        // while the metadata write floor is unavailable. Mutations never use
        // this fallback as proof of publication durability.
        auto local = node_.metadata_replica().committed();
        const auto heads = node_.metadata_replica().accepted_heads();
        if (!node_.metadata_replica().recovery_required() && heads.size() == 1 &&
            heads.front().hash == local.hash && local.generation > 1) {
            (void)error;
            return cache_record(local);
        }
        throw;
    }
}

MetadataSnapshotView MetadataManager::snapshot_view() {
    if (auto cached = cached_snapshot_view())
        return *cached;
    auto record = read_record();
    if (auto cached = cached_snapshot_view())
        return *cached;

    // A successful read installed a fully decoded, immutable snapshot.  A
    // concurrent metadata notice can advance remote_metadata_generation()
    // between that read and the second cached_snapshot_view() above, making the
    // freshly installed generation look stale immediately.  That is not an I/O
    // error: this filesystem operation may safely finish against the coherent
    // generation it just obtained.  Return the installed snapshot even when a
    // newer generation is already known; the next operation will refresh via
    // the normal fast-path staleness check.
    {
        std::lock_guard lock(cache_mutex_);
        if (decoded_cache_ &&
            (decoded_generation_ > record.generation ||
             (decoded_generation_ == record.generation && decoded_hash_ >= record.hash))) {
            return coherent(MetadataSnapshotView{decoded_generation_,
                                                     decoded_namespace_revision_, decoded_hash_,
                                                     decoded_cache_});
        }
    }

    // If another reader displaced the decoded cache in an unusual interleave,
    // the record returned by read_record() is still self-contained and valid.
    // Decode that exact generation rather than turning cache churn into FUSE
    // EIO.  This path is exceptional; normal operations reuse decoded_cache_.
    auto decoded = std::make_shared<MetadataSnapshot>(decode_snapshot(record.payload));
    return coherent(MetadataSnapshotView{record.generation, 0, record.hash, std::move(decoded)});
}

std::optional<MetadataSnapshotView> MetadataManager::available_snapshot_view() const {
    // This is deliberately a no-I/O view.  Consumers such as FUSE use it to
    // adopt a newer snapshot which MetadataManager has already obtained and
    // decoded, but never to turn an OS metadata lookup into metadata traffic.
    std::lock_guard lock(cache_mutex_);
    if (!decoded_cache_)
        return {};
    return coherent(MetadataSnapshotView{decoded_generation_, decoded_namespace_revision_,
                                             decoded_hash_, decoded_cache_});
}

MetadataSnapshot MetadataManager::snapshot() {
    auto view = snapshot_view();
    return *view.snapshot;
}

std::optional<MetadataSnapshotView> MetadataManager::retention_release_view() const {
    if (node_.metadata_replica().recovery_required())
        return {};
    const auto heads = node_.metadata_replica().accepted_heads();
    if (heads.size() != 1)
        return {};

    // Claim release is local and causal, not a global-stability decision. The
    // sole accepted head's complete live set determines which local claims are
    // still needed, while its mutation clock can remove only claim dots that
    // this branch actually observed. Concurrent/unseen branch claims therefore
    // survive without requiring every participant to be online.
    auto current = available_snapshot_view();
    if (current && current->hash == heads.front().hash)
        return current;
    try {
        auto materialized = node_.metadata_replica().materialized(heads.front().hash);
        if (!materialized)
            return {};
        // The catch below turns a refusal here into "no view", which stops
        // retention release rather than running it against an empty live set.
        // Fail-closed is the correct direction for the one reader that deletes.
        return coherent(MetadataSnapshotView{heads.front().generation, 0, heads.front().hash,
                                                 materialized->snapshot});
    } catch (...) {
        return {};
    }
}

MetadataRecord MetadataManager::mutate_impl(
    const std::function<void(MetadataSnapshot&, MetadataDelta*)>& mutate, bool exact_delta,
    size_t retries, std::optional<MetadataMutationIdentity> identity) {
    std::unique_lock lock(mutation_mutex_);
    const auto origin = node_.node_id();
    std::optional<uint64_t> sequence;
    if (identity && !identity->sequence)
        throw std::invalid_argument("metadata mutation identity sequence must be non-zero");

    for (size_t attempt = 0; attempt < retries; ++attempt) {
        const auto total_started = Clock::now();
        const auto all_active = node_.membership().active();
        const auto active = compatible_replicas(all_active);
        const size_t need = node_.config().metadata_min_write_replicas;
        if (active.size() < need)
            throw MetadataNotReady("metadata write durability floor unavailable: too few policy-compatible replicas");

        MetadataRecord current;
        auto local_heads = node_.metadata_replica().accepted_heads();
        const bool recovering = node_.metadata_replica().recovery_required();
        if (recovering || local_heads.empty() ||
            (local_heads.size() == 1 && local_heads.front().generation <= 1)) {
            current = read_record_uncached();
            if (recovering)
                node_.metadata_replica().mark_recovered();
        } else if (local_heads.size() > 1 ||
                   node_.remote_metadata_generation() >
                       node_.metadata_replica().committed_generation()) {
            // Reconciliation is useful when already known, but it is not a
            // prerequisite for accepting another branch mutation. If the survey
            // cannot complete, fall back to the locally materialised accepted
            // head and preserve availability.
            try {
                std::vector<NodeId> ids;
                ids.reserve(all_active.size());
                for (const auto& peer : all_active)
                    ids.push_back(peer.id);
                current = maybe_reconfigure(read_group(ids, FrameType::read_ahead));
            } catch (const MetadataNotReady&) {
                if (local_heads.size() > 1)
                    throw;
                current = maybe_reconfigure(node_.metadata_replica().committed());
            }
        } else {
            current = maybe_reconfigure(node_.metadata_replica().committed());
        }

        auto snapshot = decode_snapshot(current.payload);
        if (snapshot.metadata_write_replicas_required != need)
            throw MetadataNotReady("metadata write-floor transition is not durably accepted");
        const bool clear_merge_parent_topology = !snapshot.merge_parents.empty();
        auto seen = snapshot.mutation_sequences.find(origin);
        if (sequence && seen != snapshot.mutation_sequences.end() && seen->second >= *sequence) {
            ensure_accepted_head_durable(active, current, need, FrameType::read_ahead);
            cache_record(current, std::make_shared<MetadataSnapshot>(std::move(snapshot)));
            return current;
        }
        if (identity) {
            auto clock = snapshot.mutation_sequences.find(identity->origin);
            if (clock != snapshot.mutation_sequences.end() &&
                clock->second >= identity->sequence) {
                // Already accepted (by this process before a crash, or by a
                // peer that merged it): the caller's work is done.
                ensure_accepted_head_durable(active, current, need, FrameType::read_ahead);
                cache_record(current, std::make_shared<MetadataSnapshot>(std::move(snapshot)));
                return current;
            }
        }
        if (!sequence) {
            const uint64_t previous =
                seen == snapshot.mutation_sequences.end() ? 0 : seen->second;
            if (previous == std::numeric_limits<uint64_t>::max())
                throw std::runtime_error("metadata mutation sequence exhausted");
            sequence = node_.metadata_replica().reserve_mutation_sequence(previous);
        }

        std::optional<MetadataSnapshot> before;
        if (!exact_delta)
            before.emplace(snapshot);
        snapshot.merge_parents.clear();
        MetadataDelta supplied_delta;
        const auto legacy_metadata_voters = snapshot.metadata_voters;
        const auto data_replication = snapshot.data_replication;
        const auto extent_size = snapshot.extent_size;
        const auto metadata_write_replicas_required =
            snapshot.metadata_write_replicas_required;
        const auto metadata_participants = snapshot.metadata_participants;
        const auto metadata_branch_floor = snapshot.metadata_branch_floor;
        const auto retention_baseline_complete = snapshot.retention_baseline_complete;
        mutate(snapshot, exact_delta ? &supplied_delta : nullptr);
        if (exact_delta && !snapshot.node_status.empty() && supplied_delta.upsert_node_status.empty())
            supplied_delta.upsert_node_status.emplace(*snapshot.node_status.begin());
        if (!same_legacy_metadata_voters(snapshot.metadata_voters, legacy_metadata_voters))
            throw std::runtime_error(
                "filesystem mutation attempted to change legacy metadata replica state");
        if (snapshot.data_replication != data_replication || snapshot.extent_size != extent_size ||
            snapshot.metadata_write_replicas_required != metadata_write_replicas_required ||
            snapshot.metadata_participants != metadata_participants ||
            snapshot.metadata_branch_floor != metadata_branch_floor ||
            snapshot.retention_baseline_complete != retention_baseline_complete)
            throw std::runtime_error("filesystem mutation attempted to change cluster policy");
        snapshot.mutation_sequences[origin] = *sequence;
        if (exact_delta)
            supplied_delta.mutation_sequences[origin] = *sequence;
        if (identity) {
            snapshot.mutation_sequences[identity->origin] = identity->sequence;
            if (exact_delta)
                supplied_delta.mutation_sequences[identity->origin] = identity->sequence;
        }

        // Discipline 4. Keep the tombstone vector in canonical order so the
        // next reconciliation's union is a delta, not a 5-8 MB full frame
        // (DLT7 sorts after applying the edits, so the replay matches), and
        // drop conflicts this mutation has just decided by rewriting their
        // subject.
        const bool resorted = !garbage_is_canonical(snapshot.garbage);
        if (resorted)
            canonicalise_garbage(snapshot.garbage);
        const auto superseded = prune_superseded_conflicts(snapshot);
        if (superseded)
            conflicts_superseded_.fetch_add(superseded, std::memory_order_relaxed);
        if (exact_delta) {
            if (resorted || !supplied_delta.upsert_garbage.empty() ||
                !supplied_delta.erase_garbage.empty())
                supplied_delta.canonical_garbage = true;
            if (superseded)
                supplied_delta.replace_conflicts = snapshot.conflicts;
        }

        // A tree-backed namespace commits by updating the tree with the change
        // set this mutation already carries, rather than by re-serialising the
        // library. Unreachable today: nothing produces a snapshot with a root.
        if (snapshot.namespace_root) {
            // Without an exact delta there is no change set, and rediscovering
            // one means materialising both namespaces and diffing them --
            // precisely the cost this replaces. A caller that cannot describe
            // its own edit cannot mutate a tree-backed namespace.
            if (!exact_delta)
                throw MetadataNotReady("a tree-backed namespace requires an exact delta");
            // A callback that wrote into the map has made a change the delta
            // does not describe, and applying the delta alone would silently
            // drop it. Refuse rather than guess which of the two is the
            // mutation. This is the contract each mutation callback is
            // converted to, one at a time, and the loud failure is how the
            // conversion is driven.
            if (!snapshot.entries.empty())
                throw std::runtime_error(
                    "metadata mutation wrote the namespace map on a tree-backed snapshot");
            if (!namespace_store_)
                throw MetadataNotReady("no namespace node store is configured");
            auto nodes =
                ControlNamespaceNodeStore::for_commit(node_, *namespace_store_, need);
            snapshot.namespace_root = apply_delta_to_namespace_tree(*snapshot.namespace_root,
                                                                    nodes, supplied_delta);
        }

        auto payload =
            snapshot.namespace_root ? encode_snapshot_v14(snapshot) : encode_snapshot(snapshot);
        if (payload == current.payload)
            return cache_record(current,
                                std::make_shared<MetadataSnapshot>(std::move(snapshot)));

        MetadataRecord proposed;
        proposed.generation = current.generation + 1;
        proposed.previous = current.hash;
        proposed.payload = std::move(payload);
        proposed.hash = metadata_hash(proposed.generation, proposed.previous, proposed.payload);

        Bytes delta_payload;
        std::optional<MetadataDelta> delta;
        if (exact_delta) {
            delta = std::move(supplied_delta);
            // An exact caller describes only its own edit; the branch topology
            // it inherited is settled here. The first write after a
            // reconciliation leaves the merge commit's merge_parents behind
            // (cleared above). Until 0.28.2 this write was forced to a full
            // snapshot instead -- 3-30 s and 15 MB per replica on the cluster,
            // after every one of ~100 merges a day; until 0.32.0 (DLT7) it
            // also had to carry the whole standing conflict set.
            if (clear_merge_parent_topology)
                delta->replace_merge_parents = snapshot.merge_parents;
        } else {
            delta = metadata_delta(*before, snapshot);
        }
        if (delta) {
            auto encoded = encode_metadata_delta(*delta);
            if (encoded.size() < proposed.payload.size())
                delta_payload = std::move(encoded);
        }

        uint64_t retention_ms = 0;
        if (publication_retention_) {
            const auto retention_started = Clock::now();
            publication_retention_(MetadataPublicationContext{
                origin, *sequence, current, snapshot, delta ? &*delta : nullptr});
            retention_ms = elapsed_ms(retention_started);
        }

        try {
            const auto publish_started = Clock::now();
            (void)publish_commit(active, proposed, delta_payload, FrameType::read_ahead);
            const auto publish_ms = elapsed_ms(publish_started);
            mutations_.fetch_add(1, std::memory_order_relaxed);
            mutation_retention_ms_total_.fetch_add(retention_ms, std::memory_order_relaxed);
            mutation_publish_ms_total_.fetch_add(publish_ms, std::memory_order_relaxed);
            const auto raise = [](std::atomic_uint64_t& slot, uint64_t value) {
                auto seen = slot.load(std::memory_order_relaxed);
                while (value > seen &&
                       !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
                }
            };
            raise(mutation_retention_ms_max_, retention_ms);
            raise(mutation_publish_ms_max_, publish_ms);

            // A concurrent writer can durably accept a sibling of `proposed`
            // while this publication is in flight.  In that case the mutation
            // must not bless its branch-specific decoded snapshot as current
            // after the acceptance notice has already invalidated the cache.
            // With W>=2, at least the later of two mutually-published writers
            // observes both accepted heads locally before returning.  Reconcile
            // that local divergence synchronously so both completed mutations
            // are visible once the writers have returned.
            auto post_publish_heads = node_.metadata_replica().accepted_heads();
            if (post_publish_heads.size() > 1) {
                std::vector<NodeId> ids;
                ids.reserve(all_active.size());
                for (const auto& peer : all_active)
                    ids.push_back(peer.id);
                try {
                    return read_group(ids, FrameType::read_ahead);
                } catch (const MetadataNotReady&) {
                    // The authored commit is already durably accepted.  Do not
                    // overwrite the invalidated cache with one sibling merely
                    // because reconciliation could not complete immediately; a
                    // subsequent read will retry the accepted-head survey.
                    return proposed;
                }
            }

            cache_record(proposed,
                         std::make_shared<MetadataSnapshot>(std::move(snapshot)));
            const auto total_ms = elapsed_ms(total_started);
            if (total_ms >= 100 && Log::enabled(LogLevel::debug)) {
                Log::debug("metadata mutate total_ms=" + std::to_string(total_ms) +
                           " retention_ms=" + std::to_string(retention_ms) +
                           " publish_ms=" + std::to_string(publish_ms) +
                           " mode=" + std::string(delta_payload.empty() ? "snapshot" : "delta") +
                           " delta_bytes=" + std::to_string(delta_payload.size()) +
                           " snapshot_bytes=" + std::to_string(proposed.payload.size()) +
                           " attempt=" + std::to_string(attempt + 1));
            }
            return proposed;
        } catch (const MetadataNotReady& error) {
            // If the commit crossed the store floor but certificate fan-out was
            // interrupted, the local accepted-head set may already contain this
            // exact mutation. Preserve the sequence across retries so the next
            // iteration recognises and returns it instead of generating a second
            // logical mutation.
            if (Log::enabled(LogLevel::debug))
                Log::debug("metadata mutation publish failed sequence=" +
                           std::to_string(*sequence) + " attempt=" +
                           std::to_string(attempt + 1) + "/" + std::to_string(retries) +
                           " error=" + error.what());
            if (attempt + 1 == retries)
                throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    throw MetadataNotReady("metadata mutation could not reach durable acceptance floor");
}

MetadataRecord MetadataManager::mutate(const std::function<void(MetadataSnapshot&)>& mutate,
                                       size_t retries) {
    return mutate_impl(
        [&](MetadataSnapshot& snapshot, MetadataDelta*) { mutate(snapshot); }, false, retries, {});
}

MetadataRecord MetadataManager::mutate_delta(
    const std::function<void(MetadataSnapshot&, MetadataDelta&)>& mutate, size_t retries,
    std::optional<MetadataMutationIdentity> identity) {
    return mutate_impl(
        [&](MetadataSnapshot& snapshot, MetadataDelta* delta) { mutate(snapshot, *delta); }, true,
        retries, identity);
}

bool MetadataManager::resolve_conflict(const std::string& id, std::string_view choice) {
    if (choice != "left" && choice != "right" && choice != "base")
        throw std::invalid_argument("conflict resolution choice must be left, right or base");
    bool resolved = false;
    // mutate_delta rather than mutate: this edits the namespace, and a
    // tree-backed commit needs the change set declared rather than
    // rediscovered by diffing two namespaces.
    mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
        auto found = snapshot.conflicts.find(id);
        if (found == snapshot.conflicts.end())
            return;
        const auto conflict = found->second;
        if (conflict.kind == MetadataConflictKind::namespace_entry) {
            const auto& chosen = choice == "left"    ? conflict.left_entry
                                 : choice == "right" ? conflict.right_entry
                                                     : conflict.base_entry;
            // Resolving a conflict is a namespace edit like any other, so it
            // goes through the working set: on a tree-backed snapshot writing
            // the map here would be a decision nothing ever applied.
            auto nodes = namespace_store_
                             ? std::optional<ControlNamespaceNodeStore>(
                                   ControlNamespaceNodeStore::for_reading(node_, *namespace_store_))
                             : std::nullopt;
            NamespaceWorkingSet working(snapshot, delta, nodes ? &*nodes : nullptr);
            if (chosen)
                working.put(conflict.key, *chosen);
            else
                working.erase(conflict.key);
        } else if (conflict.kind == MetadataConflictKind::catalogue_root) {
            snapshot.catalogue_root = choice == "left"    ? conflict.left_catalogue_root
                                      : choice == "right" ? conflict.right_catalogue_root
                                                          : conflict.base_catalogue_root;
            delta.catalogue = snapshot.catalogue_root ? CatalogueDelta::set : CatalogueDelta::clear;
            delta.catalogue_root = snapshot.catalogue_root;
        }
        snapshot.conflicts.erase(found);
        // The standing conflict set is part of the record, so an exact delta
        // has to carry it rather than leave the replay to infer it.
        delta.replace_conflicts = snapshot.conflicts;
        resolved = true;
    });
    if (resolved)
        conflicts_resolved_.fetch_add(1, std::memory_order_relaxed);
    return resolved;
}

void MetadataManager::repair_once() {
    // The background reconciliation owner must not race a foreground mutation
    // through discovery, accepted-head selection, or reconfiguration.
    std::unique_lock mutation_lock(mutation_mutex_);
    const auto all_active = node_.membership().active();
    const auto active = compatible_replicas(all_active);
    if (active.empty())
        throw MetadataNotReady("metadata replicas unavailable");

    MetadataRecord record;
    if (node_.metadata_replica().committed().generation <= 1) {
        // Maintenance must not bypass virgin-cluster discovery/policy fencing.
        // read_group() can otherwise filter incompatible peers before any
        // protocol-20 policy exists and allow a mismatched cohort to form.
        record = discover_or_form();
    } else {
        std::vector<NodeId> ids;
        ids.reserve(all_active.size());
        for (const auto& peer : all_active)
            ids.push_back(peer.id);
        record = maybe_reconfigure(read_group(ids, FrameType::speculative));
    }
    auto acceptance = node_.metadata_replica().acceptance(record.hash);
    if (!acceptance)
        throw MetadataNotReady("selected metadata head has no acceptance certificate");

    // Convergence is replication, not head replacement. Every active node is
    // offered the accepted immutable head plus its proof. A node holding a
    // different accepted branch keeps that branch as another head; read_group()
    // will reconcile the maximal set rather than overwriting it.
    const auto selected_generation = record.generation;
    size_t converged = 0;
    mutation_lock.unlock();
    try {
        for (const auto& owner : active) {
            if (replicate_accepted_head(owner, record, *acceptance, FrameType::speculative))
                ++converged;
        }
    } catch (...) {
        mutation_lock.lock();
        throw;
    }
    mutation_lock.lock();
    if (converged < active.size())
        throw MetadataNotReady("metadata accepted-head replication incomplete");
    if (node_.metadata_replica().committed_generation() > selected_generation)
        return;

    auto materialized = node_.metadata_replica().materialized(record.hash);
    if (!materialized)
        throw MetadataNotReady("metadata repair head cannot be materialized");
    auto snapshot = *materialized->snapshot;
    std::vector<NodeInfo> participants;
    participants.reserve(snapshot.metadata_participants.size());
    bool all_participants_online = !snapshot.metadata_participants.empty();
    for (const auto& participant : snapshot.metadata_participants) {
        auto found = std::find_if(active.begin(), active.end(), [&](const NodeInfo& peer) {
            return peer.id == participant;
        });
        if (found == active.end()) {
            all_participants_online = false;
            break;
        }
        participants.push_back(*found);
    }

    bool all_participants_at_head = all_participants_online;
    if (all_participants_at_head) {
        for (const auto& participant : participants) {
            if (!replicate_accepted_head(participant, record, *acceptance,
                                         FrameType::speculative)) {
                all_participants_at_head = false;
                break;
            }
        }
    }

    // A migrated SM12 cluster has no physical retention baseline. Establish it
    // only after *every durable branch-capable participant* has converged onto
    // this reconciled head. The publication guard then places/claims every
    // reachable DATA/CONTROL object before the baseline commit itself can be
    // accepted. Until this succeeds destructive mark/sweep is fenced in Service.
    if (all_participants_at_head && !snapshot.retention_baseline_complete) {
        const auto origin = node_.node_id();
        const auto found = snapshot.mutation_sequences.find(origin);
        const uint64_t previous_sequence =
            found == snapshot.mutation_sequences.end() ? 0 : found->second;
        const auto sequence = node_.metadata_replica().reserve_mutation_sequence(previous_sequence);

        auto baseline = snapshot;
        baseline.retention_baseline_complete = true;
        baseline.metadata_participants.clear();
        baseline.metadata_branch_floor = {};
        baseline.merge_parents.clear();
        baseline.mutation_sequences[origin] = sequence;

        MetadataRecord baseline_record;
        baseline_record.generation = record.generation + 1;
        baseline_record.previous = record.hash;
        baseline_record.payload = encode_snapshot(baseline);
        baseline_record.hash = metadata_hash(baseline_record.generation,
                                             baseline_record.previous,
                                             baseline_record.payload);
        if (publication_retention_) {
            publication_retention_(MetadataPublicationContext{
                origin, sequence, record, baseline, nullptr});
        }
        auto published = publish_commit(active, baseline_record, {}, FrameType::speculative);
        record = baseline_record;
        acceptance = published.acceptance;
        snapshot = std::move(baseline);

        all_participants_at_head = true;
        for (const auto& participant : participants) {
            if (!replicate_accepted_head(participant, record, *acceptance,
                                         FrameType::speculative)) {
                all_participants_at_head = false;
                break;
            }
        }
        if (all_participants_at_head)
            Log::info("metadata retention baseline established generation=" +
                      std::to_string(record.generation) +
                      " migration_participants=" + std::to_string(participants.size()));
    }

    // Retention release is driven independently by each node's sole accepted
    // head and causal mutation clock. No globally advanced branch floor is
    // needed for physical GC; unknown concurrent claim dots simply survive.

    node_.metadata_replica().compact();
    cache_record(record, std::make_shared<MetadataSnapshot>(std::move(snapshot)));
}



} // namespace macha
