// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "crypto.hpp"
#include "durability_domain.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
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

    std::filesystem::path root_, objects_, packs_, accounting_path_;
    uint64_t limit_{};
    uint64_t reserve_free_{};
    size_t pack_threshold_{};
    size_t pack_target_size_{};
    std::array<uint8_t, 32> key_{};
    LocalStoreMode mode_{LocalStoreMode::authoritative};
    std::atomic<uint64_t> used_{};
    mutable std::mutex m_;
    mutable std::condition_variable accounting_cv_;
    std::jthread scan_thread_;
    std::atomic_bool scan_complete_{};
    std::atomic_bool scan_failed_{};
    std::atomic_bool accounting_trusted_{};
    int accounting_fd_{-1};
    uint64_t accounting_sequence_{};
    unsigned accounting_slot_{};
    bool accounting_dirty_{};
    std::shared_ptr<DurabilityDomain> durability_domain_;
    uint64_t last_mutation_generation_{};
    std::map<ObjectId, uint64_t> provisional_generations_;
    std::deque<std::pair<uint64_t, ObjectId>> provisional_order_;

    std::map<ObjectId, PackEntry> packed_;
    uint64_t pack_dead_bytes_{};
    uint64_t next_pack_sequence_{1};
    std::filesystem::path active_pack_;
    uint64_t active_pack_size_{};

    std::filesystem::path path(const ObjectId&) const;
    void wait_for_accounting(std::unique_lock<std::mutex>&) const;
    bool restore_accounting();
    void persist_accounting(uint64_t used, uint8_t operation, const ObjectId&, uint64_t size,
                            bool durable);
    void mark_accounting_dirty_locked();
    void checkpoint_accounting_locked();
    void reap_durable_generations_locked();
    bool put_impl(const ObjectId&, std::span<const uint8_t>, StoreWriteDurability, uint64_t*);
    bool put_loose_locked(const ObjectId&, std::span<const uint8_t>, StoreWriteDurability,
                          uint64_t*, std::unique_lock<std::mutex>&);
    bool put_packed_locked(const ObjectId&, std::span<const uint8_t>, StoreWriteDurability,
                           uint64_t*, std::unique_lock<std::mutex>&);
    void scan(std::stop_token);
    void rebuild_pack_index_locked(bool truncate_incomplete_tail);
    std::optional<Bytes> get_packed_locked(const ObjectId&) const;
    bool append_pack_record_locked(uint8_t type, const ObjectId&, std::span<const uint8_t>,
                                   uint64_t touched_ms, PackEntry*, uint64_t* record_size);
    void select_active_pack_locked(uint64_t next_record_size);
    bool compact_packs_locked();
    bool remove_locked(const ObjectId&);
    bool physical_space_available_locked(uint64_t need) const;

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
    bool compact_packs();
    uint64_t used() const { return used_.load(std::memory_order_relaxed); }
    uint64_t limit() const { return limit_; }
    bool scan_complete() const { return scan_complete_.load(std::memory_order_acquire); }
};
NodeId load_or_create_node_id(const std::filesystem::path&);
} // namespace macha
