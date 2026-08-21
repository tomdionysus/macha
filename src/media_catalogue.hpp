// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue.hpp"
#include "catalogue_hints.hpp"
#include "config.hpp"
#include "filesystem.hpp"
#include "json.hpp"

#include <chrono>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace macha {

enum class MediaProbeKind { movie, episode, track };

enum class MediaProbeLookupStrategy { automatic, music_release_first, music_recording_first };

struct MediaProbe {
    MediaProbeKind kind{MediaProbeKind::movie};
    std::string path;
    std::string media_id;
    std::string title;
    std::optional<int32_t> year;
    std::optional<std::string> edition;
    std::string series;
    std::optional<int32_t> season;
    std::optional<int32_t> episode;
    std::string artist;
    std::string album;
    std::optional<int32_t> disc;
    std::optional<int32_t> track;
    std::optional<std::string> musicbrainz_recording_id;
    std::optional<std::string> musicbrainz_release_id;
    std::optional<std::string> musicbrainz_artist_id;
    std::string track_artist;
    std::string album_artist;
    MediaProbeLookupStrategy lookup_strategy{MediaProbeLookupStrategy::automatic};
};

struct MediaProbeContext {
    std::string_view root;
    std::string_view path;
    const FsEntry& entry;
    const MediaProbe* embedded_metadata{};
};

struct MediaProbeCandidate {
    MediaProbe probe;
    int score{};
    std::string generator;
    std::vector<std::string> evidence;
};

struct LocalArtworkCandidate {
    std::string role;
    std::string mime_type;
    Bytes bytes;
};

struct MediaProbeFile {
    std::vector<MediaProbeCandidate> candidates;
    std::vector<LocalArtworkCandidate> artwork;
};

class MediaProbeCandidateGenerator {
  public:
    virtual ~MediaProbeCandidateGenerator() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual std::vector<MediaProbeCandidate> generate(const MediaProbeContext&) const = 0;
};

std::vector<MediaProbeCandidate> probe_media_candidates(const MediaProbeContext&);
std::vector<MediaProbeCandidate> probe_media_candidates(std::string_view path, const FsEntry&,
                                                        std::string_view root = {});
// Run the same candidate generators against a host-side file before it enters
// the Macha namespace. Audio tags are read directly with libav when available.
std::vector<MediaProbeCandidate> probe_host_media_candidates(const std::filesystem::path&,
                                                             uint64_t size);
std::optional<MediaProbe> probe_media_path(std::string_view path, const FsEntry&);

struct RemoteArtwork {
    std::string item_id;
    std::string role;
    std::string url;
};

struct ProviderMatch {
    std::vector<CatalogueItem> items;
    std::vector<RemoteArtwork> artwork;
};

struct RemoteHttpResponse {
    long status{};
    std::string content_type;
    Bytes body;
};

class HttpClient {
  public:
    virtual ~HttpClient() = default;
    virtual void request_stop() noexcept {}
    virtual void reset_stop() noexcept {}
    virtual bool stop_requested() const noexcept { return false; }
    virtual RemoteHttpResponse get(std::string_view url,
                                   const std::vector<std::string>& headers = {},
                                   size_t maximum_bytes = 16 * 1024 * 1024) = 0;
};

class CurlHttpClient final : public HttpClient {
    std::atomic_bool stop_requested_{};

  public:
    CurlHttpClient();
    ~CurlHttpClient() override;
    void request_stop() noexcept override { stop_requested_.store(true, std::memory_order_relaxed); }
    void reset_stop() noexcept override { stop_requested_.store(false, std::memory_order_relaxed); }
    bool stop_requested() const noexcept override {
        return stop_requested_.load(std::memory_order_relaxed);
    }
    RemoteHttpResponse get(std::string_view, const std::vector<std::string>&, size_t) override;
};

class MetadataProvider {
  public:
    virtual ~MetadataProvider() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual bool supports(MediaProbeKind) const = 0;
    virtual std::optional<ProviderMatch> lookup(const MediaProbe&) = 0;
};

