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

// A configuration value that may be decided by the node itself. `automatic`
// is resolved at run time from evidence (and, for inbound_capable, re-checked
// periodically); the resolved value is what the node gossips.
enum class Tristate : uint8_t { yes, no, automatic };

std::string_view tristate_name(Tristate) noexcept;
// Accepts true/false/auto and the usual YAML spellings of the booleans.
Tristate parse_tristate(std::string_view value, const char* what);

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
    // Background effort ceiling: loader/speculative DATA leases (publication
    // and repair, one extent each) active at once. 0 = half the hardware
    // threads, minimum 1. Viewer work is never counted. Until 0.32.13 gbni-1
    // (4 GB, undervolt-prone) needed an external CPU quota to stay up while
    // publishing (2026-09-07).
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
    // A publication worker blocked on retained-memory admission fails after
    // this long with NO quantum completing anywhere in the pipeline. It is a
    // no-progress budget, not a time limit on publishing: any worker finishing
    // a quantum re-arms it, so a slow node is never punished for being slow.
    // Zero disables it and restores the unbounded wait. Without this the wait
    // had no deadline at all, so a wedged pipeline never failed, never retried
    // and never parked -- es-1 held 492 MB across eight blocked workers with
    // `parked_publications` reading 0 (2026-09-09).
    std::chrono::milliseconds publication_no_progress_deadline{30000};
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
    // Upper bound on inodes holding a provisional writer at once.
    //
    // A writer is deliberately retained across clean yields and retryable
    // failures so a resumed publication never replays spool bytes, and it keeps
    // its retained-memory extent leases while it waits: one buffer being filled
    // plus the pipeline above. Publication scheduling is breadth-first, so
    // without a bound the number of writers holding partial state is simply the
    // width of the backlog. On es-1 that reached 123 leases -- the entire
    // durable-lower budget -- after which every writer needed one more extent
    // and none could release one, and no budget setting could change it because
    // any budget fills the same way (2026-09-09, classic hold-and-wait).
    //
    // Bounded so the open set's worst case fits the loader reserve, a writer
    // waiting on the ledger is waiting for control/viewer work, which releases,
    // rather than for another publication which is itself waiting. Past the
    // bound the scheduler is depth-first over the already-open set, which is
    // what drains a backlog anyway. Zero derives it from the loader reserve;
    // validation resolves it before service startup.
    //
    // This is a SOFT bound. The scheduler tests the count before selecting an
    // inode and the worker opens the writer later, so N workers can each pass
    // the test at bound-1 and overshoot by up to commit_workers-1. Observed
    // live: 9 open against a bound of 8 (es-1, 2026-09-10). The headroom
    // absorbs it -- 9 writers is 108 MB against a 512 MB durable-lower budget
    // -- but the reserve is a target rather than a ceiling until the slot is
    // reserved at selection time the way transcode entitlements already are.
    size_t publication_max_open_writers{};
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
    // How long the frontend waits for this node's metadata replica to produce
    // any namespace at all before giving up and reporting a fault its
    // supervisor can retry and, eventually, show an operator. Before this the
    // wait was unbounded and silent: a node whose replica never became
    // available hung in the FuseFrontend constructor forever with one debug
    // line to show for it.
    //
    // It bounds "no metadata has arrived at all", NOT "startup is slow" --
    // the 2026-09-06 lesson about elapsed-time gates (a 120 s deadline turned
    // a progressing 5-minute replay into a crash loop) is why this is
    // generous by default and why nothing else on the startup path is timed
    // against it. 0 restores the unbounded wait.
    std::chrono::milliseconds initial_namespace_timeout{std::chrono::minutes(10)};
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

