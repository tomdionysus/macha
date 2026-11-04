# Moving Macha's network layer and store onto Dannan and Dagda: evaluation

Status: evaluation, 2026-11-04, written for Tom before any plan is adopted.
Nothing here is built or decided. The question asked: can Macha's network
layer and storage backend move to Dannan and Dagda with no loss of
function, performance, stability or accuracy, and if not, what has to
change where. Macha is the source of truth: where the two libraries
differ from Macha, the difference is listed for them to close, unless Tom
has already ruled it a change Macha takes.

Sources: the three trees at HEAD (`macha` 5776392, 0.92.2; `dannan`
811c448, 0.4.1; `dagda` 938c753, 0.1.0 in progress on Dannan 0.4.1); their
`TODO/` documents (`../dannan/TODO/ACTIVE.md`, the extraction proposal, the
function inventory, the Context audit; `../dagda/TODO/ACTIVE.md`,
`COMPLETED.md`, the 92-row store requirements, `docs/migration.md`); three
code audits run this evening, one per area (transport parity, store
parity, Macha-side blast radius), each row checked against the named test
bodies, not the documents' claims; and the two live nodes over ssh.
File:line citations are to those trees at those commits.

## 1. Verdict

Macha cannot move today, and the premise that the libraries "were written
by extracting Macha code, so it should work" holds for about half of what
would move.

- **Dagda's local store, ledger, repair and admission** are Macha's code,
  moved and generalised, with Macha's bytes pinned by known-answer tests
  (loose objects, packs, accounting slots, tries, journals, claims, the
  trie wire forms, the storage key). Macha's five LocalStore races that
  Dagda found are fixed in Dagda's copy and still open in Macha's. This
  part is close.
- **Dagda's heads do not exist.** There is no `dagda/heads/`; all thirty
  head requirements (H1 to H30) are missing: the commit DAG, acceptance,
  certificates, head sets, reconciliation, conflicts, dots, truncation,
  the Merkle tree over keys, genesis, tombstone batches as head state, the
  cache seed, the metadata readiness plane, the ordered registration. The
  namespace, which is the filesystem, cannot move until this is built,
  and Dagda has it waiting on Macha's tree-native merge and fixtures.
- **Dannan is not extracted code.** It was written from the design with
  its own wire (versions 1 and 2, not protocol 23), its own thread model
  (one Asio strand per connection and 13 shared workers, against Macha's
  two threads per connection) and its own state files. The protocol-23
  interop tests were deleted by decision, so nothing in Dannan has run
  against a Macha binary since 0.1.0. Its behaviour inventory is well
  covered by tests (15 of the 21 transport rows proven, 5 partial), but
  its performance is unmeasured: there is no throughput or latency figure
  anywhere in Dannan, no test moves more than 2 MiB, and nothing has run
  on a Pi.
- **The cutover tooling fails on a real node.** `dagda-migrate` refuses a
  Macha state directory whose `metadata/journal.log` is non-empty, after
  it has already re-tagged the objects; Dannan refuses Macha's roster
  file as corrupt; the node id, inbound resolution, telemetry and legacy
  retention files each need handling nobody has written or documented;
  and the migration's cost (a full read of the DATA set plus about four
  AES-GCM passes over it, dry run included) is unmeasured against 4 TB on
  a Pi 5.
- **Both moves are flag days.** Wire v1 means no mixed cluster; layout
  v19 means no mixed disks. Both nodes go down together, and there is no
  reverse migration, so there is no rollback once a disk is v19.

What follows is the evidence, the lists for each project, and a proposed
sequence with gates. Sections 5 and 6 are the actionable part.

## 2. The three trees and the two nodes, as they stand

| | Macha | Dannan | Dagda |
|---|---|---|---|
| Version | 0.92.2 (develop) | 0.4.1 (tagged, pushed) | 0.1.0 in progress, on Dannan 0.4.1 |
| Library lines | 103,000; the layers in question about 38,500 (cluster 15,500, metadata 11,700, storage 6,400, ledger 2,700, contract 1,600, subsystem 600) plus 5,800 of root utilities | 17,000 (net 5,500, reach 2,600, identity 2,300, status 2,100) | 20,500 (store 12,700, ledger 4,400, lease 1,100, migrate 800, domain 600) |
| Test lines, cases | 56,000; 785 cases (744 + 20 + 21) | 9,400; 423 | 17,500; 389 (1 disabled) |
| Wire | protocol 23 | own wire v1 (cluster key) and v2 (certificates) | Dannan's, types from 512 |
| Disk | layout v18 | roster `DNNMEM01`, inbound `DNNINB01`, telemetry `DNNTEL01` | layout v19 only; `dagda-migrate` rewrites v18 in place |
| Sanitizers | targeted | ASan, UBSan, TSan clean at each release, macOS only | not recorded for a release yet |

