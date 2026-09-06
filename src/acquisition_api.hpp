// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "http.hpp"
#include "ingest.hpp"
#include "subsystem_registry.hpp"
#include "torrent.hpp"

namespace macha {

// HTTP surface for host-filesystem ingest and acquisition producers.  This
// deliberately exposes Macha-owned typed state only; torrent provider HTML,
// download links and credentials never cross the API boundary.
//
// The download engine is reached through the registry rather than held
// directly: it is provided by the libmacha-torrent plugin, may be absent on
// this node, and is replaced in place if it faults and restarts.
class AcquisitionApi {
    IngestManager& ingest_;
    SubsystemRegistry& subsystems_;
    TorrentSearchManager& search_;

  public:
    AcquisitionApi(IngestManager& ingest, SubsystemRegistry& subsystems,
                   TorrentSearchManager& search)
        : ingest_(ingest), subsystems_(subsystems), search_(search) {}

    HttpResponse handle(const HttpRequest&);
};

} // namespace macha
