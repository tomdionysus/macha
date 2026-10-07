// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "media/ffmpeg_log.hpp"
#include "log.hpp"
#include "retry_policy.hpp"
#include "types.hpp"
#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

namespace macha {

// `automatic` is resolved at run time from evidence (inbound_capable is
// re-checked periodically); the node gossips the resolved value.
enum class Tristate : uint8_t { yes, no, automatic };

std::string_view tristate_name(Tristate) noexcept;
// Accepts true/false/auto and the usual YAML spellings of the booleans.
Tristate parse_tristate(std::string_view value, const char* what);

struct StorageBackendConfig {
    std::filesystem::path path;
    // Maximum authoritative DATA bytes; metadata/control objects never count.
    uint64_t limit{};
    // Free space kept for the OS, control state and crash recovery; data
    // admission stops before crossing it.
    uint64_t reserve_free{};
};

struct StoragePackingConfig {
    // Immutable DATA objects at or below this size go into append-only packs
    // (a local physical representation only).
    size_t threshold{1024 * 1024};
    // Approximate; one record may push the final pack slightly over.
    size_t target_size{64 * 1024 * 1024};
};

struct MetadataObjectStoreConfig {
    // Empty means <state_path>/metadata-objects.
    std::filesystem::path path;
    // Ceiling for content-addressed control objects such as catalogue shards.
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
    // The absence horizon. An object's bytes go only once this node has seen
    // it unreferenced for this long, which is how long a node may be away and
    // still find what its branch refers to; a node unheard of for this long
    // is forgotten.
    std::chrono::milliseconds garbage_grace{std::chrono::hours(24 * 30)};
    // Rebalance and scrub only; repair always earns credit at the idle fraction
    // and shares time by the weights below.
    double busy_bandwidth_fraction{0.0};
    double idle_bandwidth_fraction{0.10};
    // Duty-cycle weights while repair and a higher class (viewer, interactive,
    // loader) are both runnable. Repair is paced, never stopped, so a node
    // that is always busy still restores lost copies (law 4).
    size_t foreground_weight{95};
    size_t repair_weight{5};
    double cpu_target{0.10};
    uint64_t initial_bandwidth{32ULL * 1024 * 1024};
    uint64_t max_bandwidth{}; // 0 = no configured cap; observed bandwidth is still used.
    double scrub_fraction{0.02};
    // Full-object scrub campaign period. The next due-time is persisted under
    // state_path so restarts do not postpone it; ordinary reads still verify
    // every object they consume.
    std::chrono::milliseconds scrub_interval{std::chrono::hours(24 * 30)};
    // Pause after a no-op pass, so a settled node stays quiescent instead of
    // rescanning because byte credit remains.
    std::chrono::milliseconds no_progress_backoff{300000};
    // Loader/speculative DATA leases (publication and repair, one extent each)
    // active at once; viewer work never counts. 0 = half the hardware threads,
    // minimum 1.
    size_t background_concurrency{0};
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
    // No mount when unset.
    std::optional<std::filesystem::path> mount_path;

    bool allow_other{};

    // Defaults: state_path/fuse-spool and <spool>/operations.log. The spool
    // holds the unpublished byte backlog; the journal holds the descriptors
    // that make it and optimistic namespace mutations recoverable.
    std::optional<std::filesystem::path> spool_path;
    std::optional<std::filesystem::path> operation_journal_path;

    // Kernel-side namespace/attribute cache lifetimes.
    std::chrono::milliseconds entry_timeout{1000};
    std::chrono::milliseconds attr_timeout{1000};
    std::chrono::milliseconds negative_timeout{500};

    // Per-class bounds plus an absolute ceiling, kept well below macFUSE's 60 s
    // eject timeout.
    FuseOperationTimeouts timeouts;
    std::chrono::milliseconds absolute_request_timeout{15000};