class TmdbProvider final : public MetadataProvider {
    HttpClient& http_;
    CatalogueTmdbConfig config_;
    std::string token_;
    std::map<std::string, std::optional<Json>> movie_cache_;
    std::map<std::string, std::optional<Json>> show_cache_;
    std::map<std::string, std::optional<Json>> season_cache_;

    Json api(std::string_view path, const std::vector<std::pair<std::string, std::string>>& query = {});
    std::optional<Json> api_optional(std::string_view path,
                                    const std::vector<std::pair<std::string, std::string>>& query = {});
    std::string image_url(std::string_view path) const;
    std::optional<Json> find_show(const MediaProbe&);

  public:
    TmdbProvider(HttpClient&, CatalogueTmdbConfig);
    std::string_view name() const noexcept override { return "tmdb"; }
    bool supports(MediaProbeKind) const override;
    std::optional<ProviderMatch> lookup(const MediaProbe&) override;
};

class MusicBrainzProvider final : public MetadataProvider {
    HttpClient& http_;
    CatalogueMusicBrainzConfig config_;
    std::map<std::string, std::optional<Json>> release_cache_;
    std::map<std::string, std::optional<Json>> release_id_cache_;
    std::map<std::string, std::optional<Json>> recording_cache_;
    std::map<std::string, std::optional<std::string>> cover_cache_;
    std::chrono::steady_clock::time_point last_request_{};
    std::chrono::steady_clock::time_point unavailable_until_{};

    Json api(std::string_view path, const std::vector<std::pair<std::string, std::string>>& query = {});
    std::optional<Json> release_by_id(std::string_view);
    std::optional<Json> find_release(const MediaProbe&);
    std::optional<Json> find_recording(const MediaProbe&);
    std::optional<std::string> cover_url(std::string_view release_id);

  public:
    MusicBrainzProvider(HttpClient&, CatalogueMusicBrainzConfig);
    std::string_view name() const noexcept override { return "musicbrainz"; }
    bool supports(MediaProbeKind) const override;
    std::optional<ProviderMatch> lookup(const MediaProbe&) override;
};

class DiscogsProvider final : public MetadataProvider {
    HttpClient& http_;
    CatalogueDiscogsConfig config_;
    std::string token_;
    std::map<std::string, std::optional<Json>> search_cache_;
    std::map<std::string, std::optional<Json>> release_cache_;
    std::chrono::steady_clock::time_point last_request_{};
    std::chrono::steady_clock::time_point unavailable_until_{};

    Json api(std::string_view path,
             const std::vector<std::pair<std::string, std::string>>& query = {});
    std::optional<Json> release_by_id(std::string_view);
    std::optional<Json> find_release(const MediaProbe&);

  public:
    DiscogsProvider(HttpClient&, CatalogueDiscogsConfig);
    std::string_view name() const noexcept override { return "discogs"; }
    bool supports(MediaProbeKind) const override;
    std::optional<ProviderMatch> lookup(const MediaProbe&) override;
};

class CatalogueScanProvider {
  public:
    virtual ~CatalogueScanProvider() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual const std::vector<std::string>& roots() const noexcept = 0;
    virtual MediaProbeFile probe_file(FileSystem&, std::string_view root,
                                      std::string_view path, const FsEntry&) = 0;
    std::vector<MediaProbeCandidate> probe_candidates(FileSystem& fs, std::string_view root,
                                                       std::string_view path, const FsEntry& entry) {
        return probe_file(fs, root, path, entry).candidates;
    }
    std::optional<MediaProbe> probe(FileSystem& fs, std::string_view root,
                                    std::string_view path, const FsEntry& entry) {
        auto candidates = probe_candidates(fs, root, path, entry);
        if (candidates.empty()) return {};
        return std::move(candidates.front().probe);
    }
    virtual std::optional<ProviderMatch> lookup(const MediaProbe&) = 0;
};

