// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "metadata/metadata_server.hpp"
#include "catalogue/catalogue.hpp"
#include "catalogue/catalogue_hints.hpp"
#include "config.hpp"
#include "contract/thread_safety.hpp"
#include "filesystem/filesystem.hpp"
#include "metadata/namespace_tree.hpp"
#include "json.hpp"

#include <chrono>
#include <functional>
#include <array>
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

class MediaInformationService;

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
    std::optional<int32_t> episode_end;
    std::string artist;
    std::string album;
    std::optional<int32_t> disc;
    std::optional<int32_t> track;
    std::optional<std::string> musicbrainz_recording_id;
    std::optional<std::string> musicbrainz_release_id;
    std::optional<std::string> musicbrainz_artist_id;
    // A TMDB movie or TV id: the lookup fetches that record instead of
    // searching by title.
    std::optional<std::string> tmdb_id;
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
// The same candidate generators over a host file not yet in the namespace;
// audio tags are read with libav when available.
std::vector<MediaProbeCandidate> probe_host_media_candidates(const std::filesystem::path&,
                                                             uint64_t size);
std::optional<MediaProbe> probe_media_path(std::string_view path, const FsEntry&);
FrameType catalogue_media_profile_frame_type() noexcept;

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
    // Retry-After, when given in seconds.
    std::optional<std::chrono::milliseconds> retry_after;
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

// A metadata editor request the providers could not satisfy; `status` and
// `code` are the API's answer.
struct ProviderRequestError : std::runtime_error {
    int status;
    std::string code;
    // When a provider said, or this node knows, how long until it can answer.
    std::optional<std::chrono::milliseconds> retry_after;
    ProviderRequestError(int http_status, std::string error_code, const std::string& message,
                         std::optional<std::chrono::milliseconds> retry = {})
        : std::runtime_error(message), status(http_status), code(std::move(error_code)),
          retry_after(retry) {}
};

// A provider that could not answer, as the client is told: the code,
// "Provider unavailable" and, when known, how long until it may. The reason is
// logged here, not sent.
ProviderRequestError provider_unavailable(std::string_view provider, std::string_view reason);
ProviderRequestError provider_unavailable(std::string_view provider, const std::exception& error);

// MusicBrainz allows one request a second per client: every MusicBrainzProvider
// on a node shares this gate and its backoff. After a failure or a rate-limit
// answer the gate backs off for the Retry-After MusicBrainz gives, else for
// `backoff_first`, doubling to `backoff_max`; a success resets it.
struct MusicBrainzGate {
    // The least time between two requests.
    const std::chrono::steady_clock::duration interval;
    static constexpr std::chrono::seconds backoff_first{2};
    static constexpr std::chrono::seconds backoff_max{60};
    explicit MusicBrainzGate(std::chrono::steady_clock::duration interval = std::chrono::seconds(1))
        : interval(interval) {}
    // Held across the pacing sleep and the MusicBrainz HTTP request.
    IoMutex mutex;
    std::chrono::steady_clock::time_point last_request MACHA_GUARDED_BY(mutex){};
    std::chrono::steady_clock::time_point unavailable_until MACHA_GUARDED_BY(mutex){};
    std::chrono::steady_clock::duration backoff MACHA_GUARDED_BY(mutex){};
};

// The numbers a provider reference needs to name one playable item: a TV
// show's season and episode, a release's disc and track.
struct ProviderRefNumbers {
    std::optional<int32_t> season;
    std::optional<int32_t> episode;
    std::optional<int32_t> disc;
    std::optional<int32_t> track;
};

// One image a provider offers for an artwork role.
struct ArtworkOption {
    std::string option_id;
    std::string role;
    std::optional<int32_t> width;
    std::optional<int32_t> height;
    std::string language;
    std::string preview_url; // the provider's own small image, for the client
    std::string url;         // full size; the server fetches it, never the client
};

// A metadata editor's search of one provider.
struct ProviderSearchQuery {
    std::string kind; // movie, show, album
    std::string text;
    std::optional<int32_t> year;
    std::string artist; // albums: narrows to releases credited to this artist
    size_t limit{10};
};

