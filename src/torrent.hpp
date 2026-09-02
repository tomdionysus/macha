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

  public:
    TorrentManager(IngestManager&, TorrentConfig, const std::filesystem::path& state_path);
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
};

std::optional<std::string> sanitize_magnet_uri(std::string_view);
bool safe_torrent_fetch_url(std::string_view);

} // namespace macha
