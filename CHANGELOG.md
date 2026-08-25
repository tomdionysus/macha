# Changelog

## 0.16.0 - 2026-08-25

- replace layered store/RPC/FUSE ownership of authoritative object barriers with one filesystem-scoped `DurabilityDomain` per `st_dev`; completed filesystem mutations receive process-local generations, publication waits on generation tickets, and only the domain coordinator performs authoritative object `syncfs()`/portable fallback barriers;
- add scheduled group commit for replayable publication generations: batchable FUSE/distributed durability requests share a 500 ms domain window while strict writes use the same ticket path with immediate scheduling, and generation cuts are captured before the physical barrier without blocking admission of later writes;
- remove StoragePool's synthetic node-wide generation ledger and carry the accepting physical placement directly as `(process epoch, durability domain, generation, backend incarnation)`; transport v15 and v15-authenticated session labels fence the new semantics and intentionally reject older peers;
- move authoritative capacity accounting out of publication transactions: one durable DIRTY marker covers the process/store mutation session, normal publication and GC update derived in-memory usage without accounting fsyncs, clean teardown writes one exact CLEAN checkpoint, and unclean restart reconciles the object tree then establishes one physical durability baseline before trusting pre-existing objects;
- make authoritative deletion lazily durable and retain the explicitly ephemeral cache path, so lost unlink durability can only retain unreachable garbage and cache loss remains a cache miss; successful FUSE publication continues to retire completed spool files by immediate best-effort unlink after durable journal completion;
- add `docs/durability.md` with the authority tiers, invariants, domain/ticket model, group-commit rules, crash matrix, accounting/GC/cache semantics and expected observability, and add regressions for session-scoped accounting, unclean-process baseline recovery, physical-cut coverage, filesystem-domain sharing, backend incarnation fencing, local/RPC group commit and publication-before-metadata ordering.

## 0.15.2 - 2026-08-25

- replace boolean deferred-durability state with monotonic LocalStore and node-wide StoragePool generations so a filesystem barrier records the complete physical cut it actually made durable; queued barriers for already-covered generations now return immediately even when newer unrelated recovery writes have made the store dirty again;
- bump the authenticated cluster transport to v14 and extend provisional replica acknowledgements and durability-barrier RPCs from process epoch alone to `(process epoch, generation)`, coalesce per-replica requirements to the highest generation needed, and retain exact backend/store generation mappings so group commit remains correct with multiple storage backends;
- advance StoragePool durability through every contiguous deferred placement whose underlying LocalStore generation was covered incidentally by a barrier, eliminating repeated `syncfs()` calls from stale RPC data-worker barriers without weakening publication quorum or restart fencing;
- retire completed or abandoned FUSE spools by immediate best-effort unlink after the durable journal completion marker instead of truncate+`fsync`+deferred startup cleanup; a crash may resurrect only an already-completed pathname, which existing recovery cleanup safely removes;
- add Linux architecture regressions for remembered LocalStore cuts, node-wide StoragePool group commit, RPC-level reuse of an already-covered generation after newer dirty writes, and immediate successful spool retirement, while updating the existing deferred-replica epoch test for generation-qualified wire tokens.

## 0.15.1 - 2026-08-25

- move spool-backed publication durability from per-extent `fsync()` transactions to explicit publication generations: authoritative extents are staged with buffered writes, exact replica placements cross one stable-storage barrier before metadata commit, and ordinary non-WAL-backed object PUTs retain their existing immediate-durability contract;
- make deferred replica acknowledgements process-epoch-bound and make local barriers target the exact storage-backend instances which accepted provisional objects, so a node/backend restart or disappearance between placement and publication cannot be mistaken for durable quorum;
- treat the FUSE spool plus operation journal as the write-ahead log for publication, preserve the last published manifest until the new generation is durable, bypass cache admission during crash recovery, and add backward-compatible per-chunk spool digests so missing, truncated or corrupt dirty data abandons only the affected inode generation;
- make the persistent block cache explicitly ephemeral: cache objects, evictions and cache metadata no longer issue stable-storage barriers, and startup reconciliation remains sufficient because the complete cache may be discarded after a crash;
- change authoritative capacity accounting for deferred generations to one durable DIRTY marker plus one post-barrier CLEAN checkpoint, retaining O(1) clean-start accounting while falling back to the existing object-tree reconciliation after a crash in the middle of a generation;
- add architecture regressions for generation-batched durability, strict reaffirmation of provisional objects, pre-metadata durability ordering, ephemeral-cache barrier elimination, stale remote durability epochs, missing-spool isolation and checksum-corrupt spool recovery.

## 0.15.0 - 2026-08-25

- replace the legacy monolithic backend test runner and separate architecture-regression executable with one self-registering `macha-tests` framework; each case is process-isolated, independent cases execute concurrently under a weighted slot budget, and per-case timings/timeouts make slow or wedged tests attributable;
- preserve all 97 pre-0.15 functional/architecture test situations while folding architecture invariants into their owning subsystem or the unified invariant suite; remove only two duplicate architecture implementations whose exact production contracts are already exercised by the FUSE durable-journal and LocalStore tests;
- remove synchronization-by-sleep from RPC/HTTP concurrency regressions in favour of explicit gates that prove the competing production operation is actually blocked before the assertion is made, and scale several policy intervals so timing tests preserve the same ordering invariant without paying production-duration waits;
- extract the durable FUSE journal frame scanner as production code shared by recovery and tests: retain the two real restart/torn-tail lifecycle regressions, then exhaust every final-frame cut boundary plus EOF checksum and mid-journal corruption semantics without restarting FUSE for each case;
- add cheap state/property coverage for metadata-delta transitions, hydration scheduling, weighted capacity placement, durable state-file replacement, torrent URI safety, and complete torrent/ingest/catalogue-hint state round trips;
- add optional `MACHA_TEST_COVERAGE=ON` GCC/Clang instrumentation so branch coverage can be compared when future scenario tests are consolidated.

## 0.14.12 - 2026-08-23

- retain one immutable `ReadHandle` per open readable FUSE descriptor so sequential callbacks reuse the already fetched/decrypted extent; adopt normal POSIX buffered-write acknowledgement for FUSE payloads, batch local spool+journal durability asynchronously, make writable close/release the local crash-recovery boundary, and make `fsync()` wait through the existing distributed extent + metadata commit policy before returning;
- make the FUSE payload spool and durable operation journal independently configurable with `fuse.spool_path` and `fuse.operation_journal_path`; omitting them preserves the existing `state_path/fuse-spool` and `<spool>/operations.log` locations;
- preserve non-empty FUSE spool files which have no recoverable operation-journal attribution as durable `*.orphan.*` quarantine artifacts instead of aborting frontend startup and unwinding the whole node; the bytes are never guessed at or replayed without descriptors, but an upgrade/recovery artifact can no longer terminate RPC membership and continuous peer reconnect/backoff;
- keep journal-referenced missing/short spools fail-closed, because those cases prove that durable operation metadata exists for bytes which are actually unavailable and therefore cannot be safely reconstructed.

## 0.14.11 - 2026-08-23

- make metadata startup identify the exact encrypted checkpoint/journal path and journal byte offset when authentication or replay fails instead of terminating with the context-free `AES-GCM authentication failed`;
- recover a complete-length but unauthenticated final metadata-journal frame as a torn append: preserve the rejected tail as `journal.log.corrupt.*`, durably trim only the unauthenticated EOF frame, and keep corruption before later records fatal;
- when the primary metadata checkpoint/journal is not recoverable, use the independently encrypted persistent metadata-cache snapshot as a non-authoritative recovery seed, quarantine the damaged primary files, persist a `metadata/recovery.required` marker, and require a successful quorum metadata read before stale local state can be used for mutation or isolated read fallback;
- refresh the independent persistent metadata snapshot on compact delta/commit paths as well as full checkpoints, so it remains a current recovery source instead of drifting behind ordinary namespace mutations;
- route metadata checkpoint replacement through the shared fsync+rename durability primitive and check metadata-journal fsync/close failures rather than acknowledging persistence after ignored host I/O errors.

## 0.14.10 - 2026-08-23

- preserve the 0.14.9 global ordered FUSE operation journal as the crash-recovery authority, while fixing remote namespace replacement so an open/dirty inode is detached from a pathname whose content was replaced elsewhere and cannot later publish stale writes into the replacement;
- recover an otherwise-valid FUSE journal when the final full-length frame has a bad checksum, treating only the EOF frame as a torn append while retaining fatal handling for corruption before subsequent records;
- stop failed catalogue commits and artwork updates from synchronously deleting staged content-addressed objects. Failed staging is now reclaimed only by the existing grace-period reachability GC, removing check-then-delete races with concurrent successful commits of the same hash;
- make catalogue discovery enumerate one immutable metadata snapshot and fence destructive reconciliation with that snapshot's namespace signature at the metadata mutation boundary, so pruning cannot commit after the namespace it scanned has changed;
- make catalogue maintenance fail closed for physical GC when current catalogue reachability cannot be converged. The metadata-referenced catalogue root remains protected even when its contents are temporarily unavailable;
- make replica-presence decisions used by quorum, repair, rebalance and local hydration authenticate/decrypt and hash-verify stored objects. Re-putting known-good bytes now replaces an existing corrupt replica instead of counting its pathname as success;
- bound unauthenticated RPC admission to eight concurrent inbound handshakes and apply a five-second handshake I/O deadline to both inbound and outbound authentication, clean up failed reader-thread creation, and enforce CONTROL/DATA message classes at the secure-channel receive boundary for both inbound and outbound-created peer sessions;
- bound catalogue HTTP client I/O occupancy with `catalogue.api.client_io_timeout_ms` (default 30000 ms, valid 1000..300000), preventing incomplete or stalled clients from pinning a bounded worker indefinitely;
- add one fsync+atomic-rename durable state-file primitive and use it for accepted catalogue hints, resumable ingest/torrent job state, and storage-backend identity. Failed ingest/torrent admission now rolls back newly queued/live work rather than returning failure while unacknowledged work remains active;
- add architecture regression coverage for FUSE replacement/torn-tail recovery, catalogue staging/pruning/GC safety, corrupt-replica healing and repair accounting, RPC pre-auth/lane enforcement, HTTP slow-client occupancy, and durable control-state boundaries.

