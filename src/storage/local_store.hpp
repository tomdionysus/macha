// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "crypto.hpp"
#include "storage/durability_domain.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include "storage/presence_index.hpp"
#include "contract/object_store.hpp"
#include "contract/thread_safety.hpp"
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>
namespace macha {
class StorageLock {
    int fd_{-1};

  public:
    explicit StorageLock(const std::filesystem::path&);
    ~StorageLock();
    StorageLock(const StorageLock&) = delete;
    StorageLock& operator=(const StorageLock&) = delete;
};

enum class StoreWriteDurability : uint8_t {
    immediate,
    deferred,
};

enum class LocalStoreMode : uint8_t {
    authoritative,
    ephemeral,
};

struct LocalStoreOptions {
    uint64_t limit{};
    uint64_t reserve_free{};
    size_t pack_threshold{};
    size_t pack_target_size{};
};

struct LocalStoreDiagnostics {
    uint64_t loose_reaffirmation_fast_paths{};
    uint64_t loose_reaffirmation_full_validations{};
    // Loose objects the start-up presence walk found (0 until it finishes).
    uint64_t presence_index_entries{};
    // Pack recovery at the last open: torn tails discarded, unreadable spans
    // skipped. Non-zero skipped figures mean lost objects, repaired from
    // replicas.
    uint64_t pack_recovery_truncated_tails{};
    uint64_t pack_recovery_skipped_regions{};
    uint64_t pack_recovery_skipped_bytes{};
};

class LocalStore final : public ObjectStore {
  public:
    struct Cursor {
        std::optional<ObjectId> packed_after;
        bool packed_done{};
        std::filesystem::recursive_directory_iterator iterator{};
        bool loose_initialized{};
    };

  private:
    struct PackEntry {
        std::filesystem::path file;
        uint64_t payload_offset{};
        uint64_t payload_size{};
        uint64_t plain_size{};
        uint64_t record_size{};
        uint64_t touched_unix_ms{};
        std::array<uint8_t, 12> nonce{};
        std::array<uint8_t, 16> tag{};
    };

    struct LooseStamp {
        uint64_t device{};
        uint64_t inode{};
        uint64_t size{};
        int64_t modified_ns{};
        int64_t changed_ns{};
        auto operator<=>(const LooseStamp&) const = default;
    };

    struct VerifiedLoose {
        LooseStamp stamp;
        uint64_t sequence{};
    };