The live cluster: macnessa (root@10.44.1.50) and inverbeg, both Pi 5 on
Debian 13, four cores, 8 GB and 16 GB. macnessa holds 4.0 TB of DATA in
1,031,852 loose objects and 17 packs, 1.7 GB of CONTROL objects, 42 MB of
metadata, 161 MB of retention and 145 MB of ledgers; inverbeg holds 583 GB
and is still filling. Both have Boost 1.83; Dannan needs Boost 1.87
headers (Asio is header-only, so only the build node needs them), OpenSSL
3 and yaml-cpp, all otherwise present.

Macha's blast radius (section 3 of the Macha audit): 785 test cases, of
which between 225 and 287 touch the network layer and between 238 and 282
the store (the two counts depend on whether fixture accessors are
counted); `tests/test_support.hpp` is included by 42 of the 45 case files
and builds `BareNode` on `NodeRuntime`'s constructor; there are 498
reach-through hits where cases call `node().membership()`,
`.local_state()`, `.metadata_server()`, `.ledger()`, `.local_store()` and
the like; `test_rpc_cluster.cpp` has 114 cases, 100 of them on the
transport, and more than 30 construct `RpcClient` and `RpcServer`
directly on `NetworkLinks`. Outside the layers, `FsEntry` has 136
references in 22 files, `MetadataSnapshot` 70 in 15 (the FS, the torrent
coordinator and the manage API mutate it directly through
`MetadataView::mutate_delta` lambdas), `DistributedStore` 58 in 17 and
`NodeRuntime` 55 in 31.

Nothing in `src/cluster`, `src/storage`, `src/ledger`, `src/metadata`,
`src/contract` or `src/subsystem` changed between the inventory's baseline
(6c7ec64, 0.91.2) and HEAD except 0.91.3's reply rule (`net.cpp`) and
0.91.5's `StorageUnavailable` (`distributed_store.{hpp,cpp}`). The books
work added no objects the GC provider does not already return (covers are
ordinary catalogue artwork; conversions are a node-local cache outside the
horizon). The inventory and requirements documents are therefore current
for the layers; what is stale in them is named in section 4.5.

## 3. What is covered, and proven

So that the losses below are read in proportion.

