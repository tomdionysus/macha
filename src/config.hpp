// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ffmpeg_log.hpp"
#include "log.hpp"
#include "types.hpp"
#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

namespace macha {

struct StorageBackendConfig {
    std::filesystem::path path;
    uint64_t limit{};
};

struct CacheConfig {
    std::filesystem::path path;
    size_t max_blocks{};
    bool prefer_metadata{true};
};

struct MaintenanceConfig {
    std::chrono::milliseconds interval{1000};
    std::chrono::milliseconds foreground_quiet{2000};
    // Minimum retirement/orphan age before authoritative reachability GC may
    // reclaim an unreferenced physical object.
    std::chrono::milliseconds garbage_grace{std::chrono::hours(24)};
    double busy_bandwidth_fraction{0.0};
    double idle_bandwidth_fraction{0.10};
    double cpu_target{0.10};
    uint64_t initial_bandwidth{32ULL * 1024 * 1024};
    uint64_t max_bandwidth{}; // 0 = no configured cap; observed bandwidth is still used.
    double scrub_fraction{0.02};
    // A complete no-op maintenance pass must not immediately rescan the same
    // settled object set simply because byte credit remains available. Five
    // minutes is deliberately long: a healthy settled media server should be
    // close to quiescent rather than continuously proving that it is settled.
    std::chrono::milliseconds no_progress_backoff{300000};
};

struct FuseOperationTimeouts {
    std::chrono::milliseconds lookup{1000};
    std::chrono::milliseconds namespace_mutation{3000};
    std::chrono::milliseconds read{10000};
    std::chrono::milliseconds write{5000};
    std::chrono::milliseconds sync{5000};
    std::chrono::milliseconds lifecycle{2000};
};

struct FuseConfig {
    // Expose a FUSE mount to users other than the process that mounted it.
    bool allow_other{};

    // Short-lived kernel-side namespace/attribute caches.
    std::chrono::milliseconds entry_timeout{250};
    std::chrono::milliseconds attr_timeout{250};
    std::chrono::milliseconds negative_timeout{100};

    // Every kernel-facing operation is bounded by its semantic class and this
    // absolute ceiling. The default remains far below macFUSE's 60s eject timeout.
    FuseOperationTimeouts timeouts;
    std::chrono::milliseconds absolute_request_timeout{15000};

    // Local request broker and asynchronous publication queues.
    size_t request_workers{24};
    size_t max_pending_requests{4096};
    size_t commit_workers{8};
    // While mounted filesystem activity is recent, cap asynchronous data
    // publication so local spool acceptance remains responsive. Once the
    // frontend is quiet, all commit_workers may drain the backlog.
    size_t foreground_commit_workers{1};
    std::chrono::milliseconds publication_quiet{5000};
    size_t max_pending_operations{4096};

    // FUSE demand is emitted into the existing hydration scheduler.
    uint32_t hydration_priority{2000};
    size_t read_ahead_extents{2};
    std::chrono::milliseconds hint_lifetime{5000};
    bool write_through_cache{true};

