// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "catalogue.hpp"
#include "catalogue_hints.hpp"
#include "catalogue_api.hpp"
#include "manage_api.hpp"
#include "acquisition_api.hpp"
#include "status_api.hpp"
#include "ingest.hpp"
#include "torrent.hpp"
#include "filesystem.hpp"
#include "hydration.hpp"
#include "media_catalogue.hpp"
#include "playback.hpp"
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <memory>
#include <optional>
#include <vector>

namespace macha {

std::chrono::milliseconds maintenance_background_interval(const MaintenanceConfig&);

class Service {
    NodeRuntime node_;
    DistributedStore store_;
    MetadataManager metadata_;
    ClusterStatusService cluster_status_;
    CatalogueManager catalogue_;
    PlaybackTracker playback_;
    FileSystem fs_;
    CatalogueHintQueue catalogue_hints_;
    CatalogueScanner scanner_;
    HydrationManager hydration_;
    IngestManager ingest_;
    TorrentManager torrents_;
    TorrentSearchManager torrent_search_;
    AcquisitionApi acquisition_api_;
    CatalogueApi catalogue_api_;
    ManageApi manage_api_;
    PlaybackManager streaming_;
    std::unique_ptr<HttpServer> catalogue_http_;
    std::jthread maintenance_;
    std::mutex maintenance_wait_mutex_;
    std::condition_variable_any maintenance_wait_cv_;
    uint64_t maintenance_inventory_generation_{};
    std::shared_ptr<const std::vector<ObjectId>> maintenance_live_;
    std::shared_ptr<const std::vector<ObjectId>> maintenance_universal_;
    std::shared_ptr<const std::vector<ObjectId>> maintenance_control_live_;
    // Live sets reconstructed from the sole accepted local metadata head.
    // Retention claims may be released against these sets even if the cluster
    // partitions again later; newer deletions remain claimed until a newer
    // stability horizon is established.
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
    void loop(std::stop_token);
    std::vector<GarbageRef> collect_garbage(const std::vector<GarbageRef>&);
    void maintain_garbage_metadata(const std::vector<GarbageRef>& erase,
                                   const std::vector<GarbageRef>& stamp);
    void retain_metadata_publication(const MetadataPublicationContext&);

  public:
    Service(Config, ClusterKeys);
    ~Service();
    void start();
    void request_stop();
    void stop();
    void reload_config();
    FileSystem& filesystem() {
        return fs_;
    }
    NodeRuntime& node() {
        return node_;
    }
    MetadataManager& metadata_manager() {
        return metadata_;
    }
    CatalogueManager& catalogue() {
        return catalogue_;
    }
    CatalogueHintQueue& catalogue_hints() {
        return catalogue_hints_;
    }
    HydrationManager& hydration() {
        return hydration_;
    }
};
} // namespace macha
