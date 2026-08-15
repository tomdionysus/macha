// SPDX-License-Identifier: GPL-3.0-or-later
#include "service.hpp"
#include "log.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <set>
#include <thread>

namespace macha {
Service::Service(Config config, ClusterKeys keys)
    : node_(std::move(config), keys), store_(node_), metadata_(node_),
      catalogue_(node_, store_, metadata_), fs_(node_, store_, metadata_, &playback_),
      scanner_(node_, fs_, catalogue_, node_.config().catalogue.scanner),
      hydration_(store_, playback_, fs_, catalogue_, node_.config().hydration,
                 node_.config().read_ahead_extents), catalogue_api_(catalogue_) {
    if (node_.config().catalogue.api.enabled) {
        catalogue_http_ = std::make_unique<HttpServer>(
            node_.config().catalogue.api,
            [this](const HttpRequest& request) { return catalogue_api_.handle(request); });
    }
}

Service::~Service() {
    stop();
}

void Service::start() {
    node_.start();
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
    node_.reconfigure_local(updated);
    scanner_.reconfigure(updated.catalogue.scanner);
    hydration_.reconfigure(updated.hydration, updated.read_ahead_extents);
    Log::info("reloaded storage backends, persistent cache, catalogue scanner and hydration configuration");
}

void Service::collect_garbage(const std::vector<ObjectId>& garbage) {
    const auto grace = node_.config().maintenance.garbage_grace;
    const auto now = Clock::now();
    std::set<ObjectId> candidates(garbage.begin(), garbage.end());

    for (auto it = garbage_seen_.begin(); it != garbage_seen_.end();) {
        if (!candidates.contains(it->first))
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
    auto last_wall = Clock::now();
    auto last_cpu = std::clock();
    auto last_metadata = Clock::time_point{};
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

        auto foreground = store_.take_foreground_bytes();
        bool busy = foreground > 0 || store_.foreground_idle_for() < policy.foreground_quiet;
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
            if (last_metadata == Clock::time_point{} || now - last_metadata >= std::chrono::seconds(5)) {
                metadata_.repair_once();
                last_metadata = now;
            }

            // Catalogue state is cluster metadata. A joiner pulls the complete
            // immutable snapshot and all referenced artwork before reporting the
            // catalogue ready; this runs ahead of ordinary data repair.
            try {
                catalogue_.repair_once();
            } catch (const std::exception& e) {
                Log::debug("catalogue sync: " + std::string(e.what()));
            }

            auto objects = fs_.maintenance_objects();
            std::set<ObjectId> live(objects.live.begin(), objects.live.end());
            std::set<ObjectId> universal;
            auto catalogue_objects = catalogue_.maintenance_objects();
            live.insert(catalogue_objects.live.begin(), catalogue_objects.live.end());
            universal.insert(catalogue_objects.universal.begin(),
                             catalogue_objects.universal.end());
            std::erase_if(objects.garbage,
                          [&](const ObjectId& id) { return live.contains(id); });

            const bool allow_network_repair =
                !busy || policy.busy_bandwidth_fraction > 0.0;
            if (allow_network_repair && network_credit >= node_.config().extent_size) {
                auto used = store_.repair_once(static_cast<uint64_t>(network_credit), &live,
                                               &universal);
                network_credit = std::max(0.0, network_credit - static_cast<double>(used));
            }

            // Local disk rebalance has an independent byte credit: adding or
            // returning a disk never consumes the network repair allowance.
            if (!busy && local_credit >= node_.config().extent_size) {
                auto used = node_.local_store().rebalance_once(static_cast<uint64_t>(local_credit));
                local_credit = std::max(0.0, local_credit - static_cast<double>(used));
            }

            if (!busy && scrub_credit >= node_.config().extent_size) {
                auto used = store_.scrub_once(static_cast<uint64_t>(scrub_credit));
                scrub_credit = std::max(0.0, scrub_credit - static_cast<double>(used));
            }

            collect_garbage(objects.garbage);
        } catch (const std::exception& e) {
            Log::debug("maintenance: " + std::string(e.what()));
        }

        auto until = Clock::now() + policy.interval;
        while (!stop.stop_requested() && Clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}
} // namespace macha
