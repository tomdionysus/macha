// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "acquisition/ingest.hpp"
#include "subsystem/subsystem_registry.hpp"
#include "torrent/torrent.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace macha {

class NodeRuntime;

// Every node's torrent and ingest jobs, held by the answering node (0.64.0).
//
// Until 0.64.0 each job route surveyed every peer while the client waited:
// on 2026-09-27 a list from gbni-1 cost 0.1-0.5 s, and 1-5 s earlier the
// same day, all of it fi-1's round trip over a congested link -- and fi-1
// cannot even run torrents. A route now answers from this node's own live
// state plus what a background loop last heard from each peer, and says how
// old that is. Actions still go to the node that owns the job, found here
// rather than by asking everyone.
//
// Which peers run torrents is learned from the same replies: a node without
// the plugin answers the job query with "torrents not available".
class ClusterJobView {
  public:
    static constexpr auto default_refresh_interval = std::chrono::seconds(5);

    ClusterJobView(NodeRuntime&, IngestManager&, SubsystemRegistry&,
                   std::chrono::milliseconds refresh_interval = default_refresh_interval);
    ~ClusterJobView();
    ClusterJobView(const ClusterJobView&) = delete;
    ClusterJobView& operator=(const ClusterJobView&) = delete;

    void start();
    void stop();
    // Polls every active peer once, now, on the caller's thread.
    void refresh_now();
    std::chrono::milliseconds refresh_interval() const noexcept { return refresh_interval_; }
    NodeId local_node_id() const;

    // Where a list's entries came from, and how old they are. `reachable`
    // false: that node's jobs are shown from its last successful poll at
    // `as_of_unix_ms` (possibly stale); a node never reached has none and
    // `as_of_unix_ms` 0.
    struct Source {
        NodeId node_id;
        bool local{};
        bool reachable{};
        uint64_t as_of_unix_ms{};
    };

    struct TorrentListing {
        std::vector<ClusterTorrentJob> jobs;
        std::vector<Source> sources;
    };
    TorrentListing torrent_jobs() const;
    std::optional<ClusterTorrentJob> torrent_job(std::string_view id) const;
    TorrentActionResult torrent_action(std::string_view id, std::string_view action);

    struct TorrentNode {
        NodeId node_id;
        std::string host;
        bool local{};
        bool reachable{};
        uint64_t as_of_unix_ms{};
        TorrentService::Offer offer;
        std::optional<StagingStatus> staging;
    };
    // Only torrent-capable nodes.
    std::vector<TorrentNode> torrent_nodes() const;
    // torrent.remove_on_complete_after_ms: what an add that names no
    // remove_after_ms gets. Empty is off.
    std::optional<std::chrono::milliseconds> default_remove_after() const;
    // Live reload of torrent.remove_on_complete_after_ms.
    void reconfigure(const TorrentConfig&);

    struct IngestListing {
        std::vector<ClusterIngestJob> jobs;
        std::vector<Source> sources;
    };
    IngestListing ingest_jobs() const;
    std::optional<ClusterIngestJob> ingest_job(std::string_view id) const;
    IngestActionResult ingest_action(std::string_view id, std::string_view action);

  private:
    // What one peer last told us. `torrent_known` false: never answered the
    // torrent query; `torrent_capable` false: answered that it has no torrents.
    struct Peer {
        std::string host;
        bool reachable{};
        uint64_t as_of_unix_ms{};
        bool torrent_known{};
        bool torrent_capable{};
        std::vector<TorrentJob> torrents;
        TorrentService::Offer offer;
        std::optional<StagingStatus> staging;
        bool ingest_known{};
        std::vector<IngestJob> ingests;
    };

    void loop(std::stop_token);
    void poll(const NodeInfo&);
    std::optional<NodeInfo> active_peer(const NodeId&) const;

    NodeRuntime& node_;
    IngestManager& ingest_;
    SubsystemRegistry& registry_;
    const std::chrono::milliseconds refresh_interval_;
    // torrent.remove_on_complete_after_ms, -1 when off. Its own copy: the
    // node's config snapshot is not updated on reload for this section, and
    // is read here from HTTP threads.
    std::atomic<int64_t> default_remove_after_ms_{-1};

    mutable std::mutex mutex_;
    std::map<NodeId, Peer> peers_;
    // One poll at a time: the loop and refresh_now() never interleave.
    std::mutex poll_mutex_;
    std::condition_variable_any wake_;
    std::jthread worker_;
};

} // namespace macha
