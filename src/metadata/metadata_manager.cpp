// SPDX-License-Identifier: GPL-3.0-or-later
#include "metadata/metadata_manager.hpp"

#include "observation.hpp"
#include "diagnostics.hpp"
#include "cluster/placement.hpp"
#include "metadata/namespace_control_store.hpp"
#include "metadata/namespace_tree.hpp"

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

MetadataManager::MetadataManager(NodeRuntime& node, LocalState& local,
                                 MetadataServer& metadata_server,
                                 DistributedStore* namespace_store,
                                 PublicationRetention publication_retention,
                                 const TimeSource& time)
    : node_(node), local_(local), metadata_server_(metadata_server), namespace_store_(namespace_store),
      time_(time), publication_retention_(std::move(publication_retention)),
      replicator_([this](std::stop_token stop) { replication_loop(stop); }) {}

MetadataManager::~MetadataManager() {
    stop_replication();
}

void MetadataManager::request_replication_stop() {
    replicator_.request_stop();
    owed_changed_.notify_all();
}

void MetadataManager::stop_replication() {
    request_replication_stop();
    if (replicator_.joinable())
        replicator_.join();
}

size_t MetadataManager::replication_pending() const {
    Lock lock(owed_mutex_);
    return owed_.size() + (replicating_ ? 1 : 0);
}

bool MetadataManager::wait_replicated(std::chrono::milliseconds timeout) {
    Lock lock(owed_mutex_);
    return owed_changed_.wait_for(lock.native(), timeout, [&]() MACHA_REQUIRES(owed_mutex_) {
        return owed_.empty() && !replicating_;
    });
}