## 0.14.9 - 2026-08-22

- add a durable local FUSE operation journal at `state_path/fuse-spool/operations.log`. Namespace mutations and ordered write/truncate descriptors are fsynced locally before the corresponding FUSE mutation is acknowledged; write payload bytes are fsynced to the per-inode spool before their journal descriptor is made durable;
- reconstruct the optimistic FUSE namespace and pending data overlay at frontend startup from the local committed metadata snapshot plus the operation journal, then resume asynchronous namespace/data publication in recorded order. Rename/unlink state shadows stale committed paths while a durable local operation remains unconfirmed, preventing a stale snapshot from resurrecting a locally accepted name;
- retain durable publication markers until MetadataManager's decoded snapshot demonstrates the namespace/content effect. Completed spool data is truncated and synced before an otherwise-idle journal is compacted, so a restart does not silently detach non-empty spool bytes from their operation history;
- fail FUSE frontend startup when a journal-referenced spool is missing/short or when a non-empty `inode-*.spool` has no recoverable journal history. A torn final journal frame is trimmed to the last checksum-valid frame; an unexplained tail on an otherwise referenced spool is preserved as an `.orphan.*` file and reported rather than treated as acknowledged data;
- require pre-0.14.9 upgrades to drain pending FUSE publication before restart when possible: legacy non-empty spool files have no operation journal describing their pathname/order and are therefore refused rather than guessed at;
- serialise journal admission/compaction and preserve inode-before-journal lock ordering, including rename-over/subtree rename, and make data publication wait through the inode's latest accepted namespace sequence so rename/unlink cannot race an older pathname commit;
- add restart-boundary regression coverage for pending mkdir/create/write recovery, ordered write/truncate/write plus directory rename/unlink/root metadata recovery, torn-tail recovery, missing referenced spool rejection and unreferenced non-empty spool rejection.
- correct the 0.14.9 restart-boundary test harness to use the maximum valid 30-second `fuse.publication_quiet` window rather than an invalid one-hour value; this changes test setup only, not FUSE runtime semantics.

## 0.14.8 - 2026-08-22

- classify write-handle traffic as interactive rather than playback foreground traffic. This prevents asynchronous FUSE publication from refreshing the playback quiet deadline itself after every replay chunk, which could stall publication for `publication_quiet` between chunks and make `wait_for_idle()` time out; writes still suppress maintenance through the interactive activity class;
- make seek updates semantic rather than syntactic: a `PATCH` that supplies `seek_ms` plus preferences identical to the active session now reuses the prepared random-access/VOD plan instead of reopening container metadata and remote extents merely because the client repeated its current preferences;
- make playback demand visible before the first object read completes, so the first slow cache/remote read immediately suppresses lower-priority convergence work instead of becoming the read that has to wait for that work;
- treat asynchronous FUSE data publication as background convergence once bytes are safe in the local spool: while playback is active, new publications do not start and an active publication yields between bounded replay chunks and before metadata commit, then resumes after the configured publication quiet period;
- reserve two of the eight DATA RPC execution slots from lower-priority read-ahead/speculative work. Foreground playback/probe/seek requests can therefore enter a storage handler even when bulk FUSE/object publication has saturated the lower-priority queue; foreground work may still use the full pool when needed;
- add regression coverage for seek requests that repeat unchanged preferences, FUSE publication quiescence during playback, and foreground DATA execution under a saturated lower-priority worker pool.

## 0.14.7 - 2026-08-22

- stop Clear Metadata from forcing a full-library catalogue rescan: the mutation now returns the exact immutable media identities it released and queues only those paths for rematching; a definitely absent item returns 404 from current in-memory catalogue state without first entering distributed repair;
- persist the catalogue scanner's last reconciled namespace identity and next safety-check deadline, so daemon restart or coordinator election does not itself traverse an unchanged library; upgrades without prior scanner state defer one migration reconciliation to the normal safety deadline, after which periodic safety checks compare namespace identity first and walk provider roots only when it actually changed;
- coalesce terminal catalogue-hint persistence instead of rewriting the complete hints JSON for every matched/no-match item; the durable queue remains at-least-once, with admission/failure boundaries flushed immediately and ordinary worker progress flushed within two seconds or 32 updates;
- stop the periodic DEBUG catalogue backlog line once `pending=0`; terminal history remains available through the catalogue hint/status API;
- make playback media-id resolution consume MetadataManager's already-decoded immutable snapshot before falling back to authoritative quorum I/O, and retain a bounded immutable-media HLS VOD-plan cache across sessions;
- remove the GitHub Actions workflow; contributors still build locally with `MACHA_WARNINGS_AS_ERRORS=ON`.
- make torrent ingestion compile across libtorrent 2.0/2.1 API variants by using the common throwing `load_torrent_buffer` overload and treating version-specific pre-download torrent states as queued without an exhaustive enum switch.
- Fix stop-token condition-variable waits that could swallow producer notifications because their predicates were permanently false.
- Hydration wakeups now carry a monotonic revision, so providers can wake an idle hydrator immediately without reintroducing background polling or losing an event that races with scheduling.
- Playback session cleanup now wakes correctly when sessions are created, touched, replaced, removed or the idle policy changes; a newly-created session can therefore expire even when the cleanup worker was previously idle.
- Active torrent status sampling can now be interrupted immediately when the job set becomes fully paused/terminal instead of waiting for the next 500 ms sample.
- Add regression coverage for event-driven hydration wakeup and playback idle-session expiry.

## 0.14.6 - 2026-08-22

- make FUSE namespace adoption strictly local and cheap: kernel `getattr`/`readdir` traffic never performs distributed metadata I/O merely because a newer remote metadata generation has been advertised; the control-plane maintenance owner obtains/decodes newer snapshots and FUSE adopts only an already-available immutable view;
- separate FUSE namespace revision from global metadata generation so catalogue-root, garbage-accounting, voter and other metadata-only commits do not rebuild the FUSE inode/path graph; the common unchanged FUSE lookup path is now a lock-free revision comparison;
- stop copying complete extent manifests for kernel metadata operations: `getattr` and `readdir` return compact FUSE attribute records containing only type/mode/owner/size/timestamps/version;
- make idle FUSE publication workers block indefinitely until data work is queued, waking only for queue/admission changes or the configured post-foreground quiet deadline;
- make speculative hydration notification-driven while idle. Playback progress and FUSE demand wake the hydrator directly; the configured sampling interval is retained only while real asynchronous hydration I/O is outstanding, and failed objects wake at their retry deadline;
- make playback session GC sleep until the earliest actual session expiry, idle/paused ingest and torrent workers block until work arrives, and replace short-slice node-heartbeat/RPC-health/service-maintenance sleeps with stop-aware condition waits;
- remove the full metadata-record copy from the node heartbeat path by reading only the replica generation; allow background maintenance CPU credit to fall to zero when the process is already above its configured CPU target;
- raise default FUSE kernel attribute/entry cache TTLs from 250 ms to 1 s and negative lookup TTL from 100 ms to 500 ms, reducing macOS metadata callback churn while keeping namespace visibility deliberately short-lived;
- add regression coverage for no-I/O FUSE behaviour while a remote generation is known, catalogue-only metadata changes not advancing namespace revision, hydration idle blocking/direct wakeup, playback-change hydration notifications, and the new FUSE cache defaults.

## 0.14.5 - 2026-08-22

- remove the FUSE namespace refresh timer entirely. Namespace-facing FUSE operations now validate the known metadata generation on demand and adopt `MetadataManager`'s shared immutable decoded snapshot only when their cached namespace generation is stale; a completely idle mount therefore performs no namespace refresh work;
- serialise on-demand namespace adoption so concurrent lookup workers cannot rebuild the same generation repeatedly, while retaining the existing coherent local view if a newer distributed generation is temporarily unreachable;
- remove the 10 ms `fuse_session_exited()` shutdown polling thread. `fuse_loop_mt()` already returns when the session exits, and the normal post-loop shutdown path performs cancellation and service shutdown;
- remove the obsolete `fuse.refresh_interval_ms` setting. Existing YAML keys are harmlessly ignored; `fuse.watchdog_interval_ms` remains because mount-table loss is external OS state that still requires periodic observation;
- add regression coverage showing a FUSE frontend sees namespace mutations made outside that frontend on the next namespace request without a refresh timer.

## 0.14.4 - 2026-08-22

- Eliminated the dominant idle FUSE cost: namespace refresh now checks metadata generation first and reuses the shared immutable decoded metadata snapshot only when metadata actually advances.
- Made catalogue hint consumption event-driven. An idle scanner no longer linearly scans the persisted hint/negative-result map at 10 Hz; deferred hints wake at their next eligible time and new hints notify the worker immediately.
- Reduced default settled maintenance pressure: 1 s scheduler interval, 10% idle bandwidth/CPU targets, 2% scrub share and a 5 minute completed-pass backoff. Metadata/catalogue control verification remains capped at 30 seconds.

