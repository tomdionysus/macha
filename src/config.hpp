// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ffmpeg_log.hpp"
#include "log.hpp"
#include "retry_policy.hpp"
#include "types.hpp"
#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

namespace macha {

struct StorageBackendConfig {
    std::filesystem::path path;
    // Maximum authoritative DATA bytes admitted to this backend. Metadata/control
    // objects never consume this quota.
    uint64_t limit{};
    // Minimum physical filesystem free space preserved for the operating system,
    // Macha control state and crash recovery. Data admission stops before crossing it.
    uint64_t reserve_free{};
};

struct StoragePackingConfig {
    // Immutable DATA objects at or below this size are packed into append-only
    // containers. Packing is purely a local physical representation.
    size_t threshold{1024 * 1024};
    // Rotate packs at approximately this physical size. A single record may make
    // the final pack slightly larger.
    size_t target_size{64 * 1024 * 1024};
};

struct MetadataObjectStoreConfig {
    // Empty means <state_path>/metadata-objects.
    std::filesystem::path path;
    // Safety ceiling for content-addressed control objects such as catalogue shards.
    uint64_t limit{4ULL * 1024 * 1024 * 1024};
    StoragePackingConfig packing{};
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
    // Proactive full-object integrity scrub is a campaign, not continuous idle
    // work. The next campaign due-time is persisted under state_path so daemon
    // restarts do not restart or continually postpone the schedule. Ordinary
    // reads still authenticate and content-hash every object they consume.
    std::chrono::milliseconds scrub_interval{std::chrono::hours(24 * 30)};
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
    // Filesystem path to mount at. No mount is attempted when unset.
    std::optional<std::filesystem::path> mount_path;

    // Expose a FUSE mount to users other than the process that mounted it.
    bool allow_other{};

    // Durable local admission state. By default the spool remains at
    // state_path/fuse-spool and the operation journal lives inside it as
    // operations.log. Either location may
    // be moved independently. The spool can contain the full unpublished byte
    // backlog; the journal contains only the descriptors which make those bytes
    // and optimistic namespace mutations recoverable after restart.
    std::optional<std::filesystem::path> spool_path;
    std::optional<std::filesystem::path> operation_journal_path;

    // Short-lived kernel-side namespace/attribute caches.
    std::chrono::milliseconds entry_timeout{1000};
    std::chrono::milliseconds attr_timeout{1000};
    std::chrono::milliseconds negative_timeout{500};

    // Every kernel-facing operation is bounded by its semantic class and this
    // absolute ceiling. The default remains far below macFUSE's 60s eject timeout.
    FuseOperationTimeouts timeouts;
    std::chrono::milliseconds absolute_request_timeout{15000};

