// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/store_node.hpp"

#include "cluster/dannan_bridge.hpp"

#include <algorithm>
#include <memory>
#include <thread>

namespace macha {
namespace {

// Macha's 0 for "no deadline" on a credit wait; Dagda refuses 0 and bounds every
// wait, so a century stands for it.
constexpr std::chrono::milliseconds unbounded_wait{3153600000000LL};

dagda::store::StorageBackendOptions backend(const StorageBackendConfig& config) {
    dagda::store::StorageBackendOptions out;
    out.path = config.path;
    out.limit = config.limit;
    out.reserve_free = config.reserve_free;
    return out;
}

} // namespace

dagda::NodeOptions store_node_options(const Config& config, const ClusterKeys& keys,
                                      bool hosts_extents,
                                      std::chrono::milliseconds durability_batch_window) {
    dagda::NodeOptions out;
    // One state directory: Macha's, Dannan's and the store's.
    out.state = config.state_path;
    out.key = std::make_shared<const dagda::domain::StorageKey>(
        dagda::domain::StorageKey::derive_v1(dannan_cluster_key(keys)));
    out.garbage_grace = config.maintenance.garbage_grace;
    out.ledger_cache_bytes = config.ledger_cache;
    // Macha's replica keeps the metadata until stage 8c.
    out.heads = false;

    for (const auto& configured : config.storage_backends)
        out.pool.backends.push_back(backend(configured));
    out.pool.durability_batch_window = durability_batch_window;
    out.pool.pack_threshold = config.storage_packing.threshold;
    out.pool.pack_target_size = config.storage_packing.target_size;

    out.control.path = config.metadata_store.path.empty() ? config.state_path / "metadata-objects"
                                                          : config.metadata_store.path;
    out.control.limit = config.metadata_store.limit;
    out.control.pack_threshold = config.metadata_store.packing.threshold;
    out.control.pack_target_size = config.metadata_store.packing.target_size;

    out.cache.path = config.cache.path;
    out.cache.max_blocks = config.cache.max_blocks;
    out.cache.seed = config.cache.prefer_metadata;

    dagda::AdmissionOptions admission;
    admission.data_inflight_bytes = config.data_inflight_bytes;
    admission.foreground_reserve_bytes = config.data_viewer_reserve_bytes;
    // Macha's 0: half the hardware threads, at least one.
    admission.background_concurrency =
        config.maintenance.background_concurrency
            ? config.maintenance.background_concurrency
            : std::max<size_t>(1, std::thread::hardware_concurrency() / 2);
    admission.no_progress = config.data_credit_no_progress_deadline.count()
                                ? config.data_credit_no_progress_deadline
                                : unbounded_wait;
    admission.min_background_under_pressure = config.io_pressure_min_background;
    // A zero slowdown turns the device gate off, as in Macha.
    admission.device_pressure.overhead = std::chrono::milliseconds(config.io_pressure_overhead_ms);
    admission.device_pressure.per_mib = std::chrono::milliseconds(config.io_pressure_per_mib_ms);
    admission.device_pressure.slowdown_percent = config.io_pressure_slowdown_percent;
    admission.device_pressure.release_percent = config.io_pressure_release_percent;
    admission.device_pressure.outlier_percent = config.io_pressure_outlier_percent;
    admission.device_reserve = false;
    out.admission = admission;

    out.server.extent_size = config.extent_size;
    out.server.hosts_extents = hosts_extents;

    out.store.replication = config.replication;
    out.store.write_copies = config.write_copies;
    out.store.metadata_write_copies = config.metadata_write_copies;
    out.store.extent_size = config.extent_size;
    out.store.write_stall = config.write_stall;
    out.store.retention_check_batch_size = config.retention_check_batch_size;
    out.store.retention_check_concurrency = config.retention_check_concurrency;
    // Macha's prompt read takes no credit.
    out.store.prompt_read_admission = false;

    out.repair.foreground_weight = static_cast<uint32_t>(config.maintenance.foreground_weight);
    out.repair.repair_weight = static_cast<uint32_t>(config.maintenance.repair_weight);
    out.repair.foreground_quiet = config.maintenance.foreground_quiet;
    // Macha's peer window: three telemetry intervals, at least 3 s.
    out.repair.peer_window =
        std::max<std::chrono::milliseconds>(3 * config.telemetry_interval, std::chrono::seconds(3));
    out.repair.idle_bandwidth_fraction = config.maintenance.idle_bandwidth_fraction;
    out.repair.initial_bandwidth = config.maintenance.initial_bandwidth;
    out.repair.max_bandwidth = config.maintenance.max_bandwidth;
    out.repair.cpu_target = config.maintenance.cpu_target;
    out.repair.extent_size = config.extent_size;

    // Macha's loop drives the node's steps until stage 8c.
    out.maintenance.automatic = false;
    out.maintenance.interval = config.maintenance.interval;
    out.maintenance.gc_interval = config.maintenance.no_progress_backoff;
    out.maintenance.collector.retry_window = config.maintenance.no_progress_backoff;
    out.maintenance.idle_bandwidth_fraction = config.maintenance.idle_bandwidth_fraction;
    out.maintenance.busy_bandwidth_fraction = config.maintenance.busy_bandwidth_fraction;
    out.maintenance.scrub_fraction = config.maintenance.scrub_fraction;
    out.maintenance.scrub_interval = config.maintenance.scrub_interval;
    out.maintenance.cpu_target = config.maintenance.cpu_target;
    return out;
}

} // namespace macha
