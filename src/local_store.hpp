// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "crypto.hpp"
#include "durability_domain.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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
};

class LocalStore {
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

    std::filesystem::path root_, objects_, packs_, accounting_path_;
    uint64_t limit_{};
    uint64_t reserve_free_{};
    size_t pack_threshold_{};
    size_t pack_target_size_{};
    std::array<uint8_t, 32> key_{};
    LocalStoreMode mode_{LocalStoreMode::authoritative};
    std::atomic<uint64_t> used_{};
    mutable std::mutex m_;
    // Serialises the append-only pack stream and compaction. It is deliberately
    // distinct from m_: crypto and pack filesystem I/O must not exclude
    // unrelated loose-object or index operations.
    mutable std::mutex pack_io_mutex_;
    // Physical work for unrelated immutable objects must not serialize behind
    // the store index/accounting mutex. Weak entries give an exact per-object
    // single-flight domain and disappear after their last active operation.
    mutable std::mutex object_mutex_map_mutex_;
    mutable std::map<ObjectId, std::weak_ptr<std::mutex>> object_mutexes_;
    uint64_t reserved_write_bytes_{};
    std::function<void(const ObjectId&)> before_loose_write_for_tests_;
    std::function<void(const ObjectId&)> before_packed_read_for_tests_;
    std::function<void(const ObjectId&)> before_packed_write_for_tests_;
    std::function<void()> before_pack_compaction_for_tests_;
    static constexpr size_t verified_loose_limit = 4096;
    mutable std::map<ObjectId, VerifiedLoose> verified_loose_;
    mutable std::deque<std::pair<uint64_t, ObjectId>> verified_loose_order_;
    mutable uint64_t verified_loose_sequence_{};
    std::atomic_uint64_t loose_reaffirmation_fast_paths_{};
    std::atomic_uint64_t loose_reaffirmation_full_validations_{};
    mutable std::condition_variable accounting_cv_;
    std::jthread scan_thread_;
    std::atomic_bool scan_complete_{};
    std::atomic_bool scan_failed_{};
    std::atomic_bool accounting_trusted_{};
    int accounting_fd_{-1};
    uint64_t accounting_sequence_{};
    unsigned accounting_slot_{};
    bool accounting_dirty_{};
    bool accounting_dirty_in_progress_{};
    mutable std::condition_variable accounting_state_cv_;
    std::shared_ptr<DurabilityDomain> durability_domain_;
    uint64_t last_mutation_generation_{};
    std::map<ObjectId, uint64_t> provisional_generations_;
    std::deque<std::pair<uint64_t, ObjectId>> provisional_order_;

    std::map<ObjectId, PackEntry> packed_;
    mutable std::map<std::filesystem::path, size_t> active_pack_readers_;
    mutable std::condition_variable pack_readers_cv_;
    uint64_t pack_dead_bytes_{};
    uint64_t next_pack_sequence_{1};
    std::filesystem::path active_pack_;
    uint64_t active_pack_size_{};

    std::filesystem::path path(const ObjectId&) const;
    void wait_for_accounting(std::unique_lock<std::mutex>&) const;
    bool wait_for_accounting(std::unique_lock<std::mutex>&, std::stop_token) const;
    bool restore_accounting();
    void persist_accounting(uint64_t used, uint8_t operation, const ObjectId&, uint64_t size,
                            bool durable);
    void ensure_accounting_dirty(std::unique_lock<std::mutex>&);
    void checkpoint_accounting_locked();
    void reap_durable_generations_locked();
    bool put_impl(const ObjectId&, std::span<const uint8_t>, StoreWriteDurability, uint64_t*);
    bool put_loose_locked(const ObjectId&, std::span<const uint8_t>, StoreWriteDurability,
                          uint64_t*, std::unique_lock<std::mutex>&);
    bool put_packed_locked(const ObjectId&, std::span<const uint8_t>, StoreWriteDurability,
                           uint64_t*, std::unique_lock<std::mutex>&);
    void scan(std::stop_token);
    void rebuild_pack_index_locked(bool truncate_incomplete_tail);
    std::optional<Bytes> get_packed_locked(const ObjectId&,
                                           std::unique_lock<std::mutex>&) const;
    bool append_pack_record_locked(uint8_t type, const ObjectId&, std::span<const uint8_t>,
                                   uint64_t touched_ms, PackEntry*, uint64_t* record_size,
                                   std::unique_lock<std::mutex>&);
    void select_active_pack_locked(uint64_t next_record_size);
    bool compact_packs_locked(std::unique_lock<std::mutex>&);
    bool remove_locked(const ObjectId&, std::unique_lock<std::mutex>&);
    bool physical_space_available_locked(uint64_t need) const;
    bool filesystem_space_available_for_reservations() const;
    static std::optional<LooseStamp> loose_stamp(const std::filesystem::path&);
    void remember_verified_loose_locked(const ObjectId&, const LooseStamp&) const;
    void forget_verified_loose_locked(const ObjectId&) const;
    std::shared_ptr<std::mutex> object_mutex(const ObjectId&) const;

  public:
    LocalStore(std::filesystem::path, LocalStoreOptions, std::array<uint8_t, 32>,
               LocalStoreMode = LocalStoreMode::authoritative,
               std::shared_ptr<DurabilityDomain> = {});
    // Compatibility constructor for cache/tests which deliberately want loose
    // objects. Production StoragePool passes explicit LocalStoreOptions.
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
    bool has(const ObjectId&) const;
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
        std::lock_guard lock(m_);
        before_loose_write_for_tests_ = std::move(hook);
    }
    void set_before_packed_read_for_tests(std::function<void(const ObjectId&)> hook) {
        std::lock_guard lock(m_);
        before_packed_read_for_tests_ = std::move(hook);
    }
    void set_before_packed_write_for_tests(std::function<void(const ObjectId&)> hook) {
        std::lock_guard lock(m_);
        before_packed_write_for_tests_ = std::move(hook);
    }
    void set_before_pack_compaction_for_tests(std::function<void()> hook) {
        std::lock_guard lock(m_);
        before_pack_compaction_for_tests_ = std::move(hook);
    }
    uint64_t used() const { return used_.load(std::memory_order_relaxed); }
    uint64_t limit() const { return limit_; }
    bool scan_complete() const { return scan_complete_.load(std::memory_order_acquire); }
    LocalStoreDiagnostics diagnostics() const noexcept {
        return {loose_reaffirmation_fast_paths_.load(std::memory_order_relaxed),
                loose_reaffirmation_full_validations_.load(std::memory_order_relaxed)};
    }
};
NodeId load_or_create_node_id(const std::filesystem::path&);
} // namespace macha