    // Local metadata refresh and external mount-loss containment.
    bool fail_closed_mountpoint{true};
    std::chrono::milliseconds watchdog_interval{1000};
};

struct FilesystemConfig {
    // These values become the ownership/mode of the distributed filesystem
    // root when a brand-new metadata group is formed. They are stored in DHT
    // metadata thereafter; changing the config does not rewrite an existing
    // filesystem root.
    uint32_t root_uid{};
    uint32_t root_gid{};
    uint32_t root_mode{0755};
};

struct CatalogueApiConfig {
    bool enabled{};
    std::string listen{"127.0.0.1"};
    uint16_t port{7438};
    std::optional<std::filesystem::path> token_file;
    size_t max_request_bytes{8 * 1024 * 1024};
    size_t workers{16};
    size_t max_queued_connections{128};
    size_t stream_chunk_bytes{256 * 1024};
};

struct CatalogueTmdbConfig {
    bool enabled{true};
    std::optional<std::filesystem::path> token_file;
    std::string language{"en-GB"};
    std::string image_size{"w500"};
};

struct CatalogueMusicBrainzConfig {
    bool enabled{true};
    std::string contact{"https://github.com/tomdionysus/macha"};
    std::string cover_size{"500"};
};

struct CatalogueDiscogsConfig {
    bool enabled{false};
    std::optional<std::filesystem::path> token_file;
};

struct CatalogueMovieProviderConfig {
    bool enabled{true};
    std::vector<std::string> roots{"/Movies"};
    CatalogueTmdbConfig tmdb;
};

struct CatalogueTvProviderConfig {
    bool enabled{true};
    std::vector<std::string> roots{"/TV"};
    CatalogueTmdbConfig tmdb;
};

struct CatalogueMusicProviderConfig {
    bool enabled{true};
    std::vector<std::string> roots{"/Music"};
    CatalogueMusicBrainzConfig musicbrainz;
    CatalogueDiscogsConfig discogs;
};

struct CatalogueScannerConfig {
    bool enabled{};
    std::chrono::milliseconds interval{std::chrono::hours(6)};
    // Namespace mutations are coalesced before a scan. Large copies commonly
    // publish many metadata generations; wait for a quiet period where possible,
    // but cap deferral so sustained writes still produce occasional catalogue scans.
    std::chrono::milliseconds rescan_debounce{10000};
    std::chrono::milliseconds rescan_max_delay{600000};
    // Bound online provider metadata work per pass. When the budget is exhausted,
    // the scanner commits the completed discoveries and schedules a continuation
    // rather than monopolising a scanner pass with remote lookups.
    size_t max_provider_requests_per_scan{32};
    std::chrono::milliseconds provider_batch_delay{30000};
    size_t max_artwork_bytes{16 * 1024 * 1024};
    CatalogueMovieProviderConfig movies;
    CatalogueTvProviderConfig tv;
    CatalogueMusicProviderConfig music;
};

struct CatalogueConfig {
    CatalogueApiConfig api;
    CatalogueScannerConfig scanner;
};



struct IngestConfig {
    bool enabled{false};
    // Host-local scratch used by acquisition producers (currently BitTorrent).
    // External filesystem imports may stream directly from their source path.
    std::filesystem::path staging_path;
    uint64_t staging_limit{100ULL * 1024 * 1024 * 1024};
    std::vector<std::filesystem::path> source_roots;
    size_t copy_chunk_bytes{1024 * 1024};
    uint64_t checkpoint_bytes{64ULL * 1024 * 1024};
    std::chrono::milliseconds blocked_retry{5000};
    // Cleanup policy is applied when a terminal job is explicitly cleared.
    // "owned" means an internal producer such as the BitTorrent staging tree.
    bool delete_owned_source_on_clear{true};
    bool delete_external_source_on_clear{false};
    bool delete_owned_source_on_cancel{true};
};

struct TorrentSearchProviderConfig {
    bool enabled{true};
    std::string name;
    std::string type{"torznab"};
    std::string url;
    std::optional<std::filesystem::path> api_key_file;
    size_t max_results{100};
};

struct TorrentConfig {
    bool enabled{false};
    size_t max_active{4};
    uint64_t max_download_rate{}; // bytes/s, 0 = unlimited
    uint64_t max_upload_rate{};   // bytes/s, 0 = unlimited
    bool dht{true};
    bool pex{true};
    bool lsd{true};
    std::vector<TorrentSearchProviderConfig> search_providers;
};

struct StreamingConfig {
    bool enabled{};
    // libav is linked into Macha. temp_path is only an overflow store for old
    // generated fragments; active publication is in memory.
    std::optional<std::filesystem::path> temp_path;
    size_t max_sessions{8};
    size_t max_video_transcodes{1};
    size_t max_audio_transcodes{4};
    std::chrono::milliseconds session_idle{std::chrono::minutes(30)};
    std::chrono::milliseconds startup_timeout{15000};
    std::chrono::milliseconds segment_duration{4000};
    size_t max_ahead_segments{8};
    uint64_t segment_memory_bytes{64ULL * 1024 * 1024};
    uint64_t probe_bytes{8ULL * 1024 * 1024};
    std::chrono::milliseconds probe_analyze_duration{5000};
    std::chrono::milliseconds probe_timeout{20000};
};

struct HydrationEngineConfig {
    bool enabled{true};
    uint32_t priority{};
};

struct HydrationConfig {
    bool enabled{true};
    std::chrono::milliseconds interval{100};
    std::chrono::milliseconds active_timeout{30000};
    size_t max_inflight{4};
    HydrationEngineConfig read_ahead{true, 1000};
    HydrationEngineConfig current_file{true, 700};
    HydrationEngineConfig catalogue{true, 300};
    size_t catalogue_lookahead{1};
};

struct Config {
    std::filesystem::path state_path;
    std::vector<StorageBackendConfig> storage_backends;
    CacheConfig cache;
    MaintenanceConfig maintenance;
    FilesystemConfig filesystem;
    FuseConfig fuse;
    CatalogueConfig catalogue;
    IngestConfig ingest;
    TorrentConfig torrent;
    StreamingConfig streaming;
    HydrationConfig hydration;

    std::filesystem::path key_file;
    std::optional<std::filesystem::path> mount_path;
    std::optional<std::filesystem::path> config_file;
    std::string listen_host{"0.0.0.0"};
    std::string advertise_host;
    std::string failure_domain;
    uint16_t port{7437};
    size_t replication{3};
    size_t metadata_replication{3};
    size_t min_write_replicas{1};
    std::chrono::milliseconds write_stall{2500};
    size_t extent_size{16 * 1024 * 1024};
    size_t read_ahead_extents{3};
    std::vector<Endpoint> bootstrap;
    std::chrono::milliseconds heartbeat{5000};
    std::chrono::milliseconds dead_after{30000};
    std::chrono::milliseconds connect_timeout{2500};
    size_t max_frame_size{256 * 1024};
    // These are observability thresholds only. They never terminate a healthy
    // RPC; 0 disables the corresponding stalled-request DEBUG message.
    std::chrono::milliseconds control_stall_notice{5000};
    std::chrono::milliseconds data_stall_notice{120000};
    std::chrono::milliseconds metadata_cache{250};
    LogLevel log_level{LogLevel::info};
    FfmpegLogLevel ffmpeg_log_level{FfmpegLogLevel::error};
};

Config parse_config(int, char**);
Config load_yaml_config(const std::filesystem::path&);
Config normalize_config(Config);
uint64_t parse_size(const std::string&);
Endpoint parse_endpoint(const std::string&, uint16_t default_port);
void print_usage(const char* executable);
} // namespace macha