    size_t request_workers{24};
    size_t max_pending_requests{4096};
    // Heap held by write requests not yet in the spool; request counts alone
    // would let large copied callbacks exhaust memory under spool backpressure.
    uint64_t max_pending_write_bytes{32ULL * 1024 * 1024};
    size_t commit_workers{8};
    std::chrono::milliseconds publication_quiet{5000};
    // A worker blocked on retained-memory admission fails after this long with
    // no quantum completing anywhere in the pipeline. A no-progress budget, not
    // a time limit: any completed quantum re-arms it. Zero waits unbounded.
    std::chrono::milliseconds publication_no_progress_deadline{30000};
    // Retry for one file's data publication: 250 ms backoff capped at 30 s;
    // more than 100 failures within 30 minutes parks the file (Status
    // `parked_publications`, manage `parked-publications`).
    RetryPolicy publication_retry{100, std::chrono::minutes(30), std::chrono::milliseconds(250),
                                  std::chrono::seconds(30)};
    // How often a parked file checks whether membership or storage changed,
    // which is when it is tried again. Not read from YAML.
    std::chrono::milliseconds parked_recheck{std::chrono::seconds(5)};
    // Retry for a retryably failing namespace operation. Past the budget it is
    // reported blocked (manage `blocked-namespace-operation`) and keeps trying
    // at the ceiling.
    RetryPolicy namespace_retry{200, std::chrono::minutes(30), std::chrono::milliseconds(50),
                                std::chrono::seconds(5)};
    // Weights while viewer and loader work are both runnable; work-conserving
    // when either is idle.
    size_t viewer_weight{95};
    size_t loader_weight{5};
    // A worker yields after this much spool input so other inodes get service;
    // the in-flight budget bounds concurrently admitted quanta.
    uint64_t publication_quantum_bytes{32ULL * 1024 * 1024};
    uint64_t publication_inflight_bytes{256ULL * 1024 * 1024};
    // Provisional extent data in flight for one file publication, so viewer
    // demand has little started loader I/O to retire. 0 = two extents, capped
    // at one quantum; resolved by validation.
    uint64_t publication_pipeline_bytes{};
    // Inodes holding a provisional writer at once. A writer keeps its extent
    // leases across yields and retryable failures (no spool replay), so with
    // breadth-first scheduling an unbounded open set fills any budget and
    // deadlocks by hold-and-wait. Bounded so the worst case fits the loader
    // reserve; past it scheduling is depth-first over the open set. 0 derives
    // it from the loader reserve; resolved by validation.
    // Soft: the count is tested at selection and the writer opened later, so
    // it can overshoot by up to commit_workers-1.
    size_t publication_max_open_writers{};
    size_t max_pending_operations{4096};
    // Heap for DataOp history, checksum vectors and the publication snapshot,
    // independent of spool bytes so many tiny writes cannot grow it unbounded.
    uint64_t max_operation_metadata_bytes{64ULL * 1024 * 1024};
    // Hard bounds on namespace operations batched into one metadata
    // publication; a single operation is always admitted.
    size_t namespace_batch_operations{256};
    size_t namespace_batch_bytes{256 * 1024};

    // Aggregate write-back admission. Below half the ceiling writes burst at
    // disk speed; above it writers are paced against measured drain rate.
    // spool_reserve_free is a physical free-space floor besides this.
    uint64_t max_spool_bytes{16ULL * 1024 * 1024 * 1024};
    uint64_t spool_reserve_free{2ULL * 1024 * 1024 * 1024};
    // Bound on crash-recovery forensic tails; oldest discarded first.
    uint64_t max_orphan_bytes{1024ULL * 1024 * 1024};
    // New admissions stop at this WAL size; admitted operations may still
    // append completions so the queue drains and the journal resets.
    uint64_t max_operation_journal_bytes{256ULL * 1024 * 1024};

    // FUSE demand feeds the hydration scheduler.
    uint32_t hydration_priority{2000};
    size_t read_ahead_extents{2};
    std::chrono::milliseconds hint_lifetime{5000};
    bool write_through_cache{true};

