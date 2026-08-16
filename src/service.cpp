// SPDX-License-Identifier: GPL-3.0-or-later
#include "service.hpp"
#include "log.hpp"
#include "diagnostics.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
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
    if (ms < 50 || !Log::enabled(LogLevel::debug))
        return;
    Log::debug("DIAG maintenance-stage stage=" + std::string(stage) +
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

void Service::stop() {
    Log::debug("shutdown: Service::stop begin");
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

void Service::collect_garbage(const std::vector<ObjectId>& garbage) {
    const auto grace = node_.config().maintenance.garbage_grace;
    const auto now = Clock::now();
    const auto& candidates = garbage;

    for (auto it = garbage_seen_.begin(); it != garbage_seen_.end();) {
        if (!std::binary_search(candidates.begin(), candidates.end(), it->first))
            it = garbage_seen_.erase(it);
        else
            ++it;
    }
    for (const auto& id : candidates)
        garbage_seen_.try_emplace(id, now);

    for (const auto& [id, first_seen] : garbage_seen_) {
        if (now - first_seen >= grace) {
            // The tombstone is cluster metadata, so every node independently
            // removes its copy. A disconnected node sees the same tombstone
            // after rejoining and converges without a remote-delete race.
            node_.local_store().remove(id);
            node_.block_cache().remove(id);
        }
    }
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

            // Metadata notices and foreground reads provide prompt convergence.
            // Settled background verification uses the no-progress backoff rather
            // than creating a control-plane quorum burst every five seconds.
            if (!busy &&
                (last_catalogue == Clock::time_point{} || now - last_catalogue >= background_interval)) {
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

            // Enumerating every live extent is O(namespace size), and doing it on
            // every scheduler tick made a settled node burn CPU while performing
            // no I/O. Build the inventory only when repair can actually spend a
            // budget or when garbage accounting is due.
            if (network_due || garbage_due) {
                const auto inventory_stage = Clock::now();
                auto objects = fs_.maintenance_objects_cached();
                bool rebuilt_inventory = false;
                if (!maintenance_live_ ||
                    maintenance_inventory_generation_ != objects->metadata_generation) {
                    auto live = std::make_shared<std::vector<ObjectId>>(objects->live);
                    auto universal = std::make_shared<std::vector<ObjectId>>();
                    auto catalogue_objects = catalogue_.maintenance_objects();
                    live->insert(live->end(), catalogue_objects.live.begin(), catalogue_objects.live.end());
                    universal->insert(universal->end(), catalogue_objects.universal.begin(),
                                      catalogue_objects.universal.end());
                    std::sort(live->begin(), live->end());
                    live->erase(std::unique(live->begin(), live->end()), live->end());
                    std::sort(universal->begin(), universal->end());
                    universal->erase(std::unique(universal->begin(), universal->end()),
                                     universal->end());
                    maintenance_garbage_ = objects->garbage;
                    std::erase_if(maintenance_garbage_, [&](const ObjectId& id) {
                        return std::binary_search(live->begin(), live->end(), id);
                    });
                    maintenance_inventory_generation_ = objects->metadata_generation;
                    maintenance_live_ = std::move(live);
                    maintenance_universal_ = std::move(universal);
                    rebuilt_inventory = true;
                }
                if (rebuilt_inventory && Log::enabled(LogLevel::debug)) {
                    Log::debug("DIAG maintenance-inventory generation=" +
                               std::to_string(objects->metadata_generation) +
                               " entries=" + std::to_string(objects->entries) +
                               " extents=" + std::to_string(objects->extents) +
                               " live=" + std::to_string(maintenance_live_->size()) +
                               " garbage=" + std::to_string(maintenance_garbage_.size()) +
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
                        Log::debug("maintenance: repair yielded to foreground I/O");
                    } else if (!repair.bytes_transferred && repair.complete) {
                        network_credit = 0.0;
                        network_quiescent_until = Clock::now() + policy.no_progress_backoff;
                        Log::debug("maintenance: repair quiescent; backing off no-progress scan");
                    }
                }
                if (garbage_due) {
                    collect_garbage(maintenance_garbage_);
                    last_garbage_inventory = now;
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
                    Log::debug("maintenance: local rebalance yielded to foreground I/O");
                } else if (rebalance.complete && !rebalance.bytes) {
                    local_credit = 0.0;
                    local_quiescent_until = Clock::now() + policy.no_progress_backoff;
                    Log::debug("maintenance: local rebalance quiescent; backing off no-progress scan");
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
                    Log::debug("maintenance: scrub yielded to foreground I/O");
                } else if (scrub.complete) {
                    // A scrub is a complete integrity pass, not an endless loop.
                    // After reaching the end, pause before beginning at object zero
                    // again even though useful bytes were checked during the pass.
                    scrub_credit = 0.0;
                    scrub_quiescent_until = Clock::now() + policy.no_progress_backoff;
                    Log::debug("maintenance: scrub pass complete; backing off");
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