## 0.14.3 - 2026-08-22

- make catalogue provider-budget scheduling fair at the metadata-candidate boundary: a hint now persists its fallback candidate cursor and yields after one metadata hypothesis so one path cannot consume the whole provider batch before other catalogue roots get a turn; provider lookups may still issue the multiple HTTP requests required to resolve one hypothesis;
- make catalogue fallback progress restart-safe by persisting the candidate cursor in hint state and resetting it when media content/provenance reopens the hint or the hint reaches a terminal result;
- remove stale hard-coded `0.13.2` server-version expectations from catalogue/playback API tests and compare against `kServerVersion`;
- log every test case start and end with PASS/FAIL status and elapsed milliseconds, including the failing test name before propagating an exception; raise the CTest suite timeout from 60 to 300 seconds because the current integration suite legitimately exceeds one minute.

## 0.14.2 - 2026-08-22

- make bootstrap metadata formation fail closed after an incomplete committed-checkpoint survey. A fresh configured joiner may no longer fall through to genesis merely because a transient metadata RPC failed while another active node may hold durable namespace history; this fixes the intermittent replication-1 replacement race where rendezvous could select the fresh node and create an empty generation-2 namespace instead of recovering the surviving checkpoint;
- carry checkpoint-survey completeness and durable-history evidence out of replacement recovery so genesis is permitted only when every active member answered and all committed records are genuinely virgin; preserve symmetric bootstrap genesis when all active peers positively report generation 1;
- add a deterministic regression test with an unreachable active bootstrap peer whose identity is chosen so the fresh node would otherwise win single-voter genesis placement.

## 0.14.1 - 2026-08-21

- fix ambiguous bounded-FUSE mutation timeouts: lookup/read requests remain hard cancellable, but write/namespace/sync/lifecycle requests may now time out only while queued; once a local mutation starts Macha waits for its actual result instead of returning `ETIMEDOUT` while spool or namespace side effects continue asynchronously;
- treat transient asynchronous publication transport failures including `ECONNABORTED`, `ENOTCONN`, `ECONNREFUSED`, `EHOSTUNREACH` and `EPIPE` as retryable, preventing a temporary peer/transport failure from poisoning the inode and being returned directly to a later rsync write;
- make publication admission track open writable FUSE handles in addition to recent callback activity, and raise the default post-foreground quiet period from 250 ms to 5 seconds so brief rsync gaps cannot start the full publication fan-out;
- serialise final FUSE metadata commits while retaining concurrent immutable-extent preparation, avoiding several large metadata CAS operations contending for the same snapshot;
- require three consecutive mount-table watchdog misses before declaring the FUSE mount lost, avoiding fail-closed shutdown on one transient mount-table observation; extend bounded-frontend DEBUG startup output with publication concurrency settings.

## 0.14.0 - 2026-08-21

- add a persistent, coalescing catalogue hint queue under `state_path/catalogue/hints.json`; namespace discovery, namespace-mutation scans, explicit rescans and ingest completion are producers, while catalogue provider workers consume path work independently of full-tree traversal; successful matches are reconciled in batches so a provider batch still costs one catalogue-root/metadata commit rather than one commit per file;
- prioritise catalogue hints by source (`ingest` 100, manual rescan 80, namespace mutation 50, periodic scan 10) and schedule equal-priority work fairly across top-level catalogue roots; duplicate canonical paths coalesce, per-origin provenance is retained, stronger producers raise priority, and unchanged terminal negative results are reused until media identity changes or an explicit rescan reopens them;
- preserve full namespace scans as the only destructive reconciliation authority: hint processing is additive/idempotent, while complete scans still prune vanished scanner-owned bindings and partial scans suppress pruning;
- make ingest completion observable through catalogue results. Completed namespace files enqueue high-priority hints, ingest jobs remain `cataloguing` until their hints become terminal, then report matched/no-match/failed catalogue counts and per-file hint details through the acquisition API;
- add `GET /api/v1/catalogue/hints` for queue diagnostics and expose hint provenance, priority, retry state, provider result and catalogue item IDs;
- replace automatic post-copy source deletion with explicit terminal-job `clear` semantics. Macha-owned torrent staging is retained after successful ingest by default and deleted when the completed job is cleared; cancelled owned acquisitions delete partial/staging payloads immediately; external filesystem sources are preserved by default; when external-source deletion is explicitly enabled, only files in the persisted completed ingest plan are removed and unrelated files in the submitted tree are retained;
- add configurable ingest cleanup policy (`delete_owned_source_on_clear`, `delete_external_source_on_clear`, `delete_owned_source_on_cancel`) and per-filesystem-job `delete_source_on_clear` override;
- make torrent jobs expose a distinct `cataloguing` phase, propagate linked ingest catalogue status and add terminal `clear` handling for torrent and generic ingest jobs;
- migrate persisted 0.13.x completed ingest jobs back through the catalogue-hint phase once after upgrade so already-imported media is reconsidered;
- tolerate a null/comment-only `torrent.search.providers:` YAML node as an empty provider list and make the example configuration unambiguous with `providers: []`.

## 0.13.2 - 2026-08-21

- Fix libtorrent 2.1 builds under warnings-as-errors by removing use of the deprecated `torrent_flags::override_web_seeds` flag. Macha already strips magnet web-seed parameters and clears `.torrent` `url_seeds` before adding the torrent, preserving the hostile-input boundary without deprecated API.
- Select CMake CMP0167 NEW when available so libtorrent 2.1's exported package resolves Boost through modern config-mode discovery without the CMake 3.30+ policy warning.

## 0.13.1 - 2026-08-21

- fix FUSE handle release semantics: closing a read-only handle no longer requests data publication for dirty writes owned by another handle on the same inode; writable-handle release retains final publication semantics;
- add foreground-aware FUSE publication admission: configured `commit_workers` still drain backlog when the mount is quiet, but sustained mounted-filesystem activity defaults to one asynchronous publisher. This prevents several multi-second extent/metadata commits from contending with the local spool and causing bulk `rsync` writes to hit the bounded FUSE deadline;
- add a regression test covering a dirty writer plus concurrent read-only handle, asserting that reader close leaves backend data unpublished and writer close publishes it exactly once;
- prefer libtorrent-rasterbar's exported CMake package when available, including automatic Homebrew prefix discovery on macOS, with pkg-config retained as a fallback. This carries libtorrent's own Boost/OpenSSL/platform link requirements and fixes Homebrew libtorrent builds;
- derive the libtorrent user-agent Macha version from the generated server version rather than a hard-coded `0.13` string.

## 0.13.0 - 2026-08-21

- route all libav/FFmpeg diagnostics through Macha's logger with an independent configurable `ffmpeg_log_level`, so FFmpeg verbosity can be raised for media debugging without enabling Macha DEBUG/ALL output; default FFmpeg admission is `ERROR`;
- replace the path-bound synchronous FUSE adapter with a bounded inode-based `FuseFrontend`; kernel callbacks now perform only local/bounded work and never synchronously require metadata quorum, remote replica placement, checkpoint propagation, catalogue work or maintenance;
- add configurable FUSE operation-class deadlines, an absolute request ceiling, independent request workers and bounded namespace/data publication queues under the top-level `fuse:` YAML section; queue overload fails back to the kernel rather than waiting without bound;
- keep stable frontend inode identities across rename and move namespace publication behind a FIFO asynchronous worker, including correct rename-over-open-destination and unlink-before-release semantics so a stale open descriptor cannot resurrect a pathname;
- stage FUSE writes in a local append-only operation spool, preserve exact truncate/write ordering, coalesce overlapping/adjacent dirty ranges, deduplicate repeated publication requests, and publish sealed per-inode operation prefixes asynchronously through the existing `WriteHandle`/extent machinery;
- define `fsync` as local spool durability plus queued publication, while distributed metadata/object convergence remains asynchronous; reads combine committed immutable extents with pending local operations and carry a hard FUSE deadline into remote extent retrieval;
- make FUSE immediate demand a dynamic high-priority `HydrationHintProvider` in the existing cache architecture, with ordered configurable read-ahead and ordinary `DistributedStore` object-fetch coalescing; optionally write newly published FUSE extents through the existing persistent block cache;
- make hydration providers lifetime-safe for dynamic add/remove while a hint collection is in flight;
- protect the covered mountpoint after mounting and add an independent OS mount-table watchdog: unexpected FUSE/macFUSE disappearance leaves the naked directory non-writable and requests node shutdown, while clean unmount restores the original mode;
- increase the catalogue namespace-rescan hard deferral ceiling from 60 seconds to 10 minutes while retaining the 10-second quiet-period debounce;
- reduce the README logo display width to one third of its previous intrinsic size.

## 0.12.3 - 2026-08-19

- make manual catalogue edits scanner-stable with an internal metadata lock, and add `DELETE /api/v1/catalogue/items/{id}/metadata` to remove an entity (plus dependent hierarchy entries), release its underlying media IDs for fresh provider matching, and request an immediate scanner pass on the serving node;
- treat FFmpeg `AV_DISPOSITION_ATTACHED_PIC` streams as embedded artwork rather than playable video, so audio files with APIC/cover images remain audio-only during playback negotiation and can direct-play as MP3 where client capabilities allow;
- ingest embedded audio cover art during the same tag-first catalogue probe and retain it alongside provider-supplied artwork instead of replacing one cover candidate with another;
- make scanner artwork reconciliation identity-based (`role` + immutable object ID) rather than one-slot-per-role, allowing multiple cover candidates to coexist while still deduplicating identical artwork objects and repeated provider URLs.

