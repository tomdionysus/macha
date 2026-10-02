// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "acquisition/ingest.hpp"
#include "catalogue/media_catalogue.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace macha {

enum class TorrentJobState {
    queued,
    metadata,
    downloading,
    verify_queued,
    verifying,
    downloaded,
    importing,
    cataloguing,
    paused,
    blocked,
    completed,
    cancelled,
    failed,
};

std::string torrent_job_state_name(TorrentJobState);
std::optional<TorrentJobState> parse_torrent_job_state(std::string_view);

struct TorrentJob {
    std::string id;
    std::string name;
    std::string source_uri;
    std::string info_hash;
    std::filesystem::path save_path;
    TorrentJobState state{TorrentJobState::queued};
    uint64_t bytes_total{};
    uint64_t bytes_completed{};
    uint64_t download_rate{};
    uint64_t upload_rate{};
    uint64_t uploaded_total{};
    unsigned peers{};
    unsigned seeds{};
    size_t catalogue_total{};
    size_t catalogue_pending{};
    size_t catalogue_catalogued{};
    size_t catalogue_no_match{};
    size_t catalogue_failed{};
    std::optional<uint64_t> eta_seconds;
    std::optional<std::string> ingest_job_id;
    uint64_t created_unix_ms{};
    uint64_t updated_unix_ms{};
    // Why the job is blocked or failed: the code clients act on and its English
    // message. An ingest failure carries the ingest's code.
    std::string error_code;
    std::string error;
    // Extent publication progress, from the first verified piece until the
    // ingest is submitted. Wire only, never persisted. `progress_age_ms`: since
    // the published count last advanced, on the owner's clock.
    struct Publication {
        uint64_t published_extents{};
        uint64_t extents{};
        uint64_t published_bytes{};
        uint64_t bytes{};
        uint64_t progress_age_ms{};
    };
    std::optional<Publication> publication;
    // Why a stopped job waits: "extent_publication" (handed to the ingest once
    // every extent is published) or empty. Wire only.
    std::string waiting_reason;
};

// API JSON for a torrent job, shared by the HTTP handler and the cluster RPC
// so remote and local jobs render alike.
Json torrent_job_api_json(const TorrentJob&);
Json torrent_publication_json(const TorrentJob::Publication&);
// The jobs.json shape, and the cluster RPC shape (plus the transient rates,
// peers and ETA).
Json torrent_job_json(const TorrentJob&);
// What an update changed in a job's persisted form, ignoring updated_unix_ms:
// nothing, only the transfer counters, or the record.
enum class TorrentJobChange : uint8_t { none, progress, record };
TorrentJobChange torrent_job_change(const TorrentJob& before, const TorrentJob& after);
TorrentJob parse_torrent_job(const Json&);
Json torrent_job_wire_json(const TorrentJob&);
TorrentJob parse_torrent_job_wire(const Json&);

// libtorrent's listen_interfaces: torrent.listen_interfaces if set, else the
// advertised address, as the default enumeration never binds wlan0 and a
// wireless-only node would silently listen on loopback alone.
std::string torrent_listen_interfaces(const TorrentConfig&, std::string_view advertise);

struct ClusterTorrentJob {
    NodeId node_id;
    TorrentJob job;
};

struct TorrentActionResult {
    bool exists{};
    bool changed{};
    // The job's node could not be reached.
    bool unreachable{};
    std::optional<ClusterTorrentJob> updated;
};

struct TorrentSearchResult {
    std::string acquisition_ref;
    std::string provider;
    std::string title;
    std::optional<uint64_t> size_bytes;
    std::optional<uint64_t> seeders;
    std::optional<uint64_t> leechers;
    std::string published;
    std::optional<std::string> magnet_uri;
    std::optional<std::string> torrent_url;
};

struct TorrentSearchResponse {
    std::vector<TorrentSearchResult> results;
    std::map<std::string, std::string, std::less<>> errors;
};

class TorrentSearchProvider {
  public:
    virtual ~TorrentSearchProvider() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual std::vector<TorrentSearchResult> search(std::string_view query) = 0;
};

