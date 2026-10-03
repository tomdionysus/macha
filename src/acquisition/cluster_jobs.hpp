// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "acquisition/ingest.hpp"
#include "contract/thread_safety.hpp"
#include "subsystem/subsystem_registry.hpp"
#include "torrent/torrent.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace macha {

class NodeRuntime;

// Every node's torrent and ingest jobs, as seen from this node: local live
// state plus each peer's last answer to a background poll, with its age, so
// job routes never wait on peers. Actions go to the job's owning node.
// A peer without the torrent plugin answers "torrents not available".
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
    // Polls every active peer once on the caller's thread.
    void refresh_now();
    std::chrono::milliseconds refresh_interval() const noexcept { return refresh_interval_; }
    NodeId local_node_id() const;

    // Where a list's entries came from. Unreachable: jobs from the last
    // successful poll at `as_of_unix_ms`; never reached: none, and 0.
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
    // torrent.remove_on_complete_after_ms, for an add without remove_after_ms.
    // Empty is off.
    std::optional<std::chrono::milliseconds> default_remove_after() const;
    void reconfigure(const TorrentConfig&);

    struct IngestListing {
        std::vector<ClusterIngestJob> jobs;
        std::vector<Source> sources;
    };
    IngestListing ingest_jobs() const;
    std::optional<ClusterIngestJob> ingest_job(std::string_view id) const;
    IngestActionResult ingest_action(std::string_view id, std::string_view action);

  private:
    // A peer's last answers. `torrent_known` false: never answered the torrent
    // query; `torrent_capable` false: answered that it has no torrents.
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
    // -1 when off. Own copy: the node's config snapshot is not reloaded for
    // this section, and HTTP threads read it.
    std::atomic<int64_t> default_remove_after_ms_{-1};

    mutable Mutex mutex_;
    std::map<NodeId, Peer> peers_ MACHA_GUARDED_BY(mutex_);
    // Serialises polls between the loop and refresh_now(); guards nothing.
    // Held across the peer RPCs.
    IoMutex poll_mutex_;
    std::condition_variable_any wake_;
    std::jthread worker_;
};

} // namespace macha