## 0.12.2 - 2026-08-19

- add Discogs as an optional authenticated fallback music metadata provider behind the existing `MetadataProvider` interface; MusicBrainz remains first choice, while Discogs can resolve release/track metadata and cover art when MusicBrainz misses or is temporarily unavailable;
- add 60-second transient-provider circuit breaking for MusicBrainz and Discogs on HTTP 429/5xx or transport failure, and defer a music provider pass only when every configured music metadata provider is unavailable;
- harden TMDB TV matching by treating a one-year premiere difference as weak evidence instead of a hard failure and requiring parsed episode titles to corroborate remote episode titles before accepting a match;
- improve TV diagnostics with local/remote show and episode metadata plus episode-title scores, and strip leading file ordinals such as `01 -` from filename-derived series names.

## 0.12.1 - 2026-08-19

- make bounded catalogue provider work fair across Movies, TV and Music using round-robin per-provider queues and one candidate hypothesis per provider turn, with continuation cursors and a hard HTTP request ceiling;
- add a yearless TV filename candidate and cache TMDB season 404s as semantic misses, so ambiguous premiere years can fall back without repeating the same failed season request for every episode;
- promote embedded audio metadata into `MediaProbeContext` and resolve Music through distinct embedded-tag, recording-first, tags-plus-path and structured-path candidate generators instead of collapsing tags and paths into one interpretation;
- preserve album artist and track artist independently, allowing compilation releases to be identified by album context while recording-first MusicBrainz lookup uses the actual track artist and retains the album as a release-selection hint;
- derive catalogue-provider HTTP user agents from the server version rather than stale hard-coded release strings.

## 0.12.0 - 2026-08-19

- replace single-pass movie/TV/path parsing with extensible scored `MediaProbeCandidateGenerator` implementations; keep multiple hypotheses with evidence, retain the legacy parser as a low-scoring fallback, and allow bounded provider fallback across the best distinct candidates;
- improve release-name inference around technical boundaries, competing year tokens, edition metadata, TV filename/directory agreement and tag-first music fallbacks; make TMDB comparison tolerant of punctuation, ampersands, sequel numerals and common title abbreviations while retaining year/rank safeguards;
- replace monolithic on-demand WebVTT extraction with lazy time-segmented subtitle manifests aligned to the active playback timeline; subtitle segments are generated and cached only when requested;
- correct FFmpeg ASS/SSA event parsing so style/event metadata such as `0,Default,0,0,0,...` is not emitted as subtitle text;
- make subtitle-only playback-session PATCHes update the selected external WebVTT resource in place without replacing, seeking, or restarting the active A/V generation;
- give selected subtitle resources stream-specific URLs so switching tracks cannot reuse a cached WebVTT response from another track;
- advertise only subtitle codecs that the in-process text decoder can convert to WebVTT, excluding bitmap formats such as PGS from `options.subtitle_streams`.

## 0.11.0 - 2026-08-19

- decouple immutable extent write availability from desired replica placement: `dht.min_write_replicas` (default `1`) is now the degraded-mode durable floor for data PUTs. Healthy writes still commit at the normal replica quorum; when that quorum cannot be reached without already-hedged stalled owners, the write may commit at the configured floor and repair restores desired placement later;
- hedge stalled remote extent PUTs onto deterministic fallback owners after `dht.write_stall_ms` (default `2500`) of transport-level no progress. The original RPC remains valid and may still complete, but a stale preferred owner no longer blocks an otherwise writable cluster until membership expiry;
- keep metadata mutation semantics unchanged: metadata continues to require its configured voter quorum;
- wire and storage formats remain unchanged from 0.10.0.

## 0.10.4 - 2026-08-18

- make catalogue API reads live but memory-fast: warm GET/list/search requests never perform metadata quorum or catalogue-root I/O; generation notices and the short validation TTL are converged by the service control plane, which atomically publishes the replacement immutable snapshot;
- store the decoded catalogue as a shared immutable snapshot so ordinary API reads no longer copy the complete catalogue before selecting or searching items;
- make artwork GETs use the same shared snapshot and return MIME metadata with the object lookup, removing the previous whole-catalogue copy and duplicate full artwork traversal for every poster/backdrop request;
- keep catalogue refresh single-flight, retain the last coherent snapshot if background validation temporarily fails, and expose cached/known metadata generations in catalogue status;
- make decoded metadata snapshot caching honour `metadata_cache_ms`, so a missed generation announcement cannot leave the decoded metadata view current forever; immutable decoded metadata and catalogue objects are still reused when their record/hash or catalogue root is unchanged;
- add `known_metadata_generation` to `/api/v1/catalogue/status` for direct cache/convergence diagnosis;
- wire/storage formats remain unchanged from 0.10.0.

## 0.10.3 - 2026-08-18

- make catalogue roots properties of the movie, TV and music scan providers instead of one scanner-wide list; each provider traverses and parses only its own roots;
- preserve complete live-media enumeration before bounded metadata enrichment, including partial-root safety, so provider request budgeting still cannot cause erroneous scanner pruning;
- make music scanning read embedded audio tags first through libavformat over Macha's own distributed read path, including title, album artist/artist, album, track/disc, year/date and MusicBrainz IDs;
- make music filename fallback conservative: recognise `Artist - Title` and canonical `Artist/Album/File` or disc-directory layouts, but do not promote arbitrary collection/grouping directories to artist/album metadata;
- add MusicBrainz recording lookup when artist/title are known but no trustworthy album is available, and use embedded MusicBrainz release/recording/artist IDs when present;
- replace the README logo asset with the supplied SVG;
- change scanner configuration to `providers.movies`, `providers.tv` and `providers.music`, each with its own `roots` and nested metadata-provider configuration. The old scanner-wide `roots` and top-level `providers.tmdb` / `providers.musicbrainz` keys are removed;
- wire and storage formats remain unchanged from 0.10.0.

## 0.10.2 - 2026-08-18

- correct TMDB positive TV-search caching: cache the selected JSON result object itself rather than permitting the `const Json*` result to convert through `bool` into `Json(true)`; repeated show/season lookups now reuse valid cached provider data without additional HTTP requests;
- fix macOS/macFUSE Unicode pathname identity without rewriting persisted Macha namespace keys: return directory-entry names in Unicode Normalization Form D as macFUSE requires, canonicalise lookup aliases in the in-memory namespace index, and resolve them back to the exact persisted path spelling. New names use NFC beneath the actual stored parent spelling, so previous-version NFC, NFD and mixed-component namespace keys remain readable. Raw incoming/stored/emitted pathname bytes are available in `log_level=all` FUSE tracing; this covers accented file names as well as accented directory components. The implementation deliberately does not depend on macFUSE's historical `norm_insensitive` mount option because the libfuse3 path can reject it;
- negative-cache semantic provider misses for TMDB movie/show searches and MusicBrainz release searches for the lifetime of the configured provider instance. Successful HTTP responses that contain no usable match are not retried for every duplicate file/track or every scanner pass; transport/HTTP failures are deliberately not cached and remain retryable;
- bound synchronous online metadata enrichment with `catalogue.scanner.max_provider_requests_per_scan` (default 32). The budget is checked between complete provider lookups, so an in-progress search/detail lookup finishes and forward progress is guaranteed even with a very small budget; completed discoveries are reconciled normally and unfinished enrichment is continued after `catalogue.scanner.provider_batch_delay_ms` (default 30000);
- separate full namespace/media-ID enumeration from provider enrichment so a provider-budgeted scan still has the complete live media set and cannot prune scanner-owned catalogue bindings merely because their provider work was deferred;
- add regression coverage for byte-preserving accented file/directory metadata, macOS D-form directory-entry conversion, provider negative caching, provider request budgeting/continuation semantics, and the new scanner configuration.

## 0.10.1 - 2026-08-18

- replace zero-duration FUSE entry/attribute/negative caches with short configurable defaults (`250/250/100 ms`) while retaining conservative file-content cache behaviour across opens;
- make mounted shutdown two-phase: libfuse signal exit immediately requests service cancellation before waiting for the FUSE loop to drain;
- cancel mounted extent reads as well as writes during shutdown;
- make catalogue HTTP fetches abortable during scanner shutdown and stop a scan between traversal/provider/artwork units without committing a partial reconciliation;
- signal scanner, hydrator, HTTP API, playback cleanup, service maintenance and node background work before joining any of them;
- stop RPC transport before joining node maintenance so an in-flight maintenance RPC cannot hold shutdown behind its normal network timeout.

## 0.10.0 - 2026-08-18

- replace tombstone-only object reclamation with bounded local reachability GC. Each node walks its own authoritative object files with a persistent cursor, marks the combined committed filesystem+catalogue object set live, and removes unreachable objects only after `maintenance.garbage_grace_ms`; this also reclaims immutable objects left behind when a data put completed but its metadata mutation never committed;
- make physical GC cooperative with foreground work and bounded to 64 examined objects per scheduler slice. Age-check-and-remove is atomic with respect to `LocalStore::put()`, and reaffirming an already-present content hash refreshes its age, so a concurrent/retried write cannot lose an old identical object to the orphan sweep;
- make garbage tombstones finite. SM8 records a committed retirement time plus an ABA-safe retirement ID; DLT2 can upsert and erase tombstones, live reachability removes stale tombstones, and matured retirements are pruned after the grace period. Pre-0.10 tombstones are stamped on first maintenance and receive a fresh full grace period rather than being collected immediately;
- keep storage compatibility with existing installations without adding mixed-protocol fallback. SM5/SM6/SM7 snapshots remain readable, and persisted DLT1 journal records replay using the exact historical SM7 successor encoding so their stored hashes remain valid. Existing encrypted object files and backend/accounting layouts are unchanged; newly written metadata uses SM8/DLT2;
- bump the authenticated transport to v13 (`MC13`, `macha/session/v13`, protocol field 13) for DLT2 metadata mutation. v12-and-earlier peers are intentionally incompatible; stop the whole cluster before upgrading. After 0.10.0 writes SM8/DLT2 metadata, rollback to 0.9.x metadata handling is unsupported.

