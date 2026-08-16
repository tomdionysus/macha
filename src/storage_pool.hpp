// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "local_store.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

namespace macha {

class StoragePool {
    struct Backend;

  public:
    // Persistent local-object traversal. Maintenance owns one cursor per class
    // and advances it a bounded number of objects on each scheduler slice.
    struct Cursor {
        size_t backend_index{};
        size_t completed_backends{};
        size_t backend_count{};
        std::shared_ptr<LocalStore> store;
        LocalStore::Cursor local;
    };

    struct MaintenanceResult {
        uint64_t bytes{};
        size_t objects{};
        bool complete{};
        bool yielded{};
    };

  private:
    struct CursorItem {
        std::shared_ptr<Backend> backend;
        std::shared_ptr<LocalStore> store;
        std::filesystem::path path;
        ObjectId id;
    };

    std::filesystem::path state_path_;
    NodeId node_id_;
    std::array<uint8_t, 32> key_{};
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Backend>> backends_;
    Cursor rebalance_cursor_;
    Cursor scrub_cursor_;
    mutable std::atomic_uint64_t diag_gets_{};
    mutable std::atomic_uint64_t diag_get_bytes_{};
    mutable std::atomic_uint64_t diag_get_ms_{};
    mutable std::atomic_uint64_t diag_get_max_ms_{};
    mutable std::atomic_int64_t diag_get_report_ns_{};
    mutable std::atomic_uint64_t full_list_scans_{};

    void observe_get(size_t, uint64_t) const;

    std::vector<std::shared_ptr<Backend>> snapshot() const;
    std::filesystem::path identity_path(const std::filesystem::path&) const;
    bool activate(const std::shared_ptr<Backend>&);
    void deactivate(const std::shared_ptr<Backend>&, const std::shared_ptr<LocalStore>&,
                    const std::string&, uint64_t expected_generation = 0) const;
    std::vector<std::shared_ptr<Backend>> ranked(const ObjectId&) const;
    std::optional<CursorItem> next_physical(Cursor&, bool& pass_complete) const;

  public:
    StoragePool(std::filesystem::path state_path, NodeId, std::vector<StorageBackendConfig>,
                std::array<uint8_t, 32> key);
    void reconfigure(const std::vector<StorageBackendConfig>&);
    void refresh();

    bool put(const ObjectId&, std::span<const uint8_t>);
    std::optional<Bytes> get(const ObjectId&) const;
    bool has(const ObjectId&) const;
    bool remove(const ObjectId&);
    std::vector<ObjectId> list() const;
    uint64_t full_list_scans() const { return full_list_scans_.load(std::memory_order_relaxed); }
    std::optional<ObjectId> next_object(Cursor&, bool& pass_complete) const;
    bool older_than(const ObjectId&, std::chrono::seconds) const;

    MaintenanceResult rebalance_step(uint64_t budget_bytes, size_t operation_budget,
                                     const std::function<bool()>& should_yield = {});
    MaintenanceResult scrub_step(uint64_t budget_bytes, size_t operation_budget,
                                 const std::function<bool()>& should_yield = {});

    // Compatibility helper for callers/tests that explicitly request a complete
    // pass. Service maintenance uses rebalance_step() so a settled large store
    // is never enumerated in one scheduler tick.
    uint64_t rebalance_once(uint64_t budget_bytes = 0);

    uint64_t used() const;
    uint64_t limit() const;
    size_t online_backends() const;
};
} // namespace macha
