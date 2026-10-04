// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "metadata/metadata_server.hpp"
#include "acquisition/ingest.hpp"
#include "api/acquisition_api.hpp"
#include "api/catalogue_api.hpp"
#include "api/manage_api.hpp"
#include "catalogue/catalogue.hpp"
#include "catalogue/catalogue_hints.hpp"
#include "catalogue/media_catalogue.hpp"
#include "catalogue/media_information.hpp"
#include "cluster/distributed_store.hpp"
#include "cluster/node_resources.hpp"
#include "filesystem/filesystem.hpp"
#include "filesystem/hydration.hpp"
#include "ledger/node_horizon_builder.hpp"
#include "ledger/retention_ledger.hpp"
#include "api/files_api.hpp"
#include "service/availability_service.hpp"
#include "metadata/metadata_manager.hpp"
#include "playback/playback.hpp"
#include "service/maintenance.hpp"
#include "service/maintenance_clock.hpp"
#include "subsystem/subsystem_registry.hpp"
#include "subsystem/subsystem_supervisor.hpp"
#include "torrent/torrent.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

// The node's services, built once local state has recovered: the composition
// root. Members are declared in dependency order (each takes references to
// members declared before it), so construction follows the graph and
// destruction reverses it. start() runs in that order; stop() reverses it,
// pinning the one ordering the graph leaves free (see stop()).
//
// The graph, provider -> dependants:
//   node, resources, routes, registry, port (outside, outlive this)
//   routes         <- ingest, torrent coordinator (their job messages, bound
//                     here once built, unbound before they are destroyed)
//   resources      -> store (activity, DATA, memory, events), filesystem (memory),
//                     playback (rate book, memory), subsystems (DATA, memory)
//   playback       -> filesystem, hydration
//   store          -> metadata, catalogue, filesystem, hydration, builder, maintenance
//   metadata       -> catalogue, filesystem, torrent coordinator, manage API, maintenance
//   catalogue      -> media info, scanner, hydration, catalogue API, manage API, playback,
//                     builder, maintenance; metadata's publication guard calls back
//                     into it (wired at construction, used only after)
//   filesystem     -> media info, scanner, hydration, ingest, catalogue API, manage API,
//                     playback, subsystems, builder
//   hints          -> scanner, ingest, catalogue API, manage API
//   media engine   -> media info, scanner, playback
//   media info     -> scanner, ingest, catalogue API, playback, maintenance
//   scanner        -> catalogue API, manage API, playback
//   hydration      -> subsystems
//   ingest         -> cluster jobs, acquisition API, subsystems
//   torrent search -> acquisition API
//   cluster jobs   -> torrent coordinator, acquisition API
//   torrent coord  -> acquisition API
//   subsystems     (plugins: torrent, FUSE) need ingest, filesystem, hydration
//   ledger, builder -> maintenance
namespace macha {

using LifecycleHook = std::function<void(std::string_view event)>;

// What the root is given besides its collaborators: the instruments a test
// injects (production passes none) and when the Service was constructed.
struct NodeServicesInstruments {
    std::shared_ptr<MaintenanceClock> clock;
    MaintenanceTraceHook trace;
    std::function<void(std::string_view)> maintenance_stage_hook;
    LifecycleHook lifecycle;
    Clock::time_point constructed;
};

class NodeServices {
  public:
    NodeServices(NodeRuntime&, NodeResources&, LocalState&, MetadataServer&, MessageRoutes&,
                 SubsystemRegistry&, MaintenancePort&,
                 NodeServicesInstruments);
    ~NodeServices();
    NodeServices(const NodeServices&) = delete;
    NodeServices& operator=(const NodeServices&) = delete;

    // Starts every service, providers before dependants.
    void start();
    // Asks every service to stop and returns: filesystem I/O is cancelled,
    // then each service is asked in reverse order.
    void request_stop();
    // Stops every service (requesting first), dependants before providers.
    void stop();
    // The live-reloadable limits of the services that have any.
    void reconfigure(const Config&);
    // What an observation window samples of the services: each class's idle
    // time, repair's figures (cumulative since start, as Status reports them;
    // a window's rate is the difference between consecutive windows) and,
    // while a mount is published, the frontend's.
    std::map<std::string, uint64_t> observation_gauges();

    PlaybackTracker& playback() { return playback_; }
    DistributedStore& store() { return store_; }
    MetadataManager& metadata() { return metadata_; }
    CatalogueManager& catalogue() { return catalogue_; }
    AvailabilityService& availability() { return availability_; }
    FilesApi& files_api() { return files_api_; }
    FileSystem& filesystem() { return filesystem_; }
    CatalogueHintQueue& catalogue_hints() { return catalogue_hints_; }
    MediaInformationService& media_information() { return media_information_; }
    HydrationManager& hydration() { return hydration_; }
    IngestManager& ingest() { return ingest_; }
    ClusterJobView& cluster_jobs() { return cluster_jobs_; }
    TorrentCoordinator& torrent_coordinator() { return torrent_coordinator_; }
    AcquisitionApi& acquisition_api() { return acquisition_api_; }
    CatalogueApi& catalogue_api() { return catalogue_api_; }
    ManageApi& manage_api() { return manage_api_; }
    PlaybackManager& streaming() { return streaming_; }
    SubsystemSupervisor& subsystems() { return subsystems_; }

  private:
    void note(std::string_view event) const {
        if (instruments_.lifecycle)
            instruments_.lifecycle(event);
    }
    // The claims barrier run before every metadata commit is published.
    void retain_metadata_publication(const MetadataPublicationContext&);

    NodeRuntime& node_;
    NodeResources& resources_;
    LocalState& local_;
    MetadataServer& metadata_server_;
    MessageRoutes& routes_;
    SubsystemRegistry& registry_;
    MaintenancePort& port_;
    NodeServicesInstruments instruments_;
    bool started_{};
    bool stopped_{};

    PlaybackTracker playback_;
    DistributedStore store_;
    MetadataManager metadata_;
    RetentionLedger ledger_;
    AvailabilityService availability_;
    CatalogueManager catalogue_;
    FileSystem filesystem_;
    CatalogueHintQueue catalogue_hints_;
    std::shared_ptr<MediaEngine> media_engine_;
    MediaInformationService media_information_;
    CatalogueScanner scanner_;
    HydrationManager hydration_;
    IngestManager ingest_;
    TorrentSearchManager torrent_search_;
    ClusterJobView cluster_jobs_;
    TorrentCoordinator torrent_coordinator_;
    AcquisitionApi acquisition_api_;
    CatalogueApi catalogue_api_;
    ManageApi manage_api_;
    PlaybackManager streaming_;
    SubsystemSupervisor subsystems_;
    NodeHorizonBuilder horizon_builder_;
    FilesApi files_api_;
    Maintenance maintenance_;
};

} // namespace macha
