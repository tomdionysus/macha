// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "api/acquisition_api.hpp"
#include "catalogue/catalogue.hpp"
#include "api/catalogue_api.hpp"
#include "catalogue/catalogue_hints.hpp"
#include "service/convergence_demand.hpp"
#include "filesystem/filesystem.hpp"
#include "fuse/fuse_frontend.hpp"
#include "filesystem/hydration.hpp"
#include "acquisition/ingest.hpp"
#include "api/manage_api.hpp"
#include "catalogue/media_catalogue.hpp"
#include "catalogue/media_information.hpp"
#include "playback/playback.hpp"
#include "api/session_api.hpp"
#include "api/users_api.hpp"
#include "api/web_api.hpp"
#include "api/status_api.hpp"
#include "torrent/torrent.hpp"
#include "observation.hpp"
#include "service/maintenance.hpp"
#include "service/maintenance_clock.hpp"
#include "service/node_services.hpp"
#include "cluster/node_resources.hpp"
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace macha {


// What a test injects into a Service. Production passes none of it: the
// system clock, no trace and no lifecycle record.
struct ServiceInstruments {
    std::shared_ptr<MaintenanceClock> clock;
    MaintenanceTraceHook trace;
    LifecycleHook lifecycle;
};

class Service {
  public:
    using MaintenanceStageHook = std::function<void(std::string_view)>;
    // Test-only interception point for a detected startup stall (see
    // wait_services_ready()). Production leaves this empty and terminates the
    // process instead; a test can observe the stall without killing itself.
    using StartupStallHandler = std::function<void(std::string_view diagnostic)>;

  private:
    // First, so startup time is measured from the start of construction.
    Clock::time_point constructed_{Clock::now()};
    // Before node_, which is constructed after them.
    std::shared_ptr<MaintenanceClock> clock_;
    MaintenanceTraceHook maintenance_trace_;
    LifecycleHook lifecycle_;
    void note_lifecycle(std::string_view event) {
        if (lifecycle_)
            lifecycle_(event);
    }
    // Built, and stopped, before the node that waits on them.
    NodeResources resources_;
    // Inbound requests by message type; each part binds what it answers.
    MessageRoutes routes_;
    NodeRuntime node_;
    ClusterStatusService cluster_status_;
    // Torrent runs as a plugin and FUSE as a supervised builtin. Each publishes
    // what it provides in `registry_`, declared first and destroyed last so a
    // subsystem being torn down can still withdraw itself.
    SubsystemRegistry registry_;
    SessionApi session_api_;
    UsersApi users_api_;
    WebApi web_;
    std::unique_ptr<HttpServer> catalogue_http_;
    // Service rings the maintenance pass through its port, and reads its
    // diagnostics there, before the pass exists and after it has gone.
    MaintenancePort maintenance_port_;
    MaintenanceStageHook maintenance_stage_hook_;
    // The node's composition root (see node_services.hpp), built once local
    // state has recovered. Set once by the startup thread; published by
    // services_ready_.
    std::unique_ptr<NodeServices> services_;

    std::jthread startup_;
    std::atomic_bool services_ready_{};
    std::atomic_bool stopped_{};
    std::atomic_bool startup_failed_{};
    mutable std::mutex startup_mutex_;
    std::condition_variable startup_cv_;
    std::string startup_error_;
    StartupStallHandler startup_stall_handler_;

    // Observation probes: written to a local file under the state path, never to
    // Status or any API response.
    std::unique_ptr<ObservationRecorder> observation_recorder_;
    std::atomic_bool observation_stopping_{};
    std::map<std::string, uint64_t> observation_gauges();

    void initialise_services(std::stop_token);
    void wait_services_ready();
    std::string describe_readiness_stall() const;
    HttpResponse handle_http(const HttpRequest&);
    StatusSources status_sources();
    // Unauthenticated liveness: whether this node is serving, and nothing more.
    HttpResponse health_response() const;
    bool capability_request(const HttpRequest&);

  public:
    // The role a request needs, or empty when a valid session is enough.
    static std::string_view required_role(const HttpRequest&);
    Service(Config, ClusterKeys, NodeRuntime::StartupStageHook startup_stage_hook = {},
            MaintenanceStageHook maintenance_stage_hook = {},
            StartupStallHandler startup_stall_handler = {},
            ServiceInstruments instruments = {});
    ~Service();
    void start();
    void request_stop();
    void stop();
    void reload_config();
    bool ready() const noexcept {
        return services_ready_.load(std::memory_order_acquire);
    }
    FileSystem& filesystem() {
        wait_services_ready();
        return services_->filesystem();
    }
    NodeRuntime& node() {
        return node_;
    }
    NodeResources& resources() {
        return resources_;
    }
    MetadataManager& metadata_manager() {
        wait_services_ready();
        return services_->metadata();
    }
    DistributedStore& store() {
        wait_services_ready();
        return services_->store();
    }
    CatalogueManager& catalogue() {
        wait_services_ready();
        return services_->catalogue();
    }
    CatalogueHintQueue& catalogue_hints() {
        wait_services_ready();
        return services_->catalogue_hints();
    }
    HydrationManager& hydration() {
        wait_services_ready();
        return services_->hydration();
    }
    IngestManager& ingest() {
        wait_services_ready();
        return services_->ingest();
    }
    // Null when no torrent plugin is loaded here, or while a faulted one is
    // between restarts. Callers hold the returned pointer for the duration of
    // their use of it; see SubsystemRegistry.
    std::shared_ptr<TorrentService> torrents() {
        wait_services_ready();
        return registry_.torrent();
    }
    ClusterJobView& cluster_jobs() {
        return services_->cluster_jobs();
    }
    TorrentCoordinator& torrent_coordinator() {
        return services_->torrent_coordinator();
    }
    AcquisitionApi& acquisition_api() {
        wait_services_ready();
        return services_->acquisition_api();
    }
    uint64_t maintenance_wakeups() const noexcept {
        return maintenance_port_.wakeups.load(std::memory_order_acquire);
    }
    const char* maintenance_stage() const noexcept {
        return maintenance_port_.stage.load(std::memory_order_acquire);
    }
    std::string maintenance_sleep_diagnostic() const {
        const auto flags = maintenance_port_.last_flags.load(std::memory_order_acquire);
        return "wait_ms=" +
               std::to_string(maintenance_port_.last_wait_ms.load(std::memory_order_acquire)) +
               " gc_quiet_ms=" +
               std::to_string(maintenance_port_.last_gc_quiet_ms.load(std::memory_order_acquire)) +
               " busy=" + std::to_string(flags & 1) + " gc_due=" + std::to_string((flags >> 1) & 1);
    }
    ConvergenceDemandDiagnostics metadata_convergence_diagnostics() const noexcept {
        return maintenance_port_.metadata_convergence.diagnostics();
    }
    DistributedStore::RepairDiagnostics repair_diagnostics() {
        wait_services_ready();
        return services_->store().repair_diagnostics();
    }
    // Null when this node has no mount: not configured for one, or its FUSE
    // subsystem is faulted between restarts. See SubsystemRegistry.
    std::shared_ptr<FuseFrontend> fuse() {
        return registry_.fuse();
    }
    // Where subsystems publish what they provide. A test driving its own
    // FuseFrontend publishes here, as FuseSubsystem does.
    SubsystemRegistry& registry() noexcept {
        return registry_;
    }
    std::optional<BlockedNamespaceOperation> blocked_namespace_operation() const;
    bool skip_blocked_namespace_operation(uint64_t sequence);
    std::vector<ParkedPublication> parked_publications() const;
    bool retry_parked_publication(uint64_t inode);
    bool abandon_parked_publication(uint64_t inode);
};
} // namespace macha
