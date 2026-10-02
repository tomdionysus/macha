// SPDX-License-Identifier: GPL-3.0-or-later
// The plugin's boundary with core: the exported kSubsystemEntrySymbol and the
// Subsystem owning a TorrentManager's lifecycle and registry publication.

#include "log.hpp"
#include "macha_version.hpp"
#include "subsystem/subsystem_abi.hpp"
#include "subsystem/subsystem_registry.hpp"
#include "torrent/torrent_manager.hpp"

#include <stdexcept>

namespace macha {
namespace {

class TorrentSubsystem final : public Subsystem {
    SubsystemRegistry& registry_;
    MessageRoutes& routes_;
    std::shared_ptr<TorrentManager> manager_;

  public:
    TorrentSubsystem(SubsystemRegistry& registry, MessageRoutes& routes, NodeRuntime& node,
                     DataResourceArbiter& data_resources, IngestManager& ingest,
                     TorrentConfig config, const std::filesystem::path& state_path)
        : registry_(registry), routes_(routes),
          manager_(std::make_shared<TorrentManager>(node, data_resources, ingest,
                                                    std::move(config), state_path)) {
        // Published at construction, which makes the engine usable; start()
        // only runs the worker. A failed start destroys this, withdrawing it.
        registry_.publish_torrent(manager_);
        routes_.bind(MessageType::get_torrent_jobs,
                     [this](const NodeInfo&, FrameType, const RpcMessage& request) {
                         return RpcMessage{MessageType::torrent_jobs_reply,
                                           manager_->handle_jobs_query(request.payload)};
                     });
        routes_.bind(MessageType::torrent_job_action,
                     [this](const NodeInfo&, FrameType, const RpcMessage& request) {
                         return RpcMessage{MessageType::torrent_job_action_reply,
                                           manager_->handle_job_action(request.payload)};
                     });
    }

    // The manager is rebuilt on fault: unbinding waits out any call in flight.
    ~TorrentSubsystem() override {
        routes_.unbind(MessageType::torrent_job_action);
        routes_.unbind(MessageType::get_torrent_jobs);
        stop();
    }

    std::string_view name() const noexcept override { return "torrent"; }

    // A fault the worker cannot contain to one job: the supervisor rebuilds
    // this subsystem from jobs.json.
    void attach_fault_sink(FaultSink sink) override { manager_->set_fault_sink(std::move(sink)); }

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
            if (!context.config || !context.node || !context.data_resources ||
                !context.ingest || !context.registry || !context.routes)
                throw std::runtime_error("torrent subsystem requires config, node, DATA "
                                         "resources, ingest, registry and routes in its "
                                         "context");
            if (!context.config->torrent.enabled) {
                // Disabled, not a fault: no instance, and no supervisor retry.
                macha::Log::info("torrent subsystem not started: torrent.enabled is false");
                return {};
            }
            return std::make_unique<macha::TorrentSubsystem>(
                *context.registry, *context.routes, *context.node, *context.data_resources,
                *context.ingest,
                context.config->torrent,
                context.config->state_path);
        }};
    return &entry;
}
