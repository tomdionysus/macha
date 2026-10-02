// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "service/convergence_demand.hpp"
#include "cluster/distributed_store.hpp"
#include "fuse/fuse_frontend.hpp"
#include "http/http.hpp"
#include "metadata/metadata_manager.hpp"
#include "subsystem/subsystem_supervisor.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace macha {

class ClusterStatusService {
    NodeRuntime& node_;
    std::atomic<MetadataView*> metadata_{nullptr};
    std::jthread persistence_;
    std::mutex wait_mutex_;
    std::condition_variable_any wait_cv_;
    mutable std::mutex operational_diagnostics_mutex_;
    std::function<std::optional<FuseFrontendDiagnostics>()> fuse_diagnostics_;
    std::function<ConvergenceDemandDiagnostics()> convergence_diagnostics_;
    std::function<std::vector<SubsystemStatus>()> subsystem_diagnostics_;
    std::function<DistributedStore::RepairDiagnostics()> repair_diagnostics_;
    std::function<std::optional<HttpServerDiagnostics>()> http_diagnostics_;

    void persistence_loop(std::stop_token);
    void persist_local_status();
    HttpResponse status_response(const std::optional<NodeId>& only = {});
    // The expensive half, behind its own route: its counters sit behind most
    // subsystems' locks, some held by the busy paths an operator is investigating.
    // Ordinary polling must not pay that.
    HttpResponse diagnostics_response();
    HttpResponse connectivity_check(const std::optional<NodeId>& only);

  public:
    explicit ClusterStatusService(NodeRuntime&);
    void attach_metadata(MetadataView& metadata) {
        metadata_.store(&metadata, std::memory_order_release);
    }
    void detach_metadata() {
        metadata_.store(nullptr, std::memory_order_release);
    }
    void attach_fuse_diagnostics(std::function<std::optional<FuseFrontendDiagnostics>()> provider);
    void detach_fuse_diagnostics();
    void attach_convergence_diagnostics(std::function<ConvergenceDemandDiagnostics()> provider);
    // Replica repair's view of what it could not obtain. Arrives like the FUSE and
    // convergence diagnostics, since this service holds a NodeRuntime, not the store.
    void attach_repair_diagnostics(std::function<DistributedStore::RepairDiagnostics()> provider);
    void detach_repair_diagnostics();
    // Cheap per-subsystem health (see SubsystemSupervisor), in the lightweight
    // part of the response.
    void attach_subsystem_diagnostics(std::function<std::vector<SubsystemStatus>()> provider);
    void detach_subsystem_diagnostics();
    // The HTTP server's own counters: reactor stalls, open and idle
    // connections, lane queues. Absent when the API is disabled.
    void attach_http_diagnostics(std::function<std::optional<HttpServerDiagnostics>()> provider);
    ~ClusterStatusService();
    void start();
    void request_stop();
    void stop();
    HttpResponse handle(const HttpRequest&);
};

} // namespace macha
