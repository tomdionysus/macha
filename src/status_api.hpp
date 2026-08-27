// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "http.hpp"
#include "metadata_manager.hpp"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace macha {

class ClusterStatusService {
    NodeRuntime& node_;
    MetadataManager& metadata_;
    std::jthread persistence_;
    std::mutex wait_mutex_;
    std::condition_variable_any wait_cv_;

    void persistence_loop(std::stop_token);
    void persist_local_status();
    HttpResponse status_response(const std::optional<NodeId>& only = {});
    HttpResponse connectivity_check(const std::optional<NodeId>& only);

  public:
    ClusterStatusService(NodeRuntime&, MetadataManager&);
    ~ClusterStatusService();
    void start();
    void request_stop();
    void stop();
    HttpResponse handle(const HttpRequest&);
};

} // namespace macha