// What the server compresses on the way out, and how hard. Only complete
// in-memory text bodies are ever eligible -- a streamed body (media, a large
// client asset) is sent from the reactor without a copy and is never
// transformed. Disabling this is a supported deployment rather than a
// degraded one: a node behind a proxy that already compresses has no reason
// to pay for it twice.
struct HttpCompressionConfig {
    bool enabled{true};
    // Bodies smaller than this are sent as they are. Below roughly a packet
    // there is nothing to win, and gzip's own header is a real fraction of it.
    size_t min_bytes{1024};
    // zlib level. 6 is zlib's own default and the usual balance; 1 gives most
    // of the ratio for a fraction of the CPU, which is the setting a
    // Pi-class node wants.
    int level{6};
    // The largest web-client asset compressed on demand when the client build
    // shipped no precompressed sibling next to it. Past this the file is
    // streamed as it is rather than read whole into memory once per request.
    // A build that emits .gz files never reaches this path at all.
    size_t max_asset_bytes{4 * 1024 * 1024};
};

struct CatalogueApiConfig {
    bool enabled{};
    std::string listen{"127.0.0.1"};
    uint16_t port{7438};
    // How this node's API is advertised to clients (Status
    // nodes[].api_endpoint), as a complete URL: scheme, host and optional
    // port. This describes the OUTER address and `listen`/`port` above
    // describe the inner bind, and the two are deliberately independent --
    // with a TLS-terminating proxy in front of the API, Macha serves plain
    // HTTP on `listen`:`port` while clients must be told
    // `https://host[:443]`. Neither the scheme nor the port of the outer
    // address can be derived from the bind, which is why this is stated
    // rather than constructed.
    //
    // A path is NOT supported and is rejected at configuration: a proxy
    // fronting a node at a subpath is not a deployment this serves, and a
    // client that assumed an origin would silently produce 404s against it.
    //
    // Empty defaults to http:// this node's resolved RPC advertise address
    // (network.advertise, or its own fallback) and `port` above -- not
    // `listen`, which is conventionally a wildcard bind (0.0.0.0) and not
    // itself dialable by anyone else.
    std::string advertised_endpoint;
    std::optional<std::filesystem::path> token_file;
    size_t max_request_bytes{8 * 1024 * 1024};
    // The server is one reactor thread that owns every socket and never
    // waits, plus two bounded pools that only compute (see
    // TODO/2026-09-15-http-server-reactor-plan.md). `workers` is the data
    // lane -- catalogue, playback, web assets, and every body read that can
    // block on a disk or a replica. `control_workers` is the control lane
    // -- health, status, session, users -- so control traffic never queues
    // behind playback (governing law 1, as a data structure).
    size_t workers{16};
    size_t control_workers{2};
    // Open connections, not queued ones: an idle kept-alive connection is an
    // fd and a small struct, not a thread, so this is what bounds memory.
    size_t max_connections{1024};
    // Requests waiting for a lane worker before the reactor answers 503
    // rather than letting the queue grow without bound.
    size_t max_queued_requests{256};
    // Deadlines the reactor checks, not socket options a thread waits under.
    std::chrono::milliseconds client_io_timeout{30000};
    size_t stream_chunk_bytes{256 * 1024};
    // Chunks a streaming response may hold in memory ahead of the client.
    // Memory for streaming is therefore connections x staging_chunks x
    // stream_chunk_bytes, worst case; backpressure from the client's TCP
    // window stops the pump asking for more.
    size_t staging_chunks{2};
    size_t keep_alive_max_requests{100};
    std::chrono::milliseconds keep_alive_idle_timeout{15000};
    // A handler slower than this is logged, from the pool, with its route.
    std::chrono::milliseconds slow_request_threshold{1000};
    // A reactor pass longer than this is counted as a stall in diagnostics.
    // The reactor may not call anything that sleeps; if that rule is ever
    // broken this is where it shows, on the first slow disk.
    std::chrono::milliseconds reactor_stall_threshold{50};
    // Response compression. Applied on a lane worker, never on the reactor:
    // it is CPU work, and the reactor may not do any.
    HttpCompressionConfig compression;
    // Lifetime of a signed artwork capability URL embedded in catalogue
    // responses (GET .../artwork/{id}?exp=...&sig=...), which lets a client
    // load artwork via a plain <img src> without a bearer header. Artwork is
    // content-addressed/immutable and lower-stakes than a playback session,
    // so this deliberately outlives a typical browsing session; the intended
    // recovery for an expired URL is simply re-fetching the catalogue item.
    // Thirty days. The URL is byte-identical within a bucket of this length and
    // the response's max-age equals it, so at 24 h every poster URL changed at
    // UTC midnight and every browser re-downloaded every poster the next day.
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
    // How many jobs may import at once. Each concurrent job holds its own
    // WriteHandle, so this bounds retained publication memory as much as it
    // bounds throughput -- raise it against the durable-lower budget, not on
    // its own. Applied at start(); a live change logs and waits for restart.
    size_t max_concurrent_jobs{10};
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
    // Threads doing the torrent's file I/O. Every read, write and hash they
    // perform is admitted by the DATA arbiter at loader class and timed into
    // the disk service monitor (src/torrent_disk_io.hpp). libtorrent's own
    // backend ran ten, outside both, and on 2026-09-23 they wrote 65 MB/s onto
    // es-1's DATA spindle while macha's own writes waited behind them.
    //
    // This replaces pressure_download_rate, a rate clamp gated on a two-second
    // viewer window that flipped 300 times an hour and limited nothing.
    size_t disk_threads{2};
    // Threshold for the libtorrent alert stream bridged into the journal,
    // independent of the process log_level in the same way ffmpeg_log_level
    // is. INFO (the default) keeps the explicit lines -- listen, DHT
    // bootstrap, port mapping, and every WARN -- and drops the per-alert
    // chatter; DEBUG bridges every alert the session already subscribes to;
    // ALL additionally subscribes the tracker, peer and libtorrent internal
    // log categories. Split out on 2026-09-23 after DHT alerts at DEBUG were
    // 99.9% of gbni-1's journal and had evicted its entire diagnostic
    // record in nine hours.
    LogLevel log_level{LogLevel::info};
    bool dht{true};
    bool pex{true};
    bool lsd{true};
    // libtorrent maps its own listen port with UPnP and NAT-PMP, and both
    // default to on inside libtorrent. Until these existed the session did it
    // regardless of what the rest of the configuration said: a node with
    // network.upnp.enabled false still had libtorrent creating a 6881 mapping
    // on the router, which no setting mentioned and nothing could stop.
    //
    // Deliberately separate from network.upnp, which maps the cluster RPC
    // port through Macha's own miniupnpc: the two map different ports for
    // different reasons, and an operator may reasonably want one without the
    // other. Both default true, which is what libtorrent was doing anyway --
    // the point of these is that it is now stated and refusable.
    bool upnp{true};
    bool natpmp{true};
    // libtorrent's default listen_interfaces ("0.0.0.0:port,[::]:port") is
    // expanded by its own device enumeration, which on these nodes binds eth0
    // and loopback but never wlan0. On a node whose only live link is
    // wireless that leaves the session holding loopback sockets alone: no
    // peers, no DHT, and magnets that sit in `metadata` forever with no error
    // (gbni-2, 2026-09-10 -- eth0 NO-CARRIER, two torrents dead for hours).
    // Empty means "bind the node's own advertised address", which is correct
    // whichever device carries it. Set explicitly to override, in libtorrent's
    // own syntax (an address or a device name, e.g. "wlan0:6881").
    std::string listen_interfaces;
    uint16_t listen_port{6881};
    std::vector<TorrentSearchProviderConfig> search_providers;
};

