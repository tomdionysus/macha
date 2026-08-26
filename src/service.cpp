// SPDX-License-Identifier: GPL-3.0-or-later
#include "service.hpp"
#include "ffmpeg_log.hpp"
#include "log.hpp"
#include "diagnostics.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <limits>
#include <set>
#include <thread>

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
    Log::trace("DIAG maintenance-stage stage=" + std::string(stage) +
               " elapsed_ms=" + std::to_string(ms) +
               (detail.empty() ? std::string{} : " " + detail));
}
} // namespace

Service::Service(Config config, ClusterKeys keys)
    : node_(std::move(config), keys), store_(node_), metadata_(node_),
      catalogue_(node_, store_, metadata_), fs_(node_, store_, metadata_, &playback_),
      catalogue_hints_(node_.config().state_path),
      scanner_(node_, fs_, catalogue_, catalogue_hints_, node_.config().catalogue.scanner),
      hydration_(store_, playback_, fs_, catalogue_, node_.config().hydration,
                 node_.config().read_ahead_extents),
      ingest_(node_, fs_, catalogue_hints_, node_.config().ingest),
      torrents_(ingest_, node_.config().torrent, node_.config().state_path),
      torrent_search_(node_.config().torrent),
      acquisition_api_(ingest_, torrents_, torrent_search_),
      catalogue_api_(catalogue_, catalogue_hints_,
                     [this](const std::vector<std::string>& media_ids) {
                         scanner_.request_media_rescan(media_ids);
                     }),
      streaming_(fs_, catalogue_, node_.config().catalogue.api, node_.config().streaming) {
    if (node_.config().catalogue.api.enabled) {
        catalogue_http_ = std::make_unique<HttpServer>(
            node_.config().catalogue.api,
            [this](const HttpRequest& request) {
                if (request.path.starts_with("/api/v1/playback/"))
                    return streaming_.handle(request);
                if (request.path.starts_with("/api/v1/ingest/") ||
                    request.path.starts_with("/api/v1/torrents/"))
                    return acquisition_api_.handle(request);
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
    ingest_.start();
    torrents_.start();
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
    torrents_.request_stop();
    ingest_.request_stop();
    scanner_.request_stop();
    hydration_.request_stop();
    if (catalogue_http_)
        catalogue_http_->request_stop();
    streaming_.request_stop();
    if (maintenance_.joinable()) {
        maintenance_.request_stop();
        maintenance_wait_cv_.notify_all();
    }
    node_.request_stop();
}

void Service::stop() {
    Log::debug("shutdown: Service::stop begin");
    request_stop();
    torrents_.stop();
    ingest_.stop();
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
    if (updated.fuse.spool_path != node_.config().fuse.spool_path ||
        updated.fuse.operation_journal_path != node_.config().fuse.operation_journal_path)
        throw std::runtime_error("FUSE spool/journal paths cannot be changed by live reload");
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
    Log::set_logger(std::make_shared<ConsoleLogger>(updated.log_level));
    configure_ffmpeg_logging(updated.ffmpeg_log_level);
    node_.reconfigure_local(updated);
    scanner_.reconfigure(updated.catalogue.scanner);
    hydration_.reconfigure(updated.hydration, updated.read_ahead_extents);
    ingest_.reconfigure(updated.ingest);
    torrents_.reconfigure(updated.torrent);
    if (streaming_restart_required)
        Log::warn("streaming enable/buffer/probe/path changes require restart; live limits were reloaded");
    streaming_.reconfigure(updated.streaming);
    Log::info("reloaded storage backends, persistent cache, catalogue scanner, ingest, torrent, hydration and streaming limits");
}

std::vector<GarbageRef> Service::collect_garbage(const std::vector<GarbageRef>& garbage) {
    const auto grace = node_.config().maintenance.garbage_grace;
    const auto grace_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(grace).count();
    const auto now_ns = wall_time_ns();
    std::vector<GarbageRef> matured;
    matured.reserve(garbage.size());

    for (const auto& candidate : garbage) {
        // A zero retirement time denotes a legacy tombstone. It is deliberately
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

    metadata_.mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
        std::erase_if(snapshot.garbage, [&](const GarbageRef& current) {
            auto expected = erase_expected.find(current.id);
            const bool remove = expected != erase_expected.end() && expected->second == current;
            if (remove)
                delta.erase_garbage.push_back(current.id);
            return remove;
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
            delta.upsert_garbage.push_back(current);
        }
        std::sort(delta.erase_garbage.begin(), delta.erase_garbage.end());
        delta.erase_garbage.erase(
            std::unique(delta.erase_garbage.begin(), delta.erase_garbage.end()),
            delta.erase_garbage.end());
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
    auto scrub_due_unix_ms = initialise_scrub_due(node_.config().state_path,
                                                   policy.scrub_interval);
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
            cpu_scale = std::clamp(policy.cpu_target / cpu_load, 0.0, 1.0);

        double rate = bandwidth * fraction * cpu_scale;
        double burst_cap = std::max<double>(node_.config().extent_size, bandwidth * 5.0);
        network_credit = std::min(burst_cap, network_credit + rate * wall_seconds);
        local_credit = std::min(burst_cap, local_credit + rate * wall_seconds);
        const auto wall_now_ms = unix_ms();
        const bool scrub_due = policy.scrub_fraction > 0.0 &&
                               wall_now_ms >= scrub_due_unix_ms;
        if (scrub_due) {
            scrub_credit = std::min(
                burst_cap, scrub_credit + rate * policy.scrub_fraction * wall_seconds);
        } else {
            // Do not bank weeks of scrub credit and explode into a large burst when
            // the next campaign becomes due. Outside a campaign, scrub is truly idle.
            scrub_credit = 0.0;
        }

        try {
            // Remote generation notices wake no kernel/FUSE path and perform no
            // quorum I/O themselves. The maintenance owner advances the coherent
            // local metadata snapshot promptly, while settled verification remains
            // a bounded periodic control-plane task.
            const bool metadata_refresh_needed =
                node_.known_metadata_generation() > node_.metadata_replica().committed_generation();
            const bool metadata_periodic =
                last_metadata == Clock::time_point{} || now - last_metadata >= background_interval;
            if (metadata_refresh_needed || (!busy && metadata_periodic)) {
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
                if (!maintenance_live_ || !maintenance_catalogue_complete_ ||
                    maintenance_inventory_generation_ != objects->metadata_generation) {
                    auto live = std::make_shared<std::vector<ObjectId>>(objects->live);
                    auto universal = std::make_shared<std::vector<ObjectId>>();
                    auto control_live = std::make_shared<std::vector<ObjectId>>();
                    auto catalogue_objects = catalogue_.maintenance_objects();
                    maintenance_catalogue_complete_ = catalogue_objects.complete;
                    live->insert(live->end(), catalogue_objects.live.begin(),
                                 catalogue_objects.live.end());
                    universal->insert(universal->end(), catalogue_objects.universal.begin(),
                                      catalogue_objects.universal.end());
                    control_live->insert(control_live->end(), catalogue_objects.control_live.begin(),
                                         catalogue_objects.control_live.end());
                    std::sort(live->begin(), live->end());
                    live->erase(std::unique(live->begin(), live->end()), live->end());
                    std::sort(universal->begin(), universal->end());
                    universal->erase(std::unique(universal->begin(), universal->end()),
                                     universal->end());
                    std::sort(control_live->begin(), control_live->end());
                    control_live->erase(std::unique(control_live->begin(), control_live->end()),
                                        control_live->end());

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
                    maintenance_control_live_ = std::move(control_live);
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
                if (garbage_due && maintenance_catalogue_complete_) {
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

                if (gc_due && maintenance_catalogue_complete_ && maintenance_control_live_) {
                    const auto removed = catalogue_.control_gc_step(
                        *maintenance_control_live_, policy.garbage_grace, 32);
                    if (removed)
                        Log::debug("catalogue control GC removed=" + std::to_string(removed));
                }

                // Physical mark/sweep also catches objects that never acquired a
                // tombstone at all (for example, a data put followed by process
                // death before metadata CAS). Recent tombstones are protected for
                // the same grace interval, and legacy tombstones remain protected
                // until their first 0.10.x maintenance stamp has committed.
                if (gc_due && maintenance_catalogue_complete_ && !garbage_metadata_changed &&
                    maintenance_live_ &&
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

            if (!busy && scrub_due &&
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
                    // A proactive scrub is a low-frequency integrity campaign. Once
                    // the complete physical pass finishes, stay genuinely idle until
                    // the next scheduled campaign instead of restarting after the
                    // generic no-progress backoff used by repair/rebalance.
                    scrub_credit = 0.0;
                    const auto completed = unix_ms();
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

        } catch (const std::exception& e) {
            Log::debug("maintenance: " + std::string(e.what()));
        }

        cpu_reporter.tick();
        std::unique_lock wait_lock(maintenance_wait_mutex_);
        maintenance_wait_cv_.wait_for(wait_lock, stop, policy.interval, [] { return false; });
    }
}
} // namespace macha
