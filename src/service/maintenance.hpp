// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/catalogue.hpp"
#include "catalogue/media_information.hpp"
#include "cluster/cluster.hpp"
#include "cluster/distributed_store.hpp"
#include "cluster/node_events.hpp"
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

// Where Service and the maintenance pass meet. Service owns it; it outlives
// the pass, since diagnostics are read before the pass exists and after it
// stops.
struct MaintenancePort {
    // Which metadata convergence passes are owed; the pass alone requests them,
    // from the node's events.
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

// What the maintenance pass is given at construction: the collaborators it
// works on, and the instruments and policy that are not collaborators. Every
// reference outlives the pass.
struct MaintenanceDependencies {
    NodeRuntime& node;
    LocalState& local;
    MetadataServer& metadata_server;
    DistributedStore& store;
    MetadataView& metadata;
    MetadataMaintenance& metadata_upkeep;
    CatalogueManager& catalogue;
    HorizonBuilder& builder;
    ObjectLedger& ledger;
    // Pruned when metadata changes.
    MediaInformationService& media_information;
    // Wakes the pass; it turns what it sees into work.
    NodeEvents& events;
    MaintenancePort& port;
    std::shared_ptr<MaintenanceClock> clock;
    MaintenanceTraceHook trace;
    std::function<void(std::string_view)> stage_hook;
    // When the Service was constructed: recovery times are measured from it.
    Clock::time_point constructed;
};

// The node's maintenance pass: metadata and catalogue
// convergence, the claim walk and network repair, tombstones, retention
// release, garbage collection, rebalance, compaction and scrub, on one
// thread, paced against the classes above it.
class Maintenance final {
  public:
    explicit Maintenance(MaintenanceDependencies);
    ~Maintenance();
    Maintenance(const Maintenance&) = delete;
    Maintenance& operator=(const Maintenance&) = delete;

    void start();
    // Signals the pass to stop and returns.
    void request_stop() noexcept;
    // Stops the pass, requesting it first if nobody has, and joins it.
    void stop();

  private:
    void run(std::stop_token);
    // Turns the events counted since the last call into work: metadata and
    // topology into convergence demand, metadata into a media-information
    // prune. True when they call for a pass: a storage event, or demand that
    // scheduled a new run. Pass thread only, holding no lock.
    bool absorb_events();
    // Waits until absorb_events() calls for a pass, `deadline`, or `stop`.
    void wait_for_events(std::stop_token, Clock::time_point deadline);
    std::vector<GarbageRef> collect_garbage(const std::vector<GarbageRef>&);
    void maintain_garbage_metadata(const std::vector<GarbageRef>& erase,
                                   const std::vector<GarbageRef>& stamp);

    NodeRuntime& node_;
    LocalState& local_;
    MetadataServer& metadata_server_;
    DistributedStore& store_;
    MetadataView& metadata_;
    MetadataMaintenance& metadata_upkeep_;
    CatalogueManager& catalogue_;
    HorizonBuilder& builder_;
    ObjectLedger& ledger_;
    MediaInformationService& media_information_;
    NodeEvents& events_;
    MaintenancePort& port_;
    std::shared_ptr<MaintenanceClock> clock_;
    MaintenanceTraceHook maintenance_trace_;
    std::function<void(std::string_view)> maintenance_stage_hook_;
    Clock::time_point constructed_;

    bool cluster_stable_observed_{};
    // The event counts absorb_events() has turned into work, and their sum.
    uint64_t absorbed_storage_{};
    uint64_t absorbed_metadata_{};
    uint64_t absorbed_topology_{};
    uint64_t absorbed_total_{};

    std::jthread thread_;
};

} // namespace macha
