// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "convergence_demand.hpp"
#include "fuse_frontend.hpp"
#include "http.hpp"
#include "metadata_manager.hpp"
#include "subsystem_supervisor.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace macha {

class ClusterStatusService {
    NodeRuntime& node_;
    std::atomic<MetadataManager*> metadata_{nullptr};
    std::jthread persistence_;
    std::mutex wait_mutex_;
    std::condition_variable_any wait_cv_;
    mutable std::mutex operational_diagnostics_mutex_;
    std::function<std::optional<FuseFrontendDiagnostics>()> fuse_diagnostics_;
    std::function<ConvergenceDemandDiagnostics()> convergence_diagnostics_;
    std::function<std::vector<SubsystemStatus>()> subsystem_diagnostics_;

    void persistence_loop(std::stop_token);
    void persist_local_status();
    HttpResponse status_response(const std::optional<NodeId>& only = {});
    HttpResponse connectivity_check(const std::optional<NodeId>& only);

  public:
    explicit ClusterStatusService(NodeRuntime&);
    void attach_metadata(MetadataManager& metadata) {
        metadata_.store(&metadata, std::memory_order_release);
    }
    void detach_metadata() {
        metadata_.store(nullptr, std::memory_order_release);
    }
    void attach_fuse_diagnostics(std::function<std::optional<FuseFrontendDiagnostics>()> provider);
    void detach_fuse_diagnostics();
    void attach_convergence_diagnostics(std::function<ConvergenceDemandDiagnostics()> provider);
    // Cheap, always-present per-subsystem health (see SubsystemSupervisor):
    // unlike the expensive diagnostics block below, this belongs in the
    // lightweight part of the response -- it's exactly the kind of thing an
    // operator needs promptly, and costs nothing to compute.
    void attach_subsystem_diagnostics(std::function<std::vector<SubsystemStatus>()> provider);
    void detach_subsystem_diagnostics();
    ~ClusterStatusService();
    void start();
    void request_stop();
    void stop();
    HttpResponse handle(const HttpRequest&);
};

} // namespace macha