**Dannan, proven by named tests** (file `dannan/dannan/test/*`): the two
lanes and the probe lane, shared cold dials, simultaneous-dial settlement;
frame priority at the queue; outbound limits with the small-message
reserve; the pending-reply budget of 512 with every call answered (a
stronger case than Macha's); reassembly bounds with memory leases carried
through handoffs; cancel, abort and promote on both sides; frame size
negotiation; AEAD with counters, replay and tamper refusal; executor
cancellation on disconnect and shutdown; the three broadcasts, including
the two rows Macha could not prove (best effort skipping a busy table;
session reaping); reverse dial and maintained peers for an inbound
incapable node; `inbound_capable: auto` by dial-back with the 10 min and
1 h cadence and the per-asker rate limit; `refuse_impossible_cluster`;
health pings, per-peer backoff, round trip from pings only; traffic by
class; 0.91.3's reply rule with bounded substitutes; the server's classes,
byte bounds, fast-control list, per-peer ordered execution, rejection
counters and timings; membership liveness, the forgetting horizon,
identity resets; rendezvous and capacity placement; UPnP (a hand-written
IGD client, proven against a fake gateway), external IP and the
self-probe with Macha's status codes; the supervisor's plugin loading,
build-identity refusal, decline, fault and rebuild, the stop token; node
id, cluster key, state lock, durable files, logging, the retained-memory
ledger's four laws, budgets, retry policy, published snapshots.

**Dagda, proven by named tests** (`dagda/dagda/test/*`): object ids and
sealing; loose and packed forms with Macha's bytes; pack scan, torn tails,
damage recovery, compaction; accounting slots; admission; several
backends with the R=1 ranking pinned to Macha's known answer; the two
classes with separate quotas; backend identity and refusal of foreign or
unversioned disks; durability domains, sticky failure, tickets with
process epoch and backend incarnation; strict puts, lazy deletes, cheap
presence; the cache's ephemerality; the DATA credit arbiter and I/O
pressure (all 13 of Macha's cases); the distributed write with spill and
prompt replication; the read path with shared fetches; CONTROL objects
offered and claimed; the serving side on Dannan types; the trie, its
persistence, the union, the diff and its wire forms; the survey by diff;
repair's push and pull from the lists, paced by a duty share and by a
peer's foreground traffic; claims as observed-remove sets, release only of
dominated dots, a claim from an unseen branch surviving a partition;
checkpoints and prunes; the three gates; the horizon builder over a
provider; the claim walk with its credit; sightings and the grace; the
lease record, the claimed-collection template's field groups, three-way
merge and bounds, the keeper's takeover and the new fence.

## 4. What would be lost, or is unproven

Ranked within each heading. "Lost" means Macha has it and the library
does not; "unproven" means nothing asserts it.

### 4.1 Function

1. **Heads** (Dagda, H1 to H30, all missing). Also missing with them: the
   `metadata` readiness plane, the cache seed (`current.meta` is neither
   read nor migrated), `ordered` registration of the metadata mutations,
   leases held anywhere but one node's memory (`LocalLeaseRecords`, lost on
   restart; no renew or release), and the reference-count diff that moves
   counts root to root (Dagda keeps the counters; the tree diff that
   drives them is Macha's and has no home yet).
2. **The cutover tooling** (Dagda and Dannan). On a real Macha state
   directory: `dagda-migrate` treats `<state>/metadata/` as a trie because
   it holds a `journal.log` (`migrate.cpp:220`), probes it under both keys,
   and throws `MigrationRefused` (`:163-164`) after the objects were
   re-tagged, on every rerun and in `--dry-run` too; the fixture has no
   `metadata/` so no test sees it. The migrated backend markers keep
   Macha's node id while the Dagda node's id is Dannan's from
   `--node-state`; unless the two state paths are the same directory, every
   backend is refused as another node's (`storage_pool.cpp:289`), and the
   document does not say so. Legacy retention checkpoints
   (`claims.meta`, `claims.current`, `checkpoints/`), if any remain on a
   node, make `RetentionStore` refuse to open. On Dannan's side, Macha's
   `membership/known-nodes.bin` (`MACHMEM4`) is refused as corrupt
   (`membership.cpp:47`) and the node does not construct until it is
   deleted, after which the roster, the absent-since history and the
   identity-reset tombstones are gone; `connectivity/inbound.bin` and
   `telemetry/last-known.bin` are ignored (persisted inbound resolution and
   the Status fallback lost). The node id file is compatible.
3. **Maintenance holes** (Dagda). `dagda::Node` runs only the collector
   and repair. It never compacts DATA packs (only CONTROL, from the
   collector), never rebalances across local backends, never scrubs,
   never runs the ledger verify, and no longer calls the retention
   compaction Macha's pass calls (rebalance `maintenance.cpp:1103`,
   retention compaction `:1129`, pack compaction `:1158`, scrub `:1165`,
   ledger verify `:1214`; also the head repair `:1137` and history
   truncation `:1146`, which belong with heads). Dead pack bytes,
   misplaced copies and bit rot are not reclaimed or found. The last survey is not persisted
   (`availability/last-survey.bin`), and the survey re-asks on membership
   events only, not on adverts.
4. **Write semantics** (Dagda). The Macha-named `put`, `put_deferred` and
   `put_here` default to `Durability::replicated` with `write_copies` 2
   (`distributed_store.h:147, 221`), so a facade that does not set
   `persisted` waits for a second copy where Macha returns on one;
   `put_deferred_here` has no counterpart; the local
   `StoragePool::durability_barrier` still waits without a deadline.
5. **Status and telemetry** (Dannan). The telemetry record is generic
   (22 core fields, hosted fields from 1024) and carries none of Macha's
   storage, cache, generation, backends-online, api_endpoint, playback
   budget or transcode rate fields; `peer_viewers_active` pacing works
   only through traffic class; `refresh_public_connectivity` (the API's
   forced UPnP and self-probe refresh) has no Context operation;
   readiness planes are name and bool, so `RecoveryProgress`'s per-plane
   progress has no slot. Every field of `GET /api/v1/status`,
   `/status/nodes` and `/status/diagnostics` that section 6 of the Macha
   audit lists must be reproduced by Macha's facade from Dannan's
   `Diagnostics` and `Telemetry` and Dagda's `dagda.*` Status keys; nothing
   proves the byte identity today, and the Status golden does not exist.
6. **The journal goes dark for the transport** (Dannan). Dannan's node,
   client, server and session write no log line through the sink; only
   UPnP does. Dial failures, liveness closes, session reaping, the stall
   notice (which does not exist in Dannan at all) and the BBR warning (the
   result of setting BBR is discarded) are all gone. The operator's
   diagnostics memory and watches depend on these lines.
7. **Components not wired into Dannan's node**: `fit_to_limit` (the
   memory limit shrinking reserves pro rata with the `MEMORY OVER LIMIT`
   warning), the `WaitGuard`, the `ObservationRecorder` (the node owns a
   registry but nothing writes `observations.jsonl`), the startup
   no-progress exit (only the gate's decision exists), and the test
   `dial_hook` (on `ClientOptions`, not reachable from `NodeConfig`);
   Macha's send and admit fault seams have no equivalent at all.
8. **The torrent-request CRDT** (Dagda). Macha's known-answer bytes
   (SM16/17/19 and the DLT9 section), the bounds (65,536 records, 8,192
   byte text, format byte 1), the duplicate rule by info hash with
   `duplicate_torrent`, and the three-concurrent-adds settling were
   deleted with the ingest removal; nothing in Dagda proves the template
   reproduces them. Two deliberate rule changes (a future-stamped
   tombstone counts as fresh; an unkeyed delta record is refused) will
   change Macha's answers at the port.
9. **Settings reload** (Dagda). No reload entry point on the node or
   Context; `DistributedStoreOptions`, repair and availability options are
   const; Macha's `test_replication_policy_change_on_restart` was not
   moved. Macha's own reload is weaker than it claims (section 7 item 2 of
   the requirements), but the port must not lose what works.