class MovieScanProvider final : public CatalogueScanProvider {
    std::vector<std::string> roots_;
    std::unique_ptr<TmdbProvider> metadata_;

  public:
    MovieScanProvider(HttpClient&, CatalogueMovieProviderConfig);
    std::string_view name() const noexcept override { return "movies"; }
    const std::vector<std::string>& roots() const noexcept override { return roots_; }
    MediaProbeFile probe_file(FileSystem&, std::string_view, std::string_view,
                              const FsEntry&) override;
    std::optional<ProviderMatch> lookup(const MediaProbe& probe) override {
        return metadata_ ? metadata_->lookup(probe) : std::nullopt;
    }
};

class TvScanProvider final : public CatalogueScanProvider {
    std::vector<std::string> roots_;
    std::unique_ptr<TmdbProvider> metadata_;

  public:
    TvScanProvider(HttpClient&, CatalogueTvProviderConfig);
    std::string_view name() const noexcept override { return "tv"; }
    const std::vector<std::string>& roots() const noexcept override { return roots_; }
    MediaProbeFile probe_file(FileSystem&, std::string_view, std::string_view,
                              const FsEntry&) override;
    std::optional<ProviderMatch> lookup(const MediaProbe& probe) override {
        return metadata_ ? metadata_->lookup(probe) : std::nullopt;
    }
};

class MusicScanProvider final : public CatalogueScanProvider {
    std::vector<std::string> roots_;
    std::vector<std::unique_ptr<MetadataProvider>> metadata_;
    size_t max_artwork_bytes_{};

  public:
    MusicScanProvider(HttpClient&, CatalogueMusicProviderConfig,
                      size_t max_artwork_bytes = 16 * 1024 * 1024);
    std::string_view name() const noexcept override { return "music"; }
    const std::vector<std::string>& roots() const noexcept override { return roots_; }
    MediaProbeFile probe_file(FileSystem&, std::string_view, std::string_view,
                              const FsEntry&) override;
    std::optional<ProviderMatch> lookup(const MediaProbe& probe) override;
};

class CatalogueScanner {
    NodeRuntime& node_;
    FileSystem& fs_;
    CatalogueManager& catalogue_;
    CatalogueHintQueue& hints_;
    CatalogueScannerConfig config_;
    std::unique_ptr<HttpClient> http_;
    std::unique_ptr<HttpClient> provider_http_;
    std::vector<std::unique_ptr<CatalogueScanProvider>> providers_;
    std::atomic_bool rescan_requested_{};
    std::jthread worker_;
    mutable std::mutex config_mutex_;

    void configure_providers();
    bool coordinator() const;
    void loop(std::stop_token);
    void walk(std::string_view root, std::vector<std::pair<std::string, FsEntry>>& out,
              std::stop_token = {});
    size_t scan_once(std::stop_token, bool force, std::string_view hint_source,
                     int hint_priority, bool unique_source_ref = false);
    struct PreparedHintMatch {
        std::string hint_id;
        std::string provider;
        std::string media_id;
        std::vector<std::string> catalogue_item_ids;
        std::vector<CatalogueItem> items;
        std::string result;
        unsigned attempts{};
    };
    struct HintBatchResult {
        size_t claimed{};
        size_t catalogued{};
    };

    std::optional<PreparedHintMatch> prepare_hint(const CatalogueHint&, std::stop_token);
    HintBatchResult process_hint_batch(std::stop_token, size_t max_hints);
    CatalogueScanProvider* provider_for_path(std::string_view path, std::string& root) const;

  public:
    CatalogueScanner(NodeRuntime&, FileSystem&, CatalogueManager&, CatalogueHintQueue&,
                     CatalogueScannerConfig, std::unique_ptr<HttpClient> = {});
    ~CatalogueScanner();
    void start();
    void request_stop();
    void stop();
    void reconfigure(CatalogueScannerConfig);
    void request_rescan();
    size_t scan_once();
};

} // namespace macha
