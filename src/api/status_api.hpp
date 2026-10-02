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

struct MaintenancePort;
class LocalState;
class SubsystemRegistry;

// What a status request reads beyond the node: handed in per request by the
// root, which keeps each alive for the call. Absent (null) when this node has
// none: no API server, or services not yet built.
struct StatusSources {
    const MaintenancePort* maintenance{};
    const SubsystemRegistry* registry{};
    const HttpServer* http{};
    LocalState* local{};
    MetadataView* metadata{};
    const SubsystemSupervisor* subsystems{};
    const DistributedStore* store{};
};

// Reads the node it is built with; everything else arrives per request.
class ClusterStatusService {
    NodeRuntime& node_;
    const ActivityClocks& activity_;
    const DataResourceArbiter& data_resources_;
    const RetainedMemoryLedger& retained_memory_;
    std::jthread persistence_;
    std::mutex wait_mutex_;
    std::condition_variable_any wait_cv_;

    void persistence_loop(std::stop_token);
    void persist_local_status();
    HttpResponse status_response(const StatusSources&, const std::optional<NodeId>& only = {});
    // The expensive half, behind its own route: its counters sit behind most
    // subsystems' locks, some held by the busy paths an operator is investigating.
    // Ordinary polling must not pay that.
    HttpResponse diagnostics_response(const StatusSources&);
    HttpResponse connectivity_check(const std::optional<NodeId>& only);

  public:
    ClusterStatusService(NodeRuntime&, const ActivityClocks&, const DataResourceArbiter&,
                         const RetainedMemoryLedger&);
    ~ClusterStatusService();
    void start();
    void request_stop();
    void stop();
    HttpResponse handle(const HttpRequest&, const StatusSources& = {});
};

} // namespace macha