10. **Granularity and smaller rows**: the no-progress deadline is per
    lane only, not per type; `metadata_write_replicas_required` is an
    advert Macha must set and check; per-account bounds beside node-wide
    bounds are absent from Dannan's `Budget`; `hosts_extents` is now an
    advert (`dagda/extents`), fine, but Macha's tristate resolution stays
    Macha's; size and duration parsing and the unknown-key refusal stay
    Macha's config (Dannan's reader warns on unknown keys).

### 4.2 Performance

1. **No measurement exists in either library.** Not throughput, not
   latency, not RSS, not startup. Dannan's largest test payload is 2 MiB
   at a 4 KiB frame; Macha's transport cases move 32 MiB. The thread model
   is different in kind (a strand per connection, 13 shared server
   workers; Macha: two threads per connection plus pools), which is
   exactly what the live nodes exercise hardest.
2. **No scale test in Dagda.** The largest is a disabled trie case at
   200,000 synthetic records (measured once at 222,000 in 273 ms) and
   10,000 packed objects. macnessa has 1.03 million objects. Macha's heavy
   `test_retain_data_batches_a_large_publication_within_bounded_time` was
   not moved, and the new 64-stripe claims guard is held across the claims
   journal fsync for a whole `retain_objects` batch (up to 1,000,000 ids),
   serialising deletes and other claims behind a large publication with
   no bounded-time case.
3. **The replicated default** doubles the write wait for every Macha-named
   put unless the facade selects `persisted` (4.1 item 4).
4. **The migration's cost.** Classifying a record opens it under v19 then
   v18, and migrating it opens it under v18 again and re-seals it
   (`migrate.cpp:316-338`): about four AES-GCM passes over every byte,
   each loose file and pack payload read whole, an fsync per file;
   `--dry-run` does the same work minus the writes. On 4 TB over USB on a
   Pi 5 that is a full read plus roughly 16 TB of AEAD work; unmeasured,
   but hours to a day per run, twice if a dry run precedes it, with the
   node stopped throughout. A GHASH-only tag computation in one pass is
   possible and would cut it to one read.
5. **Linux has never run Dagda's `syncfs` path** ("compiles but has not
   run", COMPLETED 1a); both nodes are Linux.
6. **Tests bypass injected time in Dannan's health loop and watchdog**
   (`client.cpp:63, 1152-1196` use the steady clock directly), which
   makes those tests time-dependent on a loaded Pi, where Macha's rule is
   that no timing verdict is taken.

