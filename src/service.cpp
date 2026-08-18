// SPDX-License-Identifier: GPL-3.0-or-later
#include "service.hpp"
#include "log.hpp"
#include "diagnostics.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <set>
#include <thread>

namespace macha {

std::chrono::milliseconds maintenance_background_interval(const MaintenanceConfig& policy) {
    return std::max(std::chrono::milliseconds(5000), policy.no_progress_backoff);
}

namespace {
void log_slow_stage(std::string_view stage, Clock::time_point started,
                    const std::string& detail = {}) {
    const auto ms = elapsed_ms(started);
    if (ms < 50 || !Log::enabled(LogLevel::all))
        return;
    Log::trace("DIAG maintenance-stage stage=" + std::string(stage) +
               " elapsed_ms=" + std::to_string(ms) +
               (detail.empty() ? std::string{} : " " + detail));
}
} // namespace

Service::Service(Config config, ClusterKeys keys)
    : node_(std::move(config), keys), store_(node_), metadata_(node_),
      catalogue_(node_, store_, metadata_), fs_(node_, store_, metadata_, &playback_),
      scanner_(node_, fs_, catalogue_, node_.config().catalogue.scanner),
      hydration_(store_, playback_, fs_, catalogue_, node_.config().hydration,
                 node_.config().read_ahead_extents), catalogue_api_(catalogue_),
      streaming_(fs_, catalogue_, node_.config().catalogue.api, node_.config().streaming) {
    if (node_.config().catalogue.api.enabled) {
        catalogue_http_ = std::make_unique<HttpServer>(
            node_.config().catalogue.api,
            [this](const HttpRequest& request) {
                if (request.path.starts_with("/api/v1/playback/"))
                    return streaming_.handle(request);
                return catalogue_api_.handle(request);
            },
            [this](const HttpRequest& request) { return streaming_.capability_request(request); });
    }
}

Service::~Service() {
    stop();
}

void Service::start() {
    node_.start();
    streaming_.start();
    if (catalogue_http_)
        catalogue_http_->start();
    scanner_.start();
    hydration_.start();
    // The maintenance loop attempts catalogue synchronisation before ordinary
    // data repair on its first iteration. Catalogue API reads also synchronise
    // on demand; /status remains available while a joiner is converging.
    maintenance_ = std::jthread([this](std::stop_token stop) { loop(stop); });
}

void Service::request_stop() {
    // Phase one of shutdown is deliberately non-blocking. Signal anything
    // that can be waiting on mounted-filesystem or catalogue work before any
    // component is joined, so teardown cannot deadlock behind the first
    // long-running subsystem in Service::stop().
    fs_.request_io_cancellation();
    scanner_.request_stop();
    hydration_.request_stop();
    if (catalogue_http_)
        catalogue_http_->request_stop();
    streaming_.request_stop();
    if (maintenance_.joinable())
        maintenance_.request_stop();
    node_.request_stop();
}

void Service::stop() {
    Log::debug("shutdown: Service::stop begin");
    request_stop();
    scanner_.stop();
    hydration_.stop();
    if (catalogue_http_)
        catalogue_http_->stop();
    streaming_.stop();
    if (maintenance_.joinable()) {
        Log::debug("shutdown: service maintenance request_stop");
        maintenance_.request_stop();
        Log::debug("shutdown: service maintenance joining");
        maintenance_.join();
        Log::debug("shutdown: service maintenance joined");
    }
    Log::debug("shutdown: NodeRuntime::stop calling");
    node_.stop();
    Log::debug("shutdown: Service::stop complete");
}

void Service::reload_config() {
    if (!node_.config().config_file) {
        Log::warn("configuration reload requested but this node was not started with --config");
        return;
    }
    auto updated = load_yaml_config(*node_.config().config_file);
    if (updated.state_path != node_.config().state_path || updated.key_file != node_.config().key_file)
        throw std::runtime_error("state_path/key_file cannot be changed by live reload");
    if (updated.extent_size != node_.config().extent_size)
        throw std::runtime_error("extent_size cannot be changed for an existing namespace");
    if (updated.replication != node_.config().replication ||
        updated.metadata_replication != node_.config().metadata_replication)
        throw std::runtime_error("replica policy changes require a coordinated cluster restart");
    const auto& current_streaming = node_.config().streaming;
    const bool streaming_restart_required =
        updated.streaming.enabled != current_streaming.enabled ||
        updated.streaming.temp_path != current_streaming.temp_path ||
        updated.streaming.segment_memory_bytes != current_streaming.segment_memory_bytes ||
        updated.streaming.probe_bytes != current_streaming.probe_bytes ||
        updated.streaming.probe_analyze_duration != current_streaming.probe_analyze_duration ||
        updated.streaming.probe_timeout != current_streaming.probe_timeout;
    node_.reconfigure_local(updated);
    scanner_.reconfigure(updated.catalogue.scanner);
    hydration_.reconfigure(updated.hydration, updated.read_ahead_extents);
    if (streaming_restart_required)
        Log::warn("streaming enable/buffer/probe/path changes require restart; live limits were reloaded");
    streaming_.reconfigure(updated.streaming);
    Log::info("reloaded storage backends, persistent cache, catalogue scanner, hydration and streaming limits");
}

std::vector<GarbageRef> Service::collect_garbage(const std::vector<GarbageRef>& garbage) {
    const auto grace = node_.config().maintenance.garbage_grace;
    const auto grace_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(grace).count();
    const auto now_ns = wall_time_ns();
    std::vector<GarbageRef> matured;
    matured.reserve(garbage.size());

    for (const auto& candidate : garbage) {
        // A zero retirement time is a pre-0.10.0 tombstone. It is deliberately
        // ineligible until maintain_garbage_metadata() stamps it into the new
        // lifecycle, giving existing stores a fresh full grace period on upgrade.
        if (candidate.retired_at_ns <= 0 || now_ns < candidate.retired_at_ns ||
            now_ns - candidate.retired_at_ns < grace_ns)
            continue;

        // A matured tombstone no longer protects the object from the physical
        // reachability sweep. Do not delete authoritative bytes here: the sweep
        // performs an atomic age-check-and-remove, so a recently reaffirmed
        // identical object survives even if this retirement view is stale. A
        // cache copy is disposable and can be dropped immediately.
        node_.block_cache().remove(candidate.id);
        matured.push_back(candidate);
    }
    return matured;
}

void Service::maintain_garbage_metadata(const std::vector<GarbageRef>& erase,
                                        const std::vector<GarbageRef>& stamp) {
    if (erase.empty() && stamp.empty())
        return;

    std::map<ObjectId, GarbageRef> erase_expected;
    std::map<ObjectId, GarbageRef> stamp_expected;
    for (const auto& candidate : erase)
        erase_expected[candidate.id] = candidate;
    for (const auto& candidate : stamp)
        stamp_expected[candidate.id] = candidate;

    metadata_.mutate([&](MetadataSnapshot& snapshot) {
        std::erase_if(snapshot.garbage, [&](const GarbageRef& current) {
            auto expected = erase_expected.find(current.id);
            return expected != erase_expected.end() && expected->second == current;
        });

        const auto retired = wall_time_ns();
        for (auto& current : snapshot.garbage) {
            auto expected = stamp_expected.find(current.id);
            if (expected == stamp_expected.end() || expected->second != current)
                continue;
            // Legacy tombstones have neither a retirement time nor an ABA token.
            // Stamping rather than immediately collecting them preserves old
            // storage safely while allowing them to leave metadata after grace.
            current.retired_at_ns = retired;
            current.retirement_id = random_node_id();
        }
    });
}

void Service::loop(std::stop_token stop) {
    const auto& policy = node_.config().maintenance;
    const auto background_interval = maintenance_background_interval(policy);
    ThreadCpuReporter cpu_reporter("macha-maint", std::chrono::seconds(5), true);
    auto last_wall = Clock::now();
    auto last_cpu = std::clock();
    auto last_metadata = Clock::time_point{};
    auto last_catalogue = Clock::time_point{};
    auto last_garbage_inventory = Clock::time_point{};
    auto network_quiescent_until = Clock::time_point{};
    auto local_quiescent_until = Clock::time_point{};
    auto gc_quiescent_until = Clock::time_point{};
    auto scrub_quiescent_until = Clock::time_point{};
    double network_credit = 0.0;
    double local_credit = 0.0;
    double scrub_credit = 0.0;

    while (!stop.stop_requested()) {
        auto now = Clock::now();
        auto wall_seconds = std::chrono::duration<double>(now - last_wall).count();
        if (wall_seconds <= 0.0)
            wall_seconds = std::chrono::duration<double>(policy.interval).count();
        auto cpu_now = std::clock();
        double cpu_seconds = static_cast<double>(cpu_now - last_cpu) / CLOCKS_PER_SEC;
        double cpu_load = wall_seconds > 0.0 ? std::max(0.0, cpu_seconds / wall_seconds) : 0.0;
        last_wall = now;
        last_cpu = cpu_now;

        auto playback_bytes = store_.take_foreground_bytes();
        auto interactive_bytes = store_.take_interactive_bytes();
        const bool playback_busy = playback_bytes > 0 ||
            store_.foreground_idle_for() < policy.foreground_quiet;
        const bool interactive_busy = interactive_bytes > 0 ||
            store_.interactive_idle_for() < policy.foreground_quiet;
        // Priority law: playback/seek > mounted filesystem/useful prefetch >
        // repair/rebalance/scrub. Both foreground classes suppress background
        // work, while the transport queues themselves keep playback above mount I/O.
        bool busy = playback_busy || interactive_busy;
        double fraction = busy ? policy.busy_bandwidth_fraction : policy.idle_bandwidth_fraction;

        double bandwidth = store_.estimated_network_bps();
        if (bandwidth <= 0.0)
            bandwidth = static_cast<double>(policy.initial_bandwidth);
        if (policy.max_bandwidth)
            bandwidth = std::min(bandwidth, static_cast<double>(policy.max_bandwidth));

        // Process CPU includes foreground filesystem/RPC work. Above the target,
        // background work loses budget continuously rather than stopping in a
        // fixed block-per-tick pattern.
        double cpu_scale = 1.0;
        if (cpu_load > policy.cpu_target)
            cpu_scale = std::clamp(policy.cpu_target / cpu_load, 0.05, 1.0);

        double rate = bandwidth * fraction * cpu_scale;
        double burst_cap = std::max<double>(node_.config().extent_size, bandwidth * 5.0);
        network_credit = std::min(burst_cap, network_credit + rate * wall_seconds);
        local_credit = std::min(burst_cap, local_credit + rate * wall_seconds);
        scrub_credit = std::min(burst_cap,
                                scrub_credit + rate * policy.scrub_fraction * wall_seconds);

        try {
            // Metadata maintenance is small but quorum-oriented. Keep it regular
            // while avoiding a control-plane RPC burst on every scheduler tick.
            if (!busy &&
                (last_metadata == Clock::time_point{} || now - last_metadata >= background_interval)) {
                const auto stage = Clock::now();
                metadata_.repair_once();
                log_slow_stage("metadata-repair", stage);
                last_metadata = now;
            }

            // Catalogue GETs are memory-only. Convergence therefore belongs here:
            // generation notices (or the short validation TTL) trigger a refresh
            // independently of foreground activity, while the normal settled-state
            // verification remains an idle/background operation. This keeps remote
            // catalogue changes live without ever putting quorum I/O on an API thread.
            const bool catalogue_refresh_needed = catalogue_.refresh_needed();
            const bool catalogue_periodic =
                last_catalogue == Clock::time_point{} ||
                now - last_catalogue >= background_interval;
            if (catalogue_refresh_needed || (!busy && catalogue_periodic)) {
                const auto stage = Clock::now();
                try {
                    catalogue_.repair_once();
                } catch (const std::exception& e) {
                    Log::debug("catalogue sync: " + std::string(e.what()));
                }
                log_slow_stage("catalogue-repair", stage);
                last_catalogue = now;
            }

            const bool allow_network_repair =
                !busy || policy.busy_bandwidth_fraction > 0.0;
            const bool network_due = allow_network_repair && now >= network_quiescent_until &&
                                     network_credit >= node_.config().extent_size;
            const bool garbage_due =
                !busy && (last_garbage_inventory == Clock::time_point{} ||
                          now - last_garbage_inventory >= background_interval);
            const bool gc_due = !busy && now >= gc_quiescent_until;

            // Reachability GC, repair and explicit tombstone accounting share
            // one immutable namespace inventory. Rebuild it only when one of
            // those consumers can make progress or metadata has advanced.
            if (network_due || garbage_due || gc_due) {
                const auto inventory_stage = Clock::now();
                auto objects = fs_.maintenance_objects_cached();
                bool rebuilt_inventory = false;
                if (!maintenance_live_ ||
                    maintenance_inventory_generation_ != objects->metadata_generation) {
                    auto live = std::make_shared<std::vector<ObjectId>>(objects->live);
                    auto universal = std::make_shared<std::vector<ObjectId>>();
                    auto catalogue_objects = catalogue_.maintenance_objects();
                    live->insert(live->end(), catalogue_objects.live.begin(),
                                 catalogue_objects.live.end());
                    universal->insert(universal->end(), catalogue_objects.universal.begin(),
                                      catalogue_objects.universal.end());
                    std::sort(live->begin(), live->end());
                    live->erase(std::unique(live->begin(), live->end()), live->end());
                    std::sort(universal->begin(), universal->end());
                    universal->erase(std::unique(universal->begin(), universal->end()),
                                     universal->end());

                    maintenance_garbage_.clear();
                    maintenance_stale_garbage_.clear();
                    maintenance_garbage_.reserve(objects->garbage.size());
                    maintenance_stale_garbage_.reserve(objects->garbage.size());
                    for (const auto& garbage : objects->garbage) {
                        if (std::binary_search(live->begin(), live->end(), garbage.id))
                            maintenance_stale_garbage_.push_back(garbage);
                        else
                            maintenance_garbage_.push_back(garbage);
                    }

                    maintenance_inventory_generation_ = objects->metadata_generation;
                    maintenance_live_ = std::move(live);
                    maintenance_universal_ = std::move(universal);
                    rebuilt_inventory = true;
                }
                if (rebuilt_inventory && Log::enabled(LogLevel::all)) {
                    Log::trace("DIAG maintenance-inventory generation=" +
                               std::to_string(objects->metadata_generation) +
                               " entries=" + std::to_string(objects->entries) +
                               " extents=" + std::to_string(objects->extents) +
                               " live=" + std::to_string(maintenance_live_->size()) +
                               " garbage=" + std::to_string(maintenance_garbage_.size()) +
                               " stale_garbage=" +
                               std::to_string(maintenance_stale_garbage_.size()) +
                               " elapsed_ms=" + std::to_string(elapsed_ms(inventory_stage)));
                }

                if (network_due) {
                    const auto byte_budget = static_cast<uint64_t>(network_credit);
                    const auto extent = std::max<uint64_t>(1, node_.config().extent_size);
                    // The byte budget alone does not constrain have-object
                    // probes: a settled or mostly-settled namespace could issue
                    // thousands of synchronous control RPCs while consuming no
                    // network credit. Bound each repair slice independently.
                    const size_t operation_budget = static_cast<size_t>(std::clamp<uint64_t>(
                        (byte_budget / extent), 4, 16));
                    const auto repair_stage = Clock::now();
                    auto repair = store_.repair_step(
                        byte_budget, operation_budget, maintenance_live_.get(),
                        maintenance_universal_.get(),
                        [this] {
                            // End the current maintenance slice as soon as any
                            // foreground I/O appears. The next scheduler pass
                            // will re-evaluate busy_bandwidth_fraction normally.
                            const auto quiet = node_.config().maintenance.foreground_quiet;
                            return store_.foreground_idle_for() < quiet ||
                                   store_.interactive_idle_for() < quiet;
                        },
                        maintenance_inventory_generation_);
                    log_slow_stage("network-repair", repair_stage,
                                   "bytes=" + std::to_string(repair.bytes_transferred) +
                                   " push_examined=" + std::to_string(repair.push_examined) +
                                   " pull_examined=" + std::to_string(repair.pull_examined) +
                                   " remote_ops=" + std::to_string(repair.remote_operations) +
                                   " complete=" + std::to_string(repair.complete ? 1 : 0) +
                                   " yielded=" + std::to_string(repair.yielded ? 1 : 0));
                    if (repair.bytes_transferred) {
                        network_credit = std::max(
                            0.0, network_credit - static_cast<double>(repair.bytes_transferred));
                    }
                    if (repair.yielded) {
                        Log::trace("maintenance: repair yielded to foreground I/O");
                    } else if (!repair.bytes_transferred && repair.complete) {
                        network_credit = 0.0;
                        network_quiescent_until = Clock::now() + policy.no_progress_backoff;
                        Log::trace("maintenance: repair quiescent; backing off no-progress scan");
                    }
                }

                bool garbage_metadata_changed = false;
                if (garbage_due) {
                    auto matured = collect_garbage(maintenance_garbage_);
                    std::vector<GarbageRef> legacy;
                    for (const auto& candidate : maintenance_garbage_) {
                        if (candidate.retired_at_ns == 0)
                            legacy.push_back(candidate);
                    }

                    auto erase = maintenance_stale_garbage_;
                    erase.insert(erase.end(), matured.begin(), matured.end());
                    if (!erase.empty() || !legacy.empty()) {
                        maintain_garbage_metadata(erase, legacy);
                        garbage_metadata_changed = true;
                    }
                    last_garbage_inventory = now;
                }

                // Physical mark/sweep also catches objects that never acquired a
                // tombstone at all (for example, a data put followed by process
                // death before metadata CAS). Recent tombstones are protected for
                // the same grace interval, and legacy tombstones remain protected
                // until their first 0.10.x maintenance stamp has committed.
                if (gc_due && !garbage_metadata_changed && maintenance_live_ &&
                    maintenance_inventory_generation_ >= node_.known_metadata_generation()) {
                    std::vector<ObjectId> protected_ids;
                    protected_ids.reserve(maintenance_garbage_.size());
                    const auto now_ns = wall_time_ns();
                    const auto grace_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              policy.garbage_grace)
                                              .count();
                    for (const auto& candidate : maintenance_garbage_) {
                        const bool matured = candidate.retired_at_ns > 0 &&
                                             now_ns >= candidate.retired_at_ns &&
                                             now_ns - candidate.retired_at_ns >= grace_ns;
                        if (!matured)
                            protected_ids.push_back(candidate.id);
                    }
                    std::sort(protected_ids.begin(), protected_ids.end());
                    protected_ids.erase(std::unique(protected_ids.begin(), protected_ids.end()),
                                        protected_ids.end());

                    const auto gc_stage = Clock::now();
                    auto gc = node_.local_store().gc_step(
                        *maintenance_live_, protected_ids, policy.garbage_grace, 64,
                        [this] {
                            const auto quiet = node_.config().maintenance.foreground_quiet;
                            return store_.foreground_idle_for() < quiet ||
                                   store_.interactive_idle_for() < quiet;
                        });
                    log_slow_stage("garbage-collect", gc_stage,
                                   "reclaimed_bytes=" + std::to_string(gc.bytes) +
                                   " objects=" + std::to_string(gc.objects));
                    if (gc.bytes && Log::enabled(LogLevel::debug))
                        Log::debug("garbage collection reclaimed " + std::to_string(gc.bytes) +
                                   " local bytes");
                    if (gc.yielded) {
                        Log::trace("maintenance: garbage collection yielded to foreground I/O");
                    } else if (gc.complete) {
                        gc_quiescent_until = Clock::now() + policy.no_progress_backoff;
                        Log::trace("maintenance: garbage collection pass complete; backing off");
                    }
                }
            }

            // Local maintenance advances persistent filesystem cursors. A scheduler
            // slice examines at most 64 objects and yields immediately to foreground
            // work; it never rebuilds a complete object list merely to discover that
            // placement is already correct.
            if (!busy && now >= local_quiescent_until &&
                local_credit >= node_.config().extent_size) {
                const auto rebalance_stage = Clock::now();
                auto rebalance = node_.local_store().rebalance_step(
                    static_cast<uint64_t>(local_credit), 64,
                    [this] {
                        const auto quiet = node_.config().maintenance.foreground_quiet;
                        return store_.foreground_idle_for() < quiet ||
                               store_.interactive_idle_for() < quiet;
                    });
                log_slow_stage("local-rebalance", rebalance_stage,
                               "bytes=" + std::to_string(rebalance.bytes) +
                               " objects=" + std::to_string(rebalance.objects));
                if (rebalance.bytes)
                    local_credit = std::max(
                        0.0, local_credit - static_cast<double>(rebalance.bytes));
                if (rebalance.yielded) {
                    Log::trace("maintenance: local rebalance yielded to foreground I/O");
                } else if (rebalance.complete && !rebalance.bytes) {
                    local_credit = 0.0;
                    local_quiescent_until = Clock::now() + policy.no_progress_backoff;
                    Log::trace("maintenance: local rebalance quiescent; backing off no-progress scan");
                }
            }

            if (!busy && now >= scrub_quiescent_until &&
                scrub_credit >= node_.config().extent_size) {
                const auto scrub_stage = Clock::now();
                auto scrub = node_.local_store().scrub_step(
                    static_cast<uint64_t>(scrub_credit), 64,
                    [this] {
                        const auto quiet = node_.config().maintenance.foreground_quiet;
                        return store_.foreground_idle_for() < quiet ||
                               store_.interactive_idle_for() < quiet;
                    });
                log_slow_stage("scrub", scrub_stage,
                               "bytes=" + std::to_string(scrub.bytes) +
                               " objects=" + std::to_string(scrub.objects));
                if (scrub.bytes)
                    scrub_credit = std::max(0.0, scrub_credit - static_cast<double>(scrub.bytes));
                if (scrub.yielded) {
                    Log::trace("maintenance: scrub yielded to foreground I/O");
                } else if (scrub.complete) {
                    // A scrub is a complete integrity pass, not an endless loop.
                    // After reaching the end, pause before beginning at object zero
                    // again even though useful bytes were checked during the pass.
                    scrub_credit = 0.0;
                    scrub_quiescent_until = Clock::now() + policy.no_progress_backoff;
                    Log::trace("maintenance: scrub pass complete; backing off");
                }
            }

        } catch (const std::exception& e) {
            Log::debug("maintenance: " + std::string(e.what()));
        }

        cpu_reporter.tick();
        auto until = Clock::now() + policy.interval;
        while (!stop.stop_requested() && Clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}
} // namespace macha
