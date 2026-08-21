// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "http.hpp"
#include "ingest.hpp"
#include "torrent.hpp"

namespace macha {

// HTTP surface for host-filesystem ingest and acquisition producers.  This
// deliberately exposes Macha-owned typed state only; torrent provider HTML,
// download links and credentials never cross the API boundary.
class AcquisitionApi {
    IngestManager& ingest_;
    TorrentManager& torrents_;
    TorrentSearchManager& search_;

  public:
    AcquisitionApi(IngestManager& ingest, TorrentManager& torrents,
                   TorrentSearchManager& search)
        : ingest_(ingest), torrents_(torrents), search_(search) {}

    HttpResponse handle(const HttpRequest&);
};

} // namespace macha