    // Fixed at construction.
    const std::filesystem::path root_, objects_, packs_, accounting_path_;
    const uint64_t limit_{};
    const uint64_t reserve_free_{};
    const size_t pack_threshold_{};
    const size_t pack_target_size_{};
    const std::array<uint8_t, 32> key_{};
    const LocalStoreMode mode_{LocalStoreMode::authoritative};
    std::atomic<uint64_t> used_{};
    mutable std::atomic<uint64_t> losses_{};
    // The index mutex: memory work only. A holder that reaches the device
    // releases it first (Unlocked in local_store.cpp).
    mutable Mutex m_;
    // Held across pack appends, append rollback and compaction's pack I/O.
    // Serialises the pack stream apart from m_ so pack crypto and I/O do not
    // exclude loose-object or index operations. Taken before m_, never under it.
    mutable IoMutex pack_io_mutex_ MACHA_ACQUIRED_BEFORE(m_);
    // Per-object single-flight locks, so physical work on unrelated objects
    // does not serialise behind m_. Weak entries vanish after last use.
    mutable Mutex object_mutex_map_mutex_;
    mutable std::map<ObjectId, std::weak_ptr<IoMutex>>
        object_mutexes_ MACHA_GUARDED_BY(object_mutex_map_mutex_);
    uint64_t reserved_write_bytes_ MACHA_GUARDED_BY(m_){};
    std::function<void(const ObjectId&)> before_loose_write_for_tests_ MACHA_GUARDED_BY(m_);
    std::function<void(const ObjectId&)> before_loose_read_for_tests_ MACHA_GUARDED_BY(m_);
    std::function<void(const ObjectId&)> before_packed_read_for_tests_ MACHA_GUARDED_BY(m_);
    std::function<void(const ObjectId&)> before_packed_write_for_tests_ MACHA_GUARDED_BY(m_);
    std::function<void()> before_pack_compaction_for_tests_ MACHA_GUARDED_BY(m_);
    static constexpr size_t verified_loose_limit = 4096;
    mutable std::map<ObjectId, VerifiedLoose> verified_loose_ MACHA_GUARDED_BY(m_);
    // Loose objects present: published when a put installs the file, filled
    // from the object directory at start, forgotten on remove. Once warm-up
    // has listed the store, has() answers from it alone, with no per-object
    // lock and no disk; a cold stat per extent is too slow on a busy disk.
    // About 40 B per object.
    mutable PresenceIndex presence_ MACHA_GUARDED_BY(m_);
    mutable std::deque<std::pair<uint64_t, ObjectId>> verified_loose_order_ MACHA_GUARDED_BY(m_);
    mutable uint64_t verified_loose_sequence_ MACHA_GUARDED_BY(m_){};
    std::atomic_uint64_t loose_reaffirmation_fast_paths_{};
    std::atomic_uint64_t loose_reaffirmation_full_validations_{};
    mutable std::condition_variable accounting_cv_;
    // Started by the constructor, joined by the destructor.
    std::jthread scan_thread_;
    // Fills presence_ from object directory names at start (readdir only, no
    // stat). Started by the constructor, joined by the destructor.
    std::jthread presence_thread_;
    std::atomic_uint64_t presence_index_entries_{};
    std::atomic_uint64_t pack_recovery_truncated_tails_{};
    std::atomic_uint64_t pack_recovery_skipped_regions_{};
    std::atomic_uint64_t pack_recovery_skipped_bytes_{};
    std::atomic_bool scan_complete_{};
    std::atomic_bool scan_failed_{};
    std::atomic_bool accounting_trusted_{};
    // A dirty checkpoint's `used` is an estimate while the reconciling scan
    // runs; puts and removes are admitted against it rather than waiting.
    std::atomic_bool accounting_estimate_{};
    // Opened by the constructor, closed by the destructor.
    int accounting_fd_{-1};
    // Written by persist_accounting() outside m_: only by the constructor, the
    // destructor, or the holder of accounting_dirty_in_progress_.
    uint64_t accounting_sequence_{};
    unsigned accounting_slot_{};
    bool accounting_dirty_ MACHA_GUARDED_BY(m_){};
    bool accounting_dirty_in_progress_ MACHA_GUARDED_BY(m_){};
    mutable std::condition_variable accounting_state_cv_;
    // Set by the constructor, fixed afterwards.
    std::shared_ptr<DurabilityDomain> durability_domain_;
    uint64_t last_mutation_generation_ MACHA_GUARDED_BY(m_){};
    std::map<ObjectId, uint64_t> provisional_generations_ MACHA_GUARDED_BY(m_);
    std::deque<std::pair<uint64_t, ObjectId>> provisional_order_ MACHA_GUARDED_BY(m_);

    std::map<ObjectId, PackEntry> packed_ MACHA_GUARDED_BY(m_);
    mutable std::map<std::filesystem::path, size_t> active_pack_readers_ MACHA_GUARDED_BY(m_);
    mutable std::condition_variable pack_readers_cv_;
    uint64_t pack_dead_bytes_ MACHA_GUARDED_BY(m_){};
    uint64_t next_pack_sequence_ MACHA_GUARDED_BY(pack_io_mutex_){1};
    std::filesystem::path active_pack_ MACHA_GUARDED_BY(pack_io_mutex_);
    uint64_t active_pack_size_ MACHA_GUARDED_BY(pack_io_mutex_){};

