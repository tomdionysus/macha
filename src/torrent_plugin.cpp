// SPDX-License-Identifier: GPL-3.0-or-later
// The libmacha-torrent plugin's boundary with core: the one exported C symbol
// (kSubsystemEntrySymbol) plus the Subsystem that owns a TorrentManager's
// lifecycle and publishes it to the SubsystemRegistry while it is running.
// See TODO/2026-09-05-subsystem-plugin-isolation-plan.md, Phase 1.

#include "log.hpp"
#include "macha_version.hpp"
#include "subsystem_abi.hpp"
#include "subsystem_registry.hpp"
#include "torrent_manager.hpp"

#include <stdexcept>

namespace macha {
namespace {

class TorrentSubsystem final : public Subsystem {
    SubsystemRegistry& registry_;
    std::shared_ptr<TorrentManager> manager_;

  public:
    TorrentSubsystem(SubsystemRegistry& registry, NodeRuntime& node, IngestManager& ingest,
                     TorrentConfig config, const std::filesystem::path& state_path)
        : registry_(registry),
          manager_(std::make_shared<TorrentManager>(node, ingest, std::move(config), state_path)) {
        // Published here rather than in start(): construction is what makes
        // the engine usable (it loads persisted jobs and answers queries and
        // actions immediately), and start() only spins the polling worker. If
        // start() does throw, the supervisor destroys this object, whose
        // destructor withdraws it again -- so a failed start still leaves the
        // capability absent.
        registry_.publish_torrent(manager_);
    }

    ~TorrentSubsystem() override { stop(); }

    std::string_view name() const noexcept override { return "torrent"; }

    void start() override { manager_->start(); }

    void stop() override {
        registry_.withdraw_torrent(manager_.get());
        manager_->stop();
    }
};

} // namespace
} // namespace macha

extern "C" const macha::SubsystemPluginEntry* macha_subsystem_entry() {
    static const macha::SubsystemPluginEntry entry{
        macha::kBuildIdentity,
        [](const macha::SubsystemContext& context) -> std::unique_ptr<macha::Subsystem> {
            if (!context.config || !context.node || !context.ingest || !context.registry)
                throw std::runtime_error("torrent subsystem requires config, node, ingest and "
                                         "registry in its context");
            if (!context.config->torrent.enabled) {
                // Not a fault: an operator turned it off. Returning no
                // instance leaves the capability unavailable without the
                // supervisor's retry/disable machinery treating it as a
                // failing plugin.
                macha::Log::info("torrent subsystem not started: torrent.enabled is false");
                return {};
            }
            return std::make_unique<macha::TorrentSubsystem>(
                *context.registry, *context.node, *context.ingest, context.config->torrent,
                context.config->state_path);
        }};
    return &entry;
}
