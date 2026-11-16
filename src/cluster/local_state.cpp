// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/local_state.hpp"

#include "log.hpp"
#include "metadata/metadata.hpp"
#include "startup_progress.hpp"

namespace macha {

LocalState::LocalState(const Config& cfg, const NodeIdentity& identity,
                       NamespaceDeltaApplier apply_namespace, RecoveryProgress& progress,
                       const StageHook& hook, const std::stop_token& stop) {
    try {
        if (hook)
            hook("metadata");
        if (stop.stop_requested())
            throw RecoveryCancelled();
        seed_ = std::make_unique<MetadataSeed>(cfg.state_path, identity.keys.storage);
        replica_ = std::make_unique<MetadataReplica>(
            cfg.state_path, identity.keys.storage, seed_->read(), cfg.bootstrap.empty(),
            cfg.metadata_materialization_cache_bytes, std::move(apply_namespace));
        seed_->remember(replica_->committed());
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

LocalState::~LocalState() = default;

} // namespace macha