    std::filesystem::path path(const ObjectId&) const;
    void wait_for_accounting(Lock&) const MACHA_REQUIRES(m_);
    bool wait_for_accounting(Lock&, std::stop_token, bool exact = false) const
        MACHA_REQUIRES(m_);
    bool restore_accounting(bool* clean);
    void persist_accounting(uint64_t used, uint8_t operation, const ObjectId&, uint64_t size,
                            bool durable);
    // Releases m_ across the marker write.
    void ensure_accounting_dirty(Lock&) MACHA_REQUIRES(m_);
    void reap_durable_generations_locked() MACHA_REQUIRES(m_);
    bool put_impl(const ObjectId&, std::span<const uint8_t>, StoreWriteDurability, uint64_t*)
        MACHA_EXCLUDES(m_, pack_io_mutex_);
    // Store a new object; m_ is released across the I/O. `generation` is the
    // durability generation to await (0 when none).
    bool put_loose_locked(const ObjectId&, std::span<const uint8_t>, uint64_t& generation,
                          Lock&) MACHA_REQUIRES(m_) MACHA_EXCLUDES(pack_io_mutex_);
    bool put_packed_locked(const ObjectId&, std::span<const uint8_t>, uint64_t& generation,
                           Lock&) MACHA_REQUIRES(m_) MACHA_EXCLUDES(pack_io_mutex_);
    void scan(std::stop_token) MACHA_EXCLUDES(m_);
    void warm_presence_index(std::stop_token) MACHA_EXCLUDES(m_);
    // Constructor only, before any other thread exists, so it takes no lock.
    void rebuild_pack_index(bool truncate_incomplete_tail) MACHA_NO_THREAD_SAFETY_ANALYSIS;
    // Releases m_ across the read.
    std::optional<Bytes> get_packed_locked(const ObjectId&, Lock&) const MACHA_REQUIRES(m_);
    // Releases m_ and takes pack_io_mutex_ across the append.
    bool append_pack_record_locked(uint8_t type, const ObjectId&, std::span<const uint8_t>,
                                   uint64_t touched_ms, PackEntry*, uint64_t* record_size,
                                   Lock&) MACHA_REQUIRES(m_) MACHA_EXCLUDES(pack_io_mutex_);
    void select_active_pack_locked(uint64_t next_record_size) MACHA_REQUIRES(pack_io_mutex_);
    // Releases m_ (keeping pack_io_mutex_) across the compaction I/O.
    bool compact_packs_locked(Lock&) MACHA_REQUIRES(m_, pack_io_mutex_);
    // Releases m_ across the unlink.
    bool remove_locked(const ObjectId&, Lock&) MACHA_REQUIRES(m_)
        MACHA_EXCLUDES(pack_io_mutex_);
    bool physical_space_available_locked(uint64_t need) const MACHA_REQUIRES(m_);
    bool filesystem_space_available_for_reservations() const MACHA_EXCLUDES(m_);
    static std::optional<LooseStamp> loose_stamp(const std::filesystem::path&);
    void remember_verified_loose_locked(const ObjectId&, const LooseStamp&) const
        MACHA_REQUIRES(m_);
    void forget_verified_loose_locked(const ObjectId&) const MACHA_REQUIRES(m_);
    // Removes a zero-byte loose file and forgets the object. Every object has
    // a fixed header, so an empty file is only left by a crash or external
    // truncation. Caller holds the object's mutex, not m_. True if pruned.
    bool prune_empty_loose(const ObjectId&, const std::filesystem::path&) const
        MACHA_EXCLUDES(m_);
    // has() from the pack and presence indexes, or none before warm-up. Waits
    // only on m_; its body is a no-I/O region, so taking a lock held across
    // I/O here does not compile under Clang.
    std::optional<bool> presence_from_index(const ObjectId&) const MACHA_EXCLUDES(m_);
    // The object's lock, held across that object's device I/O.
    std::shared_ptr<IoMutex> object_mutex(const ObjectId&) const
        MACHA_EXCLUDES(object_mutex_map_mutex_);

