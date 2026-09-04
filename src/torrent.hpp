// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "ingest.hpp"
#include "media_catalogue.hpp"

#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
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

class TorrentManager {
    struct Impl;

    NodeRuntime& node_;
    IngestManager& ingest_;
    TorrentConfig config_;
    std::filesystem::path state_file_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::map<std::string, TorrentJob, std::less<>> jobs_;
    std::unique_ptr<Impl> impl_;
    std::jthread worker_;

    void load_state();
    void save_state_locked() const;
    void restore_jobs();
    void loop(std::stop_token);
    void update_jobs();
    bool has_active_jobs_locked() const;
    std::string add_impl(std::string uri, bool allow_fetch);

    // NodeRuntime::set_torrent_bridge() handler bodies. Local-only -- never
    // call the *_cluster_wide() methods from here, or a peer's survey would
    // itself re-survey its own peers.
    Bytes handle_jobs_query(std::span<const uint8_t> request_payload) const;
    Bytes handle_job_action(std::span<const uint8_t> request_payload);
    TorrentActionResult dispatch_action_cluster_wide(std::string_view id, std::string_view action);

  public:
    TorrentManager(NodeRuntime&, IngestManager&, TorrentConfig,
                   const std::filesystem::path& state_path);
    ~TorrentManager();

    static bool build_available() noexcept;
    void start();
    void request_stop();
    void stop();
    void reconfigure(TorrentConfig);
    bool enabled() const noexcept { return config_.enabled; }

    std::string add(std::string magnet_uri);
    // Only use with a URI obtained from TorrentSearchManager::resolve().
    std::string add_search_result(std::string acquisition_uri);
    std::vector<TorrentJob> jobs() const;
    std::optional<TorrentJob> job(std::string_view id) const;
    bool pause(std::string_view id);
    bool resume(std::string_view id);
    bool retry(std::string_view id);
    bool cancel(std::string_view id);
    bool clear(std::string_view id);

    // Cluster-wide visibility: local jobs (this node's own jobs()), plus one
    // RPC survey per active peer. An unreachable/erroring peer is logged and
    // skipped, never fails the whole call.
    std::vector<ClusterTorrentJob> jobs_cluster_wide() const;
    // Local job(id) first (zero added latency for the common owned-here
    // case); only surveys peers when the job is locally absent.
    std::optional<ClusterTorrentJob> job_cluster_wide(std::string_view id) const;
    // Each: local action first; only surveys peers when the job is locally
    // absent. The first peer reporting the job exists is authoritative,
    // preserving the local 404-vs-409 distinction cluster-wide.
    TorrentActionResult pause_cluster_wide(std::string_view id);
    TorrentActionResult resume_cluster_wide(std::string_view id);
    TorrentActionResult retry_cluster_wide(std::string_view id);
    TorrentActionResult cancel_cluster_wide(std::string_view id);
    TorrentActionResult clear_cluster_wide(std::string_view id);
};

std::optional<std::string> sanitize_magnet_uri(std::string_view);
bool safe_torrent_fetch_url(std::string_view);

} // namespace macha
