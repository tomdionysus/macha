// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "acquisition/cluster_jobs.hpp"
#include "torrent/torrent_coordinator.hpp"
#include "http/http.hpp"
#include "acquisition/ingest.hpp"
#include "subsystem/subsystem_registry.hpp"
#include "torrent/torrent.hpp"

namespace macha {

// HTTP surface for host-filesystem ingest and acquisition producers.  This
// deliberately exposes Macha-owned typed state only; torrent provider HTML,
// download links and credentials never cross the API boundary.
//
// The download engine is reached through the registry rather than held
// directly: it is provided by the libmacha-torrent plugin, may be absent on
// this node, and is replaced in place if it faults and restarts.
//
// Job lists and lookups answer from `jobs` -- this node's live state plus what
// it last heard from each peer -- and never survey peers while the client
// waits (0.64.0). Every route works on every node, with or without the
// torrent plugin.
class AcquisitionApi {
    IngestManager& ingest_;
    SubsystemRegistry& subsystems_;
    TorrentSearchManager& search_;
    ClusterJobView& jobs_;
    TorrentCoordinator& torrents_;

    std::map<NodeId, uint64_t> live_ages() const;
    Json request_json(const TorrentRequest&, const std::map<NodeId, uint64_t>& live_ages) const;

  public:
    AcquisitionApi(IngestManager& ingest, SubsystemRegistry& subsystems,
                   TorrentSearchManager& search, ClusterJobView& jobs, TorrentCoordinator& torrents)
        : ingest_(ingest), subsystems_(subsystems), search_(search), jobs_(jobs), torrents_(torrents) {}

    HttpResponse handle(const HttpRequest&);
};

} // namespace macha