## 0.9.4 - 2026-08-18

- allow partial catalogue scans when configured roots are absent: available roots still add/update media, while destructive pruning is deferred until every configured root is traversable;
- rename the namespace rescan quiet-period setting to `catalogue.scanner.rescan_debounce_ms`, reduce its default to 10000 ms, and add `catalogue.scanner.rescan_max_delay_ms` (default 60000 ms) so continuous writes cannot postpone a pending catalogue scan indefinitely;
- remove the per-generation `catalogue: metadata mutation observed; namespace rescan debounce reset` debug log; scanner logs now focus on actual rescan execution and partial-root conditions;
- fix foreground object replication when a preferred owner cannot even launch its `put_object` RPC (for example, because an advertised endpoint is stale or unreachable). Synchronous launch failures and local-store failures now enter the same failed-replica accounting as completed negative RPCs, so the deterministic fallback owner is tried immediately instead of leaving `DistributedStore::put()` in an impossible quorum state forever;
- add a bounded regression which makes an unreachable peer the preferred R=1 owner and proves the write falls back to the local second-ranked owner rather than requiring cancellation to escape;
- remove the temporary `DIAG put-*` / DATA-route instrumentation used to isolate the reported rsync stall. Existing normal slow-write diagnostics remain unchanged;
- wire protocol remains v12.

## 0.9.3 - 2026-08-18

- move catalogue-root transfer onto a dedicated metadata-object RPC carried by the CONTROL TCP session at speculative worker priority. A node that has converged metadata can now fetch or receive the immutable catalogue root without first establishing the DATA lane used for media/object payloads; root reads search every active peer because catalogue roots are universal rather than ordinary DHT-placement objects;
- stop making catalogue readiness depend on eagerly downloading every referenced artwork object. A successfully decoded catalogue root is immediately usable for item/list/search operations; artwork locality remains visible in status and normal universal-object maintenance/on-demand fetch converges artwork independently;
- publish catalogue roots with the same metadata-object transport before committing the new `catalogue_root` pointer, closing the failure mode where CONTROL metadata quorum succeeded while DATA-lane root replication failed;
- add regressions proving metadata-object traffic stays on the existing CONTROL connection and that a catalogue with non-local artwork still becomes ready;
- bump the authenticated transport to v12 (`MC12`, `macha/session/v12`) for the metadata-object RPC. v11-and-earlier peers are intentionally incompatible; stop the whole cluster before upgrading.

## 0.9.2 - 2026-08-18

- Canonicalise Arabic, Roman and common spelled-out number tokens when scoring catalogue provider titles. This allows provider results such as `Men in Black II` and `Twelve Monkeys` to match release filenames using `Men In Black 2` and `12 Monkeys` while retaining year scoring.
- Add provider-level regressions for those two title variants; filename parsing remains unchanged.
- Include the parsed movie/episode/track identity in catalogue `no provider match` debug messages so future provider-scoring failures are distinguishable from parser failures.

## 0.9.1 - 2026-08-16

- made the `/api/v1/playback/sessions` response server-authoritative for playback state. Sessions now return persisted preferences, resolved mode, selected streams, original source/container/stream metadata and the actual output stream description separately; source and copied stream bitrates are included when libav reports them, while transcoded output reports only deterministic encoder facts rather than inventing CRF bitrates;
- made playback controls capability-driven. Session responses advertise only mode, quality, audio and subtitle choices that successfully negotiate under the current source, client capabilities and session constraints, and only source representations still present in the namespace. PATCHing mode/quality/track/media preferences with the current `seek_ms` rebuilds the generation at that logical position and the returned session is authoritative;
- added debounced catalogue rescanning after committed namespace mutations. The elected scanner coordinator observes cluster metadata generation changes, waits `catalogue.scanner.mutation_debounce_ms` (default 30000 ms), compares a namespace-content signature that excludes catalogue metadata, and rescans only when file/directory content changed. Catalogue commits therefore do not self-trigger and a mutation during a scan schedules one follow-up pass;
- hardened movie/TV path parsing for leading release years, dimensions such as `1920x816`, zero-padded collection ordinals, common release/codec/site suffixes, parenthesised years, season-range folders, and explicit `SxxEyy - Episode Title` names. Added regressions for the supplied Men In Black, 12 Monkeys, Pulp Fiction, Big Mistakes, Blackadder, Black Books, Black Jesus and Stranger Things examples;
- wire protocol remains v11. The HTTP `/api/v1` playback session schema is intentionally changed in place; there is no compatibility shim because Macha Client is currently the sole implementation.

## 0.9.0 - 2026-08-16

- replaced ordinary whole-snapshot metadata mutation with deterministic delta CAS. Filesystem/catalogue mutations now transmit only changed mutation-sequence clocks, changed/deleted namespace entries, catalogue-root changes and newly appended garbage tombstones. The proposer still constructs the canonical resulting snapshot and every voter independently applies the delta and verifies the same generation/hash before acknowledging it; conflict replies and rare policy/reconfiguration operations retain the full-snapshot primitive;
- replaced per-mutation `current.meta`/`committed.meta` rewrites with an encrypted append-only metadata journal. A successful voter CAS appends a durable prepare record and quorum commit appends a compact commit marker. Accepted-but-uncommitted votes therefore survive restart without becoming recovery witnesses; complete snapshots remain the repair/recovery interchange format;
- added idle checkpoint compaction. After 128 journal records or 8 MiB of journal growth, the existing speculative metadata-maintenance pass writes one durable committed `checkpoint.meta` and truncates the journal. Checkpoint publication precedes truncation and replay ignores exact generations already covered by the checkpoint, so a crash in the compaction window is idempotent. Compaction never runs on the foreground mutation critical path;
- automatically migrate 0.8.x metadata state on first 0.9.0 startup. The old committed snapshot becomes the journal checkpoint and any newer accepted current vote is preserved as an uncommitted full-seed journal record before the checkpoint is published. Successfully migrated `current.meta`/`committed.meta` files are renamed with `.v10` suffixes so accidentally starting an older binary fails rather than rolling namespace state backwards;
- stopped synchronously rewriting the persistent SSD metadata-cache snapshot on every quorum commit. It is refreshed by full checkpoint/repair activity instead and skips identical hashes, leaving ordinary mutation durability entirely on the small voter journals;
- added compact-delta observability: slow metadata mutation logs now report `mode=delta|snapshot`, `delta_bytes` and `snapshot_bytes`; idle compaction logs the generation, journal record/byte count and checkpoint snapshot size. Added regression coverage for delta codec/application, journal replay, uncommitted-vote recovery, torn-tail recovery and bounded compaction;
- bumped the authenticated transport to v11 (`MC11`, `macha/session/v11`) for the new `cas_metadata_delta` RPC. v10-and-earlier peers are intentionally incompatible; rolling mixed-version operation is unsupported.

## 0.8.7 - 2026-08-16

- replaced whole-file rematerialisation for ordinary append/resume writes with an extent-native fast path. Existing complete extents remain immutable manifest references; an unaligned final extent is fetched lazily at most once to seed the append buffer, after which only newly completed extents are stored. Repeated FUSE flush/fsync on an open handle re-arms only the committed partial tail, so later appends remain extent-native;
- changed the generic arbitrary-overwrite staging fallback to hash each rebuilt extent before storage and reuse an existing committed/staged `ExtentRef` when the bytes are unchanged. A random write therefore rereads the staged file as before but does not retransmit every unaffected object;
- made mounted-filesystem object writes cooperatively cancellable. The FUSE adapter now owns the high-level libfuse session lifecycle directly, watches the session exit flag installed by libfuse signal handling, and cancels outstanding asynchronous `put_object` RPCs so SIGINT/SIGTERM cannot remain indefinitely behind a blocked FUSE write/flush callback;
- extended write diagnostics with append-tail/materialisation/new-put/rebuild-reuse counters and retained the 0.8.6 slow-stage timing. Added regression coverage proving unaligned resume fetches only the partial tail, aligned resume fetches no old extents, append-after-flush stays on the fast path, and arbitrary overwrite reuses all untouched extents. Existing byte-for-byte fresh/resumed correctness coverage remains;
- wire protocol remains v10. The metadata mutation model, playback/UI paths, RPC lane priorities, replication policy and transcoding behaviour are unchanged.

## 0.8.6 - 2026-08-16

