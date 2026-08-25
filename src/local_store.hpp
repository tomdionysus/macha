// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "crypto.hpp"
#include <atomic>
#include <cstdint>
#include <filesystem>
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

class LocalStore {
  public:
    // A maintenance cursor owns the filesystem iterator state between scheduler
    // slices. It deliberately does not hold any LocalStore mutex or file handle
    // open across calls beyond what recursive_directory_iterator itself needs.
    struct Cursor {
        std::filesystem::recursive_directory_iterator iterator{};
        bool initialized{};
    };

  private:
    std::filesystem::path root_, objects_, accounting_path_;
    uint64_t limit_;
    std::array<uint8_t, 32> key_;
    LocalStoreMode mode_{LocalStoreMode::authoritative};
    std::atomic<uint64_t> used_{};
    mutable std::mutex m_;
    std::jthread scan_thread_;
    std::atomic_bool scan_complete_{};
    std::atomic_bool accounting_trusted_{};
    int accounting_fd_{-1};
    uint64_t accounting_sequence_{};
    unsigned accounting_slot_{};
    bool accounting_dirty_{};
    uint64_t mutation_generation_{};
    uint64_t durable_generation_{};
#if !defined(__linux__)
    // Linux can establish one filesystem-wide durability generation with
    // syncfs(). Portable fallback platforms retain the paths touched by a
    // deferred generation and fsync them only at the publication barrier.
    std::vector<std::filesystem::path> deferred_files_;
    std::vector<std::filesystem::path> deferred_directories_;
#endif
    std::filesystem::path path(const ObjectId&) const;
    void wait_for_accounting(std::unique_lock<std::mutex>&) const;
    bool restore_accounting();
    void persist_accounting(uint64_t used, uint8_t operation, const ObjectId&, uint64_t size,
                            bool durable);
    void mark_accounting_dirty_locked();
    void checkpoint_accounting_locked();
    bool put_impl(const ObjectId&, std::span<const uint8_t>, StoreWriteDurability, uint64_t*);
    void durability_barrier_locked(uint64_t required_generation);
    void scan(std::stop_token);
    bool remove_locked(const ObjectId&, StoreWriteDurability);

  public:
    LocalStore(std::filesystem::path, uint64_t, std::array<uint8_t, 32>,
               LocalStoreMode = LocalStoreMode::authoritative);
    ~LocalStore();
    bool put(const ObjectId&, std::span<const uint8_t>,
             StoreWriteDurability = StoreWriteDurability::immediate);
    // Stage one WAL-backed authoritative mutation and return the local mutation
    // generation which must be durable before that placement can be published.
    std::optional<uint64_t> put_deferred(const ObjectId&, std::span<const uint8_t>);
    // Establish stable storage through required_generation. A barrier which has
    // already covered that generation is a no-op even if newer mutations are
    // currently dirty; one physical barrier may therefore group-commit many
    // otherwise independent publication generations.
    void durability_barrier(uint64_t required_generation);
    // Flush every deferred authoritative mutation currently admitted. Used by
    // strict reaffirmation/destruction; publication paths should use the
    // generation-qualified overload above.
    void durability_barrier();
    uint64_t durable_generation() const;
    std::optional<Bytes> get(const ObjectId&) const;
    bool has(const ObjectId&) const;
    // Strong presence predicate for durability/repair decisions. Unlike has(),
    // this authenticates the encrypted object and re-verifies its content hash.
    bool valid(const ObjectId&) const noexcept;
    bool remove(const ObjectId&);
    bool remove_if_older_than(const ObjectId&, std::chrono::milliseconds);
    std::vector<ObjectId> list() const;
    // Returns one physical object and advances cursor. exhausted is true only
    // when this cursor has reached the end of a complete pass; the next call
    // starts a fresh pass.
    std::optional<ObjectId> next_object(Cursor&, bool& exhausted) const;
    bool older_than(const ObjectId&, std::chrono::milliseconds) const;
    std::filesystem::path object_path(const ObjectId&) const;
    uint64_t stored_size(const ObjectId&) const;
    std::filesystem::file_time_type last_write(const ObjectId&) const;
    void touch(const ObjectId&);
    uint64_t used() const {
        return used_.load(std::memory_order_relaxed);
    }
    uint64_t limit() const {
        return limit_;
    }
    bool scan_complete() const {
        return scan_complete_.load(std::memory_order_acquire);
    }
};
NodeId load_or_create_node_id(const std::filesystem::path&);
} // namespace macha
