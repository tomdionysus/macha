// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"
#include "ledger/object_trie.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace macha {

// Which objects one store holds, kept on the state device so a restarted
// store knows at once instead of walking its disk. Once seeded it is the
// store's whole answer to "do I hold this"; until then the store answers as
// it always has and only queues what changes.
//
// The store queues each change under its own index lock, so they are queued
// in the order they happened, and flushes outside that lock: a flush journals
// the queue (an fsync) without holding the lookup lock, then installs it, so
// a lookup never waits on another write's sync. Until seeded a flush leaves
// the queue alone, so the seed's build never holds a write up. Thread safe.
class HeldLedger {
  public:
    HeldLedger(std::filesystem::path dir, std::array<uint8_t, 32> key,
               ObjectTrie::Options options);

    // Whether it holds a complete record: seeded once, kept since.
    bool seeded() const noexcept {
        return seeded_.load(std::memory_order_acquire);
    }
    // From the record once seeded; nothing before.
    std::optional<bool> held(const ObjectId&) const;
    // The recorded ids whose first two bytes are `prefix` (big-endian), in
    // order.
    std::vector<ObjectId> held_with_prefix(uint16_t prefix) const;
    // Queues a change. The caller holds the lock that orders its changes.
    void record(const ObjectId&, bool held);
    // Journals and installs every queued change, in order.
    void flush();
    // Replaces the record with what `snapshot` returns (sorted, unique),
    // marks it seeded and applies what was queued after the snapshot.
    // `snapshot` must drop the queued changes the snapshot already includes
    // (drop_queued() under the same lock it records under). Writes go on
    // meanwhile: their flushes leave the queue for the seed.
    void seed(const std::function<std::vector<ObjectId>()>& snapshot);
    // Drops every queued change. Called from within seed's snapshot.
    void drop_queued();
    // The record as installed now, and how many ids installs have marked not
    // held since open (a seed counts one, as it may drop any), read together.
    struct View {
        ObjectTrie::Snapshot records;
        uint64_t removals{};
    };
    View view() const;
    // The removals alone, without the lookup lock.
    uint64_t removals() const noexcept { return removals_.load(std::memory_order_acquire); }
    ObjectTrie::Stats stats() const;
    uint64_t size() const;
    // The objects/xx/yy directory the store's check against its disk
    // resumes at, kept across restarts. Single owner (that check).
    uint16_t verify_next() const noexcept { return verify_next_; }
    void set_verify_next(uint16_t);

  private:
    const std::filesystem::path dir_;
    std::atomic_bool seeded_{};
    uint16_t verify_next_{};
    // Held across a flush, so journal writes and installs keep the order the
    // changes were queued in.
    IoMutex flush_mutex_ MACHA_ACQUIRED_BEFORE(trie_mutex_);
    // Held to read or install; a lookup may read a node from disk.
    mutable IoMutex trie_mutex_;
    std::unique_ptr<ObjectTrie> trie_ MACHA_PT_GUARDED_BY(trie_mutex_);
    // Advanced under trie_mutex_ with the install it counts.
    std::atomic<uint64_t> removals_{};
    // Journals without the lookup lock: it touches only the trie's journal,
    // never the nodes or cache a lookup reads, and every journal write and
    // install happens under flush_mutex_, in order.
    void write_journal(std::span<const ObjectTrie::Change>) MACHA_REQUIRES(flush_mutex_)
        MACHA_NO_THREAD_SAFETY_ANALYSIS;
    Mutex queue_mutex_;
    std::vector<ObjectTrie::Change> queued_ MACHA_GUARDED_BY(queue_mutex_);
};

} // namespace macha
