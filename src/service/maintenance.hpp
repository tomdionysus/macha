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
#include "dagda/context.h"
#include "dagda/node.h"
#include "metadata/metadata_manager.hpp"
#include "metadata/metadata_server.hpp"
#include "service/availability_service.hpp"
#include "service/convergence_demand.hpp"
#include "service/maintenance_clock.hpp"
#include "storage/unreferenced_since.hpp"

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

// How much of its byte rate background work keeps under CPU load: 1 while
// the process's CPU, less the maintenance thread's own, stays within
// `cpu_target` of a core; otherwise cpu_target over that load.
double background_cpu_scale(double process_cpu_seconds, double own_cpu_seconds, double wall_seconds,
                            double cpu_target);

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
    // The store node: its maintenance steps, which this pass drives until
    // heads move to it (MaintenanceOptions::automatic off), and its Context,
    // where reads and writes note their activity, the pacing signal.
    dagda::Node& store_node;
    MetadataView& metadata;
    MetadataMaintenance& metadata_upkeep;
    CatalogueManager& catalogue;
    // Its path table follows each survey of the store node's.
    AvailabilityService& availability;
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

// The node's maintenance pass, on one thread: Macha's metadata and catalogue
// convergence, head repair and history truncation, and between them the store
// node's steps in Macha's order (the claim walk and network repair, the
// inventory, tombstones, retention release, garbage collection, rebalance,
// compaction, scrub and the ledger's verify), paced against the classes
// above it.
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
    // scheduled a new run. Demand for a run already scheduled is a retry
    // trigger instead. Pass thread only, holding no lock.
    bool absorb_events();
    // Waits until absorb_events() calls for a pass, `deadline`, the retry
    // triggers fall due, or `stop`.
    void wait_for_events(const std::stop_token&, Clock::time_point deadline);
    // When the retry triggers bring a scheduled convergence run forward: at
    // the first unless the last run failed, else `convergence_settle` after
    // the last, at most `convergence_max_delay` after the first. Nullopt
    // without a trigger.
    std::optional<Clock::time_point> retry_trigger_due() const;
    void note_retry_trigger(Clock::time_point now);

    NodeRuntime& node_;
    LocalState& local_;
    MetadataServer& metadata_server_;
    dagda::Node& store_node_;
    dagda::Context& context_;
    MetadataView& metadata_;
    MetadataMaintenance& metadata_upkeep_;
    CatalogueManager& catalogue_;
    AvailabilityService& availability_;
    MediaInformationService& media_information_;
    NodeEvents& events_;
    MaintenancePort& port_;
    std::shared_ptr<MaintenanceClock> clock_;
    MaintenanceTraceHook maintenance_trace_;
    std::function<void(std::string_view)> maintenance_stage_hook_;
    Clock::time_point constructed_;

    bool cluster_stable_observed_{};

    // The event counts absorb_events() has turned into work.
    dagda::MaintenanceEvents absorbed_{};
    // Convergence demand that found a run already scheduled since that run
    // began: the first and the last.
    std::optional<Clock::time_point> retry_trigger_first_;
    Clock::time_point retry_trigger_last_{};
    bool convergence_failed_{};

    std::jthread thread_;
};

} // namespace macha