// One record a provider search found.
struct ProviderSearchResult {
    std::string ref; // what a match by reference takes
    std::string provider;
    std::string kind;
    std::string title;
    std::optional<int32_t> year;
    std::string overview;
    std::string artist;       // albums
    std::string catalogue_id; // the id a match gives the item
};

// One track of a provider's release. A number or length the provider does not
// give is absent.
struct ProviderReleaseTrack {
    std::optional<int32_t> disc_number;  // the medium's position
    std::optional<int32_t> track_number; // the track's position on its medium
    std::string title;                   // the track's title on this release
    std::optional<int64_t> length_ms;
    std::string recording_id;
};

class MetadataProvider {
  public:
    virtual ~MetadataProvider() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual bool supports(MediaProbeKind) const = 0;
    virtual std::optional<ProviderMatch> lookup(const MediaProbe&) = 0;
    virtual std::vector<ProviderSearchResult> search(const ProviderSearchQuery&) { return {}; }
    // Images for a role of record `kind`:`id` (TMDB `movie`/`tv`, MusicBrainz
    // `release`); `numbers` narrow a show to a season or episode.
    virtual std::vector<ArtworkOption> artwork_options(std::string_view /*kind*/,
                                                       std::string_view /*id*/,
                                                       std::string_view /*role*/,
                                                       const ProviderRefNumbers& /*numbers*/) {
        return {};
    }
    // The tracks of release `id`, in the release's own order.
    virtual std::vector<ProviderReleaseTrack> release_tracks(std::string_view /*id*/) {
        return {};
    }
};

class TmdbProvider final : public MetadataProvider {
    HttpClient& http_;
    CatalogueTmdbConfig config_;
    std::string token_;
    std::map<std::string, std::optional<Json>> movie_cache_;
    std::map<std::string, std::optional<Json>> show_cache_;
    std::map<std::string, std::optional<Json>> season_cache_;
    size_t cache_bytes_{};

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
    std::vector<ProviderSearchResult> search(const ProviderSearchQuery&) override;
    std::vector<ArtworkOption> artwork_options(std::string_view kind, std::string_view id,
                                               std::string_view role,
                                               const ProviderRefNumbers& numbers) override;
    size_t cache_entries() const noexcept {
        return movie_cache_.size() + show_cache_.size() + season_cache_.size();
    }
    size_t cache_bytes() const noexcept { return cache_bytes_; }
};

class MusicBrainzProvider final : public MetadataProvider {
    HttpClient& http_;
    CatalogueMusicBrainzConfig config_;
    std::map<std::string, std::optional<Json>> release_cache_;
    std::map<std::string, std::optional<Json>> release_id_cache_;
    std::map<std::string, std::optional<Json>> recording_cache_;
    std::map<std::string, std::optional<std::string>> cover_cache_;
    size_t cache_bytes_{};
    std::shared_ptr<MusicBrainzGate> gate_;
    bool interactive_{};

    Json api(std::string_view path, const std::vector<std::pair<std::string, std::string>>& query = {});
    std::optional<Json> release_by_id(std::string_view);
    std::optional<Json> find_release(const MediaProbe&);
    std::optional<Json> find_recording(const MediaProbe&);
    std::optional<std::string> cover_url(std::string_view release_id);

  public:
    // An interactive provider (the editor's) waits out a backoff of up to
    // `interactive_wait_max` and tries a failed request once more; a
    // background one is refused at once.
    static constexpr std::chrono::seconds interactive_wait_max{5};
    MusicBrainzProvider(HttpClient&, CatalogueMusicBrainzConfig,
                        std::shared_ptr<MusicBrainzGate> gate = {}, bool interactive = false);
    std::string_view name() const noexcept override { return "musicbrainz"; }
    bool supports(MediaProbeKind) const override;
    std::optional<ProviderMatch> lookup(const MediaProbe&) override;
    std::vector<ProviderSearchResult> search(const ProviderSearchQuery&) override;
    std::vector<ArtworkOption> artwork_options(std::string_view kind, std::string_view id,
                                               std::string_view role,
                                               const ProviderRefNumbers& numbers) override;
    std::vector<ProviderReleaseTrack> release_tracks(std::string_view id) override;
    size_t cache_entries() const noexcept {
        return release_cache_.size() + release_id_cache_.size() + recording_cache_.size() +
               cover_cache_.size();
    }
    size_t cache_bytes() const noexcept { return cache_bytes_; }
};

