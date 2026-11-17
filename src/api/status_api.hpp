// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/paging.hpp"
#include "contract/thread_safety.hpp"

#include "auth/accounts.hpp"
#include "dagda/node.h"
#include "fuse/fuse_frontend.hpp"
#include "http/http.hpp"
#include "metadata/metadata_manager.hpp"
#include "service/convergence_demand.hpp"
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
    // The metadata replica's figures, once local state has recovered.
    LocalState* local{};
    MetadataView* metadata{};
    const SubsystemSupervisor* subsystems{};
    // The store node's figures (its pool, repair, prompt replication and
    // admission), once its store is open.
    dagda::Node* store_node{};
};

// Reads the node it is built with; everything else arrives per request.
class ClusterStatusService {
    NodeRuntime& node_;
    Accounts& accounts_;
    const dagda::Context::Activity& activity_;
    const RetainedMemoryLedger& retained_memory_;
    std::jthread persistence_;
    // Guards nothing; the observation thread waits on it.
    Mutex wait_mutex_;
    std::condition_variable_any wait_cv_;

    void persistence_loop(const std::stop_token&);
    void persist_local_status();
    // `nodes_page`: the nodes as a paged list (GET /api/v1/status/nodes).
    HttpResponse status_response(const StatusSources&, const std::optional<NodeId>& only = {},
                                 const std::optional<PageQuery>& nodes_page = {});
    // The expensive half, behind its own route: its counters sit behind most
    // subsystems' locks, some held by the busy paths an operator is investigating.
    // Ordinary polling must not pay that.
    HttpResponse diagnostics_response(const StatusSources&);
    HttpResponse connectivity_check(const std::optional<NodeId>& only);

  public:
    // `activity`: the store node's clock, which the store and admission read.
    ClusterStatusService(NodeRuntime&, Accounts&, const dagda::Context::Activity& activity,
                         const RetainedMemoryLedger&);
    ~ClusterStatusService();
    void start();
    void request_stop();
    void stop();
    HttpResponse handle(const HttpRequest&, const StatusSources& = {});
};

} // namespace macha