    // Local request broker and asynchronous publication queues.
    size_t request_workers{24};
    size_t max_pending_requests{4096};
    // Heap bytes owned by FUSE write requests before they reach the durable
    // spool. Request-count admission alone permits thousands of large copied
    // callbacks to exhaust memory while spool backpressure is working.
    uint64_t max_pending_write_bytes{32ULL * 1024 * 1024};
    size_t commit_workers{8};
    // Compatibility setting for older configurations. Journal-restored spool
    // is user-requested loader work and is no longer demoted merely because the
    // process restarted. Retain this value for a future genuinely background
    // recovery lane, or remove it after the compatibility window.
    size_t recovery_commit_workers{2};
    // Retained for configuration compatibility. Viewer demand now gates new
    // publication work directly, while loader activity is work-conserving up to
    // commit_workers; treating every mounted write as foreground caused bulk
    // imports to remain permanently single-threaded.
    size_t foreground_commit_workers{1};
    std::chrono::milliseconds publication_quiet{5000};
    // Retry discipline for one file's data publication (discipline 2 of the
    // self-healing plan). Backoff starts small so a namespace race resolves
    // in a blink, caps so a peer outage costs at most one attempt per
    // ceiling, and a file that keeps failing is parked for an operator
    // (Status `parked_publications`, manage `parked-publications`) instead
    // of retrying forever. Defaults: 250 ms → 30 s, park after more than
    // 100 failures within 30 minutes (~45 min of failing).
    RetryPolicy publication_retry{100, std::chrono::minutes(30), std::chrono::milliseconds(250),
                                  std::chrono::seconds(30)};
    // Same discipline for a namespace operation failing retryably (write
    // floor unavailable, peer down). Past the budget it is reported as
    // blocked (manage `blocked-namespace-operation`) and keeps trying at the
    // ceiling, so a cleared cause still resolves it without an operator.
    RetryPolicy namespace_retry{200, std::chrono::minutes(30), std::chrono::milliseconds(50),
                                std::chrono::seconds(5)};
    // Relative service weights while genuine viewer traffic and loader
    // publication are both runnable. Capacity is work conserving: either
    // class may borrow all of it while the other is idle.
    size_t viewer_weight{95};
    size_t loader_weight{5};
    // In-process deterministic crash-fixture hook. It is intentionally absent
    // from YAML/CLI parsing and cannot disable loader service in production.
    bool suspend_loader_for_tests{};
    // In-process transient-publication fixture. Zero disables it. This is not
    // parsed from configuration; tests use it to prove that a retryable error
    // after bounded spool progress retains the exact publication cursor.
    uint64_t fail_publication_once_after_spool_bytes_for_tests{};
    // A publication worker yields after this much durable spool input so other
    // inodes and newly closed files receive service without restarting the
    // generation. The aggregate budget bounds concurrently admitted quanta.
    uint64_t publication_quantum_bytes{32ULL * 1024 * 1024};
    uint64_t publication_inflight_bytes{256ULL * 1024 * 1024};
    // Maximum provisional extent data admitted concurrently by one file
    // publication. This is bounded independently from the aggregate quantum
    // budget so viewer demand has only a small, known amount of already-started
    // loader I/O to retire.
    // Zero selects two extents, capped at one publication quantum. Validation
    // resolves this to an effective byte value before service startup.
    uint64_t publication_pipeline_bytes{};
    size_t max_pending_operations{4096};
    // Conservative retained-heap budget for durable DataOp history, checksum
    // vectors and the publication snapshot which may coexist with it. This is
    // independent of spool payload bytes: many tiny writes/truncates must not
    // turn a bounded spool into an unbounded descriptor heap.
    uint64_t max_operation_metadata_bytes{64ULL * 1024 * 1024};
    // Ordered namespace recovery/backlog operations may share one metadata
    // publication. Both limits are hard bounds; a single operation is always
    // admitted so an unusually large rename cannot deadlock the queue.
    size_t namespace_batch_operations{256};
    size_t namespace_batch_bytes{256 * 1024};

    // Aggregate local write-back admission. Queue counts alone do not bound a
    // hot inode: one pending publication can otherwise accumulate arbitrary
    // spool bytes. Below half this ceiling admission may burst at local disk
    // speed; above it the frontend starts publication and paces writers against
    // measured drain throughput. Preserve a physical free-space floor
    // independently of the logical byte ceiling.
    uint64_t max_spool_bytes{16ULL * 1024 * 1024 * 1024};
    uint64_t spool_reserve_free{2ULL * 1024 * 1024 * 1024};
    // Crash-recovery forensic tails are useful, but never authoritative. Keep
    // their aggregate disk usage bounded and discard oldest evidence first.
    uint64_t max_orphan_bytes{1024ULL * 1024 * 1024};
    // New durable operation admissions stop at this WAL size. Already-admitted
    // operations may append their completion records so the queue can drain to
    // zero and trigger the existing atomic journal reset.
    uint64_t max_operation_journal_bytes{256ULL * 1024 * 1024};