class TorznabSearchProvider final : public TorrentSearchProvider {
    TorrentSearchProviderConfig config_;
    std::unique_ptr<HttpClient> http_;
    std::string api_key_;

  public:
    explicit TorznabSearchProvider(TorrentSearchProviderConfig,
                                   std::unique_ptr<HttpClient> = {});
    std::string_view name() const noexcept override { return config_.name; }
    std::vector<TorrentSearchResult> search(std::string_view query) override;
};

class TorrentSearchManager {
    struct CachedAcquisition {
        std::string uri;
        uint64_t expires_unix_ms{};
    };
    std::vector<std::unique_ptr<TorrentSearchProvider>> providers_;
    mutable std::mutex mutex_;
    std::map<std::string, CachedAcquisition, std::less<>> acquisitions_;
    static constexpr size_t max_acquisitions_ = 4096;

  public:
    explicit TorrentSearchManager(const TorrentConfig&);
    bool enabled() const noexcept { return !providers_.empty(); }
    TorrentSearchResponse search(std::string_view query);
    std::optional<std::string> resolve(std::string_view acquisition_ref);
};

// Core's whole view of BitTorrent acquisition, from SubsystemRegistry::torrent().
// TorrentManager implements it in the libmacha-torrent plugin; without the
// plugin a node has no torrent capability. Lifecycle is the plugin
// Subsystem's, not this interface's.
class TorrentService {
  public:
    virtual ~TorrentService() = default;

    virtual bool enabled() const noexcept = 0;
    // Live reload: only limits changeable without a restart take effect.
    virtual void reconfigure(TorrentConfig) = 0;

    virtual std::string add(std::string magnet_uri) = 0;
    // Only use with a URI obtained from TorrentSearchManager::resolve().
    virtual std::string add_search_result(std::string acquisition_uri) = 0;
    virtual std::vector<TorrentJob> jobs() const = 0;
    virtual std::optional<TorrentJob> job(std::string_view id) const = 0;
    virtual bool pause(std::string_view id) = 0;
    virtual bool resume(std::string_view id) = 0;
    virtual bool retry(std::string_view id) = 0;
    virtual bool cancel(std::string_view id) = 0;
    virtual bool clear(std::string_view id) = 0;

    // Adds a torrent on this node; the caller (ClusterJobView) chose the node.
    struct Placement {
        NodeId node_id;
        std::string job_id;
        bool placed{};
        // Why not placed: a code (node_not_member, node_not_torrent_capable,
        // node_refused, node_unreachable, node_did_not_start, missing_uri,
        // add_failed, torrent_already_added, or the peer's own) and its English
        // message. For torrent_already_added, job_id is the existing job.
        std::string reason;
        std::string error;
        // The job as recorded, when placed.
        std::optional<TorrentJob> job;
    };
    // `search_result`: a URI resolved from an acquisition_ref, which may be a
    // trusted provider's .torrent URL; anything else must be a magnet.
    virtual Placement place(std::string_view magnet_or_uri, bool search_result) = 0;
    // What this node offers for new jobs, for GET /api/v1/torrents/nodes.
    struct Offer {
        bool accepting{};
        std::string not_accepting_reason; // slots_full, staging_full, draining
        size_t max_active{};
        size_t active_jobs{};
    };
    virtual Offer offer() const = 0;
    // A URI's canonical magnet, info hash and name, without adding it. A
    // .torrent URL is fetched here, as only the plugin parses metainfo.
    struct Resolved {
        std::string magnet;
        std::string info_hash;
        std::string name;
    };
    virtual Resolved resolve(std::string_view uri, bool search_result) = 0;
    // Adds the torrent of a cluster request this node has claimed, under the
    // request's id; a job with that id already here is left as it is.
    virtual std::string adopt(std::string_view id, std::string_view magnet, bool held) = 0;
};

std::optional<std::string> sanitize_magnet_uri(std::string_view);
bool safe_torrent_fetch_url(std::string_view);

} // namespace macha
