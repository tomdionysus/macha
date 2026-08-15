// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue.hpp"
#include "config.hpp"
#include "filesystem.hpp"
#include "json.hpp"

#include <chrono>
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

struct MediaProbe {
    MediaProbeKind kind{MediaProbeKind::movie};
    std::string path;
    std::string media_id;
    std::string title;
    std::optional<int32_t> year;
    std::string series;
    std::optional<int32_t> season;
    std::optional<int32_t> episode;
    std::string artist;
    std::string album;
    std::optional<int32_t> disc;
    std::optional<int32_t> track;
};

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
    virtual RemoteHttpResponse get(std::string_view url,
                                   const std::vector<std::string>& headers = {},
                                   size_t maximum_bytes = 16 * 1024 * 1024) = 0;
};

class CurlHttpClient final : public HttpClient {
  public:
    CurlHttpClient();
    ~CurlHttpClient() override;
    RemoteHttpResponse get(std::string_view, const std::vector<std::string>&, size_t) override;
};

class MetadataProvider {
  public:
    virtual ~MetadataProvider() = default;
    virtual bool supports(MediaProbeKind) const = 0;
    virtual std::optional<ProviderMatch> lookup(const MediaProbe&) = 0;
};

class TmdbProvider final : public MetadataProvider {
    HttpClient& http_;
    CatalogueTmdbConfig config_;
    std::string token_;
    std::map<std::string, Json> show_cache_;
    std::map<std::string, Json> season_cache_;

    Json api(std::string_view path, const std::vector<std::pair<std::string, std::string>>& query = {});
    std::string image_url(std::string_view path) const;
    std::optional<Json> find_show(const MediaProbe&);

  public:
    TmdbProvider(HttpClient&, CatalogueTmdbConfig);
    bool supports(MediaProbeKind) const override;
    std::optional<ProviderMatch> lookup(const MediaProbe&) override;
};

class MusicBrainzProvider final : public MetadataProvider {
    HttpClient& http_;
    CatalogueMusicBrainzConfig config_;
    std::map<std::string, Json> release_cache_;
    std::map<std::string, std::optional<std::string>> cover_cache_;
    std::chrono::steady_clock::time_point last_request_{};

    Json api(std::string_view path, const std::vector<std::pair<std::string, std::string>>& query = {});
    std::optional<Json> find_release(const MediaProbe&);
    std::optional<std::string> cover_url(std::string_view release_id);

  public:
    MusicBrainzProvider(HttpClient&, CatalogueMusicBrainzConfig);
    bool supports(MediaProbeKind) const override;
    std::optional<ProviderMatch> lookup(const MediaProbe&) override;
};

class CatalogueScanner {
    NodeRuntime& node_;
    FileSystem& fs_;
    CatalogueManager& catalogue_;
    CatalogueScannerConfig config_;
    std::unique_ptr<HttpClient> http_;
    std::vector<std::unique_ptr<MetadataProvider>> providers_;
    std::jthread worker_;
    mutable std::mutex config_mutex_;

    void configure_providers();
    bool coordinator() const;
    void loop(std::stop_token);
    void walk(std::string_view root, std::vector<std::pair<std::string, FsEntry>>& out);

  public:
    CatalogueScanner(NodeRuntime&, FileSystem&, CatalogueManager&, CatalogueScannerConfig,
                     std::unique_ptr<HttpClient> = {});
    ~CatalogueScanner();
    void start();
    void stop();
    void reconfigure(CatalogueScannerConfig);
    size_t scan_once();
};

} // namespace macha
