// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/catalogue.hpp"
#include "cluster/cluster.hpp"
#include "cluster/distributed_store.hpp"
#include "component/component.hpp"
#include "component/dependencies.hpp"
#include "contract/horizon.hpp"
#include "contract/horizon_builder.hpp"
#include "contract/object_ledger.hpp"
#include "metadata/metadata_manager.hpp"
#include "service/convergence_demand.hpp"
#include "service/maintenance_clock.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace macha {

std::chrono::milliseconds maintenance_background_interval(const MaintenanceConfig&);

// One decision of the maintenance pass, for the decision trace: `kind` names
// what was decided (a gate, a claim walked, tombstones erased) and `detail`
// says how, deterministically (ids in hex, no times).
using MaintenanceTraceHook = std::function<void(std::string_view kind, std::string_view detail)>;

// Where Service and the maintenance pass meet. Service owns it and it
// outlives the component: events arrive and the diagnostics are read from
// the start of the node, before the pass exists, and after it has stopped.
struct MaintenancePort {
    // Service rings it: `event` advances, and the pass wakes when told to.
    std::mutex wait_mutex;
    std::condition_variable_any wait_cv;
    std::atomic_uint64_t event{1};
    // Which metadata convergence passes are owed.
    ConvergenceDemand metadata_convergence;

    // Diagnostics the pass writes. `stage` is where it is spending its time
    // (or where it is stuck). What the last pass decided before sleeping:
    // its wait deadline and the GC quiet window, both as ms from then (-1 =
    // unbounded, -3 = unset), plus the busy and gc-due flags.
    std::atomic_uint64_t wakeups{};
    std::atomic<const char*> stage{"starting"};
    std::atomic<int64_t> last_wait_ms{-3};
    std::atomic<int64_t> last_gc_quiet_ms{-3};
    std::atomic<uint8_t> last_flags{};
};

// The contracts the maintenance pass requires: concrete components at stage
// 0, the ledger and the horizon builder excepted; the metadata and ledger
// contracts (T3, T4) replace them. Every reference outlives the component.
using MaintenanceContracts = Dependencies<NodeRuntime, DistributedStore, MetadataManager,
                                          CatalogueManager, HorizonBuilder, const ObjectLedger,
                                          MaintenancePort>;

// What the maintenance pass is given: its contracts, and the instruments
// and policy that are not contracts.
struct MaintenanceDependencies {
    MaintenanceContracts contracts;
    std::shared_ptr<MaintenanceClock> clock;
    MaintenanceTraceHook trace;
    std::function<void(std::string_view)> stage_hook;
    // When the Service was constructed: recovery times are measured from it.
    Clock::time_point constructed;
};

// The node's maintenance pass, as a component: metadata and catalogue
// convergence, the claim walk and network repair, tombstones, retention
// release, garbage collection, rebalance, compaction and scrub, on one
// thread, paced against the classes above it.
class Maintenance final : public Component {
  public:
    explicit Maintenance(MaintenanceDependencies);
    ~Maintenance() override;

    std::string_view name() const noexcept override { return "maintenance"; }
    std::vector<std::string> required() const override { return MaintenanceContracts::names(); }
    void start() override;
    void request_stop() noexcept override;
    void stop() override;

  private:
    void run(std::stop_token);
    std::vector<GarbageRef> collect_garbage(const std::vector<GarbageRef>&);
    void maintain_garbage_metadata(const std::vector<GarbageRef>& erase,
                                   const std::vector<GarbageRef>& stamp);

    NodeRuntime& node_;
    DistributedStore& store_;
    MetadataManager& metadata_;
    CatalogueManager& catalogue_;
    HorizonBuilder& builder_;
    const ObjectLedger& ledger_;
    MaintenancePort& port_;
    std::shared_ptr<MaintenanceClock> clock_;
    MaintenanceTraceHook maintenance_trace_;
    std::function<void(std::string_view)> maintenance_stage_hook_;
    Clock::time_point constructed_;

    // The pass's state, carried from one pass to the next: the last
    // inventory built and the last complete release horizon (none until the
    // first of each).
    std::shared_ptr<const InventoryHorizon> inventory_;
    std::shared_ptr<const ReleaseHorizon> release_;
    bool cluster_stable_observed_{};

    std::jthread thread_;
};

} // namespace macha