void MetadataManager::owe(OwedCommit commit) {
    {
        Lock lock(owed_mutex_);
        owed_.push_back(std::move(commit));
        // The newest head is always offered; only claims are shed.
        size_t with_claims = 0;
        for (const auto& item : owed_)
            with_claims += item.claims ? 1 : 0;
        for (auto it = owed_.begin(); with_claims > owed_claims_max && it != owed_.end(); ++it) {
            if (!it->claims)
                continue;
            it->claims.reset();
            --with_claims;
            owed_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    owed_changed_.notify_all();
}

void MetadataManager::replication_loop(std::stop_token stop) {
    for (;;) {
        std::deque<OwedCommit> batch;
        {
            Lock lock(owed_mutex_);
            owed_changed_.wait(lock.native(), stop, [&]() MACHA_REQUIRES(owed_mutex_) {
                return !owed_.empty();
            });
            if (stop.stop_requested())
                return;
            batch.swap(owed_);
            replicating_ = true;
        }
        // Each commit's claims on the peers, in order, then the newest head
        // once: its history carries the commits before it.
        for (const auto& commit : batch) {
            if (stop.stop_requested())
                break;
            if (!commit.claims || !publication_retention_)
                continue;
            try {
                publication_retention_(MetadataPublicationContext{
                    commit.claims->origin, commit.claims->sequence, commit.claims->parent,
                    *commit.claims->proposed,
                    commit.claims->delta ? &*commit.claims->delta : nullptr, true});
            } catch (const std::exception& error) {
                if (Log::enabled(LogLevel::debug))
                    Log::debug("metadata claims on peers deferred generation=" +
                               std::to_string(commit.record.generation) + " error=" +
                               error.what());
            }
        }
        if (!stop.stop_requested()) {
            const auto& newest = batch.back().record;
            // To every node present, not only the copies a commit seeks:
            // nobody is waiting on this.
            const auto acceptance = local_.replica().acceptance(newest.hash);
            for (const auto& owner : node_.membership().active()) {
                if (!acceptance || owner.id == node_.node_id() || stop.stop_requested())
                    continue;
                try {
                    if (!replicate_accepted_head(owner, newest, *acceptance,
                                                 FrameType::read_ahead) &&
                        Log::enabled(LogLevel::debug))
                        Log::debug("metadata head offer deferred peer=" + owner.host +
                                   " generation=" + std::to_string(newest.generation));
                } catch (const std::exception& error) {
                    if (Log::enabled(LogLevel::debug))
                        Log::debug("metadata head offer deferred peer=" + owner.host +
                                   " generation=" + std::to_string(newest.generation) +
                                   " error=" + error.what());
                }
            }
        }
        {
            Lock lock(owed_mutex_);
            replicating_ = false;
        }
        owed_changed_.notify_all();
    }
}

void MetadataManager::accept_locally(const MetadataRecord& record, std::span<const uint8_t> delta,
                                     FrameType frame_type) {
    auto self = node_info(node_.node_id());
    if (!self) {
        self.emplace();
        self->id = node_.node_id();
    }
    (void)publish_commit({*self}, record, delta, frame_type);
}

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
    // Every known node is metadata-capable and any node may accept a mutation
    // alone; copies on other nodes are sought, never required.
    auto view = available_snapshot_view();
    const auto membership = node_.membership().snapshot();
    const size_t required = node_.config().metadata_write_copies;
    const size_t known = membership.all.size();
    const size_t online = membership.active.size();
    const uint64_t generation = view ? view->generation : 0;

    MetadataAvailability next = MetadataAvailability::unavailable;
    // Validation describes convergence, not write authority: a local committed
    // branch suffices to mutate; divergent peers are reconciled by the
    // mutation/read path.
    if (view)
        next = MetadataAvailability::writable;

    replica_generation_.store(generation, std::memory_order_release);
    metadata_replicas_.store(static_cast<uint32_t>(known), std::memory_order_release);
    metadata_replicas_online_.store(static_cast<uint32_t>(online), std::memory_order_release);
    metadata_write_replicas_required_.store(static_cast<uint32_t>(required),
                                            std::memory_order_release);
    replica_observed_unix_ms_.store(unix_ms(), std::memory_order_release);
    metadata_write_available_.store(next == MetadataAvailability::writable,
                                    std::memory_order_release);
    metadata_replica_set_stable_.store(validated, std::memory_order_release);

    const auto previous = metadata_availability_.exchange(next, std::memory_order_acq_rel);
    if (previous == next)
        return;

    std::string transition_reason;
    if (next == MetadataAvailability::writable) {
        transition_reason = validated ? "local metadata state ready"
                                      : "local metadata state ready; reconciliation pending";
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

MetadataRecord MetadataManager::cache_record(
    const MetadataRecord& record, std::shared_ptr<const MetadataSnapshot> decoded) {
    // Durable management tombstones are operational constraints once decoded.
    for (const auto& [_, reset] : decoded->identity_resets)
        node_.apply_identity_reset(reset);

    Lock lock(cache_mutex_);
    // Reads can complete out of order; never move the cache back to an older
    // record.
    if (cache_ && newer_than(*cache_, record))
        return *cache_;
    cache_ = record;
    cache_until_ = time_.now() + node_.config().metadata_cache;
    cache_remote_epoch_ = node_.remote_metadata_epoch();
    if (!decoded_cache_ || decoded_generation_ != record.generation || decoded_hash_ != record.hash) {
        // The change witness FUSE and the catalogue wake on. Tree-backed namespaces
        // compare roots: their entry maps are always empty.
        const bool namespace_changed =
            !decoded_cache_ || namespace_differs(*decoded_cache_, *decoded);
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
        Lock lock(cache_mutex_);
        if (cache_ && newer_than(*cache_, record))
            return *cache_;
        cache_ = record;
        cache_until_ = time_.now() + node_.config().metadata_cache;
        cache_remote_epoch_ = node_.remote_metadata_epoch();
        if (decoded_cache_ && decoded_generation_ == record.generation && decoded_hash_ == record.hash)
            return record;
    }

    // Decode only when the canonical record changes: rebuilding every
    // FsEntry/extent per cache refresh dominates namespace cost.
    if (auto materialized = local_.replica().materialized(record.hash);
        materialized && materialized->record.generation == record.generation &&
        materialized->record.payload == record.payload)
        return cache_record(record, materialized->snapshot);
    auto decoded = std::make_shared<const MetadataSnapshot>(decode_snapshot(record.payload));
    return cache_record(record, std::move(decoded));
}

std::optional<MetadataRecord> MetadataManager::cached_record() {
    Lock lock(cache_mutex_);
    if (!cache_ || time_.now() >= cache_until_ ||
        cache_remote_epoch_ != node_.remote_metadata_epoch())
        return {};
    if (local_.replica().committed_generation() > cache_->generation ||
        metadata_server_.remote_generation() > cache_->generation)
        return {};
    return cache_;
}

void require_coherent_namespace(const MetadataSnapshot& snapshot) {
    if (snapshot.namespace_root && !snapshot.entries.empty())
        throw MetadataNotReady("metadata snapshot carries both a namespace root and an entry map; "
                               "the two could disagree about what the namespace is");
}

namespace {
// The view is where the system gets a namespace, so the invariant is
// enforced here.
MetadataSnapshotView coherent(MetadataSnapshotView view) {
    if (view.snapshot)
        require_coherent_namespace(*view.snapshot);
    return view;
}
} // namespace

std::optional<MetadataSnapshotView> MetadataManager::cached_snapshot_view() {
    Lock lock(cache_mutex_);
    // The decoded snapshot is valid for its record but not proof the record is
    // current: honour cached_record()'s TTL so missed generation notices
    // eventually force replica validation.
    if (!cache_ || !decoded_cache_ || time_.now() >= cache_until_ ||
        cache_remote_epoch_ != node_.remote_metadata_epoch())
        return {};
    if (cache_->generation != decoded_generation_ || cache_->hash != decoded_hash_)
        return {};
    if (local_.replica().committed_generation() > decoded_generation_ ||
        metadata_server_.remote_generation() > decoded_generation_)
        return {};
    return coherent(MetadataSnapshotView{decoded_generation_, decoded_namespace_revision_,
                                             decoded_hash_, decoded_cache_});
}

bool MetadataManager::import_history_from_peer(const NodeInfo& owner, const Hash256& target,
                                               FrameType frame_type) {
    auto& local = local_.replica();
    if (local.history_contains(target)) {
        // A peer's full checkpoint may lack an older part of its ancestry, so a
        // hole below a continuous suffix can make a real ancestor look rootless.
        // Heal only while the accepted heads lack a common ancestor, and stop once
        // it is provable. Traverses the resident link index, not checkpoint
        // payloads (multi-megabyte each).
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
            // A full checkpoint is self-contained; its previous/merge-parent links are
            // ancestry, not transfer prerequisites, and following them would copy
            // unrelated retained history. A delta needs its primary predecessor, so
            // follow that chain to the nearest full checkpoint.
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
    auto& local = local_.replica();
    const auto broken = local.unreconstructable_heads();
    if (broken.empty())
        return 0;
    std::vector<NodeInfo> peers;
    for (auto& peer : node_.membership().active())
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
    auto& local = local_.replica();
    if (owner.id == node_.node_id())
        return local.history_contains(target);
    if (!local.history_contains(target) || stalled(owner.id))
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
            // As on the pull side: only a delta's primary predecessor is required;
            // following optional links would exchange entire retained histories.
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

    // Requests to one peer execute FIFO at the receiver. A small in-flight
    // window overlaps latency and appends while bounding RPC memory.
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

bool MetadataManager::stalled(const NodeId& id) const {
    Lock lock(stalled_mutex_);
    const auto found = stalled_until_.find(id);
    if (found == stalled_until_.end())
        return false;
    if (Clock::now() < found->second)
        return true;
    stalled_until_.erase(found);
    return false;
}

RpcReply MetadataManager::commit_call(const NodeInfo& owner, MessageType type,
                                      std::span<const uint8_t> payload, FrameType frame_type) {
    if (stalled(owner.id))
        throw std::runtime_error("peer stalled on an earlier commit call");
    auto rpc = node_.call_async(owner, type, payload, frame_type);
    const auto stall = node_.config().write_stall;
    const auto started = Clock::now();
    while (rpc.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {
        // A route that cannot say how long it has been idle is judged by how
        // long the call has taken.
        auto quiet = rpc.idle_for();
        if (quiet == std::chrono::milliseconds::max())
            quiet = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        if (quiet < stall)
            continue;
        rpc.cancel();
        {
            Lock lock(stalled_mutex_);
            stalled_until_[owner.id] = Clock::now() + node_.config().dead_after;
        }
        Log::warn("metadata commit call stalled peer=" + owner.host + " no_progress_ms=" +
                  std::to_string(stall.count()) + "; not asked again for " +
                  std::to_string(node_.config().dead_after.count()) + " ms");
        throw std::runtime_error("peer made no progress on a commit call");
    }
    return rpc.get();
}

bool MetadataManager::store_commit_on(const NodeInfo& owner,
                                      const MetadataHistoryEntry& compact,
    const MetadataRecord& record,
                                      FrameType frame_type) {
    if (owner.id == node_.node_id()) {
        if (compact.body != MetadataHistoryEntry::Body::delta)
            return local_.replica().store_commit(record);
        if (local_.replica().store_commit(record, compact.payload))
            return true;

        // The full record is in the proposal. A compact body can be rejected for a
        // missing parent or a non-reconstructing exact delta; fall back to the full
        // body once, as remote replicas do.
        Log::warn("local metadata delta rejected; retrying full record generation=" +
                  std::to_string(record.generation));
        return local_.replica().store_commit(record);
    }

    try {
        auto encoded = encode_metadata_history_entry(compact);
        auto reply = commit_call(owner, MessageType::put_metadata_commit, encoded, frame_type);
        if (bool_reply(reply))
            return true;

        // A valid replica may not yet have the delta's parent. Supply it and retry
        // the compact body before a full fallback, so replica lag does not turn
        // every reconciliation into a full snapshot.
        if (compact.body == MetadataHistoryEntry::Body::delta) {
            if (push_history_to_peer(owner, compact.previous, frame_type)) {
                encoded = encode_metadata_history_entry(compact);
                if (bool_reply(
                        commit_call(owner, MessageType::put_metadata_commit, encoded, frame_type)))
                    return true;
            }
            auto full = commit_history_entry(record);
            encoded = encode_metadata_history_entry(full);
            return bool_reply(
                commit_call(owner, MessageType::put_metadata_commit, encoded, frame_type));
        }
    } catch (const std::exception& error) {
        Log::debug("metadata commit store " + owner.host + ": " + error.what());
    }
    return false;
}

bool MetadataManager::accept_commit_on(const NodeInfo& owner,
                                       const MetadataAcceptance& acceptance,
                                       FrameType frame_type) {
    // NodeRuntime detects and notifies accepted-head changes for the local
    // path; do not announce again after this returns.
    if (owner.id == node_.node_id())
        return metadata_server_.accept_commit(acceptance);
    try {
        const auto encoded = encode_metadata_acceptance(acceptance);
        return bool_reply(
            commit_call(owner, MessageType::accept_metadata_commit, encoded, frame_type));
    } catch (const std::exception& error) {
        Log::debug("metadata acceptance " + owner.host + ": " + error.what());
        return false;
    }
}

MetadataManager::PublishedCommit MetadataManager::publish_commit(
    const std::vector<NodeInfo>& nodes, const MetadataRecord& record,
    std::span<const uint8_t> delta, FrameType frame_type) {
    // A commit is accepted once this node holds it. Further copies are sought
    // from the nodes present and owed to the rest; repair_once() delivers them.
    const size_t sought = node_.config().metadata_write_copies;

    const auto compact = commit_history_entry(record, delta);
    PublishedCommit out;
    out.record = record;

    auto ordered = order_commit_replicas(nodes, node_.node_id(), [&](const NodeId& peer) {
        return node_.peer_latency(peer);
    });

    // Store the commit on the nearest replicas until enough hold it or none is
    // left to ask. Receivers validate and append to the DAG without comparing
    // to their head. The local replica must hold it.
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
        if (out.stored_on.size() >= sought)
            break;
    }
    if (std::none_of(out.stored_on.begin(), out.stored_on.end(), [&](const NodeInfo& owner) {
            return owner.id == node_.node_id();
        }))
        throw std::runtime_error("local metadata commit store failed");

    out.acceptance.generation = record.generation;
    out.acceptance.hash = record.hash;
    out.acceptance.required = static_cast<uint32_t>(out.stored_on.size());
    out.acceptance.replicas.reserve(out.stored_on.size());
    for (const auto& owner : out.stored_on)
        out.acceptance.replicas.push_back(owner.id);
    std::sort(out.acceptance.replicas.begin(), out.acceptance.replicas.end());
    out.acceptance.replicas.erase(
        std::unique(out.acceptance.replicas.begin(), out.acceptance.replicas.end()),
        out.acceptance.replicas.end());

    // Acceptance is evidence about completed stores, not a second consensus.
    // Each holder first receives the commit's parent history, needed to
    // reconstruct the branch later.
    const auto parents = metadata_record_parents(record);
    // Local first: a peer must never hold an accepted commit its author lacks.
    std::stable_partition(out.stored_on.begin(), out.stored_on.end(), [&](const NodeInfo& owner) {
        return owner.id == node_.node_id();
    });
    bool local_accepted = false;
    for (const auto& owner : out.stored_on) {
        if (owner.id != node_.node_id() && !local_accepted)
            throw std::runtime_error("local metadata acceptance persistence failed");
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
        if (accepted_here)
            local_accepted = local_accepted || owner.id == node_.node_id();
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
    return out;
}

std::vector<std::pair<NodeInfo, MetadataAcceptance>> MetadataManager::discover_accepted_heads(
    const std::vector<NodeInfo>& nodes, FrameType frame_type) {
    std::vector<std::pair<NodeInfo, MetadataAcceptance>> out;
    for (const auto& owner : nodes) {
        try {
            std::vector<MetadataAcceptance> heads;
            if (owner.id == node_.node_id()) {
                heads = metadata_server_.heads();
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
        if (!local_.replica().store_commit(record))
            return false;
        return metadata_server_.accept_commit(acceptance);
    }
    if (!push_history_to_peer(owner, record.hash, frame_type)) {
        // Local history may be compactly rooted at this record; a full commit
        // suffices for any replica without earlier ancestry.
        if (!store_commit_on(owner, commit_history_entry(record), record, frame_type))
            return false;
    }
    return accept_commit_on(owner, acceptance, frame_type);
}

void MetadataManager::truncate_history(size_t record_threshold, uint64_t byte_threshold) {
    // Serialised against foreground mutations: both read and change the
    // accepted-head set.
    Lock mutation_lock(mutation_mutex_);
    if (local_.replica().recovery_required())
        return;
    if (const auto dropped = local_.replica().expire_set_aside(
            unix_ms(), node_.config().maintenance.garbage_grace))
        Log::warn("metadata: dropped " + std::to_string(dropped) +
                  " head(s) set aside for the absence horizon; their content never arrived");
    // A node's history is its own: once it holds one head and the history
    // has grown past the thresholds, the head becomes its root. Nothing a
    // peer may still hold depends on it, since heads merge without ancestry.
    (void)local_.replica().compact_history_if_safe(record_threshold, byte_threshold);
}

namespace {
// A merge that cannot be made with what this node can reach now.
struct MergeUnavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};
} // namespace

uint64_t MetadataManager::membership_stamp() const {
    auto active = node_.membership().active();
    std::sort(active.begin(), active.end(),
              [](const NodeInfo& a, const NodeInfo& b) { return a.id < b.id; });
    uint64_t stamp = 1469598103934665603ULL;
    for (const auto& peer : active)
        for (const auto byte : peer.id.bytes)
            stamp = (stamp ^ byte) * 1099511628211ULL;
    return stamp;
}

std::vector<MetadataRecord> MetadataManager::usable_heads() const {
    if (local_.replica().set_aside_generation() &&
        set_aside_stamp_.load(std::memory_order_acquire) != membership_stamp())
        local_.replica().clear_set_aside();
    if (!local_.replica().set_aside_generation()) {
        set_aside_count_.store(0, std::memory_order_relaxed);
        return local_.replica().accepted_heads();
    }
    const auto all = local_.replica().accepted_heads().size();
    auto usable = local_.replica().usable_heads();
    set_aside_count_.store(all > usable.size() ? all - usable.size() : 0,
                           std::memory_order_relaxed);
    return usable;
}

MetadataRecord MetadataManager::own_head(const std::vector<MetadataRecord>& heads) const {
    // The head carrying the most of what this node authored: by its current
    // author id first, then by each earlier one.
    std::vector<std::pair<std::vector<uint64_t>, const MetadataRecord*>> ranked;
    std::optional<std::vector<NodeId>> authors;
    for (const auto& head : heads) {
        try {
            const auto snapshot = decode_snapshot(head.payload);
            if (!authors)
                authors = local_.replica().author_ids(node_.node_id(),
                                                      snapshot.mutation_sequences);
            std::vector<uint64_t> sequences;
            for (const auto& author : *authors) {
                const auto found = snapshot.mutation_sequences.find(author);
                sequences.push_back(found == snapshot.mutation_sequences.end() ? 0
                                                                               : found->second);
            }
            ranked.emplace_back(std::move(sequences), &head);
        } catch (const std::exception&) {
        }
    }
    if (ranked.empty())
        return heads.front();
    const auto best = std::max_element(ranked.begin(), ranked.end(),
                                       [](const auto& a, const auto& b) {
                                           if (a.first != b.first)
                                               return a.first < b.first;
                                           return a.second->hash > b.second->hash;
                                       });
    return *best->second;
}

void MetadataManager::set_aside(const MetadataRecord& head, std::string_view reason) const {
    set_aside_stamp_.store(membership_stamp(), std::memory_order_release);
    if (local_.replica().set_aside(head.hash, unix_ms()))
        Log::warn("metadata head set aside until membership changes generation=" +
                  std::to_string(head.generation) + " reason=\"" + std::string(reason) + "\"");
}

MetadataRecord MetadataManager::read_group(const std::vector<NodeId>& replicas,
                                           FrameType frame_type) {
    auto nodes = replica_nodes(replicas);
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

    // Import each accepted head independently: the receiver stores the DAG
    // material and certificate, and is never asked to replace its head.
    for (const auto& [hash, head] : observed) {
        // Skip hashes recently confirmed unacceptable (see
        // unacceptable_head_retry_at_).
        {
            Lock lock(unacceptable_head_mutex_);
            auto found = unacceptable_head_retry_at_.find(hash);
            if (found != unacceptable_head_retry_at_.end() && Clock::now() < found->second)
                continue;
        }
        auto& local = local_.replica();
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
            accepted = metadata_server_.accept_commit(certificate) || accepted;
        if (!accepted) {
            constexpr auto retry_cooldown = std::chrono::seconds(30);
            {
                Lock lock(unacceptable_head_mutex_);
                unacceptable_head_retry_at_[hash] = Clock::now() + retry_cooldown;
            }
            Log::warn("ignoring metadata head without a valid acceptance certificate hash=" +
                      to_string(hash));
        } else {
            {
                Lock lock(unacceptable_head_mutex_);
                unacceptable_head_retry_at_.erase(hash);
            }
            // This certificate may expose a second head whose history this
            // node lacks; revisit it so the rootless healer fetches the
            // missing records now.
            for (const auto& owner : history_sources) {
                if (owner.id != node_.node_id())
                    (void)import_history_from_peer(owner, hash, frame_type);
            }
        }
    }

    // Serialises only merge-and-publish, so concurrent readers seeing one
    // divergence do not each mint a reconciliation commit. Taken lazily on
    // seeing more than one head; heads are re-read once held.
    // Guards no state, so the analysis need not see it held.
    std::optional<Lock> reconciliation_lock;
    for (;;) {
        auto heads = usable_heads();
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
            auto materialized = local_.replica().materialized(selected.hash);
            if (!materialized)
                throw MetadataNotReady("selected metadata head cannot be materialized");
            if (materialized->snapshot->extent_size &&
                materialized->snapshot->extent_size != node_.config().extent_size)
                throw std::runtime_error("cluster extent size does not match local configuration");
            // A cluster-wide configuration change may leave the accepted branch on
            // the old write floor; maybe_reconfigure() commits the transition at
            // max(old_floor, new_floor) before any write.
            return cache_record(selected, materialized->snapshot);
        }

        if (!reconciliation_lock) {
            const auto waited = Clock::now();
            reconciliation_lock.emplace(reconciliation_mutex_);
            observations().record("metadata.reconcile_wait_us", elapsed_us(waited));
            continue; // re-read heads now that we hold the lock; may already be resolved
        }
        const auto reconcile_started = Clock::now();
        // Where a reconciliation's time goes, stage by stage.
        auto stage_started = reconcile_started;
        const auto stage_ms = [&] {
            const auto now = Clock::now();
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - stage_started).count();
            stage_started = now;
            return ms;
        };

        // Fold the maximal head set deterministically, two branches at a time.
        // Each merge names both parents; a conflict decided on either head
        // stays decided.
        // A merge that cannot be made now never holds reads or writes: the
        // head that is not this node's own is set aside and the loop goes on
        // with the rest.
        const auto aside = [&](std::string_view reason) {
            const auto own = own_head({heads[0], heads[1]});
            set_aside(own.hash == heads[0].hash ? heads[1] : heads[0], reason);
        };
        try {
        auto left = heads[0];
        auto right = heads[1];
        auto left_materialized = local_.replica().materialized(left.hash);
        auto right_materialized = local_.replica().materialized(right.hash);
        if (!left_materialized || !right_materialized)
            throw MergeUnavailable("metadata merge head cannot be materialized");
        const auto materialise_ms = stage_ms();
        // The merge reads the two heads and nothing else. Two trees merge by
        // what differs between them; a branch still held as a map is
        // materialised with the other, and the result re-rooted.
        const auto& left_snapshot = *left_materialized->snapshot;
        const auto& right_snapshot = *right_materialized->snapshot;
        const auto tree_backed =
            left_snapshot.namespace_root.has_value() || right_snapshot.namespace_root.has_value();
        if (tree_backed && !namespace_store_)
            throw MetadataNotReady("no namespace node store is configured");
        const bool all_trees = left_snapshot.namespace_root && right_snapshot.namespace_root;
        MetadataMergeResult merged;
        std::optional<NamespaceTreeMerge> tree_merge;
        if (all_trees) {
            const auto reading =
                ControlNamespaceNodeStore::for_reading(local_.control(), *namespace_store_);
            tree_merge = merge_tree_backed_heads(left_snapshot, right_snapshot, left.hash,
                                                 right.hash, reading);
            merged = std::move(tree_merge->merged);
        } else {
            auto reading = namespace_store_
                               ? std::optional<ControlNamespaceNodeStore>(
                                     ControlNamespaceNodeStore::for_reading(local_.control(),
                                                                            *namespace_store_))
                               : std::nullopt;
            const auto materialise = [&](const MetadataSnapshot& snapshot) {
                if (!snapshot.namespace_root)
                    return snapshot;
                if (!reading)
                    throw MetadataNotReady("no namespace node store is configured");
                return attach_namespace(snapshot, *reading);
            };
            merged = merge_metadata_heads(materialise(left_snapshot),
                                          materialise(right_snapshot), left.hash, right.hash);
        }
        if (merged.snapshot.extent_size &&
            merged.snapshot.extent_size != node_.config().extent_size)
            throw std::runtime_error("cluster extent size does not match local configuration");
        const auto merge_ms = stage_ms();

        // The lower hash is the primary parent so every reconciler of the same
        // head pair produces the same merge commit. A merge is a pure join, not a
        // user mutation: the merged clock covers both parents. A fresh local
        // origin/sequence here would make simultaneous reconcilers mint sibling
        // merges forever.
        if (right.hash < left.hash) {
            std::swap(left, right);
        }
        merged.snapshot.metadata_voters.clear();
        merged.snapshot.merge_parents = {right.hash};

        // The merged namespace's tree; its nodes are written and replicated
        // before the record naming the root is published.
        if (tree_backed) {
            auto commit_nodes =
                ControlNamespaceNodeStore::for_commit(local_.control(), *namespace_store_);
            if (tree_merge)
                merged.snapshot.namespace_root =
                    update_namespace_tree(tree_merge->onto, commit_nodes, tree_merge->changes);
            else
                merged.snapshot = detach_namespace(std::move(merged.snapshot), commit_nodes);
        }

        const auto tree_ms = stage_ms();
        MetadataRecord reconciliation;
        reconciliation.generation = std::max(left.generation, right.generation) + 1;
        reconciliation.previous = left.hash;
        reconciliation.payload = tree_backed ? encode_snapshot_v14(merged.snapshot)
                                             : encode_snapshot(merged.snapshot);
        reconciliation.hash = metadata_hash(reconciliation.generation,
                                            reconciliation.previous,
                                            reconciliation.payload);

        Bytes reconciliation_delta;
        const auto& primary =
            left_materialized->record.hash == reconciliation.previous ? *left_materialized
                                                                      : *right_materialized;
        // A tree merge knows its namespace changes against the primary parent.
        // A merge of materialised trees does not, and is published whole.
        const auto delta =
            tree_merge ? tree_merge_delta(*primary.snapshot, merged.snapshot, tree_merge->changes)
            : tree_backed ? std::nullopt
                          : metadata_delta(*primary.snapshot, merged.snapshot);
        if (delta) {
            auto encoded = encode_metadata_delta(*delta);
            if (encoded.size() < reconciliation.payload.size())
                reconciliation_delta = std::move(encoded);
        }
        const auto encode_ms = stage_ms();

        // The merge claims what it introduces before it is published, as a
        // mutation does. Its dot is a fresh local sequence the merged clock
        // does not carry, so every reconciler still mints the same commit and
        // no earlier release here can void the claim; the head that carries
        // this node's next mutation covers it.
        if (publication_retention_) {
            const auto claim = local_.replica().reserve_mutation_dot(
                node_.node_id(), merged.snapshot.mutation_sequences);
            publication_retention_(MetadataPublicationContext{
                claim.author, claim.sequence, primary.record, merged.snapshot,
                delta ? &*delta : nullptr, false});
            // A merge is made off every caller's path, so the peers' share
            // is done here too, before the merge is offered to them.
            try {
                publication_retention_(MetadataPublicationContext{
                    claim.author, claim.sequence, primary.record, merged.snapshot,
                    delta ? &*delta : nullptr, true});
            } catch (const std::exception& error) {
                if (Log::enabled(LogLevel::debug))
                    Log::debug("metadata merge claims on peers deferred: " +
                               std::string(error.what()));
            }
        }
        const auto retention_ms = stage_ms();
        (void)publish_commit(nodes, reconciliation, reconciliation_delta, frame_type);
        const auto publish_ms = stage_ms();
        if (merged.conflicts_superseded)
            conflicts_superseded_.fetch_add(merged.conflicts_superseded, std::memory_order_relaxed);
        const auto reconcile_us = elapsed_us(reconcile_started);
        observations().record("metadata.reconcile_us", reconcile_us);
        Log::info("metadata histories reconciled generation=" +
                  std::to_string(reconciliation.generation) +
                  " ms=" + std::to_string(reconcile_us / 1000) +
                  " materialise_ms=" + std::to_string(materialise_ms) +
                  " merge_ms=" + std::to_string(merge_ms) +
                  " tree_ms=" + std::to_string(tree_ms) +
                  " encode_ms=" + std::to_string(encode_ms) +
                  " retention_ms=" + std::to_string(retention_ms) +
                  " publish_ms=" + std::to_string(publish_ms) +
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
        // accept_commit() drops accepted ancestors from the head set, so the loop
        // folds any remaining heads into this commit.
        } catch (const MergeUnavailable& error) {
            aside(error.what());
        } catch (const DecodeError& error) {
            // A tree node of one branch that no node present can supply.
            aside(error.what());
        }
    }
}

MetadataRecord MetadataManager::maybe_reconfigure(const MetadataRecord& initial) {
    auto snapshot = decode_snapshot(initial.payload);
    if (snapshot.extent_size && snapshot.extent_size != node_.config().extent_size)
        throw std::runtime_error("cluster extent size does not match local configuration");

    // Zero marks a legacy snapshot; any other value is only that marker.
    const bool policy_transition = snapshot.metadata_write_replicas_required == 0;
    const bool clear_legacy_metadata_voters = !snapshot.metadata_voters.empty();
    const bool data_policy_change = snapshot.data_replication != node_.config().replication;

    const auto active = node_.membership().active();

    if (!policy_transition && !clear_legacy_metadata_voters && !data_policy_change)
        return initial;

    snapshot.metadata_voters.clear();
    if (policy_transition)
        snapshot.metadata_write_replicas_required = 1;
    snapshot.merge_parents.clear();
    snapshot.data_replication = static_cast<uint32_t>(node_.config().replication);
    MetadataRecord proposed;
    proposed.generation = initial.generation + 1;
    proposed.previous = initial.hash;
    proposed.payload =
        snapshot.namespace_root ? encode_snapshot_v14(snapshot) : encode_snapshot(snapshot);
    proposed.hash = metadata_hash(proposed.generation, proposed.previous, proposed.payload);
    (void)publish_commit(active, proposed, {}, FrameType::control);
    Log::info("metadata policy transition committed generation=" +
              std::to_string(proposed.generation));
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
                              local_.replica().committed().generation > 1;
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

    // A joiner may form a virgin namespace only once every active bootstrap
    // peer has shown no durable post-genesis history exists.
    survey.complete = failed == 0;
    survey.durable_history = durable_history;
    return survey;
}

MetadataRecord MetadataManager::discover_or_form() {
    const auto active = node_.membership().active();
    if (!node_.config().bootstrap.empty() && active.size() == 1)
        throw MetadataNotReady("metadata replica set forming: waiting for bootstrap peer");

    std::vector<NodeId> ids;
    ids.reserve(active.size());
    for (const auto& peer : active)
        ids.push_back(peer.id);

    // Import any post-genesis accepted commit (and siblings) through the
    // ordinary branch path.
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

    // Virgin founders build byte-identical generation 2 without a
    // coordinator, so root timestamps use a fixed non-zero sentinel.
    root->second.ctime_ns = root->second.mtime_ns = 1;
    snapshot.metadata_voters.clear();
    snapshot.data_replication = static_cast<uint32_t>(node_.config().replication);
    snapshot.extent_size = node_.config().extent_size;
    snapshot.metadata_write_replicas_required = 1;
    snapshot.metadata_participants.clear();
    // Genesis is the first branch point; background convergence advances the
    // horizon to generation 2 once every founder has durably accepted it.
    snapshot.metadata_branch_floor = {};
    snapshot.retention_baseline_complete = true; // virgin namespace has no inherited objects

    MetadataRecord formed;
    formed.generation = base.generation + 1;
    formed.previous = base.hash;
    formed.payload = encode_snapshot(snapshot);
    formed.hash = metadata_hash(formed.generation, formed.previous, formed.payload);
    (void)publish_commit(active, formed, {}, FrameType::control);

    Log::info("metadata replica set formed active=" + std::to_string(active.size()));
    return cache_record(formed,
                        std::make_shared<MetadataSnapshot>(std::move(snapshot)));
}

MetadataRecord MetadataManager::read_record_base() {
    auto active = node_.membership().active();
    std::vector<NodeId> ids;
    ids.reserve(active.size());
    for (const auto& peer : active)
        ids.push_back(peer.id);
    if (local_.replica().committed().generation <= 1)
        return discover_or_form();
    return read_group(ids, FrameType::control);
}

MetadataRecord MetadataManager::read_record_uncached() {
    auto record = maybe_reconfigure(read_record_base());
    if (local_.replica().recovery_required())
        local_.replica().mark_recovered();
    return record;
}

MetadataRecord MetadataManager::read_record() {
    if (auto cached = cached_record())
        return *cached;
    try {
        return cache_record(read_record_uncached());
    } catch (const std::exception& error) {
        // Reads may use the last persisted local snapshot while the write floor is
        // unavailable; mutations never treat it as durable.
        // Reads never wait for peers or for a merge: this node's own head
        // serves.
        const auto heads = usable_heads();
        if (!local_.replica().recovery_required() && !heads.empty()) {
            const auto local = own_head(heads);
            if (local.generation > 1) {
                (void)error;
                return cache_record(local);
            }
        }
        throw;
    }
}

MetadataSnapshotView MetadataManager::snapshot_view(const WorkContext& context) {
    (void)WaitGuard::enter(context, MetadataView::converged_waits,
                           "MetadataManager::snapshot_view");
    return snapshot_view();
}

MetadataSnapshotView MetadataManager::snapshot_view() {
    if (auto cached = cached_snapshot_view())
        return *cached;
    auto record = read_record();
    if (auto cached = cached_snapshot_view())
        return *cached;

    // A concurrent notice can make the just-installed generation look stale.
    // That is not an error: return the coherent snapshot just obtained; the
    // next operation refreshes.
    {
        Lock lock(cache_mutex_);
        if (decoded_cache_ &&
            (decoded_generation_ > record.generation ||
             (decoded_generation_ == record.generation && decoded_hash_ >= record.hash))) {
            return coherent(MetadataSnapshotView{decoded_generation_,
                                                     decoded_namespace_revision_, decoded_hash_,
                                                     decoded_cache_});
        }
    }

    // Another reader may have displaced the decoded cache; decode the returned
    // record rather than fail with EIO.
    auto decoded = std::make_shared<MetadataSnapshot>(decode_snapshot(record.payload));
    return coherent(MetadataSnapshotView{record.generation, 0, record.hash, std::move(decoded)});
}

MetadataSnapshotView MetadataManager::local() {
    const auto revision = local_.replica().heads_revision();
    {
        Lock lock(cache_mutex_);
        if (local_view_ && local_view_revision_ == revision)
            return *local_view_;
    }
    if (local_.replica().recovery_required())
        return snapshot_view();
    const auto identities = local_.replica().usable_head_identities();
    if (identities.empty() || (identities.size() == 1 && identities.front().generation <= 1))
        return snapshot_view();

    MetadataRecord head;
    std::shared_ptr<const MetadataSnapshot> decoded;
    {
        // The head set moves far more often than the head this node reads.
        Lock lock(cache_mutex_);
        if (identities.size() == 1 && local_view_ &&
            local_view_->hash == identities.front().hash) {
            local_view_revision_ = revision;
            return *local_view_;
        }
        if (identities.size() == 1 && cache_ && decoded_cache_ &&
            cache_->hash == identities.front().hash && decoded_hash_ == cache_->hash) {
            head = *cache_;
            decoded = decoded_cache_;
        }
    }
    if (!decoded) {
        auto heads = usable_heads();
        if (heads.empty())
            return snapshot_view();
        head = heads.size() == 1 ? std::move(heads.front()) : own_head(heads);
        if (auto materialized = local_.replica().materialized(head.hash))
            decoded = materialized->snapshot;
        else
            decoded = std::make_shared<const MetadataSnapshot>(decode_snapshot(head.payload));
    }
    // The decoded cache follows, so the namespace revision that FUSE and the
    // catalogue wake on moves with this node's own head.
    (void)cache_record(head, decoded);
    Lock lock(cache_mutex_);
    auto view = coherent(MetadataSnapshotView{
        head.generation,
        decoded_hash_ == head.hash ? decoded_namespace_revision_ : 0, head.hash, decoded});
    local_view_ = view;
    local_view_revision_ = revision;
    return view;
}

std::optional<MetadataSnapshotView> MetadataManager::available_snapshot_view() const {
    // No I/O: adopts a snapshot already obtained and decoded, never turns an OS
    // lookup into metadata traffic.
    Lock lock(cache_mutex_);
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
    if (local_.replica().recovery_required())
        return {};
    const auto heads = usable_heads();
    if (heads.size() != 1)
        return {};

    // Claim release is local and causal: the sole accepted head's live set
    // decides which claims are needed, and its clock removes only dots this
    // branch observed, so unseen branch claims survive.
    auto current = available_snapshot_view();
    if (current && current->hash == heads.front().hash)
        return current;
    try {
        auto materialized = local_.replica().materialized(heads.front().hash);
        if (!materialized)
            return {};
        // A refusal becomes "no view" below, which stops retention release
        // rather than running it against an empty live set.
        return coherent(MetadataSnapshotView{heads.front().generation, 0, heads.front().hash,
                                                 materialized->snapshot});
    } catch (...) {
        return {};
    }
}

MetadataRecord MetadataManager::mutate_impl(
    const std::function<void(MetadataSnapshot&, MetadataDelta*)>& mutate, bool exact_delta,
    size_t retries, std::optional<MetadataMutationIdentity> identity) {
    Lock lock(mutation_mutex_);
    // This mutation's dot, kept across retries so an attempt that was
    // accepted after all is recognised rather than made twice.
    std::optional<MetadataDot> dot;
    if (identity && !identity->sequence)
        throw std::invalid_argument("metadata mutation identity sequence must be non-zero");

    for (size_t attempt = 0; attempt < retries; ++attempt) {
        const auto total_started = Clock::now();
        // Where a commit's time goes, stage by stage, for its debug line.
        auto stage_started = total_started;
        const auto stage_ms = [&] {
            const auto now = Clock::now();
            const auto ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - stage_started).count());
            stage_started = now;
            return ms;
        };

        MetadataRecord current;
        auto local_heads = usable_heads();
        const bool recovering = local_.replica().recovery_required();
        if (recovering || local_heads.empty() ||
            (local_heads.size() == 1 && local_heads.front().generation <= 1)) {
            current = read_record_uncached();
            if (recovering)
                local_.replica().mark_recovered();
        } else {
            // A mutation asks no peer and waits for no merge. With several
            // heads here it extends the one carrying this node's own latest
            // mutation; the merge follows in the background.
            current = maybe_reconfigure(local_heads.size() == 1 ? local_heads.front()
                                                                : own_head(local_heads));
        }

        const auto head_ms = stage_ms();
        auto snapshot = decode_snapshot(current.payload);
        const auto decode_ms = stage_ms();
        const bool clear_merge_parent_topology = !snapshot.merge_parents.empty();
        if (dot && clock_covers(snapshot.mutation_sequences, *dot)) {
            owe({current, {}});
            cache_record(current, std::make_shared<MetadataSnapshot>(std::move(snapshot)));
            return current;
        }
        if (identity) {
            auto clock = snapshot.mutation_sequences.find(identity->origin);
            if (clock != snapshot.mutation_sequences.end() &&
                clock->second >= identity->sequence) {
                // Already accepted (before a crash, or merged by a peer).
                owe({current, {}});
                cache_record(current, std::make_shared<MetadataSnapshot>(std::move(snapshot)));
                return current;
            }
        }
        if (!dot)
            dot = local_.replica().reserve_mutation_dot(node_.node_id(),
                                                        snapshot.mutation_sequences);
        const auto origin = dot->author;
        const std::optional<uint64_t> sequence = dot->sequence;

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
        // Provenance: every entry this mutation writes says which mutation
        // wrote it, against what the parent head held at its path.
        {
            const MetadataDot mutation{origin, *sequence};
            if (!snapshot.legacy_clock) {
                // Still the parent's clock here.
                snapshot.legacy_clock = snapshot.mutation_sequences;
                if (exact_delta)
                    supplied_delta.set_legacy_clock = snapshot.legacy_clock;
            }
            if (exact_delta ? supplied_delta.catalogue != CatalogueDelta::unchanged
                            : before->catalogue_root != snapshot.catalogue_root) {
                snapshot.catalogue_dot = mutation;
                if (exact_delta)
                    supplied_delta.set_catalogue_dot = mutation;
            }
            if (!exact_delta) {
                for (auto& [path, entry] : snapshot.entries) {
                    const auto prior = before->entries.find(path);
                    if (prior == before->entries.end())
                        stamp_entry_provenance(entry, path, nullptr, mutation);
                    else if (prior->second != entry)
                        stamp_entry_provenance(entry, path, &prior->second, mutation);
                }
            } else if (!supplied_delta.upsert_entries.empty() ||
                       !supplied_delta.append_entries.empty()) {
                const bool tree = snapshot.namespace_root.has_value();
                if (tree && !namespace_store_)
                    throw MetadataNotReady("no namespace node store is configured");
                // The parent's namespace: the tree is not yet rewritten; the
                // map already is, so the parent is decoded again.
                std::optional<ControlNamespaceNodeStore> reader;
                std::optional<MetadataSnapshot> parent;
                if (tree)
                    reader.emplace(ControlNamespaceNodeStore::for_reading(local_.control(),
                                                                          *namespace_store_));
                else
                    parent = decode_snapshot(current.payload);
                const auto prior_of = [&](const std::string& path) -> std::optional<FsEntry> {
                    if (tree)
                        return namespace_entry(snapshot, &*reader, path);
                    const auto found = parent->entries.find(path);
                    if (found == parent->entries.end())
                        return {};
                    return found->second;
                };
                for (auto& [path, entry] : supplied_delta.upsert_entries) {
                    const auto prior = prior_of(path);
                    stamp_entry_provenance(entry, path, prior ? &*prior : nullptr, mutation);
                    if (!tree)
                        snapshot.entries[path] = entry;
                }
                for (auto& [path, append] : supplied_delta.append_entries) {
                    append.content = mutation;
                    if (tree)
                        continue;
                    auto& entry = snapshot.entries.at(path);
                    entry.provenance.content = mutation;
                    if (entry.provenance.file_id == NodeId{})
                        entry.provenance.file_id = legacy_file_id(path);
                }
            }
        }
        snapshot.mutation_sequences[origin] = *sequence;
        if (exact_delta)
            supplied_delta.mutation_sequences[origin] = *sequence;
        if (identity) {
            snapshot.mutation_sequences[identity->origin] = identity->sequence;
            if (exact_delta)
                supplied_delta.mutation_sequences[identity->origin] = identity->sequence;
        }

        // Discipline 4. Keep tombstones in canonical order so the next
        // reconciliation's union is a delta (DLT7 sorts after applying edits), and
        // drop conflicts this mutation decided by rewriting their subject.
        const auto apply_ms = stage_ms();
        const bool resorted = !garbage_is_canonical(snapshot.garbage);
        if (resorted)
            canonicalise_garbage(snapshot.garbage);
        if (exact_delta && (resorted || !supplied_delta.upsert_garbage.empty() ||
                            !supplied_delta.erase_garbage.empty()))
            supplied_delta.canonical_garbage = true;

        // A tree-backed namespace commits by applying this mutation's change set
        // to the tree.
        if (snapshot.namespace_root) {
            // Without an exact delta there is no change set, and diffing both
            // namespaces is the cost the tree avoids.
            if (!exact_delta)
                throw MetadataNotReady("a tree-backed namespace requires an exact delta");
            // A map write is a change the delta does not describe and would be
            // silently dropped; refuse it.
            if (!snapshot.entries.empty())
                throw std::runtime_error(
                    "metadata mutation wrote the namespace map on a tree-backed snapshot");
            if (!namespace_store_)
                throw MetadataNotReady("no namespace node store is configured");
            auto nodes =
                ControlNamespaceNodeStore::for_commit(local_.control(), *namespace_store_);
            snapshot.namespace_root = apply_delta_to_namespace_tree(*snapshot.namespace_root,
                                                                    nodes, supplied_delta);
        }

        const auto tree_ms = stage_ms();
        // Drop the conflicts this mutation decided by rewriting their subject,
        // read from the namespace as the mutation leaves it.
        if (!snapshot.conflicts.empty()) {
            NamespaceLookup lookup;
            std::optional<ControlNamespaceNodeStore> reader;
            if (snapshot.namespace_root) {
                reader.emplace(
                    ControlNamespaceNodeStore::for_reading(local_.control(), *namespace_store_));
                lookup = [&](const std::string& path) {
                    return namespace_entry(snapshot, &*reader, path);
                };
            }
            if (const auto superseded = prune_superseded_conflicts(snapshot, lookup)) {
                conflicts_superseded_.fetch_add(superseded, std::memory_order_relaxed);
                if (exact_delta)
                    supplied_delta.replace_conflicts = snapshot.conflicts;
            }
        }

        const auto conflicts_ms = stage_ms();
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
            // An exact caller describes only its own edit; inherited branch topology
            // (merge_parents, cleared above) is settled here, so the write stays a
            // delta.
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

        const auto encode_ms = stage_ms();
        // This node's own claims, then the commit, both on this node alone.
        // The caller is answered from here; the peers' share is owed.
        uint64_t retention_ms = 0;
        if (publication_retention_) {
            const auto retention_started = Clock::now();
            publication_retention_(MetadataPublicationContext{
                origin, *sequence, current, snapshot, delta ? &*delta : nullptr, false});
            retention_ms = elapsed_ms(retention_started);
        }

        try {
            const auto publish_started = Clock::now();
            accept_locally(proposed, delta_payload, FrameType::read_ahead);
            local_.replica().note_author_accepted(*dot);
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

            auto committed = std::make_shared<const MetadataSnapshot>(std::move(snapshot));
            cache_record(proposed, committed);
            OwedCommit owed{proposed, {}};
            owed.claims = OwedCommit::Claims{origin, *sequence, current, committed,
                                             std::move(delta)};
            owe(std::move(owed));
            const auto total_ms = elapsed_ms(total_started);
            if (total_ms >= 100 && Log::enabled(LogLevel::debug)) {
                Log::debug("metadata mutate total_ms=" + std::to_string(total_ms) +
                           " head_ms=" + std::to_string(head_ms) +
                           " decode_ms=" + std::to_string(decode_ms) +
                           " apply_ms=" + std::to_string(apply_ms) +
                           " tree_ms=" + std::to_string(tree_ms) +
                           " conflicts_ms=" + std::to_string(conflicts_ms) +
                           " encode_ms=" + std::to_string(encode_ms) +
                           " retention_ms=" + std::to_string(retention_ms) +
                           " publish_ms=" + std::to_string(publish_ms) +
                           " mode=" + std::string(delta_payload.empty() ? "snapshot" : "delta") +
                           " delta_bytes=" + std::to_string(delta_payload.size()) +
                           " snapshot_bytes=" + std::to_string(proposed.payload.size()) +
                           " attempt=" + std::to_string(attempt + 1));
            }
            return proposed;
        } catch (const MetadataNotReady& error) {
            // If the commit reached the floor but certificate fan-out was interrupted,
            // the head set may already hold it; keep the sequence so the next
            // iteration recognises it rather than minting a second mutation.
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

    throw MetadataNotReady("metadata mutation could not be accepted");
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
    // A tree-backed commit needs the change set declared, not diffed.
    mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
        auto found = snapshot.conflicts.find(id);
        if (found == snapshot.conflicts.end())
            return;
        const auto conflict = found->second;
        if (conflict.kind == MetadataConflictKind::namespace_entry) {
            const auto& chosen = choice == "left"    ? conflict.left_entry
                                 : choice == "right" ? conflict.right_entry
                                                     : conflict.base_entry;
            // Through the working set: on a tree-backed snapshot a map write would
            // never be applied.
            auto nodes = namespace_store_
                             ? std::optional<ControlNamespaceNodeStore>(
                                   ControlNamespaceNodeStore::for_reading(local_.control(), *namespace_store_))
                             : std::nullopt;
            NamespaceWorkingSet working(snapshot, delta, nodes ? &*nodes : nullptr);
            if (chosen) {
                // A decision is a change even when it keeps the value in
                // place, so a head that has not seen it does not bring the
                // conflict back.
                auto decided = *chosen;
                ++decided.version;
                decided.ctime_ns = wall_time_ns();
                working.put(conflict.key, decided);
            } else {
                working.erase(conflict.key);
            }
        } else if (conflict.kind == MetadataConflictKind::catalogue_root) {
            snapshot.catalogue_root = choice == "left"    ? conflict.left_catalogue_root
                                      : choice == "right" ? conflict.right_catalogue_root
                                                          : conflict.base_catalogue_root;
            delta.catalogue = snapshot.catalogue_root ? CatalogueDelta::set : CatalogueDelta::clear;
            delta.catalogue_root = snapshot.catalogue_root;
        }
        snapshot.conflicts.erase(found);
        // The conflict set is part of the record, so the exact delta carries it.
        delta.replace_conflicts = snapshot.conflicts;
        resolved = true;
    });
    if (resolved)
        conflicts_resolved_.fetch_add(1, std::memory_order_relaxed);
    return resolved;
}

std::optional<std::optional<ObjectId>>
MetadataManager::common_ancestor_catalogue_root(const Hash256& left, const Hash256& right) const {
    const auto common = local_.replica().history_common_ancestor(left, right);
    if (!common)
        return {};
    const auto ancestor = local_.replica().materialized(*common);
    if (!ancestor)
        return {};
    return ancestor->snapshot->catalogue_root;
}

void MetadataManager::repair_once() {
    // Must not race a foreground mutation through discovery, head selection
    // or reconfiguration.
    std::vector<NodeInfo> active;
    MetadataRecord record;
    std::optional<MetadataAcceptance> acceptance;
    {
        Lock mutation_lock(mutation_mutex_);
        const auto all_active = node_.membership().active();
        active = all_active;
        if (active.empty())
            throw MetadataNotReady("metadata replicas unavailable");

        if (local_.replica().committed().generation <= 1) {
            // Do not bypass virgin-cluster policy fencing: read_group() can
            // filter incompatible peers before a protocol-20 policy exists and
            // let a mismatched cohort form.
            record = discover_or_form();
        } else {
            std::vector<NodeId> ids;
            ids.reserve(all_active.size());
            for (const auto& peer : all_active)
                ids.push_back(peer.id);
            record = maybe_reconfigure(read_group(ids, FrameType::speculative));
        }
        acceptance = local_.replica().acceptance(record.hash);
        if (!acceptance)
            throw MetadataNotReady("selected metadata head has no acceptance certificate");
    }

    // Convergence is replication, not head replacement: every active node is
    // offered the head and proof; a node on another branch keeps it as a
    // second head for read_group() to reconcile. Runs without the mutation
    // lock.
    // A node known only through gossip cannot be offered anything from here
    // and does not hold convergence back.
    const auto selected_generation = record.generation;
    bool incomplete = false;
    uint64_t holders = 0;
    for (const auto& owner : active) {
        if (replicate_accepted_head(owner, record, *acceptance, FrameType::speculative))
            ++holders;
        else if (node_.membership().directly_reachable(owner.id))
            incomplete = true;
    }
    head_holders_.store(holders, std::memory_order_relaxed);
    head_present_.store(active.size(), std::memory_order_relaxed);
    Lock mutation_lock(mutation_mutex_);
    if (incomplete)
        throw MetadataNotReady("metadata accepted-head replication incomplete");
    if (local_.replica().committed_generation() > selected_generation)
        return;

    auto materialized = local_.replica().materialized(record.hash);
    if (!materialized)
        throw MetadataNotReady("metadata repair head cannot be materialized");
    auto snapshot = *materialized->snapshot;
    // Retention release follows each node's sole accepted head and causal
    // clock; unknown concurrent claim dots survive.

    local_.replica().compact();
    cache_record(record, std::make_shared<MetadataSnapshot>(std::move(snapshot)));
}

Page<std::pair<std::string, FsEntry>, std::string>
MetadataManager::entries(const MetadataSnapshotView& view, Cursor<std::string> from,
                         Budget& budget) {
    if (!namespace_store_)
        return namespace_entries(*view.snapshot, nullptr, std::move(from), budget);
    (void)WaitGuard::enter(budget.context(), entries_waits, "MetadataManager::entries");
    auto nodes = ControlNamespaceNodeStore::for_reading(local_.control(), *namespace_store_);
    return namespace_entries(*view.snapshot, &nodes, std::move(from), budget);
}

} // namespace macha