    bool fail_closed_mountpoint{true};
    // Recover a stale Macha/FUSE mount left by an unclean daemon exit before startup.
    bool unmount_if_mounted{false};
    std::chrono::milliseconds watchdog_interval{1000};
    // Wait for the metadata replica to produce any namespace before reporting
    // a supervisor-retryable fault. Bounds "no metadata at all", not slow
    // startup, hence generous. 0 waits unbounded.
    std::chrono::milliseconds initial_namespace_timeout{std::chrono::minutes(10)};
};

struct FilesystemConfig {
    // Root ownership/mode applied only when a new metadata group forms;
    // changing it later does not rewrite an existing root.
    uint32_t root_uid{};
    uint32_t root_gid{};
    uint32_t root_mode{0755};
};

// Response compression. Only complete in-memory text bodies are eligible;
// streamed bodies are never transformed.
struct HttpCompressionConfig {
    bool enabled{true};
    // Smaller bodies are sent as they are.
    size_t min_bytes{1024};
    // zlib level; 1 suits Pi-class nodes.
    int level{6};
    // Largest web asset compressed on demand when no precompressed .gz sibling
    // exists; larger files are streamed uncompressed.
    size_t max_asset_bytes{4 * 1024 * 1024};
};

struct CatalogueApiConfig {
    bool enabled{};
    std::string listen{"127.0.0.1"};
    uint16_t port{7438};
    // Outer URL advertised to clients (Status nodes[].api_endpoint): scheme,
    // host, optional port; no path. Independent of the `listen`/`port` bind,
    // e.g. behind a TLS proxy. Empty = http://<RPC advertise address>:`port`.
    std::string advertised_endpoint;
    std::optional<std::filesystem::path> token_file;
    size_t max_request_bytes{8 * 1024 * 1024};
    // One non-waiting reactor owns every socket; two pools compute. `workers`
    // is the data lane (catalogue, playback, web assets, blocking reads);
    // `control_workers` the control lane (health, status, session, users), so
    // control never queues behind playback (law 1).
    size_t workers{16};
    size_t control_workers{4};
    // Open connections; this bounds memory.
    size_t max_connections{1024};
    // Requests waiting for a lane worker before the reactor answers 503.
    size_t max_queued_requests{256};
    // Checked by the reactor, not a socket option.
    std::chrono::milliseconds client_io_timeout{30000};
    size_t stream_chunk_bytes{256 * 1024};
    // Chunks a stream may hold ahead of the client; worst-case streaming memory
    // is connections x staging_chunks x stream_chunk_bytes.
    size_t staging_chunks{2};
    size_t keep_alive_max_requests{100};
    std::chrono::milliseconds keep_alive_idle_timeout{15000};
    // Slower handlers are logged with their route.
    std::chrono::milliseconds slow_request_threshold{1000};
    // A longer reactor pass counts as a stall in diagnostics; the reactor must
    // never call anything that sleeps.
    std::chrono::milliseconds reactor_stall_threshold{50};
    // Applied on a lane worker, never on the reactor.
    HttpCompressionConfig compression;
    // Lifetime of signed artwork URLs (.../artwork/{id}?exp=&sig=), usable by a
    // plain <img src>. URLs are identical within a bucket of this length and
    // max-age equals it, so a long TTL keeps browser caches warm. An expired
    // URL is recovered by re-fetching the catalogue item.
    std::chrono::milliseconds artwork_capability_ttl{std::chrono::hours(24 * 30)};
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
    // Namespace mutations are coalesced: wait for a quiet period, but at most
    // rescan_max_delay under sustained writes.
    std::chrono::milliseconds rescan_debounce{10000};
    std::chrono::milliseconds rescan_max_delay{600000};
    // Provider lookups per pass; past it the scanner commits and schedules a
    // continuation.
    size_t max_provider_requests_per_scan{32};
    std::chrono::milliseconds provider_batch_delay{30000};
    size_t max_artwork_bytes{16 * 1024 * 1024};
    // A file is not catalogued when a word of its path below the scanner root
    // equals one of these, ignoring case. Words are the runs of letters and
    // digits, so "sample" matches `Sample/film.mkv` and `film-sample.mkv` but
    // not `Samples of Joy.mkv`.
    std::vector<std::string> ignore_terms{"sample"};
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
    // Host-local scratch for acquisition producers (BitTorrent); external
    // imports stream from their source path.
    std::filesystem::path staging_path;
    uint64_t staging_limit{100ULL * 1024 * 1024 * 1024};
    std::vector<std::filesystem::path> source_roots;
    size_t copy_chunk_bytes{1024 * 1024};
    uint64_t checkpoint_bytes{64ULL * 1024 * 1024};
    std::chrono::milliseconds blocked_retry{5000};
    // Concurrent imports. Each holds a WriteHandle, so this also bounds
    // retained publication memory. Applied at start(); a live change waits for
    // restart.
    size_t max_concurrent_jobs{10};
    // Applied when a terminal job is cleared. "Owned" sources belong to an
    // internal producer such as the BitTorrent staging tree.
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
    // Torrent file I/O threads. Their I/O is admitted by the DATA arbiter at
    // loader class and timed by the disk service monitor
    // (src/torrent/torrent_disk_io.hpp).
    size_t disk_threads{2};
    // Threshold for libtorrent alerts bridged into the journal, independent of
    // log_level. INFO keeps listen, DHT bootstrap, port mapping and WARNs;
    // DEBUG bridges every subscribed alert; ALL also subscribes tracker, peer
    // and internal log categories.
    LogLevel log_level{LogLevel::info};
    bool dht{true};
    bool pex{true};
    bool lsd{true};
    // libtorrent's own UPnP/NAT-PMP mapping of its listen port. Separate from
    // network.upnp, which maps the cluster RPC port.
    bool upnp{true};
    bool natpmp{true};
    // Empty binds the node's advertised address; libtorrent's wildcard default
    // can miss a wireless-only link. Override in libtorrent syntax, e.g.
    // "wlan0:6881".
    std::string listen_interfaces;
    uint16_t listen_port{6881};
    // False drains: existing jobs continue, no new ones, listed with
    // not_accepting_reason `draining`.
    bool accept_new_jobs{true};
    // Default remove_after_ms for adds that omit it (0..24 h; absent = off):
    // delay before a completed torrent's jobs and staging payload are removed.
    std::optional<std::chrono::milliseconds> remove_on_complete_after;
    std::vector<TorrentSearchProviderConfig> search_providers;
};

struct StreamingConfig {
    bool enabled{};
    // Overflow store for old generated fragments; active output is in memory.
    std::optional<std::filesystem::path> temp_path;
    // Node-wide. Must stay above max_sessions_per_account: the node limit is
    // checked first, so otherwise the account cap (a distinct refusal to the
    // client) is unreachable. Defaults match macha.yaml.example.
    size_t max_sessions{64};
    // Playback sessions per account on this node; 0 = only max_sessions
    // applies. Guards against a runaway client. Deliberately generous: four
    // viewers holding two sessions each, three during failover, plus sessions
    // stranded for session_idle by a failover cascade. Too low presents as
    // failover silently failing, not as a cap; transcodes, the scarce
    // resource, are bounded separately.
    size_t max_sessions_per_account{32};
    // Transcode entitlements (video or audio) one account may hold on this
    // node, so one account cannot take every slot. 0 = no bound.
    size_t max_transcodes_per_account{2};
    size_t max_video_transcodes{1};
    size_t max_audio_transcodes{4};
    // Per transcoded video; with max_video_transcodes, bounds decoder threads.
    size_t video_decoder_threads{2};
    // x264 frame threads per video transcode; 0 uses every hardware thread.
    size_t video_encoder_threads{0};
    std::chrono::milliseconds session_idle{std::chrono::minutes(30)};
    // Expiry for a session that has never served a stream object, so a client
    // that vanished before playing frees its transcode slot quickly. A session
    // that served even one object keeps session_idle.
    std::chrono::milliseconds session_unused_idle{std::chrono::seconds(120)};
    // Reclaim an abandoned physical encoder while retaining the logical
    // session long enough for client retry/reconciliation.
    std::chrono::milliseconds pipeline_idle{std::chrono::seconds(60)};
    // A transcode entitlement with no stream activity this long is released,
    // so an unclean client exit cannot hold a slot for session_idle. Keyed on
    // stream activity, not control traffic; a long-paused viewer reacquires on
    // resume and may get 429. Must lie between pipeline_idle and session_idle.
    std::chrono::milliseconds transcode_entitlement_idle{std::chrono::minutes(5)};
    std::chrono::milliseconds startup_timeout{15000};
    // `start=async`: fails only after this long without progress; planning
    // stays bounded by probe_timeout.
    std::chrono::milliseconds startup_no_progress{15000};
    // Longest hold for a start long-poll (GET ?after=&wait_ms=).
    std::chrono::milliseconds start_wait_max{25000};
    // How long a failed async start stays readable.
    std::chrono::milliseconds start_failed_retention{60000};
    std::chrono::milliseconds segment_duration{4000};
    size_t max_ahead_segments{8};
    uint64_t segment_memory_bytes{64ULL * 1024 * 1024};
    uint64_t probe_bytes{8ULL * 1024 * 1024};
    std::chrono::milliseconds probe_analyze_duration{5000};
    std::chrono::milliseconds probe_timeout{20000};
    // A request for a not-yet-produced segment is held only within this
    // distance, which matches max_ahead_segments: inside it production is
    // working toward the segment.
    size_t segment_hold_window{8};
    // One in flight plus one prefetch.
    size_t max_session_holds{2};
    // Node-wide fairness and memory bound. A held request is a parked
    // continuation, not a thread, and holds are ordinary in steady playback.
    size_t max_concurrent_holds{64};
    // A held request sends no bytes, so the hold must end (503) inside the
    // tightest client time-to-first-byte deadline; past it the client takes its
    // timeout path instead of retrying the 503. Deadlines from shipped code:
    //   hls.js 1.6.18   fragLoadPolicy.default.maxTimeToFirstByteMs  10000
    //   expo-video      bare OkHttpClient default readTimeout        10000
    //   RN track player media3 DEFAULT_READ_TIMEOUT_MILLIS            8000
    //   iOS AVFoundation                                            unknown
    // hls.js `fragLoadingTimeOut` (20000) is deprecated and inert here.
    std::chrono::milliseconds segment_timeout{6000};
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

// The web client served at the node root. Without a root directory non-API
// paths answer 404.
struct WebConfig {
    bool enabled{true};
    // Built client assets; no request path can escape it.
    std::filesystem::path root;
    // Served for any path that is not a file under `root` (SPA deep links).
    std::string index{"index.html"};
};

struct RuntimeConfig {
    // Caps glibc arenas so short-lived codec threads cannot ratchet retained memory.
    size_t glibc_arena_max{4};
    // Heap retained across asynchronous subsystem boundaries. Reserves are
    // priority headroom inside this total, not extra capacity.
    uint64_t retained_memory_bytes{768ULL * 1024 * 1024};
    uint64_t control_memory_reserve_bytes{64ULL * 1024 * 1024};
    uint64_t viewer_memory_reserve_bytes{192ULL * 1024 * 1024};
    uint64_t loader_memory_reserve_bytes{64ULL * 1024 * 1024};
    // Headroom only inbound RPC frame reassembly may use. Publication holds
    // bytes until a peer confirms, and the confirmation is reassembled into
    // this ledger, so without it the ledger deadlocks. Small on purpose: a few
    // frames at `max_frame_size`; absolute priority would starve publication.
    uint64_t reassembly_memory_reserve_bytes{32ULL * 1024 * 1024};
};

struct SessionConfig {
    // Fixed lifetime from creation (no sliding renewal); then POST /api/v1/session again.
    std::chrono::milliseconds anonymous_ttl{std::chrono::hours(24 * 30)};
    size_t max_sessions{4096};
    // POST /api/v1/session without credentials mints a session for the
    // "anonymous" account, with that account's current roles (set through the
    // users API). False requires a login for everything.
    bool allow_anonymous{true};
    size_t max_users{4096};
    // Local brake on unauthenticated scrypt work; not replicated.
    size_t max_concurrent_password_checks{2};
    size_t failed_login_attempts{5};
    std::chrono::milliseconds failed_login_lockout{std::chrono::seconds(30)};
};

struct Config {
    std::filesystem::path state_path;
    std::vector<StorageBackendConfig> storage_backends;
    // storage.hosts_extents: whether this node may hold DATA. `no` is an edge
    // node serving from cache (storage.data may be empty). `automatic` is `no`
    // without data backends or inbound connectivity, `yes` otherwise.
    Tristate hosts_extents{Tristate::automatic};
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
    WebConfig web;
    // Startup gate: the process exits for its supervisor when local-state
    // recovery makes no progress (startup_progress.hpp) for this long. The
    // absolute timeout is off at 0 so slow but progressing recovery never
    // crash-loops.
    std::chrono::milliseconds service_startup_no_progress{120000};
    std::chrono::milliseconds service_startup_timeout{0};

