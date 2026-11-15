// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/maintenance.hpp"
#include "contract/stopping.hpp"
#include "dagda/node.h"
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
      metadata_server_(dependencies.metadata_server), store_node_(dependencies.store_node),
      context_(dependencies.store_node.context()), metadata_(dependencies.metadata),
      metadata_upkeep_(dependencies.metadata_upkeep), catalogue_(dependencies.catalogue),
      media_information_(dependencies.media_information), events_(dependencies.events),
      port_(dependencies.port), clock_(std::move(dependencies.clock)),
      maintenance_trace_(std::move(dependencies.trace)),
      maintenance_stage_hook_(std::move(dependencies.stage_hook)),
      constructed_(dependencies.constructed) {}

Maintenance::~Maintenance() {
    stop();
}

void Maintenance::start() {
    thread_ = std::jthread([this](const std::stop_token& maintenance_stop) {
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

double background_cpu_scale(double process_cpu_seconds, double own_cpu_seconds, double wall_seconds,
                            double cpu_target) {
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
    uint64_t last_metadata_remote_epoch = node_.remote_metadata_epoch();
    std::vector<NodeId> last_active_nodes;
    bool metadata_dirty = false;
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
    // Macha's events and the store node's: either moving is work to look at.
    const auto events_now = [this] {
        return events_.total() + store_node_.maintenance_events().total();
    };
    uint64_t observed_event = events_now();
    auto gc_quiescent_until = Clock::time_point{};
    // The store's repair: credit, its share against the classes above it, the
    // survey and the transfers. Its own clock is the store node's, real time.
    auto& repair = store_node_.repair();
    bool repair_quiescent = false;
    const auto pause = [&stop] {
        if (stop.stop_requested())
            throw MaintenanceStopping{};
    };

    while (!stop.stop_requested()) {
        port_.wakeups.fetch_add(1, std::memory_order_relaxed);
        const auto enter_stage = [this](const char* name) {
            port_.stage.store(name, std::memory_order_release);
        };
        enter_stage("pass-begin");
        auto now = clock_->now();
        absorb_events();
        metadata_dirty = port_.metadata_convergence.pending();
        const auto current_event = events_now();
        const bool event_changed = current_event != observed_event;
        observed_event = current_event;
        const auto local_metadata_generation = local_.replica().committed_generation();
        if (local_metadata_generation != last_local_metadata_generation) {
            catalogue_dirty = true;
            if (metadata_dirty)
                note_retry_trigger(now);
            last_local_metadata_generation = local_metadata_generation;
            gc_quiescent_until = now + policy.foreground_quiet;
        }
        if (event_changed) {
            repair_quiescent = false;
            // Object arrival and namespace publication are separate durable
            // operations. Start an exact quiet window so a sweep cannot race
            // the retention claim which makes newly-arrived bytes reachable.
            gc_quiescent_until = now + policy.foreground_quiet;
        }
        std::vector<NodeId> active_nodes;
        bool established_metadata_peer = false;
        for (const auto& peer : node_.membership().active()) {
            active_nodes.push_back(peer.id);
            established_metadata_peer = established_metadata_peer || (peer.id != node_.node_id() &&
                                                                      peer.metadata_generation > 1);
        }
        std::sort(active_nodes.begin(), active_nodes.end());
        const auto remote_epoch = node_.remote_metadata_epoch();
        const bool topology_changed = active_nodes != last_active_nodes;
        if (remote_epoch != last_metadata_remote_epoch || topology_changed) {
            if (metadata_dirty)
                note_retry_trigger(now);
            if (topology_changed)
                formation_settle_due = now + node_.config().dead_after;
            last_metadata_remote_epoch = remote_epoch;
            last_active_nodes = std::move(active_nodes);
        }
        if (const auto due = retry_trigger_due(); due && now >= *due) {
            metadata_retry_due = Clock::time_point{};
            retry_trigger_first_.reset();
        }

        // Law 3: the loader outranks background work, so loader activity (an
        // ingest) counts as busy too. Viewer > loader > background.
        const auto playback_bytes = context_.activity().take_bytes(FrameType::foreground);
        const auto interactive_bytes = context_.activity().take_bytes(FrameType::read_ahead);
        const auto loader_bytes = context_.activity().take_bytes(FrameType::loader);
        const bool viewer_busy =
            playback_bytes > 0 || interactive_bytes > 0 ||
            context_.activity().idle_for(WorkClass::foreground) < policy.foreground_quiet;
        const bool loader_busy =
            loader_bytes > 0 ||
            context_.activity().idle_for(WorkClass::loader) < policy.foreground_quiet;
        const bool busy = viewer_busy || loader_busy;

        bool gc_due_this_pass = false;
        bool repair_continue = false;
        std::optional<Clock::time_point> repair_wake;
        try {
            // Remote generation notices do no kernel/FUSE or replica I/O themselves; this
            // owner advances the local metadata snapshot promptly, and settled
            // verification stays a bounded periodic task.
            bool metadata_ready_for_dependants = !metadata_dirty;
            if (metadata_dirty &&
                (metadata_retry_due == Clock::time_point{} || now >= metadata_retry_due)) {
                if (local_.replica().committed_generation() <= 1 && !established_metadata_peer &&
                    now < formation_settle_due) {
                    metadata_ready_for_dependants = false;
                    metadata_retry_due = formation_settle_due;
                } else {
                    const bool virgin_follower =
                        local_.replica().committed_generation() <= 1 &&
                        !established_metadata_peer && !last_active_nodes.empty() &&
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
                        // This run covers every demand so far.
                        retry_trigger_first_.reset();
                        convergence_failed_ = false;
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
                                metadata_dirty =
                                    port_.metadata_convergence.complete(*convergence_run);
                                metadata_ready_for_dependants = !metadata_dirty;
                                catalogue_dirty = true;
                                metadata_retry_due = Clock::time_point{};
                                metadata_retry_backoff = maintenance_background_interval(policy);
                            } catch (const std::exception& error) {
                                metadata_upkeep_.note_replica_validation(false, error.what());
                                convergence_failed_ = true;
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
                                if (local_.replica().recovery_required() || local.generation <= 1)
                                    throw;
                                Log::debug("metadata repair deferred; continuing non-destructive "
                                           "maintenance: " +
                                           std::string(error.what()));
                            } catch (...) {
                                metadata_upkeep_.note_replica_validation(
                                    false, "metadata validation failed");
                                convergence_failed_ = true;
                                metadata_ready_for_dependants = false;
                                metadata_retry_due = clock_->now() + metadata_retry_backoff;
                                metadata_retry_backoff =
                                    std::min(policy.no_progress_backoff,
                                             std::max(metadata_retry_backoff * 2,
                                                      maintenance_background_interval(policy)));
                                const auto local = local_.replica().committed();
                                if (local_.replica().recovery_required() || local.generation <= 1)
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

            // Repair, as the store paces it: credit earned every pass, a turn
            // when its share against the classes above lets one start. It is
            // paced, never switched off: a node that is always busy still
            // repairs.
            const auto repair_now = Clock::now();
            const auto credit = repair.earn_credit(repair_now, policy.interval);
            const auto paced = repair.paced_by();
            const bool allow_network_repair = repair.may_start(repair_now, paced);
            const bool network_due = allow_network_repair && !repair_quiescent;
            repair.note_gate(network_due             ? dagda::store::RepairGate::ran
                             : !allow_network_repair ? dagda::store::RepairGate::share
                                                     : dagda::store::RepairGate::quiescent,
                             paced);
            if (!allow_network_repair)
                repair_wake = Clock::now() + repair.turn_wait(repair_now, paced);
            if (!cluster_stable_observed_ && node_.membership().all_known_reachable() &&
                metadata_.status().stable) {
                // Recovery after a restart: every known node reached and
                // metadata stable, for the first time in this process.
                cluster_stable_observed_ = true;
                observations().event({unix_ms(),
                                      "cluster_stable",
                                      {{"elapsed_ms", elapsed_us(constructed_) / 1000}},
                                      {}});
            }
            const bool garbage_due = !busy && !metadata_dirty;
            const bool gc_due = !busy && now >= gc_quiescent_until;
            gc_due_this_pass = gc_due;
            // Nothing destructive without a provider: none, the collector is
            // null and the store removes nothing.
            auto* collector = store_node_.collector();

            if (network_due || garbage_due || gc_due) {
                // One inventory for repair, tombstones and both sweeps,
                // rebuilt when the provider's generation has moved. Held back
                // on a repair-only pass until metadata is ready.
                std::optional<dagda::ledger::InventoryStep> inventory;
                if (collector) {
                    enter_stage("inventory");
                    const bool repair_only = network_due && !garbage_due && !gc_due;
                    inventory = collector->inventory_step(pause, !repair_only ||
                                                                     metadata_ready_for_dependants);
                    // Never used destructively in the pass that built it.
                    if (inventory->rebuilt)
                        gc_quiescent_until = clock_->now() + policy.foreground_quiet;
                }
                enter_stage("availability");
                try {
                    (void)repair.survey(pause);
                } catch (const MaintenanceStopping&) {
                    throw;
                } catch (const std::exception& error) {
                    Log::debug("availability survey deferred: " + std::string(error.what()));
                }

                if (network_due) {
                    if (maintenance_stage_hook_)
                        maintenance_stage_hook_("network-repair-begin");
                    // A throw from either stage still closes the turn.
                    auto turn =
                        repair.begin_turn(repair_now, paced, dagda::store::PeerReading::as_begun);
                    enter_stage("retention-repair");
                    const auto walks = repair.claim_walks(turn);
                    enter_stage("network-repair");
                    const auto step = repair.transfer(turn, repair.lists());
                    switch (repair.settle(walks, step)) {
                    case dagda::store::RepairSettled::credit:
                    case dagda::store::RepairSettled::waiting_for_credit:
                        // Wake when the next extent is affordable.
                        if (credit.rate > 0.0) {
                            const auto short_by = std::max<double>(
                                0.0, static_cast<double>(node_.config().extent_size) -
                                         static_cast<double>(repair.credit()));
                            repair_wake =
                                Clock::now() +
                                std::chrono::duration_cast<Clock::duration>(
                                    std::chrono::duration<double>(short_by / credit.rate));
                        }
                        break;
                    case dagda::store::RepairSettled::yielded:
                    case dagda::store::RepairSettled::more_work:
                        repair_continue = true;
                        break;
                    case dagda::store::RepairSettled::quiescent:
                        repair_quiescent = true;
                        break;
                    }
                }

                if (collector && inventory) {
                    dagda::ledger::PassDue due;
                    due.garbage_due = garbage_due;
                    due.gc_due = gc_due;
                    due.busy = busy;
                    due.gc_waiting_for_event = gc_quiescent_until == Clock::time_point::max();
                    due.should_yield = [this, &policy] {
                        const auto quiet = policy.foreground_quiet;
                        return context_.activity().idle_for(WorkClass::foreground) < quiet ||
                               context_.activity().idle_for(WorkClass::loader) < quiet;
                    };
                    due.pause = pause;
                    enter_stage("tombstones");
                    (void)collector->tombstone_step(due, *inventory);
                    const auto release = collector->release_step();
                    enter_stage("control-gc");
                    (void)collector->control_step(due, *inventory, release);
                    enter_stage("garbage-collect");
                    const auto data = collector->data_step(due, *inventory, release);
                    if (data.gate.permitted && !data.swept.yielded && data.swept.complete)
                        gc_quiescent_until = data.swept.deferred ? clock_->now() + data.orphan_grace
                                                                 : Clock::time_point::max();
                }
            } else {
                enter_stage("availability");
                try {
                    (void)repair.survey(pause);
                } catch (const MaintenanceStopping&) {
                    throw;
                } catch (const std::exception& error) {
                    Log::debug("availability survey deferred: " + std::string(error.what()));
                }
            }

            // The store's local stages, each behind its own gate and credit,
            // with Macha's head repair and history truncation between the
            // claims' compaction and pack compaction, as its pass has them.
            auto local = store_node_.begin_local_stages(
                dagda::LocalTurnOptions{stop, busy, policy.interval});
            enter_stage("local-rebalance");
            store_node_.local_rebalance(local);
            enter_stage("retention-compact");
            store_node_.retention_compact(local);

            // An accepted head this replica cannot replay is excluded from reads
            // (MetadataReplica cooldown) and re-anchored here from any peer that can
            // materialize it. A no-op unless a head is flagged.
            try {
                enter_stage("head-repair");
                (void)metadata_upkeep_.repair_unreconstructable_heads(FrameType::speculative);
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

            enter_stage("compact-packs");
            store_node_.compact_packs(local);
            enter_stage("scrub");
            store_node_.local_scrub(local);
            enter_stage("ledger-verify");
            store_node_.ledger_verify(local);
            // NOLINTNEXTLINE(bugprone-empty-catch): stop request, handled by the loop
        } catch (const MaintenanceStopping&) {
            // Asked to stop inside a long walk: the loop's condition ends it.
            // NOLINTNEXTLINE(bugprone-empty-catch): normal state, already published
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

        cpu_reporter.tick();
        const auto now_after_work = clock_->now();
        // The store's stages keep their own credit and rest, re-evaluated on
        // its cadence: at most one interval between passes.
        auto deadline = now_after_work + policy.interval;
        // `complete()` can turn an in-flight burst into one pending follow-up without
        // another external event, so a dirty owner with no retry delay is runnable
        // now; sleeping would lose the edge and strand metadata and its catalogue
        // dependent. Likewise when catalogue repair leaves one more pass to perform.
        if (metadata_dirty) {
            deadline =
                std::min(deadline, metadata_retry_due == Clock::time_point{} ? now_after_work
                                                                             : metadata_retry_due);
            if (const auto due = retry_trigger_due())
                deadline = std::min(deadline, std::max(*due, now_after_work));
        }
        if (catalogue_dirty && !metadata_dirty)
            deadline = std::min(deadline, catalogue_retry_due == Clock::time_point{}
                                              ? now_after_work
                                              : catalogue_retry_due);
        if (gc_quiescent_until != Clock::time_point{} &&
            gc_quiescent_until != Clock::time_point::max()) {
            if (gc_quiescent_until > now_after_work)
                deadline = std::min(deadline, gc_quiescent_until);
            else if (!gc_due_this_pass && !busy)
                deadline = std::min(deadline, now_after_work);
        }
        if (repair_continue)
            deadline = std::min(deadline, now_after_work + policy.interval);
        if (repair_wake)
            deadline = std::min(
                deadline, std::max(now_after_work, now_after_work + (*repair_wake - Clock::now())));

        // A maintenance action can itself commit metadata (retiring a matured garbage
        // marker, say); that event must not be lost for arriving before the wait.
        if (events_now() != observed_event)
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
            port_.last_gc_quiet_ms.store(relative(gc_quiescent_until), std::memory_order_release);
            port_.last_flags.store(
                static_cast<uint8_t>((busy ? 1 : 0) | (gc_due_this_pass ? 2 : 0)),
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
    if (metadata != absorbed_metadata_ || topology != absorbed_topology_) {
        if (port_.metadata_convergence.request(metadata_server_.known_generation()))
            wake = true;
        else
            note_retry_trigger(clock_->now());
    }
    absorbed_storage_ = storage;
    absorbed_metadata_ = metadata;
    absorbed_topology_ = topology;
    absorbed_claims_ = claims;
    absorbed_total_ = storage + metadata + topology + claims;
    return wake;
}

void Maintenance::wait_for_events(const std::stop_token& stop, Clock::time_point deadline) {
    // The predicate only compares counts: absorbing reads the metadata
    // replica, which must not happen under the lock notify() takes.
    while (!stop.stop_requested()) {
        const auto until =
            std::min(deadline, retry_trigger_due().value_or(Clock::time_point::max()));
        if (clock_->now() >= until)
            return;
        {
            Lock wait_lock(events_.wait_mutex);
            clock_->wait_until(events_.wait_cv, wait_lock.native(), stop, until,
                               [this] { return events_.total() != absorbed_total_; });
        }
        if (events_.total() != absorbed_total_ && absorb_events())
            return;
    }
}

void Maintenance::note_retry_trigger(Clock::time_point now) {
    if (!retry_trigger_first_)
        retry_trigger_first_ = now;
    retry_trigger_last_ = now;
}

std::optional<Clock::time_point> Maintenance::retry_trigger_due() const {
    if (!retry_trigger_first_)
        return std::nullopt;
    if (!convergence_failed_)
        return retry_trigger_first_;
    const auto& policy = node_.config().maintenance;
    return std::min(*retry_trigger_first_ + policy.convergence_max_delay,
                    retry_trigger_last_ + policy.convergence_settle);
}
} // namespace macha
