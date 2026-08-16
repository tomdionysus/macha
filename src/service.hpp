// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "catalogue.hpp"
#include "catalogue_api.hpp"
#include "filesystem.hpp"
#include "hydration.hpp"
#include "media_catalogue.hpp"
#include "playback.hpp"
#include <ctime>
#include <map>
#include <memory>
#include <vector>

namespace macha {

std::chrono::milliseconds maintenance_background_interval(const MaintenanceConfig&);

class Service {
    NodeRuntime node_;
    DistributedStore store_;
    MetadataManager metadata_;
    CatalogueManager catalogue_;
    PlaybackTracker playback_;
    FileSystem fs_;
    CatalogueScanner scanner_;
    HydrationManager hydration_;
    CatalogueApi catalogue_api_;
    PlaybackManager streaming_;
    std::unique_ptr<HttpServer> catalogue_http_;
    std::jthread maintenance_;
    std::map<ObjectId, Clock::time_point> garbage_seen_;
    uint64_t maintenance_inventory_generation_{};
    std::shared_ptr<const std::vector<ObjectId>> maintenance_live_;
    std::shared_ptr<const std::vector<ObjectId>> maintenance_universal_;
    std::vector<ObjectId> maintenance_garbage_;
    void loop(std::stop_token);
    void collect_garbage(const std::vector<ObjectId>&);

  public:
    Service(Config, ClusterKeys);
    ~Service();
    void start();
    void stop();
    void reload_config();
    FileSystem& filesystem() {
        return fs_;
    }
    NodeRuntime& node() {
        return node_;
    }
    CatalogueManager& catalogue() {
        return catalogue_;
    }
    HydrationManager& hydration() {
        return hydration_;
    }
};
} // namespace macha
