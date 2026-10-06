// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/maintenance.hpp"
#include "service/claim_walk.hpp"
#include "contract/gates.hpp"
#include "metadata/namespace_control_store.hpp"
#include "diagnostics.hpp"
#include "fuse/fuse_frontend.hpp"
#include "log.hpp"
#include "observation.hpp"
#include "supervised.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <limits>
#include <map>
#include <memory>

namespace macha {

std::chrono::milliseconds maintenance_background_interval(const MaintenanceConfig& policy) {
    // Settled object passes may sleep for minutes, but metadata/catalogue control
    // convergence should still be verified regularly.
    return std::clamp(policy.no_progress_backoff, std::chrono::milliseconds(5000),
                      std::chrono::milliseconds(30000));
}

namespace {

std::filesystem::path scrub_due_path(const std::filesystem::path& state_path) {
    return state_path / "maintenance" / "scrub.next";
}

void persist_scrub_due(const std::filesystem::path& path, uint64_t due_unix_ms) {
    std::filesystem::create_directories(path.parent_path());
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot write scrub schedule " + temp.string());
        out << due_unix_ms << '\n';
        out.flush();
        if (!out)
            throw std::runtime_error("cannot flush scrub schedule " + temp.string());
    }
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    if (error) {
        std::filesystem::remove(temp, error);
        throw std::runtime_error("cannot publish scrub schedule " + path.string());
    }
}

uint64_t initialise_scrub_due(const std::filesystem::path& state_path,
                              std::chrono::milliseconds interval) {
    const auto path = scrub_due_path(state_path);
    {
        std::ifstream in(path);
        uint64_t due{};
        if (in >> due && due)
            return due;
    }

    const auto now = unix_ms();
    const auto interval_ms = static_cast<uint64_t>(std::max<int64_t>(1, interval.count()));
    const auto due = now > std::numeric_limits<uint64_t>::max() - interval_ms
                         ? std::numeric_limits<uint64_t>::max()
                         : now + interval_ms;
    try {
        persist_scrub_due(path, due);
    } catch (const std::exception& error) {
        // The schedule file is only a neighbourliness hint. Failure to persist it
        // must not stop the server; this process still honours the in-memory due time.
        Log::debug("maintenance: scrub schedule persistence unavailable: " +
                   std::string(error.what()));
    }
    return due;
}

void log_slow_stage(std::string_view stage, Clock::time_point started,
                    const std::string& detail = {}) {
    const auto ms = elapsed_ms(started);
    if (ms < 50 || !Log::enabled(LogLevel::all))
        return;
    Log::trace("DIAG maintenance-stage stage=" + std::string(stage) + " elapsed_ms=" +
               std::to_string(ms) + (detail.empty() ? std::string{} : " " + detail));
}
} // namespace

Maintenance::Maintenance(MaintenanceDependencies dependencies)
    : node_(dependencies.node), local_(dependencies.local),
      metadata_server_(dependencies.metadata_server), store_(dependencies.store), metadata_(dependencies.metadata),
      metadata_upkeep_(dependencies.metadata_upkeep), catalogue_(dependencies.catalogue),
      builder_(dependencies.builder), ledger_(dependencies.ledger),
      data_unreferenced_(dependencies.data_unreferenced),
      control_unreferenced_(dependencies.control_unreferenced),
      availability_(dependencies.availability),
      media_information_(dependencies.media_information), events_(dependencies.events),
      port_(dependencies.port),
      clock_(std::move(dependencies.clock)),
      maintenance_trace_(std::move(dependencies.trace)),
      maintenance_stage_hook_(std::move(dependencies.stage_hook)),
      constructed_(dependencies.constructed) {}

Maintenance::~Maintenance() {
    stop();
}

void Maintenance::start() {
    thread_ = std::jthread([this](std::stop_token maintenance_stop) {
        run_supervised_loop("service-maintenance", maintenance_stop,
                            [this, maintenance_stop] { run(maintenance_stop); });
    });
}

void Maintenance::request_stop() noexcept {
    if (!thread_.joinable())
        return;
    thread_.request_stop();
    {
        Lock lock(events_.wait_mutex);
    }
    events_.wait_cv.notify_all();
}

void Maintenance::stop() {
    if (!thread_.joinable())
        return;
    Log::debug("shutdown: service maintenance request_stop");
    thread_.request_stop();
    Log::debug("shutdown: service maintenance joining");
    thread_.join();
    Log::debug("shutdown: service maintenance joined");
}

// A sweep slice can add sightings every pass; the table is written when a
// sweep completes and otherwise at most twice a minute. A sighting lost to a
// crash is taken again, which only delays that object's deletion.
void Maintenance::save_sightings(UnreferencedSince& sightings, Clock::time_point& saved,
                                 bool complete) {
    const auto now = clock_->now();
    if (!complete && now - saved < std::chrono::seconds(30))
        return;
    saved = now;
    sightings.save();
}

std::vector<GarbageRef> Maintenance::collect_garbage(const std::vector<GarbageRef>& garbage) {
    const auto grace = node_.config().maintenance.garbage_grace;
    const auto grace_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(grace).count();
    const auto now_ns = clock_->wall_ns();
    std::vector<GarbageRef> matured;
    matured.reserve(garbage.size());

    for (const auto& candidate : garbage) {
        // A zero retirement time marks a legacy tombstone: ineligible until
        // maintain_garbage_metadata() stamps it, which grants a full grace period.
        if (candidate.retired_at_ns <= 0 || now_ns < candidate.retired_at_ns ||
            now_ns - candidate.retired_at_ns < grace_ns)
            continue;

        // A matured tombstone no longer protects the object from the reachability
        // sweep. Authoritative bytes are not deleted here: the sweep's atomic
        // age-check-and-remove spares a recently reaffirmed object even if this view
        // is stale. A cache copy is disposable and goes now.
        local_.cache().remove(candidate.id);
        matured.push_back(candidate);
    }
    return matured;
}

void Maintenance::maintain_garbage_metadata(const std::vector<GarbageRef>& erase,
                                        const std::vector<GarbageRef>& stamp) {
    if (erase.empty() && stamp.empty())
        return;

    std::map<ObjectId, GarbageRef> erase_expected;
    std::map<ObjectId, GarbageRef> stamp_expected;
    for (const auto& candidate : erase)
        erase_expected[candidate.id] = candidate;
    for (const auto& candidate : stamp)
        stamp_expected[candidate.id] = candidate;

    metadata_.mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
        std::erase_if(snapshot.garbage, [&](const GarbageRef& current) {
            auto expected = erase_expected.find(current.id);
            const bool remove = expected != erase_expected.end() && expected->second == current;
            if (remove)
                delta.erase_garbage.push_back(current.id);
            return remove;
        });

        const auto retired = clock_->wall_ns();
        for (auto& current : snapshot.garbage) {
            auto expected = stamp_expected.find(current.id);
            if (expected == stamp_expected.end() || expected->second != current)
                continue;
            // Legacy tombstones have no retirement time or ABA token; stamping them
            // keeps old storage safe while letting them leave metadata after grace.
            current.retired_at_ns = retired;
            current.retirement_id = random_node_id();
            delta.upsert_garbage.push_back(current);
        }
        std::sort(delta.erase_garbage.begin(), delta.erase_garbage.end());
        delta.erase_garbage.erase(
            std::unique(delta.erase_garbage.begin(), delta.erase_garbage.end()),
            delta.erase_garbage.end());
    });
}