    // FUSE demand is emitted into the existing hydration scheduler.
    uint32_t hydration_priority{2000};
    size_t read_ahead_extents{2};
    std::chrono::milliseconds hint_lifetime{5000};
    bool write_through_cache{true};

    // Local metadata refresh and external mount-loss containment.
    bool fail_closed_mountpoint{true};
    // Recover a stale Macha/FUSE mount left by an unclean daemon exit before startup.
    bool unmount_if_mounted{false};
    std::chrono::milliseconds watchdog_interval{1000};
};

struct FilesystemConfig {
    // These values become the ownership/mode of the MachaDFS namespace
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
    // Optional override for how this node's API is advertised to cluster
    // peers (Status nodes[].api_host/api_port), distinct from listen/port
    // above. Covers NAT/port-forwarding, where the bind address isn't what a
    // peer should dial. Empty host defaults to this node's resolved RPC
    // advertise address (network.advertise, or its own fallback) rather than
    // `listen` above, since `listen` is conventionally a wildcard bind
    // (0.0.0.0) and not itself dialable. Zero port defaults to `port` above.
    std::string advertised_host;
    uint16_t advertised_port{0};
    std::optional<std::filesystem::path> token_file;
    size_t max_request_bytes{8 * 1024 * 1024};
    size_t workers{16};
    size_t max_queued_connections{128};
    std::chrono::milliseconds client_io_timeout{30000};
    size_t stream_chunk_bytes{256 * 1024};
    size_t keep_alive_max_requests{100};
    std::chrono::milliseconds keep_alive_idle_timeout{15000};
    // Lifetime of a signed artwork capability URL embedded in catalogue
    // responses (GET .../artwork/{id}?exp=...&sig=...), which lets a client
    // load artwork via a plain <img src> without a bearer header. Artwork is
    // content-addressed/immutable and lower-stakes than a playback session,
    // so this deliberately outlives a typical browsing session; the intended
    // recovery for an expired URL is simply re-fetching the catalogue item.
    std::chrono::milliseconds artwork_capability_ttl{std::chrono::hours(24)};
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
    // Per transformed video. Combined with max_video_transcodes this is a
    // hard process-wide upper bound on concurrently requested decoder threads.
    size_t video_decoder_threads{2};
    std::chrono::milliseconds session_idle{std::chrono::minutes(30)};
    // Reclaim an abandoned physical encoder while retaining the logical
    // session long enough for client retry/reconciliation.
    std::chrono::milliseconds pipeline_idle{std::chrono::seconds(60)};
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

struct UpnpConfig {
    bool enabled{};
    uint16_t external_port{}; // 0 = use network.port
    std::chrono::milliseconds discovery_timeout{2000};
    uint32_t lease_seconds{}; // 0 = request a permanent mapping
};

struct ExternalIpConfig {
    bool enabled{};
    std::chrono::milliseconds timeout{3000};
};

struct ConnectivityCheckConfig {
    bool enabled{};
    std::chrono::milliseconds timeout{3000};
};

struct RuntimeConfig {
    // glibc otherwise permits a CPU-derived number of independent arenas,
    // allowing short-lived codec threads to ratchet retained process memory.
    size_t glibc_arena_max{4};
    // Aggregate heap retained across asynchronous subsystem boundaries. The
    // reserves are priority headroom inside this total, not extra capacity.
    uint64_t retained_memory_bytes{768ULL * 1024 * 1024};
    uint64_t control_memory_reserve_bytes{64ULL * 1024 * 1024};
    uint64_t viewer_memory_reserve_bytes{192ULL * 1024 * 1024};
    uint64_t loader_memory_reserve_bytes{64ULL * 1024 * 1024};
};

struct SessionConfig {
    // No sliding renewal in v1 -- a session is valid for this long from
    // creation, then the client must POST /api/v1/session again.
    std::chrono::milliseconds anonymous_ttl{std::chrono::hours(24 * 30)};
    size_t max_sessions{4096};
};

struct Config {
    std::filesystem::path state_path;
    std::vector<StorageBackendConfig> storage_backends;
    StoragePackingConfig storage_packing;
    MetadataObjectStoreConfig metadata_store;
    CacheConfig cache;
    MaintenanceConfig maintenance;
    FilesystemConfig filesystem;
    FuseConfig fuse;
    CatalogueConfig catalogue;
    IngestConfig ingest;
    TorrentConfig torrent;
    StreamingConfig streaming;
    HydrationConfig hydration;
    RuntimeConfig runtime;
    SessionConfig session;
    // Bound on Service::wait_services_ready(). Local-state readiness and
    // subsystem construction/start are expected to complete or throw well
    // inside this window; a wait that never resolves either way is treated as
    // a suspected internal stall rather than left to hang indefinitely.
    // Startup gate (discipline 2). The process is terminated for its
    // supervisor when local-state recovery has made *no progress* (see
    // startup_progress.hpp) for `service_startup_no_progress`; an absolute
    // ceiling `service_startup_timeout` is also honoured when non-zero, and is
    // off by default since 0.30 -- a slow but progressing recovery must never
    // become a crash loop.
    std::chrono::milliseconds service_startup_no_progress{120000};
    std::chrono::milliseconds service_startup_timeout{0};

