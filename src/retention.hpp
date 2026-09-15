// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crypto.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace macha {

enum class RetentionClass : uint8_t {
    data = 1,
    control = 2,
};

struct RetentionDot {
    NodeId origin{};
    uint64_t sequence{};
    auto operator<=>(const RetentionDot&) const = default;
};

using RetentionClock = std::map<NodeId, uint64_t>;

// Durable local liveness evidence for immutable objects.
//
// Claims form an observed-remove set. For each object and origin we retain only
// the highest undominated add sequence and the highest remove context observed.
// A removal can therefore erase only claims causally visible to the metadata
// mutation that produced it; a concurrent branch re-affirmation survives.
class RetentionStore {
    struct ObjectState {
        std::map<NodeId, uint64_t> adds;
        std::map<NodeId, uint64_t> removed;
    };

    using StateMap = std::map<ObjectId, ObjectState>;

    // claims.meta is the protocol-20 legacy monolithic checkpoint. New
    // checkpoints are generation directories selected by claims.current and
    // split by the first SHA-256 byte, so no checkpoint read/write is O(total
    // retained objects) in temporary buffer size.
    std::filesystem::path legacy_checkpoint_path_;
    std::filesystem::path checkpoint_root_;
    std::filesystem::path checkpoint_manifest_path_;
    std::filesystem::path journal_path_;
    std::array<uint8_t, 32> key_{};
    mutable std::mutex mutex_;
    StateMap data_;
    StateMap control_;
    std::optional<ObjectId> data_release_after_;
    std::optional<ObjectId> control_release_after_;
    std::optional<ObjectId> data_prune_after_;
    std::optional<ObjectId> control_prune_after_;
    size_t journal_records_{};
    uint64_t journal_bytes_{};

    StateMap& state_for(RetentionClass);
    const StateMap& state_for(RetentionClass) const;
    std::optional<ObjectId>& cursor_for(RetentionClass);
    std::optional<ObjectId>& prune_cursor_for(RetentionClass);
    void apply_add_locked(RetentionClass, const RetentionDot&, const std::vector<ObjectId>&);
    size_t apply_release_locked(RetentionClass, const RetentionClock&,
                                const std::vector<ObjectId>&);
    void append_frame_locked(std::span<const uint8_t>);
    Bytes encode_checkpoint_shard_locked(uint8_t shard) const;
    void decode_checkpoint_shard_locked(uint8_t shard, std::span<const uint8_t>);
    void decode_legacy_checkpoint_locked(std::span<const uint8_t>);
    void load_checkpoint_generation_locked();
    void load_journal_locked();
    void cleanup_checkpoint_generations_locked(std::string_view keep) const;
    void load();

  public:
    RetentionStore(std::filesystem::path state_path, std::array<uint8_t, 32> key);

    void retain(RetentionClass, const ObjectId&, const RetentionDot&);
    void retain_batch(RetentionClass, const std::vector<ObjectId>&, const RetentionDot&);
    bool retained(RetentionClass, const ObjectId&) const;
    // Diagnostic: the observed-remove state of one object, `adds` and
    // `removed` keyed by origin. Empty when the object has no state.
    struct Claims {
        std::map<NodeId, uint64_t> adds;
        std::map<NodeId, uint64_t> removed;
    };
    Claims claims(RetentionClass, const ObjectId&) const;
    std::vector<ObjectId> retained_ids(RetentionClass) const;
    std::optional<ObjectId> next_retained(RetentionClass, std::optional<ObjectId>& cursor,
                                          bool& complete) const;

    // Remove causally-observed claims for objects which are no longer live in
    // the local accepted metadata view. `live` must be sorted/unique. The work
    // is bounded and one durable journal frame covers the complete slice.
    size_t release_unreferenced(RetentionClass, const std::vector<ObjectId>& live,
                                const RetentionClock& observed, size_t operation_budget);

    size_t claim_objects(RetentionClass) const;

    // Compact the append journal into an atomically-selected generation of 256
    // bounded encrypted shards. The journal is also compacted on a byte ceiling,
    // not only by record count, so recovery never has to absorb an unbounded WAL.
    bool compact_if_needed(size_t record_threshold = 4096);

    // Forget causality tombstones only when no claim remains and the physical
    // object is absent. Traversal is cursor-based so a small operation budget
    // cannot starve entries later in the ordered map indefinitely.
    size_t prune_unclaimed(RetentionClass, const std::function<bool(const ObjectId&)>& exists,
                           size_t operation_budget);
};

} // namespace macha
