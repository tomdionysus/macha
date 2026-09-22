// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "io_pressure.hpp"
#include "local_store.hpp"

#include <atomic>
#include <functional>
#include <map>
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
        bool deferred{};
    };

    struct DurabilityToken {
        uint64_t domain{};
        uint64_t generation{};
        uint64_t backend_instance{};

        bool valid() const noexcept { return domain && backend_instance; }
    };

  private:
    DiskServiceMonitor service_monitor_{};
    struct CursorItem {
        std::shared_ptr<Backend> backend;
        std::shared_ptr<LocalStore> store;
        std::filesystem::path path;
        ObjectId id;
    };

    std::filesystem::path state_path_;
    NodeId node_id_;
    std::array<uint8_t, 32> key_{};
    std::chrono::milliseconds durability_batch_window_{500};
    StoragePackingConfig packing_{};
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Backend>> backends_;
    mutable std::mutex domain_mutex_;
    uint64_t next_domain_id_{1};
    uint64_t next_backend_instance_{1};
    std::map<uint64_t, std::shared_ptr<DurabilityDomain>> domains_by_device_;
    Cursor rebalance_cursor_;
    Cursor scrub_cursor_;
    Cursor gc_cursor_;
    mutable std::atomic_uint64_t diag_gets_{};
    mutable std::atomic_uint64_t diag_get_bytes_{};
    mutable std::atomic_uint64_t diag_get_ms_{};
    mutable std::atomic_uint64_t diag_get_max_ms_{};
    mutable std::atomic_int64_t diag_get_report_ns_{};
    mutable std::atomic_uint64_t full_list_scans_{};
    std::atomic_size_t online_backends_cached_{};

    void observe_get(size_t, uint64_t) const;

    std::vector<std::shared_ptr<Backend>> snapshot() const;
    std::filesystem::path identity_path(const std::filesystem::path&) const;
    bool activate(const std::shared_ptr<Backend>&);
    void deactivate(const std::shared_ptr<Backend>&, const std::shared_ptr<LocalStore>&,
                    const std::string&, uint64_t expected_generation = 0) const;
    std::vector<std::shared_ptr<Backend>> ranked(const ObjectId&) const;
    std::optional<CursorItem> next_physical(Cursor&, bool& pass_complete) const;
    std::shared_ptr<DurabilityDomain> domain_for(const std::filesystem::path&);

  public:
    StoragePool(std::filesystem::path state_path, NodeId, std::vector<StorageBackendConfig>,
                std::array<uint8_t, 32> key,
                std::chrono::milliseconds durability_batch_window = std::chrono::milliseconds(500),
                StoragePackingConfig packing = StoragePackingConfig{0, 0});
    void reconfigure(const std::vector<StorageBackendConfig>&);
    void refresh();

    // Strict write. Provisional callers use put_deferred() so the durability
    // token cannot be discarded accidentally.
    // What this pool's devices are actually doing. Fed by every put and get
    // below, whatever class of work issued it, and consulted by DATA admission
    // so that work nobody is waiting for yields a slow device to work somebody
    // is. Lives here rather than in LocalStore because the contended thing is
    // the pool's physical backends -- the control store is a different device
    // and must not be gated by their pressure, which is exactly the
    // distinction the 2026-09-20 measurements turned on.
    DiskServiceMonitor& service_monitor() noexcept {
        return service_monitor_;
    }
    const DiskServiceMonitor& service_monitor() const noexcept {
        return service_monitor_;
    }
    void configure_service_monitor(DiskServiceMonitor::Thresholds thresholds) {
        service_monitor_.configure(thresholds);
    }

    bool put(const ObjectId&, std::span<const uint8_t>);
    std::optional<DurabilityToken> put_deferred(const ObjectId&, std::span<const uint8_t>);
    void durability_barrier(const DurabilityToken&,
                            DurabilityUrgency = DurabilityUrgency::batchable);
    bool durability_covered(const DurabilityToken&) const;
    // Re-derive a placement token for an object this pool already holds: the
    // answer to a durability question whose previous token died with a
    // process or backend incarnation. Flushes the holding backend first so
    // "present" implies "durable" for the current incarnation. Empty when no
    // online backend has the object.
    std::optional<DurabilityToken> reassert_durable(const ObjectId&);
    std::optional<Bytes> get(const ObjectId&) const;
    bool has(const ObjectId&) const;
    bool valid(const ObjectId&) const;
    bool remove(const ObjectId&);
    std::vector<ObjectId> list() const;
    uint64_t full_list_scans() const { return full_list_scans_.load(std::memory_order_relaxed); }
    std::optional<ObjectId> next_object(Cursor&, bool& pass_complete) const;
    bool older_than(const ObjectId&, std::chrono::milliseconds) const;

    MaintenanceResult rebalance_step(uint64_t budget_bytes, size_t operation_budget,
                                     const std::function<bool()>& should_yield = {});
    MaintenanceResult scrub_step(uint64_t budget_bytes, size_t operation_budget,
                                 const std::function<bool()>& should_yield = {});
    // Mark/sweep one bounded slice of authoritative local objects. `live` and
    // `protected_ids` must be sorted/unique. An unreferenced object is removed
    // only after it has also aged past orphan_grace; recent uncommitted puts
    // therefore cannot race metadata commit.
    MaintenanceResult gc_step(const std::vector<ObjectId>& live,
                              const std::vector<ObjectId>& protected_ids,
                              std::chrono::milliseconds orphan_grace,
                              size_t operation_budget,
                              const std::function<bool()>& should_yield = {},
                              const std::function<bool(const ObjectId&)>& is_retained = {});

    // Compatibility helper for callers/tests that explicitly request a complete
    // pass. Service maintenance uses rebalance_step() so a settled large store
    // is never enumerated in one scheduler tick.
    uint64_t rebalance_once(uint64_t budget_bytes = 0);

    // Reclaim dead records from packed authoritative DATA incrementally. Each
    // LocalStore invocation rewrites at most one pack, so temporary disk demand
    // is bounded by a pack rather than by the backend's complete live corpus.
    size_t compact_packs(std::stop_token = {});

    uint64_t used() const;
    uint64_t limit() const;
    size_t online_backends() const;
    LocalStoreDiagnostics diagnostics() const;
};
} // namespace macha