    std::filesystem::path key_file;
    // Directory SubsystemSupervisor scans for subsystem plugins (.so/.dylib).
    // Defaults to this build's private plugin directory when unset (a
    // compile-time constant, see normalize_config()/kDefaultPluginDir and
    // MACHA_PLUGIN_INSTALL_DIR in CMakeLists.txt) -- deliberately not derived
    // from the running executable's own location, which is bindir, not where
    // a private library/plugin belongs.
    std::optional<std::filesystem::path> plugin_path;
    std::optional<std::filesystem::path> config_file;
    std::string listen_host{"0.0.0.0"};
    std::string advertise_host;
    std::string failure_domain;
    uint16_t port{7437};
    UpnpConfig upnp;
    ExternalIpConfig external_ip;
    ConnectivityCheckConfig connectivity_check;
    size_t replication{3};
    size_t metadata_min_write_replicas{2};
    size_t min_write_replicas{1};
    // retain_data()'s candidate-presence scan batches many extent IDs into one
    // have_objects RPC per peer per round instead of one have_object RPC per
    // (extent, candidate) pair. batch_size bounds ids per wire message (also
    // keeps it well under max_frame_size); concurrency bounds how many such
    // batches may be in flight at once across all peers combined, so an
    // arbitrarily large publication cannot turn into an unbounded fan-out
    // against any one peer.
    size_t retention_check_batch_size{2000};
    size_t retention_check_concurrency{8};
    std::chrono::milliseconds write_stall{2500};
    size_t extent_size{16 * 1024 * 1024};
    // End-to-end DATA object admission. Lower classes may use only the
    // non-reserved portion; viewer reads retain immediate bounded headroom.
    uint64_t data_inflight_bytes{128ULL * 1024 * 1024};
    uint64_t data_viewer_reserve_bytes{32ULL * 1024 * 1024};
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
    // Discipline 2: a call that has made no progress for this long fails
    // with a transient error the caller retries under its own policy,
    // instead of "remaining active while peer health is monitored" for
    // minutes (150-230 s accept_metadata_commit stalls, 2026-09-06). Zero
    // disables. Data-lane transfers keep their own stall/spill logic, so the
    // data deadline is off by default.
    std::chrono::milliseconds control_no_progress_deadline{30000};
    std::chrono::milliseconds data_no_progress_deadline{0};
    std::chrono::milliseconds metadata_cache{250};
    // Logical byte budget for immutable metadata records plus their decoded
    // snapshots. Durable history payloads remain disk-backed outside it.
    uint64_t metadata_materialization_cache_bytes{128ULL * 1024 * 1024};
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
