// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "filesystem.hpp"
#include <ctime>
#include <map>

namespace macha {
class Service {
    NodeRuntime node_;
    DistributedStore store_;
    MetadataManager metadata_;
    FileSystem fs_;
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
};
} // namespace macha
