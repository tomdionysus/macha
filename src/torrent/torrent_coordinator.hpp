// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "acquisition/cluster_jobs.hpp"
#include "contract/thread_safety.hpp"
#include "metadata/metadata_manager.hpp"
#include "torrent/torrent_request.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stop_token>
#include <thread>

namespace macha {

class NodeRuntime;

// Torrents belong to the cluster: an add is a request in metadata
// (torrent_request.hpp) any node can list and act on, and any torrent-capable
// node may claim. The owner drives its download from the request and writes
// progress back. One per node, plugin or not; a node without it never claims.
class TorrentCoordinator {
  public:
    // The torrent_intent message's handler.
    Bytes handle_intent(std::span<const uint8_t> payload);
    // A claim outlives a node's absence from membership for this long, so a
    // restart or a brief wifi drop does not move a download.
    static constexpr auto default_claim_lease = std::chrono::minutes(10);
    // How long a non-preferred node waits per rank before claiming.
    static constexpr auto claim_rank_step = std::chrono::seconds(30);
    // Removed requests stay as tombstones this long before they are erased.
    static constexpr auto tombstone_grace = std::chrono::hours(24 * 7);
    static constexpr auto pass_interval = std::chrono::seconds(2);

    TorrentCoordinator(NodeRuntime&, MetadataView&, SubsystemRegistry&, ClusterJobView&,
                       const std::filesystem::path& state_path,
                       std::chrono::milliseconds claim_lease = default_claim_lease);
    ~TorrentCoordinator();
    TorrentCoordinator(const TorrentCoordinator&) = delete;
    TorrentCoordinator& operator=(const TorrentCoordinator&) = delete;

    void start();
    void stop();
    // One scheduler pass, now, on the caller's thread.
    void pass_now();

    // The answer to an API call: an HTTP status, a code, and the request.
    struct Outcome {
        int status{200};
        std::string code;    // "ok" or an error code
        std::string reason;  // error.reason, when there is one
        std::string message;
        std::optional<TorrentRequest> request;
        std::optional<NodeId> node; // torrent_already_added: the holder's owner
    };

    // `remove_after`: absent means the cluster default; present-and-empty
    // means never. `paused` records the request already paused, in the same
    // metadata write, so no node starts it before a separate pause lands.
    Outcome add(std::string_view uri, bool search_result, std::optional<NodeId> pin,
                std::optional<std::optional<uint64_t>> remove_after, bool paused = false);
    // Live (not removed) requests, by id.
    std::vector<TorrentRequest> requests() const;
    std::optional<TorrentRequest> request(std::string_view id) const;
    Outcome act(std::string_view id, std::string_view action);
    Outcome patch(std::string_view id, std::optional<std::optional<uint64_t>> remove_after,
                  std::optional<std::optional<NodeId>> pin);

    // For the job JSON: has the owner acted on the current `desired`, and if
    // nothing will, why not.
    bool desired_applied(const TorrentRequest&, const std::optional<TorrentJob>& live) const;
    std::string desired_blocked_reason(const TorrentRequest&) const;
    // The owner's live job, local or from the cluster view, if known.
    std::optional<ClusterTorrentJob> live_job(const TorrentRequest&) const;

  private:
    struct Intent {
        TorrentDesired desired{};
        uint64_t changed_unix_ms{};
    };

    void loop(std::stop_token);
    void pass();
    Outcome write_desired(const TorrentRequest&, TorrentDesired);
    Outcome apply_intent_locally(const std::string& id, TorrentDesired, uint64_t changed_unix_ms);
    TorrentDesired effective_desired(const TorrentRequest&) const;
    void publish_intents();
    void load_intents();
    void save_intents_locked() const MACHA_REQUIRES(mutex_);
    std::optional<std::pair<std::string, std::string>> resolve_remote(std::string_view uri, bool search_result,
                                                                      std::string& error);
    bool write_available() const;
    std::optional<MetadataSnapshotView> current_view() const;
    std::optional<MetadataSnapshotView> served_view() const;
    std::optional<std::chrono::milliseconds> default_remove_after() const;

    NodeRuntime& node_;
    MetadataView& metadata_;
    SubsystemRegistry& registry_;
    ClusterJobView& view_;
    const std::chrono::milliseconds claim_lease_;
    std::filesystem::path intents_path_;

    // Held across the intent journal's durable write and log lines.
    mutable IoMutex mutex_;
    // Operator intent applied here while metadata could not be written.
    std::map<std::string, Intent, std::less<>> intents_ MACHA_GUARDED_BY(mutex_);
    // When each member was last seen absent from membership, for leases.
    std::map<NodeId, uint64_t> absent_since_ MACHA_GUARDED_BY(mutex_);
    // When each request first became claimable here, for rank waits.
    std::map<std::string, uint64_t, std::less<>> claimable_since_ MACHA_GUARDED_BY(mutex_);
    std::set<std::string, std::less<>> migrated_ MACHA_GUARDED_BY(mutex_);

    // Held across a whole pass: metadata commits and peer calls. Guards no
    // state; it keeps passes serial.
    IoMutex pass_mutex_;
    std::condition_variable_any wake_;
    std::jthread worker_;
};

// The cluster phase a local job's state stands for.
TorrentPhase torrent_phase_of(TorrentJobState);
// v1 hex, else v2 hex, from a sanitized magnet; empty when it names neither.
std::string magnet_info_hash(std::string_view magnet);
std::string magnet_display_name(std::string_view magnet);

} // namespace macha
