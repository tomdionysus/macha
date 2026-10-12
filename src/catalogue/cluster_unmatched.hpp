// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/catalogue_hints.hpp"
#include "cluster/telemetry.hpp"
#include "contract/thread_safety.hpp"
#include "types.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>

namespace macha {

class NodeRuntime;

// The files no scanner could match, as seen from this node: its own hints plus
// each peer's last answer to a background poll, so the unmatched list never
// waits on a peer. A scan's hints stay on the node that ran it; acting on one
// needs only its path and media id, which any node can use.
class ClusterUnmatchedView {
  public:
    static constexpr auto default_refresh_interval = std::chrono::seconds(30);
    // The first release that answers get_unmatched_hints.
    static constexpr ReleaseVersion first_version{0, 90, 87};
    // A reply names at most this many hints.
    static constexpr size_t max_reply_hints = 8192;

    ClusterUnmatchedView(NodeRuntime&, CatalogueHintQueue&,
                         std::chrono::milliseconds refresh_interval = default_refresh_interval);
    ~ClusterUnmatchedView();
    ClusterUnmatchedView(const ClusterUnmatchedView&) = delete;
    ClusterUnmatchedView& operator=(const ClusterUnmatchedView&) = delete;

    void start();
    void stop();
    // Polls every active peer once on the caller's thread.
    void refresh_now();
    // Every peer's unmatched hints from its last answer; a hint two peers
    // hold appears once. A peer that left keeps its last answer.
    std::vector<CatalogueHint> peer_hints() const;
    std::optional<CatalogueHint> peer_hint(std::string_view id) const;

    // The get_unmatched_hints handler: this node's own hints.
    Bytes handle_query(std::span<const uint8_t> payload) const;

  private:
    struct Peer {
        uint64_t instance{};
        uint64_t revision{};
        std::vector<CatalogueHint> hints;
    };

    void loop(std::stop_token);
    void poll(const NodeInfo&);

    NodeRuntime& node_;
    CatalogueHintQueue& hints_;
    const std::chrono::milliseconds refresh_interval_;
    // Tells this process's hint revisions from a restarted one's.
    const uint64_t instance_;

    mutable Mutex mutex_;
    std::map<NodeId, Peer> peers_ MACHA_GUARDED_BY(mutex_);
    // Serialises polls between the loop and refresh_now(); guards nothing.
    // Held across the peer RPCs.
    IoMutex poll_mutex_;
    std::condition_variable_any wake_;
    std::jthread worker_;
};

} // namespace macha