    std::filesystem::path key_file;
    // Plugin directory SubsystemSupervisor scans. Unset = the compile-time
    // kDefaultPluginDir (MACHA_PLUGIN_INSTALL_DIR), not the executable's bindir.
    std::optional<std::filesystem::path> plugin_path;
    std::optional<std::filesystem::path> config_file;
    std::string listen_host{"0.0.0.0"};
    std::string advertise_host;
    std::string failure_domain;
    // Display name shown in Status; nothing routes on it. Empty = none.
    std::string node_name;
    uint16_t port{7437};
    // network.inbound_capable: can peers dial this node? `no` (CGNAT): it dials
    // out and peers answer on its sessions. `automatic` is decided by a peer
    // dial-back (MessageType::dial_back_probe), persisted and re-checked.
    Tristate inbound_capable{Tristate::automatic};
    // Dial-back re-check cadence by current result. Not read from YAML.
    std::chrono::milliseconds inbound_reprobe_while_incapable{std::chrono::minutes(10)};
    std::chrono::milliseconds inbound_reprobe_while_capable{std::chrono::hours(1)};
    // Per-asker rate limit on answering dial_back_probe, which dials an
    // address the asker names. Not read from YAML.
    std::chrono::milliseconds dial_back_probe_min_interval{std::chrono::seconds(10)};
    UpnpConfig upnp;
    ExternalIpConfig external_ip;
    ConnectivityCheckConfig connectivity_check;
    size_t replication{3};
    // Copies sought before a write returns: of a metadata commit, and of a
    // DATA object. A write is accepted on one; the rest are owed to repair
    // when no further node present can take one.
    size_t metadata_write_copies{2};
    size_t write_copies{1};
    // retain_data() batches presence checks into have_objects RPCs: ids per
    // message (well under max_frame_size), and batches in flight across all peers.
    size_t retention_check_batch_size{2000};
    size_t retention_check_concurrency{8};
    std::chrono::milliseconds write_stall{2500};
    size_t extent_size{16 * 1024 * 1024};
    // DATA admission; lower classes may use only the non-viewer-reserved part.
    uint64_t data_inflight_bytes{128ULL * 1024 * 1024};
    uint64_t data_viewer_reserve_bytes{32ULL * 1024 * 1024};
    // Device service-time defence. Expected cost per operation is overhead plus
    // per-MiB; pressure is the moving average of actual/expected. Under
    // pressure loader and speculative admission is held at
    // io_pressure_min_background leases; viewers are never gated.
    uint32_t io_pressure_overhead_ms{25};
    uint32_t io_pressure_per_mib_ms{120};
    uint32_t io_pressure_slowdown_percent{300};
    uint32_t io_pressure_release_percent{150};
    // One operation this far past its expected cost trips pressure at once,
    // whatever the average.
    uint32_t io_pressure_outlier_percent{1000};
    uint32_t io_pressure_min_background{1};
    // A DATA credit wait fails after this long with no lease released anywhere
    // in the arbiter, i.e. only when it is wedged. Zero waits unbounded.
    std::chrono::milliseconds data_credit_no_progress_deadline{std::chrono::seconds(120)};
    size_t read_ahead_extents{3};
    std::vector<Endpoint> bootstrap;
    std::chrono::milliseconds heartbeat{5000};
    // Telemetry gossip period; bounds staleness of peer figures in Status.
    // Readiness changes and peer observations publish sooner, at most once
    // per second.
    std::chrono::milliseconds telemetry_interval{10000};
    std::chrono::milliseconds dead_after{30000};
    std::chrono::milliseconds connect_timeout{2500};
    size_t max_frame_size{256 * 1024};
    // Stalled-request DEBUG thresholds only; 0 disables.
    std::chrono::milliseconds control_stall_notice{5000};
    std::chrono::milliseconds data_stall_notice{120000};
    // A call with no progress this long fails transiently for the caller to
    // retry. 0 disables; off for data, whose transfers have their own logic.
    std::chrono::milliseconds control_no_progress_deadline{30000};
    std::chrono::milliseconds data_no_progress_deadline{0};
    std::chrono::milliseconds metadata_cache{250};
    // Budget for immutable metadata records and decoded snapshots; history
    // payloads stay on disk outside it.
    uint64_t metadata_materialization_cache_bytes{128ULL * 1024 * 1024};
    LogLevel log_level{LogLevel::info};
    FfmpegLogLevel ffmpeg_log_level{FfmpegLogLevel::error};
};

Config parse_config(int, char**);
Config load_yaml_config(const std::filesystem::path&);
// Legal but notable settings, logged once at start-up by the caller.
std::vector<std::string> configuration_warnings(const Config&);
Config normalize_config(Config);
uint64_t parse_size(const std::string&);
Endpoint parse_endpoint(const std::string&, uint16_t default_port);
void print_usage(const char* executable);
} // namespace macha
