// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/claim_store.hpp"
#include "contract/thread_safety.hpp"
#include "crypto.hpp"
#include "ledger/object_trie.hpp"
#include "storage/sealed_journal.hpp"

#include <filesystem>
#include <span>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace macha {

// Durable local liveness evidence for immutable objects.
//
// Claims form an observed-remove set. For each object and origin we retain only
// the highest undominated add sequence and the highest remove context observed.
// A removal can therefore erase only claims causally visible to the metadata
// mutation that produced it; a concurrent branch re-affirmation survives.
//
// Each class's claims live in an on-disk object trie (ledger-data and
// ledger-control under state/retention), one record an object, so memory is
// the tries' caches whatever the number of claims. claims.log is the write-ahead
// log: every add, release and prune is journaled and fsynced, then applied to
// the trie in memory; a checkpoint saves both tries and empties the log.
// Replaying the log over a checkpoint is idempotent.
class RetentionStore final : public ClaimStore {
    struct ClassLedger {
        std::unique_ptr<ObjectTrie> trie;
        std::optional<ObjectId> release_after;
        std::optional<ObjectId> prune_after;
    };

    std::filesystem::path root_;
    std::array<uint8_t, 32> key_{};
    // Held across the journal's fsync, a lookup's node read and a
    // checkpoint's writes.
    mutable IoMutex mutex_;
    ClassLedger data_ MACHA_GUARDED_BY(mutex_);
    ClassLedger control_ MACHA_GUARDED_BY(mutex_);
    // Objects changed since the last checkpoint: what the tries hold unsaved.
    uint64_t changed_ MACHA_GUARDED_BY(mutex_){};
    // claims.log: the add, release and prune frames since the last checkpoint.
    SealedJournal journal_ MACHA_GUARDED_BY(mutex_);

    ClassLedger& class_for(RetentionClass) MACHA_REQUIRES(mutex_);
    const ClassLedger& class_for(RetentionClass) const MACHA_REQUIRES(mutex_);
    void apply_add_locked(RetentionClass, const RetentionDot&, std::vector<ObjectId>)
        MACHA_REQUIRES(mutex_);
    size_t apply_release_locked(RetentionClass, const RetentionClock&, std::vector<ObjectId>)
        MACHA_REQUIRES(mutex_);
    size_t apply_prune_locked(RetentionClass, std::vector<ObjectId>) MACHA_REQUIRES(mutex_);
    void checkpoint_locked() MACHA_REQUIRES(mutex_);
    void checkpoint_if_due_locked() MACHA_REQUIRES(mutex_);
    // Visits records from after `cursor`, wrapping to the first once, until
    // `visit` returns false, `limit` are visited or it is back where it began.
    void scan_locked(const ClassLedger&, std::optional<ObjectId>& cursor, size_t limit,
                     const std::function<bool(const ObjectId&, std::span<const uint8_t>)>& visit)
        const MACHA_REQUIRES(mutex_);
    void migrate_locked() MACHA_REQUIRES(mutex_);
    void load_journal_locked() MACHA_REQUIRES(mutex_);
    void load(size_t cache_bytes);

  public:
    // `cache_bytes` is shared by the two tries, three quarters to the data one.
    RetentionStore(std::filesystem::path state_path, std::array<uint8_t, 32> key,
                   size_t cache_bytes = 8ULL * 1024 * 1024);

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

    // Checkpoints the tries and empties the journal once it holds
    // `record_threshold` frames or its byte ceiling, so recovery never
    // replays an unbounded log.
    bool compact_if_needed(size_t record_threshold = 4096) override;

    // Forget causality tombstones only when no claim remains and the physical
    // object is absent. Journaled. Traversal is cursor-based so a small
    // operation budget cannot starve later records indefinitely.
    size_t prune_unclaimed(RetentionClass, const std::function<bool(const ObjectId&)>& exists,
                           size_t operation_budget) override;
};

} // namespace macha