double background_cpu_scale(double process_cpu_seconds, double own_cpu_seconds,
                            double wall_seconds, double cpu_target) {
    if (wall_seconds <= 0.0)
        return 1.0;
    const double load = std::max(0.0, process_cpu_seconds - own_cpu_seconds) / wall_seconds;
    if (load <= cpu_target)
        return 1.0;
    return std::clamp(cpu_target / load, 0.0, 1.0);
}

void Maintenance::run(std::stop_token stop) {
    const auto& policy = node_.config().maintenance;
    ThreadCpuReporter cpu_reporter("macha-maint", std::chrono::seconds(5), true);
    auto last_wall = clock_->now();
    auto last_cpu = std::clock();
    auto last_own_cpu_ns = thread_cpu_time_ns();
    uint64_t last_metadata_remote_epoch = node_.remote_metadata_epoch();
    uint64_t last_metadata_demand_epoch{};
    std::vector<NodeId> last_active_nodes;
    bool metadata_dirty = port_.metadata_convergence.pending();
    bool catalogue_dirty = true;
    auto metadata_retry_due = Clock::time_point{};
    auto metadata_retry_backoff = maintenance_background_interval(policy);
    auto catalogue_retry_due = Clock::time_point{};
    auto formation_settle_due = Clock::time_point{};
    uint64_t last_local_metadata_generation = local_.replica().committed_generation();
    // Events before the pass existed are not replayed; one convergence run
    // validates the state they left.
    absorbed_storage_ = events_.count(NodeEvent::storage);
    absorbed_metadata_ = events_.count(NodeEvent::metadata);
    absorbed_topology_ = events_.count(NodeEvent::topology);
    absorbed_claims_ = events_.count(NodeEvent::claims);
    absorbed_total_ =
        absorbed_storage_ + absorbed_metadata_ + absorbed_topology_ + absorbed_claims_;
    port_.metadata_convergence.request(metadata_server_.known_generation());
    uint64_t observed_event = absorbed_total_;
    auto network_quiescent_until = Clock::time_point{};
    auto local_quiescent_until = Clock::time_point{};
    auto gc_quiescent_until = Clock::time_point{};
    auto scrub_due_unix_ms = initialise_scrub_due(node_.config().state_path, policy.scrub_interval);
    double network_credit = 0.0;
    double local_credit = 0.0;
    // Replica repair shares time with every higher class by weight, in the
    // duty-cycle form the FUSE frontend uses for viewer:loader. It is paced, never
    // switched off: a node that is always busy must still repair.
    WeightedLoaderService repair_share(policy.foreground_weight, policy.repair_weight,
                                       std::chrono::milliseconds(25));
    double scrub_credit = 0.0;
    ClaimWalk data_claim_walk(RetentionClass::data);
    ClaimWalk control_claim_walk(RetentionClass::control);
    // Why the destructive DATA sweep did not run, logged at DEBUG when the answer
    // changes.
    std::string last_gc_skip_reason;
    bool release_horizon_incomplete_logged = false;

    // The decision trace: every gate's verdict and inputs on each pass that
    // evaluates it, and every action as it happens. Pass counts are timing, not
    // decisions. Nothing is recorded without a hook.
    const auto trace_gate = [&](std::string_view gate, bool open, const std::string& inputs) {
        if (maintenance_trace_)
            maintenance_trace_(gate, (open ? "open " : "shut ") + inputs);
    };
    const auto trace_action = [&](std::string_view kind, const std::string& detail) {
        if (maintenance_trace_)
            maintenance_trace_(kind, detail);
    };
    const auto flag = [](std::string_view name, bool value) {
        return std::string(name) + (value ? "=1" : "=0");
    };
    // The tombstones before the first inventory: none.
    static const std::vector<GarbageRef> no_garbage;
    // What repair pulls: this node's missing namespace extents, from its
    // holdings, and the objects outside the namespace; rebuilt when either
    // changes.
    std::shared_ptr<const std::vector<ObjectId>> pull_missing;
    std::shared_ptr<const InventoryHorizon> pull_inventory;
    std::vector<ObjectId> pull_list;

    while (!stop.stop_requested()) {
        port_.wakeups.fetch_add(1, std::memory_order_relaxed);
        const auto enter_stage = [this](const char* name) {
            port_.stage.store(name, std::memory_order_release);
        };
        enter_stage("pass-begin");
        auto now = clock_->now();
        absorb_events();
        metadata_dirty = port_.metadata_convergence.pending();
        const auto metadata_demand = port_.metadata_convergence.diagnostics().requested_epoch;
        if (metadata_demand != last_metadata_demand_epoch) {
            metadata_retry_due = Clock::time_point{};
            last_metadata_demand_epoch = metadata_demand;
        }
        const auto current_event = events_.total();
        const bool event_changed = current_event != observed_event;
        observed_event = current_event;
        const auto local_metadata_generation = local_.replica().committed_generation();
        if (local_metadata_generation != last_local_metadata_generation) {
            catalogue_dirty = true;
            metadata_retry_due = Clock::time_point{};
            last_local_metadata_generation = local_metadata_generation;
            gc_quiescent_until = now + policy.foreground_quiet;
        }
        if (event_changed) {
            network_quiescent_until = Clock::time_point{};
            local_quiescent_until = Clock::time_point{};
            // Object arrival and namespace publication are separate durable
            // operations. Start an exact quiet window so a sweep cannot race
            // the retention claim which makes newly-arrived bytes reachable.
            gc_quiescent_until = now + policy.foreground_quiet;
        }
        std::vector<NodeId> active_nodes;
        bool established_metadata_peer = false;
        for (const auto& peer : node_.membership().active()) {
            active_nodes.push_back(peer.id);
            established_metadata_peer = established_metadata_peer ||
                                        (peer.id != node_.node_id() &&
                                         peer.metadata_generation > 1);
        }
        std::sort(active_nodes.begin(), active_nodes.end());
        const auto remote_epoch = node_.remote_metadata_epoch();
        const bool topology_changed = active_nodes != last_active_nodes;
        if (remote_epoch != last_metadata_remote_epoch || topology_changed) {
            metadata_retry_due = Clock::time_point{};
            if (topology_changed)
                formation_settle_due = now + node_.config().dead_after;
            last_metadata_remote_epoch = remote_epoch;
            last_active_nodes = std::move(active_nodes);
        }
        auto wall_seconds = std::chrono::duration<double>(now - last_wall).count();
        if (wall_seconds <= 0.0)
            wall_seconds = std::chrono::duration<double>(policy.interval).count();
        // The load background work yields to is everyone else's: this
        // thread's own CPU (repair, GC, the census) does not count against it.
        auto cpu_now = std::clock();
        const auto own_cpu_ns = thread_cpu_time_ns();
        const double own_seconds =
            static_cast<double>(own_cpu_ns >= last_own_cpu_ns ? own_cpu_ns - last_own_cpu_ns : 0) / 1e9;
        const double process_seconds = static_cast<double>(cpu_now - last_cpu) / CLOCKS_PER_SEC;
        last_wall = now;
        last_cpu = cpu_now;
        last_own_cpu_ns = own_cpu_ns;

        auto playback_bytes = store_.take_foreground_bytes();
        auto interactive_bytes = store_.take_interactive_bytes();
        auto loader_bytes = store_.take_loader_bytes();
        const bool playback_busy =
            playback_bytes > 0 || store_.foreground_idle_for() < policy.foreground_quiet;
        const bool interactive_busy =
            interactive_bytes > 0 || store_.interactive_idle_for() < policy.foreground_quiet;
        // Law 3: the loader outranks background work, so loader activity (an
        // ingest) counts as busy too.
        const bool loader_busy =
            loader_bytes > 0 || store_.loader_idle_for() < policy.foreground_quiet;
        // Priority law: playback/seek > mounted MachaDFS/useful prefetch >
        // user-requested loader work > repair/rebalance/scrub. All three
        // suppress background work, while the transport queues themselves keep
        // playback above mount I/O.
        bool busy = playback_busy || interactive_busy || loader_busy;
        // A peer's viewers pace repair as this node's own do, since repair shares
        // their links: paced, never stopped, or a later viewer waits for the copy.
        const bool peer_viewers = node_.peer_viewers_active(
            std::max(node_.config().telemetry_interval * 3, std::chrono::milliseconds(3000)));
        const bool repair_busy = busy || peer_viewers;
        double fraction = busy ? policy.busy_bandwidth_fraction : policy.idle_bandwidth_fraction;

        double bandwidth = store_.estimated_network_bps();
        if (bandwidth <= 0.0)
            bandwidth = static_cast<double>(policy.initial_bandwidth);
        if (policy.max_bandwidth)
            bandwidth = std::min(bandwidth, static_cast<double>(policy.max_bandwidth));

        // Process CPU includes foreground filesystem/RPC work. Above the target,
        // background work loses budget continuously rather than stopping in a
        // fixed block-per-tick pattern.
        const double cpu_scale =
            background_cpu_scale(process_seconds, own_seconds, wall_seconds, policy.cpu_target);

        double rate = bandwidth * fraction * cpu_scale;
        // Repair's byte rate does not drop with busyness; repair_share decides
        // how much of the time it gets. busy_bandwidth_fraction governs
        // rebalance and scrub only.
        const double repair_rate = bandwidth * policy.idle_bandwidth_fraction * cpu_scale;
        double burst_cap = std::max<double>(node_.config().extent_size, bandwidth * 5.0);
        network_credit = std::min(burst_cap, network_credit + repair_rate * wall_seconds);
        local_credit = std::min(burst_cap, local_credit + rate * wall_seconds);
        const auto wall_now_ms = clock_->wall_ms();
        const bool scrub_due = policy.scrub_fraction > 0.0 && wall_now_ms >= scrub_due_unix_ms;
        if (scrub_due) {
            scrub_credit =
                std::min(burst_cap, scrub_credit + rate * policy.scrub_fraction * wall_seconds);
        } else {
            // Do not bank weeks of scrub credit and explode into a large burst when
            // the next campaign becomes due. Outside a campaign, scrub is truly idle.
            scrub_credit = 0.0;
        }

        bool gc_due_this_pass = false;
        bool repair_continue = false;
        try {
            // Remote generation notices do no kernel/FUSE or replica I/O themselves; this
            // owner advances the local metadata snapshot promptly, and settled
            // verification stays a bounded periodic task.
            bool metadata_ready_for_dependants = !metadata_dirty;
            if (metadata_dirty &&
                (metadata_retry_due == Clock::time_point{} || now >= metadata_retry_due)) {
                if (local_.replica().committed_generation() <= 1 &&
                    !established_metadata_peer &&
                    now < formation_settle_due) {
                    metadata_ready_for_dependants = false;
                    metadata_retry_due = formation_settle_due;
                } else {
                    const bool virgin_follower =
                        local_.replica().committed_generation() <= 1 &&
                        !established_metadata_peer &&
                        !last_active_nodes.empty() &&
                        node_.node_id() !=
                            *std::min_element(last_active_nodes.begin(), last_active_nodes.end());
                    if (virgin_follower) {
                        // Exactly one deterministic founder performs generation-2
                        // formation. Followers remain event-driven and wake on its
                        // metadata notice; the retry deadline only covers a founder
                        // disappearing without a final connectivity event.
                        metadata_ready_for_dependants = false;
                        metadata_retry_due = clock_->now() + metadata_retry_backoff;
                    } else {
                        const auto convergence_run = port_.metadata_convergence.begin();
                        if (!convergence_run) {
                            metadata_dirty = false;
                            metadata_ready_for_dependants = true;
                        } else {
                            enter_stage("metadata-repair");
                            const auto stage = Clock::now();
                            try {
                                if (maintenance_stage_hook_)
                                    maintenance_stage_hook_("metadata-repair-begin");
                                metadata_upkeep_.repair_step();
                                metadata_upkeep_.note_replica_validation(true);
                                metadata_dirty = port_.metadata_convergence.complete(*convergence_run);
                                metadata_ready_for_dependants = !metadata_dirty;
                                catalogue_dirty = true;
                                metadata_retry_due = Clock::time_point{};
                                metadata_retry_backoff = maintenance_background_interval(policy);
                            } catch (const std::exception& error) {
                                metadata_upkeep_.note_replica_validation(false, error.what());
                                metadata_ready_for_dependants = false;
                                metadata_retry_due = clock_->now() + metadata_retry_backoff;
                                metadata_retry_backoff =
                                    std::min(policy.no_progress_backoff,
                                             std::max(metadata_retry_backoff * 2,
                                                      maintenance_background_interval(policy)));
                                // Failing to reconcile every active head must not
                                // stall non-destructive DATA repair: a locally
                                // committed branch is still a valid reachability
                                // source. Destructive GC is fenced on `stable`.
                                const auto local = local_.replica().committed();
                                if (local_.replica().recovery_required() ||
                                    local.generation <= 1)
                                    throw;
                                Log::debug("metadata repair deferred; continuing non-destructive "
                                           "maintenance: " +
                                           std::string(error.what()));
                            } catch (...) {
                                metadata_upkeep_.note_replica_validation(false,
                                                                   "metadata validation failed");
                                metadata_ready_for_dependants = false;
                                metadata_retry_due = clock_->now() + metadata_retry_backoff;
                                metadata_retry_backoff =
                                    std::min(policy.no_progress_backoff,
                                             std::max(metadata_retry_backoff * 2,
                                                      maintenance_background_interval(policy)));
                                const auto local = local_.replica().committed();
                                if (local_.replica().recovery_required() ||
                                    local.generation <= 1)
                                    throw;
                                Log::debug("metadata repair deferred; continuing non-destructive "
                                           "maintenance");
                            }
                            log_slow_stage("metadata-repair", stage);
                        }
                    }
                }
            }

            // The catalogue installer follows the head; this pass offers the
            // installed root's control objects to the nodes present.
            if (metadata_ready_for_dependants &&
                (catalogue_dirty || catalogue_.convergence_needed()) &&
                (catalogue_retry_due == Clock::time_point{} || now >= catalogue_retry_due)) {
                enter_stage("catalogue-repair");
                const auto stage = Clock::now();
                try {
                    if (maintenance_stage_hook_)
                        maintenance_stage_hook_("catalogue-repair-begin");
                    catalogue_.repair_once();
                    catalogue_dirty = false;
                    catalogue_retry_due = Clock::time_point{};
                } catch (const std::exception& e) {
                    Log::debug("catalogue sync: " + std::string(e.what()));
                    catalogue_dirty = true;
                    catalogue_retry_due = clock_->now() + maintenance_background_interval(policy);
                }
                log_slow_stage("catalogue-repair", stage);
            }

            // Credit gates repair's transfers inside the step, not the step:
            // probing which objects need a copy costs no bulk bandwidth.
            const bool allow_network_repair = repair_share.can_start(now, repair_busy);
            const bool network_due = allow_network_repair && now >= network_quiescent_until;
            store_.note_repair_gate(
                network_due             ? DistributedStore::RepairGate::ran
                : !allow_network_repair ? DistributedStore::RepairGate::share
                                        : DistributedStore::RepairGate::quiescent,
                network_credit,
                static_cast<uint8_t>(
                    (playback_busy ? DistributedStore::paced_by_playback : 0) |
                    (interactive_busy ? DistributedStore::paced_by_mounted_filesystem : 0) |
                    (loader_busy ? DistributedStore::paced_by_loader : 0) |
                    (peer_viewers ? DistributedStore::paced_by_peer_playback : 0)));
            trace_gate("gate.repair", network_due,
                       flag("share", allow_network_repair) + " " +
                           flag("quiescent", now < network_quiescent_until));
            const bool garbage_due = !busy && !metadata_dirty;
            const bool gc_due = !busy && now >= gc_quiescent_until;
            gc_due_this_pass = gc_due;

            // Destructive maintenance is deliberately opportunistic: degraded
            // clusters retain garbage. Reclamation is enabled only after every
            // durably-known node has been directly reached by this process and
            // metadata repair has validated/converged that complete replica set.

            // What the reachable nodes hold between them: surveyed when
            // the namespace, holdings, membership or a peer's storage has
            // changed, and a no-op otherwise.
            enter_stage("availability");
            try {
                if (const auto head = metadata_.current())
                    (void)availability_.refresh(*head, clock_->now(), unix_ms());
            } catch (const std::exception& error) {
                Log::debug("availability survey deferred: " + std::string(error.what()));
            }

            // Reachability GC, repair and explicit tombstone accounting share
            // one immutable namespace inventory. Rebuild it only when one of
            // those consumers can make progress or metadata has advanced.
            if (network_due || garbage_due || gc_due) {
                enter_stage("inventory");
                const auto inventory_stage = Clock::now();
                // The horizons this pass reads: the ledger's, as published.
                auto inventory = ledger_.inventory();
                auto release = ledger_.release();
                auto objects = builder_.namespace_objects();
                bool rebuilt_inventory = false;
                // Rebuilding runs the catalogue's repair (maintenance_objects), held back
                // until metadata is ready, so the catalogue never refreshes ahead of the
                // metadata convergence it depends on; a repair-only step uses the last
                // inventory until then.
                const bool repair_only = network_due && !garbage_due && !gc_due;
                if ((!inventory || !inventory->catalogue_complete() ||
                     inventory->generation() != objects->metadata_generation) &&
                    (!inventory || !repair_only || metadata_ready_for_dependants)) {
                    // The catalogue's repair is a step of its own (spec A4), between
                    // the head its inventory is read against and the read.
                    const auto catalogue_head = catalogue_.maintenance_head();
                    const bool catalogue_repaired = catalogue_.maintenance_repair();
                    // The pass reads what the ledger holds, never its own copy.
                    ledger_.publish(builder_.inventory(*objects, catalogue_head, catalogue_repaired));
                    inventory = ledger_.inventory();
                    rebuilt_inventory = true;
                    observations().record("maintenance.inventory.build_us",
                                          elapsed_us(inventory_stage));
                    trace_action(
                        "inventory",
                        "generation=" + std::to_string(objects->metadata_generation) +
                            " live=" + std::to_string(inventory->size(RetentionClass::data)) +
                            " control_live=" +
                            std::to_string(inventory->size(RetentionClass::control)) +
                            " garbage=" + std::to_string(inventory->garbage().size()) +
                            " stale_garbage=" +
                            std::to_string(inventory->stale_garbage().size()) + " " +
                            flag("catalogue_complete", inventory->catalogue_complete()));
                    observations().add("maintenance.inventory.live_objects",
                                       inventory->size(RetentionClass::data));
                    // A newly derived reachability set is never consumed destructively in the
                    // pass that built it. Schedule one follow-up after the foreground quiet
                    // boundary, or a quiescent service would never wake to consume it.
                    gc_quiescent_until = clock_->now() + policy.foreground_quiet;
                }
                if (rebuilt_inventory && Log::enabled(LogLevel::all)) {
                    Log::trace("DIAG maintenance-inventory generation=" +
                               std::to_string(objects->metadata_generation) +
                               " entries=" + std::to_string(objects->entries) +
                               " extents=" + std::to_string(objects->extents) +
                               " live=" + std::to_string(inventory->size(RetentionClass::data)) +
                               " garbage=" + std::to_string(inventory->garbage().size()) +
                               " stale_garbage=" +
                               std::to_string(inventory->stale_garbage().size()) +
                               " elapsed_ms=" + std::to_string(elapsed_ms(inventory_stage)));
                }

                if (network_due) {
                    const auto extent = std::max<uint64_t>(1, node_.config().extent_size);

                    size_t retained_repairs = 0;
                    bool retained_waiting_for_credit = false;
                    // The walk stopped at its per-step bound with claims left;
                    // repair is not quiescent until a walk reaches the end.
                    bool retained_walk_unfinished = false;
                    // Presence is an index lookup and costs no credit; only a missing claimed
                    // object, which may be fetched, needs an extent of credit.
                    struct CreditedRestorer final : ClaimRestorer {
                        DistributedStore& store;
                        double& credit;
                        uint64_t extent;
                        const decltype(trace_action)& trace;
                        CreditedRestorer(DistributedStore& s, double& c, uint64_t e,
                                         const decltype(trace_action)& t)
                            : store(s), credit(c), extent(e), trace(t) {}
                        Outcome restore(RetentionClass type, const ObjectId& id) override {
                            const std::string walked =
                                std::string(type == RetentionClass::data ? "data " : "control ") +
                                to_string(id);
                            if (credit < static_cast<double>(extent)) {
                                trace("claim-walk", walked + " waiting-for-credit");
                                return Outcome::waiting_for_credit;
                            }
                            const bool restored = type == RetentionClass::data
                                                      ? store.ensure_local(id, false)
                                                      : store.ensure_control_local(id);
                            trace("claim-walk", walked + (restored ? " restored" : " not-restored"));
                            if (!restored)
                                return Outcome::not_restored;
                            credit = std::max(0.0, credit - static_cast<double>(extent));
                            return Outcome::restored;
                        }
                    } restorer{store_, network_credit, extent, trace_action};
                    const auto higher_class_active = [this, &policy, peer_viewers] {
                        const auto quiet = policy.foreground_quiet;
                        return peer_viewers || store_.foreground_idle_for() < quiet ||
                               store_.interactive_idle_for() < quiet ||
                               store_.loader_idle_for() < quiet;
                    };
                    repair_share.started(clock_->now(), repair_busy);
                    // A throw from either repair stage must still close the
                    // turn, or the pacer counts it active forever and never
                    // computes another cooldown.
                    struct RepairTurn {
                        WeightedLoaderService& share;
                        const decltype(higher_class_active)& active;
                        const MaintenanceClock& clock;
                        ~RepairTurn() { share.finished(clock.now(), active()); }
                    } repair_turn{repair_share, higher_class_active, *clock_};
                    enter_stage("retention-repair");
                    for (auto* walk : {&data_claim_walk, &control_claim_walk}) {
                        const auto step = walk->step(ledger_, restorer);
                        retained_repairs += step.restored;
                        retained_waiting_for_credit |= step.waiting_for_credit;
                        retained_walk_unfinished |= step.unfinished;
                    }

                    const auto byte_budget = static_cast<uint64_t>(network_credit);
                    // The byte budget does not constrain have-object probes: a
                    // settled or mostly-settled namespace could issue thousands
                    // of synchronous control RPCs while consuming no network
                    // credit. Bound each repair slice independently.
                    constexpr size_t operation_budget = 16;
                    const auto missing = availability_.missing_here();
                    if (missing && inventory &&
                        (missing != pull_missing || inventory != pull_inventory)) {
                        const auto& outside = inventory->outside_namespace();
                        pull_list.clear();
                        pull_list.reserve(missing->size() + outside.size());
                        std::set_union(missing->begin(), missing->end(), outside.begin(),
                                       outside.end(), std::back_inserter(pull_list));
                        pull_missing = missing;
                        pull_inventory = inventory;
                    }
                    const bool pulling_listed = missing && inventory;
                    // What a peer holds, from its own account of its holdings:
                    // known only for the namespace's objects, and only for a
                    // peer that could say.
                    const auto peer_lacks = availability_.peer_lacks();
                    const auto known_present =
                        [&](const NodeId& peer, const ObjectId& id) -> std::optional<bool> {
                        if (!peer_lacks || !inventory ||
                            peer_lacks->generation != inventory->generation())
                            return std::nullopt;
                        const auto& outside = inventory->outside_namespace();
                        if (std::binary_search(outside.begin(), outside.end(), id))
                            return std::nullopt;
                        const auto found = peer_lacks->lacks.find(peer);
                        if (found == peer_lacks->lacks.end())
                            return std::nullopt;
                        return !std::binary_search(found->second->begin(), found->second->end(), id);
                    };
                    enter_stage("network-repair");
                    const auto repair_stage = Clock::now();
                    auto repair = store_.repair_step(
                        byte_budget, operation_budget,
                        inventory ? std::optional(inventory->referenced_ids(RetentionClass::data))
                                   : std::nullopt,
                        [&] {
                            // Repair's turn ends at the next operation boundary
                            // once its weighted slice is spent; it is paced,
                            // never stopped.
                            return repair_share.should_yield(clock_->now(), higher_class_active());
                        },
                        inventory ? inventory->generation() : 0,
                        // No work that cannot complete: the survey asks again
                        // when membership or storage could change the answer.
                        [this](const ObjectId& id) { return availability_.unavailable(id); },
                        pulling_listed ? std::optional<std::span<const ObjectId>>(pull_list)
                                       : std::nullopt,
                        known_present);
                    {
                        // Split by whether a higher class was active as the
                        // step ended: repair idle against repair on its share.
                        const std::string load = higher_class_active() ? "loaded" : "idle";
                        observations().record("maintenance.repair.step_us." + load,
                                              elapsed_us(repair_stage));
                        observations().add("maintenance.repair.bytes." + load,
                                           repair.bytes_transferred);
                        observations().add("maintenance.repair.push_examined." + load,
                                           repair.push_examined);
                        observations().add("maintenance.repair.pull_examined." + load,
                                           repair.pull_examined);
                        observations().add("maintenance.repair.retained_repairs",
                                           retained_repairs);
                    }
                    log_slow_stage("network-repair", repair_stage,
                                   "bytes=" + std::to_string(repair.bytes_transferred) +
                                       " retained_repairs=" + std::to_string(retained_repairs) +
                                       " push_examined=" + std::to_string(repair.push_examined) +
                                       " pull_examined=" + std::to_string(repair.pull_examined) +
                                       " remote_ops=" + std::to_string(repair.remote_operations) +
                                       " complete=" + std::to_string(repair.complete ? 1 : 0) +
                                       " yielded=" + std::to_string(repair.yielded ? 1 : 0));
                    if (repair.bytes_transferred) {
                        network_credit = std::max(
                            0.0, network_credit - static_cast<double>(repair.bytes_transferred));
                    }
                    if (repair.credit_limited) {
                        // The credit deadline below wakes the loop when the
                        // next extent is affordable.
                        store_.note_repair_gate(DistributedStore::RepairGate::credit,
                                                 network_credit);
                    } else if (repair.yielded) {
                        Log::trace("maintenance: repair yielded to foreground I/O");
                    } else if (!repair.complete || retained_repairs || repair.bytes_transferred ||
                               retained_walk_unfinished) {
                        // Work left (a probe or scan budget ran out) or work
                        // done: the next step is due after one interval, not
                        // when credit or an unrelated event next wakes the loop.
                        repair_continue = true;
                    } else if (!retained_waiting_for_credit) {
                        network_credit = 0.0;
                        network_quiescent_until = Clock::time_point::max();
                        Log::trace("maintenance: repair quiescent; waiting for an event");
                    }
                    // Otherwise the claim walk still waits for credit, and the
                    // credit deadline wakes the loop for it.
                }

                const bool cluster_gc_stable =
                    node_.membership().all_known_reachable() && metadata_.status().stable;
                if (cluster_gc_stable && !cluster_stable_observed_) {
                    // Recovery after a restart: every known node reached and
                    // metadata stable, for the first time in this process.
                    cluster_stable_observed_ = true;
                    observations().event({unix_ms(),
                                          "cluster_stable",
                                          {{"elapsed_ms", elapsed_us(constructed_) / 1000}},
                                          {}});
                }

                // What the gates read. The known generation is read again at
                // each gate, as the pass always has.
                PassFacts facts;
                facts.garbage_due = garbage_due;
                facts.gc_due = gc_due;
                facts.busy = busy;
                facts.gc_waiting_for_event = gc_quiescent_until == Clock::time_point::max();
                facts.rebuilt_inventory = rebuilt_inventory;
                const auto& garbage = inventory ? inventory->garbage() : no_garbage;

                const auto tombstones = tombstone_gate(facts, inventory.get());
                trace_gate("gate.tombstones", tombstones.permitted, tombstones.conditions);
                if (tombstones.permitted) {
                    auto matured = collect_garbage(garbage);
                    std::vector<GarbageRef> legacy;
                    for (const auto& candidate : garbage) {
                        if (candidate.retired_at_ns == 0)
                            legacy.push_back(candidate);
                    }

                    auto erase = inventory ? inventory->stale_garbage() : no_garbage;
                    erase.insert(erase.end(), matured.begin(), matured.end());
                    observations().add("maintenance.tombstones.collected", matured.size());
                    if (!erase.empty() || !legacy.empty()) {
                        const auto ids = [](const std::vector<GarbageRef>& refs) {
                            std::vector<std::string> hex;
                            for (const auto& ref : refs)
                                hex.push_back(to_string(ref.id));
                            std::sort(hex.begin(), hex.end());
                            std::string joined;
                            for (const auto& id : hex)
                                joined += (joined.empty() ? "" : ",") + id;
                            return joined;
                        };
                        trace_action("tombstones", "erase=[" + ids(erase) + "] stamp=[" +
                                                       ids(legacy) + "]");
                        maintain_garbage_metadata(erase, legacy);
                    }
                }

                // The control live set (catalogue objects and namespace tree nodes)
                // comes from the same inventory as DATA reachability. A foreground
                // mutation may advance metadata after it was built; never sweep the
                // control store with such a stale set, or a short grace could delete a
                // newly published manifest, shard or tree node.
                const auto current_metadata_view = metadata_.current();
                const auto release_metadata_view = metadata_.release_head();

                // Retention release is local and causal. A sole accepted head
                // provides a complete live-object set plus the mutation clock of
                // claim dots it has actually observed. Claims from unseen concurrent
                // branches are not dominated by that clock and therefore survive.
                if (auto floor = metadata_.release_head();
                    floor && floor->hash != (release ? release->head() : Hash256{})) {
                    ObservedDuration horizon_build(
                        observations().histogram("maintenance.release_horizon.build_us"));
                    auto built = builder_.release(*floor);
                    trace_action("release-horizon",
                                 std::string(built.complete ? "complete" : "incomplete") +
                                     " data_live=" +
                                     std::to_string(built.horizon->size(RetentionClass::data)) +
                                     " control_live=" +
                                     std::to_string(built.horizon->size(RetentionClass::control)));
                    // Refused when incomplete: the ledger keeps the previous.
                    ledger_.publish(std::move(built));
                    release = ledger_.release();
                }

                facts.release_view = release_metadata_view.has_value();
                facts.known_generation = metadata_server_.known_generation();
                const auto control = control_gate(facts, inventory.get());
                // The rule the DATA and tombstone gates keep: a newly built
                // inventory is never used destructively in the pass that
                // built it. Traced so a fixture can hold the control gate to it.
                if (rebuilt_inventory)
                    trace_action("control-gate-in-rebuilding-pass",
                                 control.permitted ? "open" : "shut");
                trace_gate("gate.control", control.permitted, control.conditions);
                if (control.permitted) {
                    // Release follows this node's sole head and its causal clock.
                    // What another node's branch may still refer to is kept by
                    // the grace the sweep applies below, on this node's clock.
                    if (release) {
                        const auto released =
                            ledger_.release_unreferenced(RetentionClass::control, *release, 64);
                        observations().add("retention.released.control", released);
                        if (released)
                            trace_action("release.control", std::to_string(released));
                    }
                    enter_stage("control-gc");
                    const auto now_unix_ms = static_cast<uint64_t>(clock_->wall_ns() / 1'000'000);
                    const auto removed = catalogue_.control_gc_step(
                        inventory->referenced_ids(RetentionClass::control), policy.garbage_grace,
                        32, &control_unreferenced_, now_unix_ms);
                    save_sightings(control_unreferenced_, control_sightings_saved_);
                    observations().add("catalogue.control_gc.removed", removed);
                    if (removed)
                        trace_action("control-gc", std::to_string(removed));
                    const auto pruned = ledger_.prune_unclaimed(RetentionClass::control, 64);
                    observations().add("retention.pruned.control", pruned);
                    if (pruned)
                        trace_action("prune.control", std::to_string(pruned));
                    if (removed)
                        Log::debug("catalogue control GC removed=" + std::to_string(removed));
                }

                // Physical mark/sweep also catches objects that never got a tombstone (a DATA
                // put followed by process death before commit acceptance). Recent and legacy
                // (unstamped) tombstones stay protected for the grace interval. Retention
                // claims are checked before every physical delete, so an inventory older
                // than the metadata generation is still safe for orphan cleanup.
                facts.known_generation = metadata_server_.known_generation();
                const auto data = data_gate(facts, inventory.get());
                const std::string& gc_skip_reason = data.reason;
                trace_gate("gate.data", data.permitted, data.conditions);
                if (gc_skip_reason != last_gc_skip_reason) {
                    last_gc_skip_reason = gc_skip_reason;
                    if (Log::enabled(LogLevel::debug))
                        Log::debug("garbage collection sweep node=" +
                                   to_string(node_.node_id()).substr(0, 6) +
                                   (gc_skip_reason.empty() ? std::string(" enabled")
                                                           : " skipped: " + gc_skip_reason));
                }
                if (data.permitted) {
                    if (release) {
                        const auto released =
                            ledger_.release_unreferenced(RetentionClass::data, *release, 64);
                        observations().add("retention.released.data", released);
                        if (released)
                            trace_action("release.data", std::to_string(released));
                        if (released && Log::enabled(LogLevel::debug))
                            Log::debug("retention released DATA claims node=" +
                                       to_string(node_.node_id()).substr(0, 6) + " count=" +
                                       std::to_string(released));
                    } else if (!release_horizon_incomplete_logged) {
                        release_horizon_incomplete_logged = true;
                        Log::debug("retention release skipped: horizon incomplete");
                    }
                    std::vector<ObjectId> protected_ids;
                    protected_ids.reserve(garbage.size());
                    const auto now_ns = clock_->wall_ns();
                    const auto grace_ns =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(policy.garbage_grace)
                            .count();
                    for (const auto& candidate : garbage) {
                        const bool matured = candidate.retired_at_ns > 0 &&
                                             now_ns >= candidate.retired_at_ns &&
                                             now_ns - candidate.retired_at_ns >= grace_ns;
                        if (!matured)
                            protected_ids.push_back(candidate.id);
                    }
                    std::sort(protected_ids.begin(), protected_ids.end());
                    protected_ids.erase(std::unique(protected_ids.begin(), protected_ids.end()),
                                        protected_ids.end());

                    enter_stage("garbage-collect");
                    const auto gc_stage = Clock::now();
                    // Tombstones carry their own retirement deadline. Untombstoned bytes may be
                    // the data half of an in-flight publication with no causal marker yet, so the
                    // orphan sweep waits one retry window; a zero tombstone grace must not
                    // collapse that window.
                    const auto orphan_grace =
                        std::max(policy.garbage_grace, policy.no_progress_backoff);
                    auto gc = local_.data().gc_step(
                        inventory->referenced_ids(RetentionClass::data), protected_ids,
                        orphan_grace, 64,
                        [this] {
                            const auto quiet = node_.config().maintenance.foreground_quiet;
                            return store_.foreground_idle_for() < quiet ||
                                   store_.interactive_idle_for() < quiet ||
                                   store_.loader_idle_for() < quiet;
                        },
                        [this](const ObjectId& id) {
                            return ledger_.retained(RetentionClass::data, id);
                        },
                        &data_unreferenced_, static_cast<uint64_t>(now_ns / 1'000'000),
                        policy.garbage_grace);
                    save_sightings(data_unreferenced_, data_sightings_saved_, gc.complete);
                    if (gc.bytes)
                        trace_action("gc", "reclaimed_bytes=" + std::to_string(gc.bytes));
                    {
                        // gc.objects counts objects examined, reclaimed or not.
                        const auto step_us = elapsed_us(gc_stage);
                        observations().record("maintenance.gc.step_us", step_us);
                        observations().add("maintenance.gc.examined", gc.objects);
                        observations().add("maintenance.gc.reclaimed_bytes", gc.bytes);
                        if (gc.objects)
                            observations().record("maintenance.gc.per_object_ns",
                                                  step_us * 1000 / gc.objects);
                    }
                    log_slow_stage("garbage-collect", gc_stage,
                                   "reclaimed_bytes=" + std::to_string(gc.bytes) +
                                       " objects=" + std::to_string(gc.objects));
                    if ((gc.deferred || gc.objects || gc.yielded) && Log::enabled(LogLevel::debug))
                        Log::debug("garbage collection sweep node=" +
                                   to_string(node_.node_id()).substr(0, 6) + " objects=" +
                                   std::to_string(gc.objects) + " bytes=" +
                                   std::to_string(gc.bytes) + " deferred=" +
                                   std::to_string(gc.deferred ? 1 : 0) + " yielded=" +
                                   std::to_string(gc.yielded ? 1 : 0) + " complete=" +
                                   std::to_string(gc.complete ? 1 : 0) + " live=" +
                                   std::to_string(inventory->size(RetentionClass::data)) +
                                   " protected=" +
                                   std::to_string(protected_ids.size()));
                    const auto pruned = ledger_.prune_unclaimed(RetentionClass::data, 64);
                    observations().add("retention.pruned.data", pruned);
                    if (pruned)
                        trace_action("prune.data", std::to_string(pruned));
                    if (gc.bytes && Log::enabled(LogLevel::debug))
                        Log::debug("garbage collection reclaimed " + std::to_string(gc.bytes) +
                                   " local bytes");
                    if (gc.yielded) {
                        Log::trace("maintenance: garbage collection yielded to foreground I/O");
                    } else if (gc.complete) {
                        if (gc.deferred) {
                            gc_quiescent_until = clock_->now() + orphan_grace;
                            Log::trace(
                                "maintenance: recent orphan deferred to exact grace deadline");
                        } else {
                            gc_quiescent_until = Clock::time_point::max();
                            Log::trace(
                                "maintenance: garbage collection complete; waiting for an event");
                        }
                    }
                }
            }

            // Local maintenance advances persistent filesystem cursors: a slice examines
            // at most 64 objects, yields to foreground work, and never rebuilds a full
            // object list to find placement already correct.
            if (!busy && now >= local_quiescent_until &&
                local_credit >= node_.config().extent_size) {
                enter_stage("local-rebalance");
                const auto rebalance_stage = Clock::now();
                auto rebalance = local_.data().rebalance_step(
                    static_cast<uint64_t>(local_credit), 64, [this] {
                        const auto quiet = node_.config().maintenance.foreground_quiet;
                        return store_.foreground_idle_for() < quiet ||
                               store_.interactive_idle_for() < quiet ||
                               store_.loader_idle_for() < quiet;
                    });
                log_slow_stage("local-rebalance", rebalance_stage,
                               "bytes=" + std::to_string(rebalance.bytes) +
                                   " objects=" + std::to_string(rebalance.objects));
                if (rebalance.bytes)
                    local_credit =
                        std::max(0.0, local_credit - static_cast<double>(rebalance.bytes));
                if (rebalance.yielded) {
                    Log::trace("maintenance: local rebalance yielded to foreground I/O");
                } else if (rebalance.complete && !rebalance.bytes) {
                    local_credit = 0.0;
                    local_quiescent_until = Clock::time_point::max();
                    Log::trace("maintenance: local rebalance quiescent; waiting for an event");
                }
            }

            // Retention publication appends are batched but safety-critical. Compact them
            // only here, so foreground metadata never pays the checkpoint rewrite.
            // Snapshot-before-truncate makes interruption idempotent.
            if (!busy) {
                enter_stage("retention-compact");
                (void)ledger_.compact_if_needed(4096);
            }

            // An accepted head this replica cannot replay is excluded from reads
            // (MetadataReplica cooldown) and re-anchored here from any peer that can
            // materialize it. A no-op unless a head is flagged.
            try {
                enter_stage("head-repair");
                (void)metadata_upkeep_.repair_unreconstructable_heads();
            } catch (const std::exception& error) {
                Log::debug("maintenance: metadata head repair failed: " +
                           std::string(error.what()));
            }

            if (!busy) {
                try {
                    enter_stage("history-truncate");
                    metadata_upkeep_.truncate_history();
                } catch (const std::exception& error) {
                    Log::debug("maintenance: history truncation failed: " +
                               std::string(error.what()));
                }
            }

            // Packed DATA tombstones are dead space. Compact one victim pack per backend
            // at a time, so the temporary-space envelope is fixed even on multi-terabyte
            // backends.
            if (!busy) {
                enter_stage("compact-packs");
                (void)local_.data().compact_packs(stop);
            }

            if (!busy && scrub_due && scrub_credit >= node_.config().extent_size) {
                enter_stage("scrub");
                const auto scrub_stage = Clock::now();
                auto scrub =
                    local_.data().scrub_step(static_cast<uint64_t>(scrub_credit), 64, [this] {
                        const auto quiet = node_.config().maintenance.foreground_quiet;
                        return store_.foreground_idle_for() < quiet ||
                               store_.interactive_idle_for() < quiet ||
                               store_.loader_idle_for() < quiet;
                    });
                log_slow_stage("scrub", scrub_stage,
                               "bytes=" + std::to_string(scrub.bytes) +
                                   " objects=" + std::to_string(scrub.objects));
                if (scrub.bytes)
                    scrub_credit = std::max(0.0, scrub_credit - static_cast<double>(scrub.bytes));
                if (scrub.yielded) {
                    Log::trace("maintenance: scrub yielded to foreground I/O");
                } else if (scrub.complete) {
                    // A proactive scrub is a low-frequency campaign: once a full pass finishes,
                    // stay idle until the next scheduled one rather than the no-progress backoff.
                    scrub_credit = 0.0;
                    const auto completed = clock_->wall_ms();
                    const auto interval_ms = static_cast<uint64_t>(policy.scrub_interval.count());
                    scrub_due_unix_ms =
                        completed > std::numeric_limits<uint64_t>::max() - interval_ms
                            ? std::numeric_limits<uint64_t>::max()
                            : completed + interval_ms;
                    try {
                        persist_scrub_due(scrub_due_path(node_.config().state_path),
                                          scrub_due_unix_ms);
                    } catch (const std::exception& error) {
                        Log::debug("maintenance: scrub schedule persistence unavailable: " +
                                   std::string(error.what()));
                    }
                    Log::trace("maintenance: scrub pass complete; next campaign scheduled");
                }
            }

        } catch (const MetadataNotReady&) {
            // Normal startup/recovery state. MetadataManager has already
            // published any availability transition; do not duplicate it on
            // every maintenance pass.
        } catch (const std::exception& e) {
            const std::string_view message(e.what());
            if (message !=
                    "metadata durable replica set unavailable; reconciliation may be required" &&
                message != "metadata write durability floor unavailable")
                Log::debug("maintenance: " + std::string(message));
        }

        if (gc_due_this_pass && !busy) {
            // An immature tombstone is future work. Once a GC pass has evaluated the
            // final accepted inventory, arm one exact wake for its earliest retirement
            // deadline; otherwise the protected object makes the sweep look quiescent and
            // it could sleep indefinitely.
            const auto grace_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(policy.garbage_grace).count();
            const auto wall_now_ns = clock_->wall_ns();
            std::optional<std::chrono::nanoseconds> earliest_remaining;
            const auto inventory = ledger_.inventory();
            for (const auto& candidate : inventory ? inventory->garbage() : no_garbage) {
                if (candidate.retired_at_ns <= 0 || grace_ns <= 0)
                    continue;
                int64_t remaining_ns{};
                if (wall_now_ns < candidate.retired_at_ns) {
                    const auto until_retirement = candidate.retired_at_ns - wall_now_ns;
                    remaining_ns = until_retirement > std::numeric_limits<int64_t>::max() - grace_ns
                                       ? std::numeric_limits<int64_t>::max()
                                       : until_retirement + grace_ns;
                } else {
                    const auto elapsed = wall_now_ns - candidate.retired_at_ns;
                    if (elapsed >= grace_ns)
                        continue;
                    remaining_ns = grace_ns - elapsed;
                }
                const auto remaining = std::chrono::nanoseconds(remaining_ns);
                if (!earliest_remaining || remaining < *earliest_remaining)
                    earliest_remaining = remaining;
            }
            if (earliest_remaining)
                gc_quiescent_until = clock_->now() + *earliest_remaining;
        }

        cpu_reporter.tick();
        auto deadline = Clock::time_point::max();
        const auto now_after_work = clock_->now();
        // `complete()` can turn an in-flight burst into one pending follow-up without
        // another external event, so a dirty owner with no retry delay is runnable
        // now; sleeping would lose the edge and strand metadata and its catalogue
        // dependent. Likewise when catalogue repair leaves one more pass to perform.
        if (metadata_dirty)
            deadline =
                std::min(deadline, metadata_retry_due == Clock::time_point{} ? now_after_work
                                                                             : metadata_retry_due);
        if (catalogue_dirty && !metadata_dirty)
            deadline = std::min(deadline, catalogue_retry_due == Clock::time_point{}
                                              ? now_after_work
                                              : catalogue_retry_due);

        if (gc_quiescent_until != Clock::time_point{} &&
            gc_quiescent_until != Clock::time_point::max()) {
            if (gc_quiescent_until > now_after_work) {
                deadline = std::min(deadline, gc_quiescent_until);
            } else if (!gc_due_this_pass && !busy) {
                // Work earlier in this pass may run past an exact GC deadline: re-evaluate
                // at once rather than sleep indefinitely. Once GC has been evaluated, gc_due
                // holds and event-driven quiescence applies.
                deadline = std::min(deadline, now_after_work);
            }
        }
        const auto scrub_now_ms = clock_->wall_ms();
        if (scrub_due_unix_ms <= scrub_now_ms) {
            if (scrub_credit < node_.config().extent_size && rate > 0.0 &&
                policy.scrub_fraction > 0.0) {
                const auto seconds =
                    (static_cast<double>(node_.config().extent_size) - scrub_credit) /
                    (rate * policy.scrub_fraction);
                if (std::isfinite(seconds) && seconds > 0.0)
                    deadline = std::min(deadline, now_after_work +
                                                      std::chrono::duration_cast<Clock::duration>(
                                                          std::chrono::duration<double>(seconds)));
            }
        } else {
            deadline = std::min(deadline, now_after_work + std::chrono::milliseconds(
                                                               scrub_due_unix_ms - scrub_now_ms));
        }

        if (busy) {
            // A busy pass suppressed GC, repair and rebalance; it re-evaluates as soon as
            // the foreground quiet period is met, or at once if it already has, rather
            // than sleeping until an unrelated event.
            auto idle = std::min({store_.foreground_idle_for(), store_.interactive_idle_for(),
                                  store_.loader_idle_for()});
            deadline = std::min(deadline, now_after_work + (idle < policy.foreground_quiet
                                                                ? policy.foreground_quiet - idle
                                                                : Clock::duration{}));
        }

        auto credit_deadline = [&](double credit, Clock::time_point quiescent) {
            if (quiescent == Clock::time_point::max() || rate <= 0.0 ||
                credit >= node_.config().extent_size)
                return;
            const auto seconds = (static_cast<double>(node_.config().extent_size) - credit) / rate;
            if (std::isfinite(seconds) && seconds > 0.0)
                deadline = std::min(deadline,
                                    now_after_work + std::chrono::duration_cast<Clock::duration>(
                                                         std::chrono::duration<double>(seconds)));
        };
        {
            // Credit alone is not enough while repair is cooling down behind a
            // busy class: wake when its next turn is due, not on the next
            // unrelated event.
            if (network_quiescent_until != Clock::time_point::max() && repair_rate > 0.0 &&
                network_credit < node_.config().extent_size) {
                const auto seconds =
                    (static_cast<double>(node_.config().extent_size) - network_credit) / repair_rate;
                if (std::isfinite(seconds) && seconds > 0.0)
                    deadline = std::min(deadline,
                                        now_after_work + std::chrono::duration_cast<Clock::duration>(
                                                             std::chrono::duration<double>(seconds)));
            }
            if (network_quiescent_until != Clock::time_point::max()) {
                const auto turn = repair_share.wait_for(now_after_work, repair_busy);
                if (turn > Clock::duration{})
                    deadline = std::min(deadline, now_after_work + turn);
            }
            if (repair_continue)
                deadline = std::min(deadline, now_after_work + policy.interval);
        }
        credit_deadline(local_credit, local_quiescent_until);
        // A deferred roll-up, or a peer to ask again.
        if (const auto due = availability_.due())
            deadline = std::min(deadline, std::max(*due, now_after_work));

        // A maintenance action can itself commit metadata (retiring a matured garbage
        // marker, say); that event must not be lost for arriving before the wait.
        if (events_.total() != observed_event)
            continue;

        enter_stage("wait");
        {
            const auto relative = [&](Clock::time_point at) -> int64_t {
                if (at == Clock::time_point::max())
                    return -1;
                if (at == Clock::time_point{})
                    return -3;
                return std::chrono::duration_cast<std::chrono::milliseconds>(at - now_after_work)
                    .count();
            };
            port_.last_wait_ms.store(relative(deadline), std::memory_order_release);
            port_.last_gc_quiet_ms.store(relative(gc_quiescent_until),
                                                std::memory_order_release);
            port_.last_flags.store(static_cast<uint8_t>((busy ? 1 : 0) |
                                                               (gc_due_this_pass ? 2 : 0)),
                                          std::memory_order_release);
        }
        wait_for_events(stop, deadline);
    }
}

