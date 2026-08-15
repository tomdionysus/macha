// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "catalogue.hpp"
#include "catalogue_api.hpp"
#include "filesystem.hpp"
#include "hydration.hpp"
#include <ctime>
#include <map>
#include <memory>

namespace macha {
class Service {
    NodeRuntime node_;
    DistributedStore store_;
    MetadataManager metadata_;
    CatalogueManager catalogue_;
    PlaybackTracker playback_;
    FileSystem fs_;
    HydrationManager hydration_;
    CatalogueApi catalogue_api_;
    std::unique_ptr<HttpServer> catalogue_http_;
    std::jthread maintenance_;
    std::map<ObjectId, Clock::time_point> garbage_seen_;
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