- decoupled catalogue/UI reads from unrelated filesystem metadata generations. Once a catalogue snapshot has been successfully synchronised, ordinary item/list/search API reads use that immutable snapshot immediately; background catalogue maintenance performs convergence. Existing content-addressed media IDs remain pinned to the immutable namespace snapshot that resolved them and only a media-ID cache miss consults newer namespace metadata. Catalogue maintenance still explicitly converges before producing GC liveness so stale API state cannot retain obsolete artwork indefinitely;
- removed the persistent block cache's foreground read/write lock convoy. Cache reads now snapshot a `shared_ptr` to the immutable object store and perform decrypt/read without holding cache mutation state, while cache insertion/eviction is separately serialised. Runtime eviction uses an in-memory LRU reconciled once when the cache opens instead of recursively enumerating the complete on-disk cache on every inserted extent; cache hits no longer rewrite file mtimes merely to persist LRU order;
- fixed a metadata snapshot race where a successful quorum/local read could be followed immediately by a newer generation notice, causing `snapshot_view()` to reject the coherent snapshot it had just installed and surface `metadata snapshot cache unavailable after successful read` as FUSE `EIO`. The current operation now completes against that immutable generation and the next operation refreshes normally;
- added targeted foreground playback source diagnostics. DEBUG logs only slow (>=250 ms) object reads with `owned`/`cache`/`remote` source and slow libav source reads with media id, purpose, offset and requested/returned bytes. These diagnostics established that the remaining seek tail is dominated by cold remote extents and, before this release, cache lock contention;
- added targeted write-stage diagnostics without changing write semantics: slow extent puts, rebuild staging/object time, write-handle mutex wait, commit data-vs-metadata time, and slow distributed object-quorum local/remote timing. This distinguishes FUSE flush waiting behind an active write from object durability/replication and metadata publication;
- added byte-for-byte regression coverage for both a fresh multi-extent sequential file and an existing-prefix/resumed append, matching the two important rsync write modes after a reported `--append-verify` verification failure;
- wire protocol remains v10. No playback/RPC priority, timeout, transcoding, replication or metadata transaction semantics changed in this release.

## 0.8.5 - 2026-08-16

- made foreground metadata mutation optimistic: a locally serialised namespace write starts from the node's durable current metadata record and lets quorum CAS detect staleness, instead of downloading and decoding the complete namespace from a quorum before every create/chmod/write commit. Metadata notices and CAS conflicts still force a quorum refresh before retry;
- removed the redundant full-record seed round after a successful quorum metadata CAS. The CAS has already persisted the successor on a quorum; non-quorum voters converge through normal repair;
- added compact `commit_metadata` quorum acknowledgements carrying only generation+hash. This replaces retransmitting the complete metadata record merely to mark already-installed CAS successors as committed recovery witnesses. Where supported, the durable `committed.meta` checkpoint is now an atomic hard link to the already-fsynced `current.meta` inode, with a full-write fallback;
- kept object payloads alone on the DATA transport. User-originated metadata mutations still use `read_ahead` frame priority and background metadata repair uses `speculative`, but both travel on CONTROL; control-priority health/membership messages pre-empt fragmented metadata there, while playback/seek object reads cannot share a TCP sequence space with metadata CAS/repair;
- reduced DEBUG noise: broad thread CPU, lock, maintenance-stage, RPC handler/queue and storage timing telemetry is now ALL-only. DEBUG retains slow FUSE operations plus one compact slow metadata-mutation timing record with base/decode/encode/CAS/commit stages and payload size. Ordinary FUSE writes are now only logged once they exceed 250 ms; namespace operations retain a 100 ms threshold;
- successful metadata CAS replies are compact: voters return generation/previous-hash/hash instead of echoing the entire accepted snapshot back to the proposer. Conflicts still return the complete current record. This removes roughly one full namespace snapshot of return traffic from the dominant CAS phase;
- normalise H.264 encoder input PTS before `avcodec_send_frame()` so duplicate/backwards demuxer timestamps do not produce a flood of libx264 `non-strictly-monotonic PTS` warnings. A media-input read failure now logs the media id, offset, requested byte count and underlying exception before libav reports EIO;
- bumped wire protocol to v10 (`macha/session/v10`) because prioritised metadata framing, compact metadata commit, and compact successful CAS replies are intentionally incompatible with earlier peers.

## 0.8.4 - 2026-08-16

- made the priority hierarchy explicit across the cluster: current playback/probe/seek object traffic is `foreground`, mounted-filesystem I/O and useful read-ahead are `read_ahead`, and repair/scrub/rebalance remain `speculative`. Incoming foreground/read-ahead object RPCs now mark activity on the serving node as well, so a remote Pi yields background maintenance while serving a viewer or mount; playback remains above mount traffic in both outbound and server-side DATA scheduling;
- moved speculative maintenance `have_object` probes off the CONTROL lane onto the DATA lane. Membership, health and metadata control RPCs can no longer queue behind replica-placement existence scans;
- bounded distributed repair by CPU/scan work as well as bytes/RPCs. One repair slice now examines at most 64 objects total across push and pull, with at most 16 remote operations, so a settled namespace cannot burn a core merely because it transfers zero bytes;
- made every user FUSE operation count as medium-priority interactive activity. Namespace-only workloads such as rsync's incremental file-list walk now suppress background repair/rebalance/scrub without being promoted above media playback;
- added a generation-cached decoded metadata snapshot and immutable namespace indexes. `getattr` is now a lookup in the shared decoded snapshot, `readdir` uses a per-directory child-path index, and media-id resolution caches paths rather than duplicating complete extent manifests. Metadata generation changes invalidate these views; unchanged metadata is not repeatedly decoded/copied for every stat;
- replaced mandatory restart-time LocalStore object-tree accounting with a two-slot checksummed crash-recoverable accounting journal. Normal restart restores exact used bytes in O(1); first 0.8.4 startup from an older store, missing/corrupt state, or explicit recovery performs the existing full reconciliation once and writes the new checkpoint. A durable pending mutation record lets restart resolve at most one content-addressed object after a crash; interrupted migration scans are never checkpointed as authoritative;
- return `server_version` from both `/api/v1/catalogue/status` and `/api/v1/playback/status`, sourced from the CMake project version;
- normalize H.264 encoder packet timestamps after rescaling into the MP4 stream timebase, matching the strict monotonic-DTS handling already used by remux. Mux rejection now logs packet timestamps, timebases, dimensions and sample aspect ratio so media-specific failures such as the observed Crow transcode can be diagnosed mechanically;
- added regression coverage for LocalStore accounting restore and corruption fallback, server-version status, and the combined repair scan ceiling. Wire protocol remains v8.

## 0.8.3 - 2026-08-16

- fixed a settled-node maintenance regression which rebuilt the complete filesystem live/garbage inventory every five seconds. Metadata repair, catalogue verification and garbage inventory now use the configured `maintenance.no_progress_backoff_ms` once settled, with a five-second minimum, and defer while foreground I/O is active;
- cache the immutable filesystem live/garbage object index by known metadata generation as a compact sorted vector and reuse it across maintenance passes. The service also retains the combined filesystem/catalogue live set for that generation, avoiding repeated O(namespace) reconstruction and copying when nothing has changed;
- replaced distributed replica repair's per-slice `local_store().list()` and full live-set vector copy with persistent bounded cursors. Push repair advances the physical local-store cursor directly; pull repair advances the immutable ordered live set with `upper_bound()`. A maintenance slice therefore examines at most a bounded number of objects instead of recursively enumerating/copying the complete store before performing 8–64 RPC operations;
- added low-volume DEBUG performance diagnostics: named long-lived threads with per-thread CPU reports, maintenance stage/repair-cursor timings and counts, live-inventory size/build time, RPC request type plus queue/handler latency, RPC stall age/type, selected metadata/catalogue/RPC/storage lock waits/holds, slow FUSE operations, and aggregated local-storage GET latency;
- moved per-object backend/object GET/PUT records, full FUSE request/result traces, read-payload hashing and detailed extent/write diagnostics from DEBUG to ALL. Log-level checks are now lock-free, so disabled hot-path diagnostics do not acquire the process-wide logger mutex. DEBUG can therefore be used for timing diagnosis without serialising every extent/FUSE operation through the console logger; ALL remains intentionally expensive trace output;
- added regression coverage for maintenance backoff selection, message-type diagnostics, generation-cached filesystem maintenance inventory, and bounded distributed replica repair. The repair regression explicitly verifies that bounded repair performs no full `StoragePool::list()` scan. Wire protocol remains v8.

## 0.8.2 - 2026-08-16

- make transformed seek-only session PATCHes reuse the session's already prepared VOD/random-access plan instead of resolving the media representation, probing the source and rebuilding the keyframe/Cues plan on every seek;
- retain source duration, segment cadence and complete remux random-access points in `HlsVodPlan`, and derive replacement generations directly from that immutable planning state;
- keep stream/track/quality/media changes on the full renegotiation path; only updates containing `seek_ms` and no other playback preference/media override use the fast path;
- preserve source sample-aspect-ratio metadata through remux and H.264 transcode output so transformed playback does not change the original display aspect ratio;
- add regression coverage proving a transformed seek-only PATCH starts a new generation without another probe or VOD preparation pass. Wire protocol remains v8.

## 0.8.1 - 2026-08-16

- fixed transformed playback startup for Matroska/WebM sources whose FFmpeg stream index was only partially populated during probing. VOD planning now triggers the demuxer seek path so deferred Matroska Cues are materialised before keyframe planning;
- reject remux VOD plans whose indexed random-access points do not cover the requested presentation densely enough for bounded fragments. This prevents a sparse partial index from being accepted as one whole-file fragment, which previously caused `POST /api/v1/playback/sessions` to wait until the startup timeout while the muxer read continuously without publishing segment 0;
- preserve remux for valid complete indexes, including moderately sparse GOP layouts, while automatic playback still falls back to H.264 transcode when the final index remains unusable and fallback is permitted;
- added regression tests for the 0.8.0 long-media `segments=1` failure, partially populated indexes that initially produce several plausible fragments, complete/sparse indexes, short one-fragment media, seek alignment, and Matroska/WebM deferred-index detection. Wire protocol remains v8.

## 0.8.0 - 2026-08-16