bool Maintenance::absorb_events() {
    const auto storage = events_.count(NodeEvent::storage);
    const auto metadata = events_.count(NodeEvent::metadata);
    const auto topology = events_.count(NodeEvent::topology);
    const auto claims = events_.count(NodeEvent::claims);
    bool wake = storage != absorbed_storage_ || claims != absorbed_claims_;
    if (metadata != absorbed_metadata_)
        media_information_.request_prune();
    if (metadata != absorbed_metadata_ || topology != absorbed_topology_)
        wake = port_.metadata_convergence.request(metadata_server_.known_generation()) || wake;
    absorbed_storage_ = storage;
    absorbed_metadata_ = metadata;
    absorbed_topology_ = topology;
    absorbed_claims_ = claims;
    absorbed_total_ = storage + metadata + topology + claims;
    return wake;
}

void Maintenance::wait_for_events(std::stop_token stop, Clock::time_point deadline) {
    // The predicate only compares counts: absorbing reads the metadata
    // replica, which must not happen under the lock notify() takes.
    while (!stop.stop_requested() && clock_->now() < deadline) {
        {
            Lock wait_lock(events_.wait_mutex);
            clock_->wait_until(events_.wait_cv, wait_lock.native(), stop, deadline,
                               [this] { return events_.total() != absorbed_total_; });
        }
        if (events_.total() != absorbed_total_ && absorb_events())
            return;
    }
}
} // namespace macha