  public:
    LocalStore(std::filesystem::path, LocalStoreOptions, std::array<uint8_t, 32>,
               LocalStoreMode = LocalStoreMode::authoritative,
               std::shared_ptr<DurabilityDomain> = {});
    // Loose objects only, for the cache and tests; StoragePool passes
    // LocalStoreOptions.
    LocalStore(std::filesystem::path, uint64_t, std::array<uint8_t, 32>,
               LocalStoreMode = LocalStoreMode::authoritative,
               std::shared_ptr<DurabilityDomain> = {});
    ~LocalStore();
    bool put(const ObjectId&, std::span<const uint8_t>);
    std::optional<uint64_t> put_deferred(const ObjectId&, std::span<const uint8_t>);
    void durability_barrier(uint64_t required_generation,
                            DurabilityUrgency = DurabilityUrgency::batchable);
    void durability_barrier();
    uint64_t durable_generation() const;
    uint64_t durability_domain_id() const noexcept;
    std::optional<Bytes> get(const ObjectId&) const;
    // True once a put has installed the object; false while it is being
    // written and after a remove. Once presence_authoritative(), waits only on
    // the index mutex and touches no device; before then a miss is checked on
    // disk under the object's lock and a zero-byte file is pruned. Never
    // verifies content: use get()/valid() for that.
    bool has(const ObjectId&) const noexcept override;
    uint64_t losses() const noexcept override {
        return losses_.load(std::memory_order_acquire);
    }
    // Whether warm-up has finished and has() answers from the index alone.
    bool presence_authoritative() const {
        Lock lock(m_);
        return presence_.authoritative();
    }
    bool valid(const ObjectId&) const noexcept;
    bool remove(const ObjectId&);
    bool remove_if_older_than(const ObjectId&, std::chrono::milliseconds);
    std::vector<ObjectId> list() const;
    std::optional<ObjectId> next_object(Cursor&, bool& exhausted) const;
    bool older_than(const ObjectId&, std::chrono::milliseconds) const;
    std::filesystem::path object_path(const ObjectId&) const;
    uint64_t stored_size(const ObjectId&) const;
    std::filesystem::file_time_type last_write(const ObjectId&) const;
    void touch(const ObjectId&);
    bool is_packed(const ObjectId&) const;
    bool compact_packs(std::stop_token = {});
    void set_before_loose_write_for_tests(std::function<void(const ObjectId&)> hook) {
        Lock lock(m_);
        before_loose_write_for_tests_ = std::move(hook);
    }
    void set_before_packed_read_for_tests(std::function<void(const ObjectId&)> hook) {
        Lock lock(m_);
        before_packed_read_for_tests_ = std::move(hook);
    }
    void set_before_loose_read_for_tests(std::function<void(const ObjectId&)> hook) {
        Lock lock(m_);
        before_loose_read_for_tests_ = std::move(hook);
    }
    void set_before_packed_write_for_tests(std::function<void(const ObjectId&)> hook) {
        Lock lock(m_);
        before_packed_write_for_tests_ = std::move(hook);
    }
    void set_before_pack_compaction_for_tests(std::function<void()> hook) {
        Lock lock(m_);
        before_pack_compaction_for_tests_ = std::move(hook);
    }

    uint64_t used() const { return used_.load(std::memory_order_relaxed); }
    uint64_t limit() const { return limit_; }
    bool scan_complete() const { return scan_complete_.load(std::memory_order_acquire); }
    LocalStoreDiagnostics diagnostics() const noexcept {
        return {loose_reaffirmation_fast_paths_.load(std::memory_order_relaxed),
                loose_reaffirmation_full_validations_.load(std::memory_order_relaxed),
                presence_index_entries_.load(std::memory_order_relaxed),
                pack_recovery_truncated_tails_.load(std::memory_order_relaxed),
                pack_recovery_skipped_regions_.load(std::memory_order_relaxed),
                pack_recovery_skipped_bytes_.load(std::memory_order_relaxed)};
    }
};
NodeId load_or_create_node_id(const std::filesystem::path&);
} // namespace macha
