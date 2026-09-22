// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "ingest.hpp"
#include "media_catalogue.hpp"

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
    std::string error;
};

// HTTP/RPC-facing JSON shape for a torrent job -- shared by the local HTTP
// handler (acquisition_api.cpp) and the cluster-wide RPC bridge, so a remote
// job renders identically to a local one (plus a "node_id" field the caller
// tags on afterward). Named distinctly from torrent_job_json()/
// parse_torrent_job() in torrent.cpp (the on-disk jobs.json persistence
// shape, file-local) since both live in that translation unit.
Json torrent_job_api_json(const TorrentJob&);

// The listen_interfaces string the download engine binds, in libtorrent's own
// syntax. An explicit torrent.listen_interfaces wins; otherwise the node's
// advertised address is used, because libtorrent's default enumeration binds
// eth0 and loopback but never wlan0 -- which on a wireless-only node leaves
// the session on loopback alone, reaching no peer and raising no error.
std::string torrent_listen_interfaces(const TorrentConfig&, std::string_view advertise);

struct ClusterTorrentJob {
    NodeId node_id;
    TorrentJob job;
};

struct TorrentActionResult {
    bool exists{};
    bool changed{};
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

// Everything core is allowed to know about BitTorrent acquisition. The
// implementation (TorrentManager, torrent_manager.hpp) and its libtorrent
// linkage live in the libmacha-torrent plugin, not in macha_core: core holds
// this interface, obtained from SubsystemRegistry::torrent(), and a node with
// no plugin installed simply has no torrent capability at runtime rather than
// a differently-compiled binary. See
// TODO/2026-09-05-subsystem-plugin-isolation-plan.md.
//
// Lifecycle (start/stop/restart-on-fault) is not part of this interface --
// that belongs to the plugin's Subsystem, which SubsystemSupervisor owns.
class TorrentService {
  public:
    virtual ~TorrentService() = default;

    virtual bool enabled() const noexcept = 0;
    // Live configuration reload (Service::reload_config). Only the limits an
    // implementation can change without a restart take effect.
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

    // Cluster-wide visibility: local jobs (this node's own jobs()), plus one
    // RPC survey per active peer. An unreachable/erroring peer is logged and
    // skipped, never fails the whole call.
    // Starts a job on a named node rather than on whichever node happened to
    // receive the request. Until this existed, placement was "wherever the
    // POST landed", which is invisible from a client configured with one
    // address and impossible to control from a UI behind a proxy -- and there
    // is every reason to care which node downloads: they differ in disk, in
    // memory, and in what else they are serving at the time.
    //
    // An empty node id means here. An unknown or unreachable node is reported
    // rather than silently downloaded locally, because a job that quietly
    // lands somewhere else is worse than one that fails to start.
    struct Placement {
        NodeId node_id;
        std::string job_id;
        bool placed{};
        std::string error;
    };
    virtual Placement add_on(const NodeId& node, std::string_view magnet_or_uri) = 0;

    virtual std::vector<ClusterTorrentJob> jobs_cluster_wide() const = 0;
    // Local job(id) first (zero added latency for the common owned-here
    // case); only surveys peers when the job is locally absent.
    virtual std::optional<ClusterTorrentJob> job_cluster_wide(std::string_view id) const = 0;
    // Each: local action first; only surveys peers when the job is locally
    // absent. The first peer reporting the job exists is authoritative,
    // preserving the local 404-vs-409 distinction cluster-wide.
    virtual TorrentActionResult pause_cluster_wide(std::string_view id) = 0;
    virtual TorrentActionResult resume_cluster_wide(std::string_view id) = 0;
    virtual TorrentActionResult retry_cluster_wide(std::string_view id) = 0;
    virtual TorrentActionResult cancel_cluster_wide(std::string_view id) = 0;
    virtual TorrentActionResult clear_cluster_wide(std::string_view id) = 0;
};

std::optional<std::string> sanitize_magnet_uri(std::string_view);
bool safe_torrent_fetch_url(std::string_view);

} // namespace macha
