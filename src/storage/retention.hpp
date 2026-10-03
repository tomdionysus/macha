// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/claim_store.hpp"
#include "contract/thread_safety.hpp"
#include "crypto.hpp"

#include <filesystem>
#include <span>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace macha {

// Durable local liveness evidence for immutable objects.
//
// Claims form an observed-remove set. For each object and origin we retain only
// the highest undominated add sequence and the highest remove context observed.
// A removal can therefore erase only claims causally visible to the metadata
// mutation that produced it; a concurrent branch re-affirmation survives.
class RetentionStore final : public ClaimStore {
    struct ObjectState {
        std::map<NodeId, uint64_t> adds;
        std::map<NodeId, uint64_t> removed;
    };

    using StateMap = std::map<ObjectId, ObjectState>;

    // claims.meta is the protocol-20 monolithic checkpoint, read if present.
    // Checkpoints are generation directories selected by claims.current and
    // sharded by the first SHA-256 byte, so no buffer is O(retained objects).
    std::filesystem::path legacy_checkpoint_path_;
    std::filesystem::path checkpoint_root_;
    std::filesystem::path checkpoint_manifest_path_;
    std::filesystem::path journal_path_;
    std::array<uint8_t, 32> key_{};
    // Held across the journal's fsync and compaction's shard writes.
    mutable IoMutex mutex_;
    StateMap data_ MACHA_GUARDED_BY(mutex_);
    StateMap control_ MACHA_GUARDED_BY(mutex_);
    std::optional<ObjectId> data_release_after_ MACHA_GUARDED_BY(mutex_);
    std::optional<ObjectId> control_release_after_ MACHA_GUARDED_BY(mutex_);
    std::optional<ObjectId> data_prune_after_ MACHA_GUARDED_BY(mutex_);
    std::optional<ObjectId> control_prune_after_ MACHA_GUARDED_BY(mutex_);
    size_t journal_records_ MACHA_GUARDED_BY(mutex_){};
    uint64_t journal_bytes_ MACHA_GUARDED_BY(mutex_){};

    StateMap& state_for(RetentionClass) MACHA_REQUIRES(mutex_);
    const StateMap& state_for(RetentionClass) const MACHA_REQUIRES(mutex_);
    std::optional<ObjectId>& cursor_for(RetentionClass) MACHA_REQUIRES(mutex_);
    std::optional<ObjectId>& prune_cursor_for(RetentionClass) MACHA_REQUIRES(mutex_);
    void apply_add_locked(RetentionClass, const RetentionDot&, const std::vector<ObjectId>&)
        MACHA_REQUIRES(mutex_);
    size_t apply_release_locked(RetentionClass, const RetentionClock&,
                                const std::vector<ObjectId>&) MACHA_REQUIRES(mutex_);
    void append_frame_locked(std::span<const uint8_t>) MACHA_REQUIRES(mutex_);
    Bytes encode_checkpoint_shard_locked(uint8_t shard) const MACHA_REQUIRES(mutex_);
    void decode_checkpoint_shard_locked(uint8_t shard, std::span<const uint8_t>)
        MACHA_REQUIRES(mutex_);
    void decode_legacy_checkpoint_locked(std::span<const uint8_t>) MACHA_REQUIRES(mutex_);
    void load_checkpoint_generation_locked() MACHA_REQUIRES(mutex_);
    void load_journal_locked() MACHA_REQUIRES(mutex_);
    void cleanup_checkpoint_generations_locked(std::string_view keep) const
        MACHA_REQUIRES(mutex_);
    void load();

  public:
    RetentionStore(std::filesystem::path state_path, std::array<uint8_t, 32> key);

    void retain(RetentionClass, const ObjectId&, const RetentionDot&) override;
    void retain_batch(RetentionClass, const std::vector<ObjectId>&, const RetentionDot&) override;
    bool retained(RetentionClass, const ObjectId&) const override;
    Claims claims(RetentionClass, const ObjectId&) const override;
    std::vector<ObjectId> retained_ids(RetentionClass) const override;
    std::optional<ObjectId> next_retained(RetentionClass, std::optional<ObjectId>& cursor,
                                          bool& complete) const override;

    // Remove causally-observed claims for objects which are no longer live in
    // the local accepted metadata view. `live` must be sorted/unique. The work
    // is bounded and one durable journal frame covers the complete slice.
    size_t release_unreferenced(RetentionClass, std::span<const ObjectId> live,
                                const RetentionClock& observed,
                                size_t operation_budget) override;

    size_t claim_objects(RetentionClass) const override;

    // Compact the append journal into an atomically-selected generation of 256
    // bounded encrypted shards. The journal is also compacted on a byte ceiling,
    // not only by record count, so recovery never has to absorb an unbounded WAL.
    bool compact_if_needed(size_t record_threshold = 4096) override;

    // Forget causality tombstones only when no claim remains and the physical
    // object is absent. Traversal is cursor-based so a small operation budget
    // cannot starve entries later in the ordered map indefinitely.
    size_t prune_unclaimed(RetentionClass, const std::function<bool(const ObjectId&)>& exists,
                           size_t operation_budget) override;
};

} // namespace macha