class DiscogsProvider final : public MetadataProvider {
    HttpClient& http_;
    CatalogueDiscogsConfig config_;
    std::string token_;
    std::map<std::string, std::optional<Json>> search_cache_;
    std::map<std::string, std::optional<Json>> release_cache_;
    size_t cache_bytes_{};
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
    size_t cache_entries() const noexcept { return search_cache_.size() + release_cache_.size(); }
    size_t cache_bytes() const noexcept { return cache_bytes_; }
};

class CatalogueScanProvider {
  public:
    virtual ~CatalogueScanProvider() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual const std::vector<std::string>& roots() const noexcept = 0;
    // Cheap discovery predicate: never reads media; probing is hint processing.
    virtual bool accepts_path(std::string_view path) const noexcept = 0;
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
    // The named metadata provider ("tmdb", "musicbrainz") when it is configured.
    virtual MetadataProvider* metadata(std::string_view) noexcept { return nullptr; }
};

class MovieScanProvider final : public CatalogueScanProvider {
    std::vector<std::string> roots_;
    std::unique_ptr<TmdbProvider> metadata_;

  public:
    MovieScanProvider(HttpClient&, CatalogueMovieProviderConfig);
    std::string_view name() const noexcept override { return "movies"; }
    const std::vector<std::string>& roots() const noexcept override { return roots_; }
    bool accepts_path(std::string_view path) const noexcept override;
    MediaProbeFile probe_file(FileSystem&, std::string_view, std::string_view,
                              const FsEntry&) override;
    std::optional<ProviderMatch> lookup(const MediaProbe& probe) override {
        return metadata_ ? metadata_->lookup(probe) : std::nullopt;
    }
    MetadataProvider* metadata(std::string_view provider) noexcept override {
        return metadata_ && provider == metadata_->name() ? metadata_.get() : nullptr;
    }
};

class TvScanProvider final : public CatalogueScanProvider {
    std::vector<std::string> roots_;
    std::unique_ptr<TmdbProvider> metadata_;

  public:
    TvScanProvider(HttpClient&, CatalogueTvProviderConfig);
    std::string_view name() const noexcept override { return "tv"; }
    const std::vector<std::string>& roots() const noexcept override { return roots_; }
    bool accepts_path(std::string_view path) const noexcept override;
    MediaProbeFile probe_file(FileSystem&, std::string_view, std::string_view,
                              const FsEntry&) override;
    std::optional<ProviderMatch> lookup(const MediaProbe& probe) override {
        return metadata_ ? metadata_->lookup(probe) : std::nullopt;
    }
    MetadataProvider* metadata(std::string_view provider) noexcept override {
        return metadata_ && provider == metadata_->name() ? metadata_.get() : nullptr;
    }
};

class MusicScanProvider final : public CatalogueScanProvider {
    std::vector<std::string> roots_;
    std::vector<std::unique_ptr<MetadataProvider>> metadata_;
    size_t max_artwork_bytes_{};

  public:
    MusicScanProvider(HttpClient&, CatalogueMusicProviderConfig,
                      size_t max_artwork_bytes = 16 * 1024 * 1024,
                      std::shared_ptr<MusicBrainzGate> musicbrainz_gate = {},
                      bool interactive = false);
    std::string_view name() const noexcept override { return "music"; }
    const std::vector<std::string>& roots() const noexcept override { return roots_; }
    bool accepts_path(std::string_view path) const noexcept override;
    MediaProbeFile probe_file(FileSystem&, std::string_view, std::string_view,
                              const FsEntry&) override;
    std::optional<ProviderMatch> lookup(const MediaProbe& probe) override;
    MetadataProvider* metadata(std::string_view provider) noexcept override;
};

