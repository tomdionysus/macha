// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/local_state.hpp"
#include "cluster/control_objects.hpp"

#include "log.hpp"
#include "metadata/metadata.hpp"
#include "metadata/namespace_control_store.hpp"
#include "startup_progress.hpp"
#include "storage/local_store.hpp"
#include "storage/persistent_cache.hpp"
#include "storage/retention.hpp"
#include "storage/storage_pool.hpp"
#include "supervised.hpp"

#include <exception>
#include <thread>

namespace macha {
namespace {

void stage(const LocalState::StageHook& hook, std::string_view name, std::stop_token stop) {
    if (hook)
        hook(name);
    if (stop.stop_requested())
        throw RecoveryCancelled();
}

} // namespace

LocalState::LocalState(const Config& cfg, const NodeIdentity& identity, NodeRuntime& node,
                       RecoveryProgress& progress, const StageHook& hook, std::stop_token stop) {
    // The DATA pool and the control-side chain are independent; recover them
    // side by side, as a large pool can take a while.
    std::exception_ptr data_failure;
    std::jthread data_recovery([&](std::stop_token) {
        run_supervised_once("cluster-storage-recovery", [&] {
            try {
                recover_data(cfg, identity, progress, hook, stop);
            } catch (...) {
                data_failure = std::current_exception();
            }
        });
    });
    std::exception_ptr state_failure;
    try {
        recover_state(cfg, identity, node, progress, hook, stop);
    } catch (...) {
        state_failure = std::current_exception();
    }
    data_recovery.join();
    for (const auto& failure : {state_failure, data_failure})
        if (failure)
            std::rethrow_exception(failure);
}

LocalState::~LocalState() = default;

void LocalState::recover_data(const Config& cfg, const NodeIdentity& identity,
                              RecoveryProgress& progress, const StageHook& hook,
                              std::stop_token stop) {
    try {
        stage(hook, "data-storage", stop);
        auto pool = std::make_unique<StoragePool>(cfg.state_path, identity.id, cfg.storage_backends,
                                                  identity.keys.storage,
                                                  std::chrono::milliseconds(500),
                                                  cfg.storage_packing);
        if (stop.stop_requested())
            throw RecoveryCancelled();
        // Device-pressure admission; a zero target disables it.
        if (cfg.io_pressure_slowdown_percent) {
            pool->configure_service_monitor(DiskServiceMonitor::Thresholds{
                std::chrono::milliseconds(cfg.io_pressure_overhead_ms),
                std::chrono::milliseconds(cfg.io_pressure_per_mib_ms),
                cfg.io_pressure_slowdown_percent, cfg.io_pressure_release_percent,
                cfg.io_pressure_outlier_percent});
            Log::info("data io pressure gate enabled expected_ms=" +
                      std::to_string(cfg.io_pressure_overhead_ms) + "+" +
                      std::to_string(cfg.io_pressure_per_mib_ms) + "/MiB slowdown_percent=" +
                      std::to_string(cfg.io_pressure_slowdown_percent) + " release_percent=" +
                      std::to_string(cfg.io_pressure_release_percent) + " outlier_percent=" +
                      std::to_string(cfg.io_pressure_outlier_percent) + " min_background=" +
                      std::to_string(cfg.io_pressure_min_background));
        }
        const auto used = pool->used();
        const auto capacity = pool->limit();
        data_ = std::move(pool);
        note_startup_progress();
        progress.mark(RecoveryProgress::data_storage);
        // An edge node runs an empty pool, so callers need no special case.
        // Said explicitly, or capacity=0 reads like a missing disk.
        Log::info("node data storage ready used=" + std::to_string(used) +
                  " capacity=" + std::to_string(capacity) +
                  (cfg.storage_backends.empty() ? " (hosts no extents)" : ""));
    } catch (const RecoveryCancelled&) {
        throw;
    } catch (const std::exception& error) {
        Log::error("node data storage recovery failed: " + std::string(error.what()));
        progress.fail("data storage: " + std::string(error.what()));
        throw;
    }
}

void LocalState::recover_state(const Config& cfg, const NodeIdentity& identity,
                               NodeRuntime& node, RecoveryProgress& progress,
                               const StageHook& hook, std::stop_token stop) {
    try {
        stage(hook, "control-storage", stop);
        control_ = std::make_unique<LocalStore>(
            cfg.metadata_store.path,
            LocalStoreOptions{cfg.metadata_store.limit, 0, cfg.metadata_store.packing.threshold,
                              cfg.metadata_store.packing.target_size},
            identity.keys.storage, LocalStoreMode::authoritative, nullptr,
            posix_local_store_files());
        control_fetch_ = std::make_unique<ControlObjectFetch>(node, *control_);
        note_startup_progress();
        progress.mark(RecoveryProgress::control_storage);

        stage(hook, "cache", stop);
        cache_ = std::make_unique<PersistentBlockCache>(cfg.cache, identity.keys.storage);
        note_startup_progress();
        progress.mark(RecoveryProgress::cache);

        stage(hook, "retention", stop);
        retention_ = std::make_unique<RetentionStore>(cfg.state_path, identity.keys.storage);
        note_startup_progress();
        progress.mark(RecoveryProgress::retention);

        stage(hook, "metadata", stop);
        // Replay writes nodes locally and replicates nothing: the commit reached
        // the floor when made, and rebuilding a local head must not depend on
        // peers.
        replica_ = std::make_unique<MetadataReplica>(
            cfg.state_path, identity.keys.storage, cache_->metadata(), cfg.bootstrap.empty(),
            cfg.metadata_materialization_cache_bytes,
            [this](const ObjectId& root, const MetadataDelta& delta) {
                auto nodes = ControlNamespaceNodeStore::for_replay(*control_, *control_fetch_);
                return apply_delta_to_namespace_tree(root, nodes, delta);
            });
        cache_->remember_metadata(replica_->committed());
        note_startup_progress();
        progress.mark(RecoveryProgress::metadata);
        Log::info("node metadata ready generation=" +
                  std::to_string(replica_->committed().generation));
    } catch (const RecoveryCancelled&) {
        throw;
    } catch (const std::exception& error) {
        Log::error("node local state recovery failed: " + std::string(error.what()));
        progress.fail("local state: " + std::string(error.what()));
        throw;
    }
}

void LocalState::reconfigure(const Config& cfg) {
    data_->reconfigure(cfg.storage_backends);
    data_->refresh();
    cache_->reconfigure(cfg.cache);
}

} // namespace macha