- changed transformed movie/episode playback from a growing HLS EVENT presentation to a complete immutable HLS VOD manifest. The full segment URI/duration plan is published from the first playlist response with `#EXT-X-PLAYLIST-TYPE:VOD` and `#EXT-X-ENDLIST`, while fragment bytes remain generated lazily;
- added VOD planning to the media-engine boundary. Remux planning uses indexed video keyframes to choose approximately `segment_duration_ms` random-access boundaries; transcode planning uses the encoder GOP cadence;
- made transformed remux seeks begin at the first indexed video keyframe at or after the requested position, so the first advertised VOD fragment is independently decodable rather than containing backward-seek pre-roll;
- replaced MP4 `frag_keyframe` output with caller-controlled fragment cuts, preventing every source keyframe from becoming an HLS segment and making generated fragment count/durations match the immutable VOD plan;
- made valid requests for not-yet-generated VOD fragments advance producer demand and wait for sequential generation instead of receiving a transient 404/503 solely because the producer has not reached that fragment yet;
- preserved legitimate stream-copy PTS-before-DTS composition offsets and enabled signed MP4 composition-time offsets instead of clamping PTS to DTS; timestamp repair logging now reports only timestamps actually modified;
- allow automatic remux to fall back to H.264 video transcode when no usable keyframe index exists only when the client advertised H.264 and the encoder is available; explicit `mode=remux` still fails rather than silently changing mode;
- fixed duplicate transformed-playback resource reservation during session creation;
- added `ROADMAP.md` documenting the former EVENT/request-driven implementation as a future explicit live/event mode, including the native-HLS edge chasing, request/producer feedback loop, forward jumps and edge stalls observed during 0.7.x testing;
- added VOD playlist/back-pressure/timestamp regression coverage. Wire protocol remains v8.

## 0.7.3 - 2026-08-16

- removed the obsolete `DistributedStore::scrub_offset_` field left behind by the 0.7.2 persistent-cursor scrub implementation; this fixes Clang `-Werror,-Wunused-private-field` builds.

## 0.7.2 - 2026-08-16

- changed `StoragePool` locking so backend mutexes protect only in-memory backend state; filesystem operations (`has/get/put/remove/list`, marker checks and `LocalStore` destruction) run after copying the relevant `shared_ptr`, preventing slow disk enumeration or accounting-thread joins from blocking unrelated foreground/control work;
- removed per-RPC storage-usage refresh from `NodeRuntime::handle()`, so `ping`, membership and metadata RPCs cannot depend on local disk state; usage/capacity is refreshed by the node loop and after successful storage mutations;
- replaced scrub and local-rebalance full-store list rebuilding with persistent physical-object cursors. Each maintenance slice examines at most a bounded number of objects, retains its position across scheduler ticks and yields promptly when foreground I/O appears;
- made scrub verify physical backend copies through the cursor and pause after a complete integrity pass instead of immediately beginning another full pass;
- made rebalance distinguish an incomplete bounded slice from a genuinely quiescent full pass, so a large settled store no longer repeatedly enumerates from object zero or enters no-progress backoff before it has actually examined the namespace;
- retained the two-lane v8 peer transport introduced in 0.7.1; this release changes local storage/maintenance behaviour only and does not change the wire protocol.

## 0.7.1 - 2026-08-16

- split peer transport into two canonical authenticated bidirectional TCP lanes: CONTROL for health/membership/metadata and DATA for object transfer traffic. Each lane is independently deduplicated by NodeId, isolating control-plane latency from TCP head-of-line blocking and congestion on bulk transfers.
- bumped the wire protocol to v8 (`MCH8`, `macha/session/v8`). v7 peers are intentionally rejected because the authenticated handshake now identifies the transport lane.
- made DATA connections lazy: cluster formation needs only CONTROL; a DATA lane is established on the first object transfer and then reused bidirectionally.
- kept transfer promotion/cancellation on the DATA lane because transfer request IDs are scoped to that connection.
- made LocalStore capacity accounting asynchronous so large object stores no longer block process startup before the backend can come online. The initial scan serialises writes/removes until accounting is reconciled while reads remain immediately available.
- retained the 0.7.0 maintenance pre-emption, bounded repair probes, in-flight transfer cancellation/promotion and route-recovery fixes developed during streaming stress testing.

## 0.7.0 - 2026-08-15

- added negotiated playback sessions with direct play, fragmented-MP4 HLS remux and selective transcoding;
- added a media-engine abstraction with an in-process libav implementation; probing, demuxing, decoding, encoding and muxing no longer shell out to `ffprobe`/`ffmpeg`;
- added custom seekable libav `AVIOContext` input backed directly by pinned Macha `ReadHandle` objects, removing the loopback HTTP source and preserving DHT/playback-hydrator semantics;
- added direct fragmented-MP4 publication through an in-process bounded `MediaSegmentStore`, removing generated-playlist filesystem polling and signal-based FFmpeg back-pressure;
- added media probe caching, multi-representation selection, validated audio/subtitle track selection, WebVTT subtitle extraction, quality limits and seek-driven pipeline replacement; path-based probes are versioned and active playback pins the resolved file snapshot;
- added explicit probe, pipeline-analysis and first-fragment startup deadlines, propagated per-caller probe cancellation through filesystem reads into independent DHT object RPCs, and added stage-specific playback 503 bodies/trace IDs for correlating server failures with client logs;
- removed two synchronous/startup CPU traps: media-ID lookup is now cached by metadata generation instead of re-hashing the complete namespace for each playback candidate, and probe/subtitle reads no longer register as active playback or trigger hydration;
- added capability-scoped stream URLs so native players can fetch media without receiving the permanent catalogue API Bearer token;
- split the HTTP server from catalogue routing and replaced the single-request server with a bounded concurrent worker server supporting streaming response bodies, HTTP byte ranges and simultaneous HLS fragment requests;
- added hard concurrent playback-session, video-transcode and audio-transcode admission limits with pending-pipeline reservations, plus idle expiry and generated-fragment cleanup;
- fixed settled-node maintenance CPU spin: no-progress repair/rebalance/scrub passes now clear accumulated byte credit and back off, catalogue repair is throttled, full filesystem live-object enumeration no longer runs every 500 ms when there is no work, and catalogue-sequence hydration no longer snapshots/repairs the catalogue when there is no active playback;
- fixed remux timestamp handling after seek: stream-copy PTS/DTS are now repaired after conversion to the muxer's final timebase, missing timestamps are synthesised, equal/backwards DTS values receive a persistent monotonic correction, and repairs are logged per stream instead of allowing the MP4 muxer to abort playback;
- added optional `seek_ms` to session creation so resumed transformed playback can create its first HLS generation at the requested position instead of requiring an immediate create-then-PATCH pipeline restart;
- added DEBUG playlist/fragment request logging for correlating server generation/segment progress with client buffering diagnostics;
- made CMake discover Homebrew's keg-only `ffmpeg@7`/`ffmpeg` pkg-config prefix automatically on macOS;
- added regression coverage for direct byte ranges, HLS session generation, initial transformed seek, remux timestamp repair, session reconfiguration, subtitle output, transcode limits, concurrent HTTP serving and segment-store back-pressure/spill behaviour;
- reduced `README.md` to an overview and moved operational documentation into topic files under `docs/`, including a two-node `quickstart.md` and streaming API guide.

## 0.6.2 - 2026-08-15

- added a per-node metadata mutation sequence clock (snapshot format 7) so quorum-CAS retries recognise an operation that became canonical, even after other writers committed descendants of it; this fixes the symmetric two-voter `EEXIST` race without making filesystem operations artificially idempotent;

- replaced equal-node media extent placement with a virtual 32-bit (2^32) stable capacity-weighted placement-shard space;
- computed exact per-node shard-slot quotas using the distinct-replica capacity constraint, avoiding the severe small-node over-selection of naive weighted sampling for replica counts greater than one;
- retained `failure_domain` diversity by placing across capacity-weighted failure-domain aggregates before selecting a node within each chosen domain;
- made deterministic fallback ordering capacity-aware while keeping current free space out of placement weights;
- applied capacity-weighted rendezvous over the stable shard space to local storage backends, so adding a disk gives it a proportional authoritative share while only moving shards won by the new backend;
- kept previously adopted configured backends in placement capacity while temporarily offline, preventing transient disk loss from redefining cluster and local ownership; permanent configuration removal drops the capacity;
- corrected `statfs` logical capacity for heterogeneous nodes using the maximum capacity supportable by `R` distinct replicas rather than `sum(capacity)/R` alone;
- bumped the wire protocol to v7 (`MCH7`, `macha/session/v7`) because v6 and v7 nodes calculate different data owners; mixed versions are intentionally rejected and no placement migration compatibility is provided.

## 0.6.1 - 2026-08-15

- added automatic distributed-filesystem catalogue scanning for movies, TV episodes and music, with conservative filename/path recognition and stable media-ID binding;
- added pluggable metadata-provider interfaces, using TMDB for movies/TV and MusicBrainz for artists/albums/tracks;
- added online artwork acquisition: movie posters/backdrops, show posters/backdrops, season posters, episode stills and Cover Art Archive album covers are downloaded into Macha's immutable object store and replicated with catalogue metadata;
- made the lowest active node ID the scanner coordinator so normally configured clusters perform one set of external lookups, with deterministic failover when membership changes;
- made scans idempotent for already-bound media, prune only scanner-owned records when files disappear, and abort deletion reconciliation if a configured root cannot be read;
- added MusicBrainz request pacing and release/cover caching, TMDB show/season caching, multi-disc music path recognition and conservative provider-match thresholds;
- fixed catalogue `external_ids` decoding to read key/value fields in defined order; the previous expression depended on function-argument evaluation order and could reverse provider/id pairs after a snapshot reload;
- added scanner/provider/artwork/reconciliation and catalogue-codec regression coverage; wire protocol remains v6.