struct ProviderRefMatch {
    std::string leaf_id;
    std::vector<std::string> item_ids;
};

// The files under one catalogue root in one metadata snapshot. Discovery and the
// reconciliation's namespace signature must share a generation: never mix this
// with live readdir()/getattr().
std::vector<std::pair<std::string, FsEntry>> catalogue_snapshot_files(
    std::string_view root, const MetadataSnapshot& namespace_snapshot,
    const NamespaceNodeStore* namespace_nodes, std::stop_token stop = {});

class CatalogueScanner {
    NodeRuntime& node_;
    MetadataServer& metadata_server_;
    FileSystem& fs_;
    CatalogueManager& catalogue_;
    CatalogueHintQueue& hints_;
    // Held across probe_unmatched()'s media probe and the namespace walk of
    // request_media_profiles().
    mutable IoMutex config_mutex_;
    CatalogueScannerConfig config_ MACHA_GUARDED_BY(config_mutex_);
    std::unique_ptr<HttpClient> http_;
    std::unique_ptr<HttpClient> provider_http_;
    std::shared_ptr<MediaEngine> profile_engine_;
    MediaInformationService* media_information_{};
    // Replaced only by reconfigure(), with the worker stopped: the worker
    // reads it under config_mutex_ and uses the providers outside it.
    std::vector<std::unique_ptr<CatalogueScanProvider>> providers_ MACHA_GUARDED_BY(config_mutex_);
    // The metadata editor's own providers, over the unbudgeted client so an
    // operator's request never waits on a scan's budget. Several sets: a
    // provider keeps unsynchronised caches, so a set serves one request at a
    // time and its lock is held across the call out to the provider.
    // Requests take the sets in turn, so one slow answer holds up one set,
    // not the editor.
    struct EditorSeat {
        IoMutex mutex;
        std::vector<std::unique_ptr<CatalogueScanProvider>> providers MACHA_GUARDED_BY(mutex);
    };
    static constexpr size_t editor_seats = 4;
    std::array<EditorSeat, editor_seats> editor_seats_;
    // The options last offered for a reference and role, by when they lapse.
    static constexpr size_t artwork_options_max = 128;
    static constexpr std::chrono::minutes artwork_options_kept{10};
    Mutex artwork_options_mutex_;
    std::map<std::string, std::pair<Clock::time_point, std::vector<ArtworkOption>>>
        artwork_options_ MACHA_GUARDED_BY(artwork_options_mutex_);
    // The artwork each provider image URL was stored as. A URL whose image
    // this node still holds is not fetched again.
    static constexpr size_t remote_artwork_max = 4096;
    Mutex remote_artwork_mutex_;
    std::map<std::string, CatalogueArtwork> remote_artwork_ MACHA_GUARDED_BY(remote_artwork_mutex_);
    // The seat for a record: every call about one record takes the same seat,
    // whose provider has already cached it, and other records spread over the
    // rest.
    EditorSeat& editor_seat(std::string_view record) noexcept {
        return editor_seats_[std::hash<std::string_view>{}(record) % editor_seats];
    }
    std::shared_ptr<MusicBrainzGate> musicbrainz_gate_{std::make_shared<MusicBrainzGate>()};
    std::atomic_bool rescan_requested_{};
    // Owned by the instantiator's thread (start/stop/reconfigure).
    std::jthread worker_;
    const std::chrono::milliseconds diagnostic_interval_{std::chrono::seconds(5)};