struct StreamingConfig {
    bool enabled{};
    // libav is linked into Macha. temp_path is only an overflow store for old
    // generated fragments; active publication is in memory.
    std::optional<std::filesystem::path> temp_path;
    // Node-wide, every account together. **This must stay above
    // max_sessions_per_account below**, or that cap can never be reached:
    // reserve_session_slot checks node-wide first, four lines earlier, so the
    // node limit refuses every time and the account cap becomes dead code --
    // and the two refusals mean opposite things to a client, so losing the
    // account one costs the distinction rather than just a number.
    //
    // It was 8 against a per-account 32 until 2026-09-21, which was exactly
    // that contradiction shipped as the default. Three separate client
    // sessions found it independently on the day 0.48.0 went out. These are
    // values an operator configures; the defaults exist so that a node with no
    // yaml is coherent, and they now match macha.yaml.example rather than
    // contradicting it.
    size_t max_sessions{64};
    // How many playback sessions one account may hold on this node at once.
    // Zero disables the per-account bound, leaving only max_sessions.
    //
    // This exists because a playback session stopped being a property of the
    // bearer: before that, one-session-per-bearer was bounding accounts by
    // accident, and removing it without this is how a rogue client launches a
    // media DoS. The number has to clear a household's transient peak by a
    // wide margin -- a coordinator-driven client holds a live session plus
    // standbys and transiently three during a failover, two televisions, a
    // phone and a browser is four viewers before any standby exists, and a
    // client adopting a session through the collection listing holds two by
    // design. It also has to survive a failover cascade returning to a node:
    // an abandoned session cannot be deleted (the DELETE's target is the node
    // that just failed) and holds its slot for session_idle.
    //
    // Deliberately generous, because the scarce resource is not this. A
    // session is a map entry; a transcode is a core, and max_video_transcodes
    // is 1. This bounds cheap records against a runaway client while the
    // expensive resource stays bounded separately and per viewer.
    // 32, arrived at rather than guessed. A household is four viewers before
    // any standby exists -- two televisions, a phone, a browser, one account.
    // A coordinator-driven client holds two per viewer and transiently three
    // during a failover, so four viewers in disturbance is twelve. A stranded
    // session holds its slot for the whole of session_idle (thirty minutes by
    // default) because a paused viewer and an abandoned one look identical
    // from here, and a cascade can strand more than one on the same node --
    // an haproxy front appears in a client's registry under two names, so
    // "one strand per node" is not true. Twelve plus a cascade's worth of
    // strands is the number this has to clear without being felt, and it has
    // to clear it on the worst day rather than the average one.
    //
    // Set too low, this does not present as a cap. It presents as seamless
    // failover ceasing to work at the moment it fires: silent, intermittent,
    // only under failover, and from a sofa indistinguishable from a freeze.
    // That is the worst failure signature on this surface, so the default errs
    // hard towards permissive and leaves the node protected by the resource
    // that is actually scarce.
    size_t max_sessions_per_account{32};
    size_t max_video_transcodes{1};
    size_t max_audio_transcodes{4};
    // Per transformed video. Combined with max_video_transcodes this is a
    // hard process-wide upper bound on concurrently requested decoder threads.
    size_t video_decoder_threads{2};
    // x264 frame threads per video transcode; 0 uses every hardware thread.
    // Until 0.32.11 the encoder ran sliced-threaded (tune=zerolatency) with
    // no thread count set, at about real time on the 4-core nodes.
    size_t video_encoder_threads{0};
    std::chrono::milliseconds session_idle{std::chrono::minutes(30)};
    // A session that has never served a single stream object -- no playlist,
    // no fragment, no subtitle -- expires on this much shorter clock instead.
    // The transcode entitlement lives on the session, not on the pipeline, so
    // until the session is erased the slot stays taken however cheap the
    // session has become; with max_video_transcodes at 1 that closes the node
    // to transcoding for the whole of session_idle. A client that crashed, was
    // force-quit, lost power, or is suspended with an unsent DELETE cannot
    // release it, and no client-side fix can. This is deliberately "never
    // accessed", not "not accessed recently": a session that has served even
    // one object has a viewer behind it and keeps the full session_idle, so a
    // paused or seeking player is never evicted on this clock.
    std::chrono::milliseconds session_unused_idle{std::chrono::seconds(120)};
    // Reclaim an abandoned physical encoder while retaining the logical
    // session long enough for client retry/reconciliation.
    std::chrono::milliseconds pipeline_idle{std::chrono::seconds(60)};
    // How long a session may hold a transcode entitlement with no stream
    // activity at all before it is released.
    //
    // The entitlement used to be sticky until the session was erased, so it
    // outlived its own pipeline by session_idle -- thirty minutes against
    // sixty seconds. On a node where max_video_transcodes is 1, one client
    // that crashed, was force-stopped or was reaped in the background closed
    // that node to transcoding for everybody for half an hour. Measured on
    // fi-1 on 2026-09-21: 57 session creates, zero DELETEs, and three separate
    // client sessions refused a transcode by a node nobody was competing for.
    //
    // Keyed on stream activity rather than on control traffic, deliberately:
    // "has this session asked for media recently" is a simpler contract to
    // state and for a client to reason about than "is anyone still polling".
    // A viewer paused for longer than this loses the entitlement and
    // reacquires it on resume, which may be refused -- a visible, attributable
    // 429 on the update path, against a session that survives intact. That is
    // the trade: a possible refusal after a long pause, instead of a certain
    // half-hour outage after any unclean exit.
    //
    // Sits between pipeline_idle and session_idle and is meaningless outside
    // that range: at or below pipeline_idle it would fire the moment the
    // engine went, and at or above session_idle the session outlives it.
    std::chrono::milliseconds transcode_entitlement_idle{std::chrono::minutes(5)};
    std::chrono::milliseconds startup_timeout{15000};
    std::chrono::milliseconds segment_duration{4000};
    size_t max_ahead_segments{8};
    uint64_t segment_memory_bytes{64ULL * 1024 * 1024};
    uint64_t probe_bytes{8ULL * 1024 * 1024};
    std::chrono::milliseconds probe_analyze_duration{5000};
    std::chrono::milliseconds probe_timeout{20000};
    // Bounded segment holds. A complete VOD playlist promises media that does
    // not exist yet, so a request for it is held rather than refused -- but
    // only inside limits that keep control traffic serviceable.
    //
    // The window is not arbitrary: it is the distance max_ahead_segments
    // already permits the producer to run ahead, so a request inside it is one
    // production is authorised to reach, and a request outside it is one
    // nothing is working toward.
    size_t segment_hold_window{8};
    // One in flight plus one prefetch.
    size_t max_session_holds{2};
    // A fairness and memory bound, nothing more. Until 0.43.0 this was 8,
    // because a held request occupied one of sixteen HTTP worker threads
    // for the whole of its wait; the server now parks a held request as a
    // continuation that costs an fd and a small struct, so what this bounds
    // is how many requests may be waiting on encoders at once across every
    // session -- steady-state playback on a 4-core node transcoding at
    // roughly real time sits at the frontier often, and holds are the
    // ordinary case rather than the exception.
    size_t max_concurrent_holds{64};
    // Under the tightest client deadline, with margin.
    //
    // The rule: a held request sends no bytes, so the hold must be shorter
    // than the client's time-to-first-byte deadline. Otherwise the client
    // aborts first, never receives the status, and takes its timeout path --
    // which is worse than not holding at all.
    //
    // Deadlines, each read out of the shipped artifact rather than from
    // documentation, because documentation was wrong twice here:
    //   hls.js 1.6.18   fragLoadPolicy.default.maxTimeToFirstByteMs  10000
    //   expo-video      bare OkHttpClient default readTimeout        10000
    //   RN track player media3 DEFAULT_READ_TIMEOUT_MILLIS            8000
    //   iOS AVFoundation                                            unknown
    // hls.js's `fragLoadingTimeOut` (20000) looks like the governing knob and
    // is not -- it is deprecated, and its compatibility shim migrates it only
    // when user config sets it, which no client here does. An earlier 12000
    // was chosen against that inert 20000, and an 8000 against the 10000
    // before the music path's 8000 was found and tied with it exactly.
    //
    // Holding past that deadline is worse than not holding at all. A held
    // request sends no bytes, so it trips the time-to-first-byte abort before
    // the answer arrives; the client never sees the 503 and instead takes the
    // timeout path -- 4 retries at 0ms delay, each timing out again -- so one
    // held fragment becomes roughly five requests and then a fatal error. A
    // 503 that arrives promptly is retried sensibly (6 attempts, 1s to 8s), so
    // answering inside the deadline is the whole game.
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

// The web client this node serves at its root. Off until a root directory
// is named: a node with none keeps answering non-API paths with a 404, which
// is what every node did before this existed.
struct WebConfig {
    bool enabled{true};
    // Directory of built client assets. Only this directory is served, and
    // only files within it -- no path from a request can climb out.
    std::filesystem::path root;
    // The document a client route resolves to. Served for any path that is
    // not a file under `root`, which is what makes deep links work in a
    // single-page application.
    std::string index{"index.html"};
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
    // Headroom only inbound RPC frame reassembly may draw on, and the reason
    // the whole ledger cannot deadlock. Publication holds its bytes until a
    // peer confirms the write, and that confirmation arrives as a frame which
    // must be reassembled into this same ledger: without a guaranteed slice,
    // publication fills the budget, reassembly is refused, the channel drops,
    // nothing confirms, and nothing is ever released (es-1, 2026-09-08 and
    // again 2026-09-09). It is deliberately small. An earlier attempt gave
    // reassembly absolute priority above every waiter gate instead, which
    // inverted the deadlock -- inbound frames on a node receiving from two
    // peers then starved that node's own publication completely. A few frames
    // at `max_frame_size` is all the invariant needs.
    uint64_t reassembly_memory_reserve_bytes{32ULL * 1024 * 1024};
};

struct SessionConfig {
    // No sliding renewal in v1 -- a session is valid for this long from
    // creation, then the client must POST /api/v1/session again.
    std::chrono::milliseconds anonymous_ttl{std::chrono::hours(24 * 30)};
    size_t max_sessions{4096};
    // Anonymous viewing: POST /api/v1/session with no credentials mints a
    // session bound to the "anonymous" account, carrying whatever roles that
    // account currently holds (media_viewer at genesis). Set false to require
    // a login for everything, including the web client.
    //
    // What an anonymous visitor may *do* is not configured here: it is the
    // anonymous account's roles, changed through the users API like anyone
    // else's, and takes effect immediately rather than on restart.
    bool allow_anonymous{true};
    size_t max_users{4096};
    // scrypt is deliberately expensive, so an unauthenticated endpoint that
    // runs it needs a local brake. Neither of these is replicated: they are
    // this node's own protection, not cluster state.
    size_t max_concurrent_password_checks{2};
    size_t failed_login_attempts{5};
    std::chrono::milliseconds failed_login_lockout{std::chrono::seconds(30)};
};

struct Config {
    std::filesystem::path state_path;
    std::vector<StorageBackendConfig> storage_backends;
    // storage.hosts_extents: whether this node is ever a DATA placement owner
    // or fallback holder. `no` is the edge node: it serves the API and media
    // to its own network from its cache and stores no extents at all, so
    // storage.data may be empty. `automatic` resolves to `no` when there are
    // no data backends or the node turns out to accept no inbound
    // connections, `yes` otherwise.
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
    // network.inbound_capable: can peers connect *to* this node? `no` is the
    // node behind CGNAT or a corporate NAT: it dials its peers, they answer
    // over the sessions it opened, and they never dial its advertised
    // endpoint. `automatic` is decided by asking a peer to dial back (see
    // MessageType::dial_back_probe), persisted, and re-checked periodically.
    Tristate inbound_capable{Tristate::automatic};
    // Cadence of the dial-back re-check while resolved false (a port forward
    // may have been added) and while resolved true (it may have been removed).
    // Not read from YAML; tests shorten them.
    std::chrono::milliseconds inbound_reprobe_while_incapable{std::chrono::minutes(10)};
    std::chrono::milliseconds inbound_reprobe_while_capable{std::chrono::hours(1)};
    // How often this node will answer one peer's dial_back_probe: the probe
    // makes a real TCP connection to whatever address the asker names, so it
    // is rate limited per asker. Not read from YAML; tests shorten it.
    std::chrono::milliseconds dial_back_probe_min_interval{std::chrono::seconds(10)};
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
    // Measured device service time the DATA backends defend. Above this,
    // loader and speculative admission for those backends is held at
    // io_pressure_min_background leases; a viewer is never gated by it.
    //
    // These bound service *time*, which data_viewer_reserve_bytes does not: a
    // viewer holding byte credit still queued behind a 17-second extent write
    // on 2026-09-19, and no available setting would have prevented it. Zero
    // target disables the mechanism and restores exactly the previous
    // behaviour.
    // What an operation on a coping device is expected to cost: a fixed
    // per-operation budget plus a budget per MiB. Pressure is the moving
    // average of actual/expected, so a 4 MiB write and a 4 KiB read are judged
    // against what each should cost rather than against one number.
    //
    // A single io_pressure_target_ms was the first design and it declared
    // permanent pressure on every node doing ordinary 4 MiB extent writes.
    uint32_t io_pressure_overhead_ms{25};
    uint32_t io_pressure_per_mib_ms{120};
    uint32_t io_pressure_slowdown_percent{300};
    uint32_t io_pressure_release_percent{150};
    // A single operation this far past its own expected cost trips pressure at
    // once, whatever the average says: the 17.7 s extent write that started
    // this work scored 3,505% on its own and barely moved the moving average.
    // Expressed as a ratio rather than the absolute 2 s it was until 0.53.0,
    // which was the last number here that was a guess about hardware -- and an
    // unequal one, since 2 s is 396% of expectation for a 4 MiB write and
    // 8,000% of it for a 4 KiB read.
    uint32_t io_pressure_outlier_percent{1000};
    uint32_t io_pressure_min_background{1};
    // How long a DATA credit wait may make no progress at all before it fails
    // instead of waiting for ever. "No progress" means not one lease was
    // released anywhere in the arbiter for this long -- under any real load
    // releases happen constantly and the window keeps resetting, so this only
    // fires when the arbiter is genuinely wedged (for example a caller holding
    // credit while acquiring more). Waiting was previously unbounded, which
    // turned such a mistake into a silent permanent hang rather than a visible
    // failure. Zero restores the old unbounded behaviour.
    std::chrono::milliseconds data_credit_no_progress_deadline{std::chrono::seconds(120)};
    size_t read_ahead_extents{3};
    std::vector<Endpoint> bootstrap;
    std::chrono::milliseconds heartbeat{5000};
    // How often a node gossips its telemetry set to peers. This is what
    // decides how stale a peer's figures can be in another node's Status, so
    // it is the knob to turn when node views must track a busy cluster more
    // closely (at proportionally more control-lane traffic). A demand-driven
    // wake -- a readiness transition or a peer observation -- publishes
    // sooner, but never more than once per second, so a reconnect storm
    // cannot turn this into a send loop.
    std::chrono::milliseconds telemetry_interval{10000};
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
// Settings that are legal but worth a line in the log at start-up (a data
// backend that will drain, a hosting node nobody can dial). validate() never
// logs; NodeRuntime::start() prints these once.
std::vector<std::string> configuration_warnings(const Config&);
Config normalize_config(Config);
uint64_t parse_size(const std::string&);
Endpoint parse_endpoint(const std::string&, uint16_t default_port);
void print_usage(const char* executable);
} // namespace macha