## 0.6.0 - 2026-08-15

- replaced protocol v5 with v6 (`MCH6`, `macha/session/v6`); mixed-version peers are intentionally rejected and no compatibility shim is retained;
- collapsed health, control and data lanes into one canonical authenticated bidirectional TCP connection per peer;
- added variable-length independently authenticated transport frames with negotiated `network.max_frame_size` (default 256 KiB, allowed 4 KiB..4 MiB), independent of storage extent size;
- made frame type the sole wire priority authority: control > foreground > read-ahead > speculative, with no independent numeric priority field;
- made outbound scheduling pre-emptive at frame boundaries by selecting the highest-priority runnable transfer after every frame;
- moved heartbeat/liveness and protocol control onto the same highest-priority stream used by real peer traffic;
- added transfer promotion and cancellation control messages so a speculative/read-ahead object already in flight can become foreground without changing request identity;
- tagged filesystem retrieval as foreground, current-file/read-ahead hydration as read-ahead, and catalogue prediction as speculative; coalesced object retrieval propagates promotions to the live transport;
- kept canonical cross-dial deduplication and graceful duplicate draining under the unified connection model;
- added v6 regression coverage for single-connection reuse, frame-type ordering, variable-length framing, foreground pre-emption and frame-size configuration.

## 0.5.2 - 2026-08-15

- added a shared replica selector below filesystem reads and cache hydration;
- spread independent extent fetches across suitable replicas using current in-flight load, recent transfer latency and failure history instead of pinning reads to the first owner;
- kept foreground reads latency-oriented while speculative hydration yields aggressively to peers carrying foreground work;
- coalesced concurrent requests for the same object so foreground playback can promote an in-flight speculative fetch instead of downloading the extent twice;
- added bounded concurrent hydration with `hydration.max_inflight` (default 4), preserving ordered file prefixes and weighted-fair interleaving while allowing several replicas to contribute bandwidth at once;
- added selector and concurrent-hydration regression tests, including striping, latency preference, foreground/speculative accounting, failure penalties and the configured in-flight bound;
- kept wire protocol v5 unchanged; 0.5.2 is an internal scheduling/retrieval change and remains wire-compatible with 0.5.1.

## 0.5.1 - 2026-08-14

- added a priority-based cache hydrator with pluggable hint-provider interfaces and weighted-fair interleaving of ordered file runs;
- reimplemented sequential read-ahead as hydration hints instead of per-read-handle asynchronous futures;
- added whole-current-file prediction at lower priority than the immediate read-ahead window;
- added catalogue prediction for the next TV episode, including season transitions, and the next movie in a collection;
- added stable content-manifest media IDs so catalogue prediction survives file renames, with path bindings retained as a fallback;
- made speculative hydration populate only the persistent non-DHT cache and never authoritative replica placement;
- added configurable hydration engine enablement, priorities, cadence, activity timeout and catalogue lookahead;
- added scheduler, prediction and two-node cache-fetch tests, including ordered-prefix and non-starvation behaviour.

## 0.5.0 - 2026-08-14

- added a quorum-rooted distributed media catalogue for movies, shows, seasons, episodes, artists, albums and tracks;
- made the complete current catalogue and all referenced artwork universal metadata objects held on every active node, with joiner synchronisation ahead of ordinary media repair;
- added local catalogue browse/search and optimistic revision-based mutation;
- added an optional HTTP/JSON API for catalogue status, list/search, item mutation and artwork upload/retrieval;
- made superseded catalogue snapshots and orphaned artwork use persistent metadata garbage tombstones so every node, including later rejoiners, converges deletion after the configured grace period;
- added `maintenance.garbage_grace_ms` and `catalogue.api` YAML configuration;
- kept 0.4.0 metadata snapshots readable, defaulting their absent catalogue root to an empty catalogue;
- bumped the wire protocol to v5 (`MCH5`, `macha/session/v5`) and reject v4-and-earlier peers.

## 0.4.0 - 2026-08-14

- renamed the project to Macha across the executable, build targets, configuration, service files, C++ namespace, FUSE identity and local storage markers;
- changed cluster cryptographic derivation labels and metadata-placement namespace to `macha/*`;
- bumped the wire protocol to v4, changed the frame magic to `MCH4`, and changed the authenticated session label to `macha/session/v4`;
- intentionally removed compatibility with pre-Macha nodes, state directories, backend markers, wire protocol, encrypted storage and service/configuration names; 0.4.0 is a clean compatibility boundary;
- fixed metadata genesis for symmetric bootstrap and metadata voter counts greater than one;
- made data and metadata replica counts mutable by coordinated whole-cluster restart, with old-quorum voter-set transitions and automatic object convergence; `extent_size` remains immutable for an existing namespace.

## 0.3.0 - 2026-08-13

- added yaml-cpp based YAML node configuration, including multiple storage backends, persistent cache and adaptive-maintenance policy;
- separated durable node identity/metadata state from bulk storage backends;
- added `StoragePool` with per-disk identities, aggregate capacity, live add/remove/return detection and deterministic local rebalance;
- added a persistent encrypted non-DHT block cache bounded by block count, with LRU-like eviction and separately retained current metadata;
- added offline read-only metadata fallback to the last valid persisted snapshot;
- made joining nodes proactively pull live objects for which they become DHT owners;
- made foreground remote reads cache their already-fetched bytes and promote them asynchronously when the reading node is also an owner, avoiding a second network transfer;
- replaced the fixed block-count repair cadence with byte-budgeted adaptive maintenance driven by observed bandwidth, foreground activity and CPU load;
- gave network repair, local rebalance and scrub separate budgets, with local rebalance/scrub suppressed during foreground activity;
- added SIGHUP reload for storage-backend/cache configuration and updated systemd/launchd examples;
- fixed Apple libc++ compilation of `std::deque<RpcServer::RequestJob>` by making `RequestJob` complete at declaration, and fixed the duplicate `yes` socket-option declaration on macOS;
- expanded tests for multi-disk operation, disk disappearance/return, persistent cache behaviour, playback-assisted promotion and automatic new-node convergence;
- replaced RPC wall-clock completion deadlines with progress-observed, peer-liveness-based transport semantics: slow healthy RPCs remain valid indefinitely, while `dead_after` governs actual peer death;
- split peer transport into independent health/control/data lanes and server execution queues so bulk object transfers cannot starve membership/metadata traffic;
- made each health/control/data session bidirectional and canonical by authenticated node identity and lane; lane identity is negotiated in the authenticated handshake, simultaneous cross-dial and endpoint aliases converge on one physical connection per peer/lane, and in-flight work drains before duplicate shutdown;
- bumped the wire protocol to v3 and removed v2 handshake/session compatibility;
- removed automatic `current.meta`-only metadata-state upgrading; existing state now requires both working and committed checkpoints;
- made socket shutdown wake blocked I/O without closing/recycling the descriptor underneath active transport threads;
- removed pre-v3 configuration aliases and the legacy single-backend CLI/state-path fallback; YAML configuration is now required and old option names are rejected.
- added conventional ALL/DEBUG/INFO/WARN/ERROR logging, config-driven log level, and INFO connection lifecycle messages.
- fixed FUSE open-write size reporting so macOS does not zero-fill and replay dirty ranges against a file still advertised as size zero.
- added durable committed metadata checkpoints on every active node and automatic voter/namespace recovery when a destroyed node is replaced with a fresh identity.
- extended replacement recovery tests through namespace restoration, object repopulation, byte verification and subsequent metadata mutation.

## 0.2.0 - 2026-08-13

WAN-hardening release:

- replaced one-RPC-per-TCP transport with persistent multiplexed peer sessions;
- added v2 request IDs, out-of-order response handling and unsolicited metadata-generation notices;
- added ephemeral X25519 with authenticated transcript/HKDF session derivation for forward secrecy;
- fixed an AES-GCM empty-payload framing bug exposed by persistent sessions;
- made metadata voter reads, CAS writes, repair and genesis discovery quorum-parallel;
- added bounded generation-aware read-only metadata caching; mutations always perform a fresh quorum read;
- made foreground object writes return as soon as durability quorum is reached while background repair restores full replication;
- added adaptive peer retry backoff and latency-derived control RPC deadlines while retaining bandwidth-appropriate data timeouts;
- added explicit failure-domain labels and deterministic HRW placement that prefers replica diversity across domains;
- added WAN transport/configuration options and documented the direct-reachability/NAT constraint;
- expanded tests for X25519, v2 framing, persistent connection reuse, true multiplexing, adaptive backoff, metadata cache invalidation, topology diversity and early replication quorum;
- bumped wire protocol to v2; mixed 0.1/0.2 peers are rejected.

## 0.1.0 - 2026-08-13

Initial complete minimal implementation:

- authenticated encrypted cluster transport;
- gossip membership and deterministic DHT-style placement;
- encrypted content-addressed extent store;
- configurable data replication, full-node fallback placement and throttled repair/rebalance;
- quorum-replicated metadata with founder-safe genesis, voter replacement and no permanent authority;
- media-oriented sequential ingest, cached range reads and multi-extent read-ahead;
- explicit metadata garbage tombstones with a conservative collection grace;
- crash-durable node identity and exclusive storage-path locking;
- FUSE3/macFUSE adapter;
- Linux systemd and macOS launchd examples;
- CMake/CTest build and multi-node integration/fault-injection tests.