    void configure_providers() MACHA_REQUIRES(config_mutex_);
    bool coordinator() const;
    void loop(std::stop_token);
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
        std::optional<MediaProbeResult> media_profile;
    };
    struct HintBatchResult {
        size_t claimed{};
        size_t catalogued{};
    };

    HintBatchResult process_hint_batch(std::stop_token, size_t max_hints);
    CatalogueScanProvider* provider_for_path(std::string_view path, std::string& root) const
        MACHA_REQUIRES(config_mutex_);
    // Fetch and stage the provider artwork `match` names onto its items. An
    // item `locked` names keeps its artwork. False when `stop` interrupted it.
    // The staging carries `frame`, the class of whoever asked.
    bool stage_remote_artwork(ProviderMatch& match,
                              const std::function<bool(std::string_view)>& locked,
                              std::stop_token stop, size_t max_artwork_bytes,
                              DistributedStore::DurabilityBatch& artwork_batch,
                              FrameType frame);
    // The editor's metadata provider for a scan provider ("movies", "tv",
    // "music") and a metadata provider name.
    static MetadataProvider* editor_metadata(
        const std::vector<std::unique_ptr<CatalogueScanProvider>>& providers,
        std::string_view scan_provider, std::string_view metadata_provider);

  public:
    // One hint against one namespace snapshot. A hint created after
    // `snapshot_taken_unix_ms` may name a file the snapshot lacks, and is
    // deferred rather than failed.
    std::optional<PreparedHintMatch> prepare_hint(
        const CatalogueHint&, std::stop_token, const MetadataSnapshot& namespace_snapshot,
        uint64_t snapshot_taken_unix_ms, DistributedStore::DurabilityBatch& artwork_batch);

    CatalogueScanner(NodeRuntime&, MetadataServer&, FileSystem&, CatalogueManager&,
                     CatalogueHintQueue&,
                     CatalogueScannerConfig, std::unique_ptr<HttpClient> = {},
                     std::chrono::milliseconds diagnostic_interval = std::chrono::seconds(5),
                     std::shared_ptr<MediaEngine> profile_engine = {},
                     MediaInformationService* media_information = nullptr);
    ~CatalogueScanner();
    void start();
    void request_stop();
    void stop();
    void reconfigure(CatalogueScannerConfig);
    // Whether scanning is configured on.
    bool enabled() const {
        Lock lock(config_mutex_);
        return config_.enabled;
    }
    void request_rescan();
    size_t request_media_profiles(const std::vector<std::string>& media_ids);
    std::vector<MediaProbeCandidate> probe_unmatched(std::string_view hint_id);
    // Match an unmatched file to a provider reference (`tmdb:movie:<id>`,
    // `tmdb:tv:<id>`, `musicbrainz:release:<mbid>`): fetch the record, build
    // the hierarchy, stage its artwork and bind the file, as a scan match
    // would. Its DATA work carries `frame`, the asking request's class.
    // Throws ProviderRequestError with the API's answer.
    ProviderRefMatch match_unmatched_ref(std::string_view hint_id, std::string_view ref,
                                         const ProviderRefNumbers& numbers, FrameType frame);
    // Search the provider for a kind (movie, show: TMDB; album: MusicBrainz).
    // A result's catalogue_id is kept only when the catalogue holds that item.
    // Throws ProviderRequestError with the API's answer.
    std::vector<ProviderSearchResult> search_providers(const ProviderSearchQuery&);
    // The images a provider offers for one role of a reference: `poster` or
    // `backdrop` for a movie or show, `poster` for a season (`numbers.season`),
    // `still` for an episode (season and episode), `cover` for a release.
    // Throws ProviderRequestError with the API's answer.
    std::vector<ArtworkOption> artwork_options(std::string_view ref, std::string_view role,
                                               const ProviderRefNumbers& numbers);
    // The tracks of a provider's release (`musicbrainz` and a release MBID),
    // in the release's own order. Throws ProviderRequestError with the API's
    // answer.
    std::vector<ProviderReleaseTrack> release_tracks(std::string_view provider,
                                                     std::string_view release_id);
    // Fetch one listed option and make it the item's artwork for the role.
    // The reference is the item's own unless `ref` names one. Locks the item
    // unless `lock` is false. Its DATA work carries `frame`, the asking
    // request's class. Throws ProviderRequestError.
    CatalogueItem choose_artwork(std::string_view item_id, std::string_view role,
                                 std::string_view option_id, std::optional<std::string> ref,
                                 ProviderRefNumbers numbers, bool lock, FrameType frame);
    size_t scan_once();
};

} // namespace macha
