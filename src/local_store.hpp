// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "crypto.hpp"
#include <atomic>
#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>
namespace macha {
class StorageLock {
    int fd_{-1};

  public:
    explicit StorageLock(const std::filesystem::path&);
    ~StorageLock();
    StorageLock(const StorageLock&) = delete;
    StorageLock& operator=(const StorageLock&) = delete;
};

class LocalStore {
    std::filesystem::path root_, objects_;
    uint64_t limit_;
    std::array<uint8_t, 32> key_;
    std::atomic<uint64_t> used_{};
    mutable std::mutex m_;
    mutable std::condition_variable scan_cv_;
    std::jthread scan_thread_;
    std::atomic_bool scan_complete_{};
    std::filesystem::path path(const ObjectId&) const;
    void wait_for_accounting(std::unique_lock<std::mutex>&) const;
    void scan(std::stop_token);

  public:
    LocalStore(std::filesystem::path, uint64_t, std::array<uint8_t, 32>);
    ~LocalStore();
    bool put(const ObjectId&, std::span<const uint8_t>);
    std::optional<Bytes> get(const ObjectId&) const;
    bool has(const ObjectId&) const;
    bool remove(const ObjectId&);
    std::vector<ObjectId> list() const;
    bool older_than(const ObjectId&, std::chrono::seconds) const;
    std::filesystem::path object_path(const ObjectId&) const;
    uint64_t stored_size(const ObjectId&) const;
    std::filesystem::file_time_type last_write(const ObjectId&) const;
    void touch(const ObjectId&);
    uint64_t used() const {
        return used_;
    }
    uint64_t limit() const {
        return limit_;
    }
    bool scan_complete() const {
        return scan_complete_.load();
    }
};
NodeId load_or_create_node_id(const std::filesystem::path&);
} // namespace macha