### 4.3 Stability

1. Flag days with no rollback: both nodes down together for the wire
   change, and again for the disk change; a v19 disk cannot return to
   0.92.x because no reverse migration exists (the tag rewrite is
   reversible in principle, by the same trick).
2. Dagda's open findings: a single stall over 180 s seen once in the
   durability loop and never reproduced; compaction's stop callback
   notifying without the mutex (kept as Macha's); the CONTROL step
   comparing a wall-clock age with a steady span (kept); the lease
   template's three-concurrent-adds merge not associative (kept for byte
   identity, now untestable there).
3. Dannan's sanitizer runs are macOS only; Dagda records none for a
   release. Neither has a soak on a Pi.
4. Macha's own five LocalStore races stay open in Macha
   (`project_dagda_found_races`); whichever store survives must carry the
   fixes.

### 4.4 Accuracy

Pinned to Macha's bytes in Dagda: object ids, loose and pack records,
accounting slots, trie nodes and journals, the retention WAL, the trie
wire questions and answers, the storage key, the R=1 backend ranking
(a 60-digit known answer), the lease record. Not pinned anywhere: R>1
placement across nodes with failure domains (Dannan's `by_capacity`; if
it differs from Macha's `capacity_placement_nodes`, every object's owner
set changes and repair re-homes 4 TB); the torrent CRDT; every head
encoding (nothing exists); the Status JSON. Dagda keeps Macha's placement
salts (`macha/data-fallback/v7`, `macha/data-extra-domain/v7`), which is
right, and they must stay.

### 4.5 Where the planning documents are stale or wrong

- `../dannan/TODO/ACTIVE.md` describes the sources at 0.91.x with fi-1
  live and names Macha releases 0.92.0, 0.93.0 and 0.94.0 for the port
  steps; 0.92.x is taken by books and the two nodes are macnessa and
  inverbeg.
- The function inventory's transport rows cite Dannan tests that were
  deleted (`Interop.*`, `Channel.ProtocolVersionIsTwentyThree`) and still
  say "protocol 23 byte for byte".
- The extraction proposal's seam gate items 7 (protocol 23, mixed-build
  test) and the "no byte moves" rule are superseded by two decisions
  (wire v1, layout v19) and have no replacement gate written down beyond
  "a ported node against a ported node".
- The requirements document's section 8 item 8 says "formats stay
  compatible, settled"; the same night's decision dropped v18. The
  document's "no layout bump and no protocol bump" from the Macha session
  is also gone.
- The context audit's "gap" rows are now closed in `context.h` except
  `refresh_public_connectivity`, the per-plane recovery progress, and the
  node-level `dial_hook`.

## 5. What goes back to Dannan and Dagda

Macha is the truth. Each item is a change in that project, with the
Macha behaviour it restores. Items Tom has already ruled as changes Macha
takes are in 5.3 for acknowledgement, not reversal.

### 5.1 Dagda

1. Heads (step 4), with the metadata readiness plane, the cache seed,
   `ordered` registration, head-backed lease records with renew and
   release, and the reference-count diff seam. Cost baselines for commit
   and verify as the requirements' section 7 item 3 demands.
2. `dagda-migrate`: skip `<state>/metadata/` (and every directory that is
   not a known ledger) explicitly; refuse before any write when a legacy
   retention checkpoint is present, or migrate it; state and test the
   node-id rule (the Dannan state directory and Macha's state path are
   the same directory, or the id is carried); migrate the cache's
   `current.meta` once heads define it; compute the v19 tag by GHASH in
   one pass and make `--dry-run` cheap (classify by one tag check); report
   progress and rate; a test over a fixture that is a real Macha state
   directory, metadata included; a reverse migration (v19 to v18) so a
   node can go back to 0.92.x.
3. Maintenance parity in `dagda::Node`: DATA pack compaction, local
   rebalance, scrub, ledger verify and retention compaction on Macha's
   schedule and under Macha's rule: GC, rebalance, retention compaction,
   pack compaction and scrub run only when neither viewers nor loaders
   have been active within `foreground_quiet` (`maintenance.cpp:417-425`,
   `:1099`, `:1127`, `:1156`, `:1161`, `:1240`); repair and the ledger
   verify are paced, never stopped (`:430`, `:1209`). Dagda's
   `higher_class_active` is that rule; corrected 2026-11-04 night after
   the Dagda session's check.
