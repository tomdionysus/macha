// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "acquisition_api.hpp"
#include "catalogue.hpp"
#include "catalogue_api.hpp"
#include "catalogue_hints.hpp"
#include "convergence_demand.hpp"
#include "filesystem.hpp"
#include "hydration.hpp"
#include "ingest.hpp"
#include "manage_api.hpp"
#include "media_catalogue.hpp"
#include "media_information.hpp"
#include "playback.hpp"
#include "status_api.hpp"
#include "torrent.hpp"
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

class FuseFrontend;

std::chrono::milliseconds maintenance_background_interval(const MaintenanceConfig&);

class Service {
  public:
    using MaintenanceStageHook = std::function<void(std::string_view)>;
    // Test-only interception point for a detected startup stall (see
    // wait_services_ready()). Production leaves this empty and terminates the
    // process instead; a test can observe the stall without killing itself.
    using StartupStallHandler = std::function<void(std::string_view diagnostic)>;

  private:
    NodeRuntime node_;
    ClusterStatusService cluster_status_;
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
    std::unique_ptr<TorrentManager> torrents_;
    std::unique_ptr<TorrentSearchManager> torrent_search_;
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

    std::jthread maintenance_;
    std::mutex maintenance_wait_mutex_;
    std::condition_variable_any maintenance_wait_cv_;
    std::atomic_uint64_t maintenance_event_{1};
    std::atomic_uint64_t maintenance_wakeups_{};
    ConvergenceDemand metadata_convergence_;
    MaintenanceStageHook maintenance_stage_hook_;
    uint64_t maintenance_inventory_generation_{};
    std::shared_ptr<const std::vector<ObjectId>> maintenance_live_;
    std::shared_ptr<const std::vector<ObjectId>> maintenance_universal_;
    std::shared_ptr<const std::vector<ObjectId>> maintenance_control_live_;
    Hash256 retention_release_floor_hash_{};
    std::shared_ptr<const std::vector<ObjectId>> retention_release_data_live_;
    std::shared_ptr<const std::vector<ObjectId>> retention_release_control_live_;
    RetentionClock retention_release_clock_;
    bool retention_release_complete_{};
    bool maintenance_catalogue_complete_{true};
    std::vector<GarbageRef> maintenance_garbage_;
    std::vector<GarbageRef> maintenance_stale_garbage_;
    std::optional<ObjectId> retention_data_repair_cursor_;
    std::optional<ObjectId> retention_control_repair_cursor_;

    void initialise_services(std::stop_token);
    void wait_services_ready();
    std::string describe_readiness_stall() const;
    HttpResponse handle_http(const HttpRequest&);
    bool capability_request(const HttpRequest&);
    void loop(std::stop_token);
    void signal_maintenance(ServiceEvent);
    std::vector<GarbageRef> collect_garbage(const std::vector<GarbageRef>&);
    void maintain_garbage_metadata(const std::vector<GarbageRef>& erase,
                                   const std::vector<GarbageRef>& stamp);
    void retain_metadata_publication(const MetadataPublicationContext&);

  public:
    Service(Config, ClusterKeys, NodeRuntime::StartupStageHook startup_stage_hook = {},
            MaintenanceStageHook maintenance_stage_hook = {},
            StartupStallHandler startup_stall_handler = {});
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
    TorrentManager& torrents() {
        wait_services_ready();
        return *torrents_;
    }
    AcquisitionApi& acquisition_api() {
        wait_services_ready();
        return *acquisition_api_;
    }
    uint64_t maintenance_wakeups() const noexcept {
        return maintenance_wakeups_.load(std::memory_order_acquire);
    }
    ConvergenceDemandDiagnostics metadata_convergence_diagnostics() const noexcept {
        return metadata_convergence_.diagnostics();
    }
    void attach_fuse_frontend(std::weak_ptr<FuseFrontend>);
};
} // namespace macha
