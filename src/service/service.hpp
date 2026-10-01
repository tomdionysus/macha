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
#include "component/composition_root.hpp"
#include "service/maintenance.hpp"
#include "service/maintenance_clock.hpp"
#include "ledger/node_horizon_builder.hpp"
#include "storage/retention_ledger.hpp"
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

// Each step of the service's start and stop, in order ("start ingest",
// "request_stop streaming", "subsystem fuse"), for the lifecycle recorder.
using LifecycleHook = std::function<void(std::string_view event)>;

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
    NodeRuntime node_;
    ClusterStatusService cluster_status_;
    // Torrent runs as a plugin (Phase 1 of
    // TODO/archive/2026-09-05-subsystem-plugin-isolation-plan.md) and FUSE as a
    // supervised builtin (Stage A of
    // TODO/archive/2026-09-14-fuse-supervised-subsystem-plan.md). `registry_` is
    // where each publishes what it provides, and it outlives `subsystems_`
    // deliberately: declared first, destroyed last, so a subsystem being torn
    // down can still withdraw itself.
    SubsystemRegistry registry_;
    SubsystemSupervisor subsystems_;
    SessionApi session_api_;
    UsersApi users_api_;
    WebApi web_;
    std::unique_ptr<HttpServer> catalogue_http_;

    std::unique_ptr<DistributedStore> store_;
    std::unique_ptr<MetadataManager> metadata_;
    std::unique_ptr<CatalogueManager> catalogue_;
    PlaybackTracker playback_;
    std::unique_ptr<FileSystem> fs_;
    std::unique_ptr<CatalogueHintQueue> catalogue_hints_;
    std::unique_ptr<MediaInformationService> media_information_;
    std::unique_ptr<CatalogueScanner> scanner_;
    std::unique_ptr<HydrationManager> hydration_;
    std::unique_ptr<IngestManager> ingest_;
    std::unique_ptr<TorrentSearchManager> torrent_search_;
    std::unique_ptr<ClusterJobView> cluster_jobs_;
    std::unique_ptr<TorrentCoordinator> torrent_coordinator_;
    std::unique_ptr<AcquisitionApi> acquisition_api_;
    std::unique_ptr<CatalogueApi> catalogue_api_;
    std::unique_ptr<ManageApi> manage_api_;
    std::unique_ptr<PlaybackManager> streaming_;

    std::jthread startup_;
    std::atomic_bool services_ready_{};
    std::atomic_bool startup_failed_{};
    mutable std::mutex startup_mutex_;
    std::condition_variable startup_cv_;
    std::string startup_error_;
    StartupStallHandler startup_stall_handler_;

    // Service rings the maintenance pass through its port, and reads its
    // diagnostics there, before the pass exists and after it has gone.
    MaintenancePort maintenance_port_;
    MaintenanceStageHook maintenance_stage_hook_;
    // Built once the node's stores exist; the pass's ObjectLedger.
    std::unique_ptr<RetentionLedger> ledger_;
    std::unique_ptr<NodeHorizonBuilder> horizon_builder_;
    // Owns the components moved out of Service so far (maintenance). After
    // every service it uses, so it stops and is destroyed before them.
    CompositionRoot root_{[this](std::string_view event) { note_lifecycle(event); }};
    // Observation for the object ledger experiment's T0: written to a local
    // file under the state path, never to Status or any API response.
    std::unique_ptr<ObservationRecorder> observation_recorder_;
    std::atomic_bool observation_stopping_{};
    std::map<std::string, uint64_t> observation_gauges();

    void initialise_services(std::stop_token);
    void wait_services_ready();
    std::string describe_readiness_stall() const;
    HttpResponse handle_http(const HttpRequest&);
    // Unauthenticated liveness: whether this node is serving, and nothing more.
    HttpResponse health_response() const;
    bool capability_request(const HttpRequest&);
    void signal_maintenance(ServiceEvent);
    void retain_metadata_publication(const MetadataPublicationContext&);

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
        return *fs_;
    }
    NodeRuntime& node() {
        return node_;
    }
    MetadataManager& metadata_manager() {
        wait_services_ready();
        return *metadata_;
    }
    CatalogueManager& catalogue() {
        wait_services_ready();
        return *catalogue_;
    }
    CatalogueHintQueue& catalogue_hints() {
        wait_services_ready();
        return *catalogue_hints_;
    }
    HydrationManager& hydration() {
        wait_services_ready();
        return *hydration_;
    }
    IngestManager& ingest() {
        wait_services_ready();
        return *ingest_;
    }
    // Null when no torrent plugin is loaded here, or while a faulted one is
    // between restarts. Callers hold the returned pointer for the duration of
    // their use of it; see SubsystemRegistry.
    std::shared_ptr<TorrentService> torrents() {
        wait_services_ready();
        return registry_.torrent();
    }
    ClusterJobView& cluster_jobs() {
        return *cluster_jobs_;
    }
    TorrentCoordinator& torrent_coordinator() {
        return *torrent_coordinator_;
    }
    AcquisitionApi& acquisition_api() {
        wait_services_ready();
        return *acquisition_api_;
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
        return store_->repair_diagnostics();
    }
    // Null when this node has no mount: not configured for one, or its FUSE
    // subsystem is faulted between restarts. See SubsystemRegistry.
    std::shared_ptr<FuseFrontend> fuse() {
        return registry_.fuse();
    }
    // Where subsystems publish what they provide. Production writes to it
    // from a Subsystem; a test that drives a FuseFrontend it constructed
    // itself publishes here to make this Service see it, which is the same
    // thing FuseSubsystem does.
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