4. Write semantics for Macha's facade: `persisted` as the level of
   Macha's forms when the caller names none (or the facade must set it
   and a test must prove it); `put_deferred_here`; a deadline on the local
   barrier.
5. Settings reload through the node (backends, cache, write copies,
   replication, intervals, grace), and `test_replication_policy_change_on_restart`
   moved.
6. Survey persistence and a topology trigger that includes advert
   changes where Macha's did.
7. Restore the torrent-request known answers as a Dagda test fixture over
   the template (bytes, bounds, duplicate rule, the settling), even though
   the instance is Macha's; Macha's port cannot gate on bytes nobody pins.
8. Bound the claims guard: release stripes between sub-batches, and move
   Macha's bounded-time large-publication case.
9. An R>1 cross-node placement known answer against Macha's
   `capacity_placement_nodes` with failure domains.
10. A scale case at Macha's size (a trie of 1 million ids; 100,000
    objects across packs), run on Linux with `syncfs`; sanitizer runs
    before 0.1.0.

### 5.2 Dannan

1. Read Macha's state files or migrate them: `known-nodes.bin`
   (`MACHMEM4`), `inbound.bin`, `last-known.bin`; or a documented one-shot
   that converts them, tested on a real Macha state directory.
2. Journal lines for transport events at the levels Macha logs them
   (dial and dial failure, liveness close, reaping, no-progress cancel,
   the stall notice, the BBR warning, identity resets), through the node's
   sink so Macha's sink keeps its line format.
3. Wire `fit_to_limit`, the `WaitGuard`, the `ObservationRecorder` and
   the startup no-progress exit into the node; expose `dial_hook` from
   `NodeConfig` and add send and admit fault seams, or Macha's
   `test_rpc_cluster` cannot be carried.
4. `refresh_public_connectivity` on the Context; per-plane progress in
   readiness.
5. Injected time in the health loop and the watchdog.
6. A measured gate: a 32 MiB DATA transfer with CONTROL ping round trips
   sampled during it, throughput and RSS recorded, in the test binary and
   on the compose bed, so a number exists to compare against Macha's.
7. Telemetry: a documented hosted-field layout for Macha's record (ids
   from 1024) or adverts, with a case that round-trips Macha's figures.
8. Linux sanitizer runs; a real IGD run (macnessa is on the LAN with the
   gateway Macha's UPnP has used).

### 5.3 Changes Tom has already ruled, which Macha takes at the port

Listed so the port names them in its commits and the Status and docs
follow: layout v19 with the domain in every AAD; every node ledger and the
sightings files under the node-local key, so custody is mandatory on a
Macha node; the control store's 256 MiB free-space reserve (Macha: 0);
durability levels, evidence and tickets in place of bool and id returns;
the instant delete; the per-domain garbage grace; maintenance inside the
node; GC skipped while viewer or loader work is present within
`foreground_quiet` (this is Macha's own rule, not a change; an ingesting
node goes without GC in Macha today too); a future-stamped tombstone fresh; an unkeyed delta record refused;
repair diff only (cursor walks, `have_*` probes and `repair/push-position`
gone); CONTROL GC out of the catalogue; every read sharing its fetch;
`have_object` refused in a control frame; the every-write scope check;
Dannan's hand-written UPnP in place of miniupnpc; `hosts_extents` and the
store figures as adverts; the roster, inbound and telemetry files in
Dannan's formats.

## 6. What Macha must do before any port step

1. **Merge the tree-native branch** (ACTIVE item 3): genesis as a
   constant, the legacy authority and inline formats gone. Dagda's heads
   move from the merged code and the fixtures are cut after it, so this
   is the head of the critical path. Fix the five LocalStore races in
   Macha, or accept that they are fixed by the replacement and say so.
2. **Cut the fixtures Dagda waits for**: a real state directory from
   macnessa (metadata included, which is what the migration tool has
   never seen) and `macha-metadata-dump` output, under
   `build/macha-fixtures/`.
3. **Write the goldens** the extraction gate assumes and nobody has: the
   config dump, the Status surface from a two-node in-process cluster with
   volatile fields masked, the route list. Without them "byte-identical"
   is a wish.
4. **Measure the baselines on the two Pis at 0.92.2**, from the status
   API and `observations.jsonl`, and keep them in `TODO/`: DATA-lane
   throughput of one extent between macnessa and inverbeg and the CONTROL
   ping round trip during it; repair convergence after a partition (diff
   rounds, bytes, time); cold start to ready with the full library (pack
   scan, ledger seed) and peak RSS; steady RSS and idle CPU per node; FUSE
   sequential read and group-commit write throughput; a large directory
   listing. These are the performance gate for every later step.
5. **Rule on the tests.** Between 225 and 287 cases touch the transport
   and between 238 and 282 the store; 100 of `test_rpc_cluster`'s 114 are
   transport cases and more than 30 construct the transport directly.
   Each needs one of three fates: rewritten over `dannan::Node` and
   `dagda::Node` through new fixtures (the 498 accessor reach-throughs say
   the fixtures must expose the same nouns), accepted as proven by the
   named Dannan or Dagda case and retired here, or kept because it is
   Macha's. "A test is never deleted" needs Tom's ruling for the second
   fate, and the mapping should be written case by case before the first
   port commit.
6. **Build and deploy**: Boost 1.87 headers on the build node (macnessa
   builds; it has 1.83); three shared libraries under `lib/macha/` with
   one `kBuildIdentity`; the deploy tarball and rollback directories
   covering all three; the plugins (FUSE, torrent) resolving them.
7. **Owed Macha case**: `StorageUnavailable` (0.91.5) has none; Dagda
   wrote its equivalent.

## 7. Proposed sequence and gates

Each phase ends green on its gate before the next starts; the gates are
the baselines of section 6 compared, never a feeling.

**Phase 0, Macha prerequisites** (section 6, items 1 to 7). Exit: tree-
native merged and deployed; fixtures handed to Dagda; goldens committed;
baselines recorded; the test mapping written; the build line proven on
macnessa.

**Phase 1, the libraries close their lists** (section 5.1 and 5.2), Dagda
step 4 among them. Exit: Dagda 0.1.0 tagged with `dagda::Context` v1
frozen; Dannan's transport benchmark number recorded; both migration
tools (Dagda's disk, Dannan's state files, and the reverse) proven on a
copy of macnessa's state directory; sanitizers on Linux.

**Phase 2, Macha on Dannan's transport and node** (the Dannan plan's
steps 3 and 6 as one port, since both are the same flag day): Macha's
store stays Macha's; the store and metadata message types re-register
from 512 with their payloads unchanged; `Config` and `NodeRuntime` split
behind a facade; Macha's sink keeps the journal format. Gate: every
mapped test green; the config and Status goldens byte-identical; the
baselines equal or better on the Pis; a soak of several days on both
nodes with viewers, ingest and a deliberate partition. Rollback: the
0.92.x tarball, since nothing on disk changed.

**Phase 3, Macha's store on Dagda** (requirements section 6, "before
8a", then 8a and 8b): rehearse the migration on inverbeg first (583 GB,
and its DATA is replaceable from macnessa), measure its time, then
macnessa with a reverse path proven. Gate: the store cases mapped; every
object `has` and `get` true after migration against a listing taken
before; repair converges; the baselines; the soak. This is the second
flag day and the only irreversible one without the reverse tool.

**Phase 4, opaque heads and the context as the API** (8c, 8d). Gate: the
metadata cases, `macha-metadata-dump` identical on the fixture, commits
both ways and a divergence merged between the two nodes, the `replicas:
3` proof in process, the FS built on the two contexts alone.

One cutover instead of two is possible (phases 2 and 3 together) and
halves the operator's downtime, at the price of debugging the transport
and the disk layout in the same incident. Two is recommended: the
transport is the part with no provenance and no numbers.

## 8. Questions for Tom

1. Two flag days (transport first, disk second) or one?
2. The ruling on Macha tests that prove transport or store internals:
   retired against a named Dannan or Dagda case, or must each be kept?
3. Is a reverse migration (v19 to v18) a gate for the first real
   migration? Without it the store cutover cannot be undone.
4. Which Phase 1 items should I send to the Dannan and Dagda sessions
   now (sections 5.1 and 5.2 as written), and which wait on 1 to 3?
5. Withdrawn: GC skipped while loader work is present is Macha's own
   rule (`maintenance.cpp:425`, `:1240`); Dagda keeps it. Whether an
   ingesting node should go days without GC is a Macha question, not a
   port question.
6. Do you want the step-by-step plan (test mapping, cutover runbook,
   baseline scripts) written next from this evaluation, or amended first?
