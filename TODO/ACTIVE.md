# Active tasks and concepts to explore

Last updated: 2026-09-20 evening, after the 0.47.0 deploy (rationalised
against 0.43.0-0.47.0 and the live cluster)

This is the authoritative, ordered backlog. Detailed plans and UAT records in
this directory remain evidence; completed work belongs in `COMPLETED.md` and is
not repeated here. Work top-to-bottom unless new evidence changes the order.

**Start here if you are new to this work.** Read, in order:

**The playback-session resource work is what is being built right now**
(operator, 2026-09-21). It sits at the top because it is active, agreed and
breaking: it is the section immediately below this preamble. The two P-1
sections come next — they are invariants and structural properties rather than
defects in features, and everything under P0 is worth doing without changing
either.

0. **The playback-session resource work**, immediately below. Agreed with the
   operator on 2026-09-21 and specified in
   [its own plan](2026-09-21-playback-sessions-as-a-resource-plan.md).
1. **The two P-1 sections after it.** The cache invariant is new on
   2026-09-20 and generalises a failure that has now cost this project twice;
   the namespace scale target is the long-running structural one.
2. **The metadata-stall P0.** Its read-only blink was root-caused and fixed on
   2026-09-21 (a hung health probe was given the whole liveness budget); the
   stall that provokes it is still unexplained, and making an ingest survive a
   read-only window is still owed.
3. The **rejoin/cache P0** after it — worked around on all three nodes on
   2026-09-20, not fixed, and the concrete instance of the first P-1.
4. The **loader-I/O P0**. The node starves its own viewer I/O with loader
   work: one ingest took es-1 to 91% iowait and aborted twelve client requests
   at ~8 s. Governing law 1 is violated on the DATA backend, and no
   configuration available prevents it. **Its reproduction is blocked** — read
   that item's first bullet before attempting one.
5. The **P0 cluster section**. The live cluster is **three** nodes as of
   2026-09-20 evening, **all on 0.47.0** and converged at generation 31663:
   es-1, fi-1 and gbni-1. gbni-2 is defunct and the operator expects it to stay
   that way for some months (2026-09-20) — do not include it in a deploy, do
   not wait for it, and do not treat its absence as an incident. Removing a
   node is something the system does not really support, and that is the first
   item there.
6. **"What the client sessions now depend on"** near the end of this file.
   These are API contracts settled in conversation with the four client
   sessions and they exist nowhere else in this repository. Breaking one breaks
   clients that cannot be fixed from here.
7. **"Cluster and repository state as of 2026-09-13"**, which records node
   addresses, what is deployed, what access works, and where the branches and
   tags stand.

None of the last three is a task list; all of them will mislead you if you assume
otherwise.

**Four suite failures were diagnosed and fixed on 2026-09-15 (0.43.0)**,
each to a written verdict in the deterministic-suite plan, step 3: the
ingest case was a product defect (two concurrent imports both creating the
shared scanner root, the loser's `EEXIST` failing its job); the divergence
case a test defect (two Services' maintenance loops reconciling the
divergence the test had just created); the edge-node placement case a test
defect (observers compared before their capacity views had converged); and
`test_three_node_cluster` a real transport deadlock, caught with gdb -- a
writer loop exiting on a broken connection left queued notifications
unanswered, and a metadata announcement waiting on one held the mutation
mutex forever -- plus a test race against asynchronous promotion. Rates
before and after, on es-1, are in the plan. The operator's rule, stated the
same day: a failing test on a node is P0 work, not a footnote in a deploy
report.

**OpenAPI is the largest piece of agreed but unstarted work, and is now P1**
(it sat at P2 until 2026-09-20, contradicting this very sentence): the operator
asked for it on 2026-09-07 and upgraded it to "soon" on 2026-09-13. Generate it
from the route table at build time so it cannot drift.

2026-09-13 (evening): rationalisation pass for a new session. Twenty-three
completed items were moved to `COMPLETED.md` in full rather than summarised —
in several cases the reasoning *is* the record: a retraction, an option the
operator declined, a measurement that disproved the thing it was taken to
support. Before moving them, open remainders buried inside completed items were
promoted to entries of their own rather than carried along or lost: the
unmeasured iOS half of the bounded-VOD work, torrent session health not
reaching the HTTP API, the TSan run nobody has made, and the web client holding
a bearer token in JS-reachable storage. One completed item was kept in place as
a numbered stub so an ordered list still reads. Nothing was deleted.

2026-09-05: full reprioritisation pass. Two independent full-repo audits were
run: (1) every doc under `TODO/`, `COMPLETED.md` and `CHANGELOG.md` in full,
cross-referenced against each other; (2) an independent, docs-blind walk of all
of `src/`/`tests/` verifying claims directly against current code (file:line
citations). Findings were then cross-checked against each other. Consequences
of that pass:

- Items confirmed shipped (by both the changelog and direct code inspection)
  were moved to `COMPLETED.md`.
- Items this file called open that code inspection shows are now partially
  shipped were reworded rather than re-litigated from scratch.
- A new **P0 — Verified correctness defects** section and a new
  **P0 — Security hardening** section were added: these are concrete bugs and
  a concrete exposure found by reading the actual code, not carried over from
  older docs. Nothing in `src/` carries a `TODO`/`FIXME` marker — the entire
  informal backlog that would normally live in comments instead had to be
  found as verified behaviour.
- A new **P1 — Scaling cliffs** and **P2 — Code health / error-handling
  consistency** section record real, verified-but-not-yet-urgent debt (the
  system is small today; several of these are O(N) or O(N²) patterns that are
  invisible at current scale and will not stay invisible).
- This pass is still not exhaustive for the 73 dated plan docs in this
  directory — see the "Documentation hygiene" item under P2 for the specific
  staleness this audit found in those docs, root docs, and `CHANGELOG.md`.

2026-09-08: pruning pass. Every remaining item was re-checked against current
source (file:line) and against 0.24.1–0.35.0 in `CHANGELOG.md`. Items that
shipped, that a later release superseded, or that duplicated another item were
removed here and ledgered in `COMPLETED.md`; items whose evidence had moved on
were reworded down to what is actually still open rather than left carrying a
history that reads as work. What went, and why:

- The self-healing programme (0.29.0–0.32.0) and its `[x]` durability-wedge
  finding: shipped. Its three sub-items went with it — the publication hot
  loop is now under `RetryPolicy` backoff and parking (0.30.0), and the 120 s
  startup gate is now a no-progress gate (`startup_progress.hpp`,
  `service_startup_no_progress_ms`, 0.30.0). Only the duplicate-path cause
  survives, promoted to its own item.
- The DLT7 scaling item: shipped as DLT7 in 0.32.0 (`metadata.cpp:780`).
- Four `[x]` correctness/security items (0.24.0–0.24.3) and the retracted
  `LocalStore::valid()` scaling item, which was already restated in P0.
- The unlink-doesn't-abandon-publication sub-item, which was the same fix as
  the "short-circuit unlink" item in P0 structural; merged into it.
- The `mount_path` deployment rule: all three nodes run 0.34.0, so every
  config has already moved.
- The node-50 SSH item: diagnosed off-list as gbni-1 browning out under build
  load, which is now a standing operational rule, not an open investigation.
- Two claims in the "dead wiring" item that code inspection now contradicts
  (`StorageLock` is used at `cluster.hpp:77`; the scanner's
  `request_media_profiles` is invoked from `catalogue_api.cpp:441`).

The governing laws are:

1. Thou Shalt Not Make The Viewer Wait.
2. Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The Viewer Wait.
3. Control traffic must remain promptly serviceable. Viewer priority is a large
   configurable share (95:5 by default), not indefinite starvation of all other work.

## P0 — A playback session is a resource, not a property of the bearer (opened 2026-09-21, RELEASE READY AS 0.48.0, NOT DEPLOYED)

**Server side is complete and documented, committed on `develop` as 0.48.0
(`60d794e`); not tagged, and nothing is deployed.** The tag is deliberately
held until the full suite has run on es-1, so that the tag names a tree
verified on the hardware it ships to rather than on a laptop. The cutover is all three nodes at once, on the
operator's word (2026-09-21), and it is gated on two things outside this
repository: a published `@machafoundation/core` carrying `410` tolerance, and
a current web bundle reaching `/etc/macha/web`. Readiness plan, with the
ordered remainder: [what stands between these routes and the
cluster](2026-09-21-playback-sessions-deploy-readiness-plan.md).

**This is the active work.** Agreed with the operator on 2026-09-21. Full
specification: [playback sessions as a
resource](2026-09-21-playback-sessions-as-a-resource-plan.md) — read that
before touching `src/playback.cpp`.

**It breaks the client contract on purpose.** Every node is under our control,
there is no fallback to an old version, and there is no dual-serve window. Two
operator constraints govern the design: **no identifier in this system is
client-generated** (the server mints ids; `idempotency_key` is a request token,
not an identifier), and **backward compatibility is not a design input**.

The defect is that a playback session belongs to the bearer rather than
existing as a resource. `create` resolves the logical viewer from the auth
session id (`logical_session_for(request.session->id)`,
`src/playback.cpp:2288`), and `session_for_logical_locked`
(`src/playback.cpp:916-921`) returns *the* session of a viewer, singular. One
bearer therefore has one playback session and a second `POST` supersedes the
first — which is why the Web Client cannot hand over, and is not what `POST` to
a collection means.

The shape:

```
POST   /api/v1/playback/sessions                                201 + Location
GET    /api/v1/playback/sessions                                the caller's live sessions
GET    /api/v1/playback/sessions/{id}
PATCH  /api/v1/playback/sessions/{id}
DELETE /api/v1/playback/sessions/{id}
GET    /api/v1/playback/sessions/{id}/stream/{token}/{generation}/{name}
GET    /api/v1/playback/sessions/{id}/stream/{token}/direct
```

`GET /api/v1/playback/stream/...` is removed. `playback/status` and
`playback/media` are different resources and do not change.

- [ ] `POST` to the collection creates a member every time; the logical viewer
  stops being keyed on the bearer.
- [ ] `GET` on the collection, under `items`. **This is the piece that unblocks
  handover** — it does not exist today, and without it a client that loses its
  id cannot find its own session.
- [ ] The stream moves under the session; the token stays in the path, because
  it is a capability and media players send no application headers.
- [ ] **The per-account cap ships in the same change, not after it.**
  `reserve_session_slot` (`src/playback.cpp:1266-1271`) is node-wide only, and
  the one-session-per-bearer rule was doing the per-account job by accident.
  Removing it without a cap is exactly the media DoS the operator named as the
  governing constraint. Transcode entitlements
  (`reserve_resources`, `src/playback.cpp:1278+`) are per logical viewer and
  must share the cap's key, or splitting a viewer into many sessions multiplies
  them.
- [ ] Supersession becomes an explicit refusal against that cap rather than a
  silent replacement. This also absorbs the direct-session exemption item from
  the seamless-handover P0.
- [ ] Brief the four client sessions **before** the release, not after.

**Supersedes** item 2 of the seamless-handover P0 below, which said a
*client-supplied* session key should go in the route as a *query parameter*.
That was wrong on both counts and is corrected there.

### What is built, and where

- Routes, collection `GET`, stream as a subresource: `src/playback.cpp`.
- Per-account cap, `streaming.max_sessions_per_account`, default **32** with
  its arithmetic in the config comment (`src/config.hpp`). Refuses `429` with
  code `account_session_limit`, `scope: request`, `node_healthy: true`, and
  the limit and count stated.
- The cap **limit** also rides `NodeTelemetry` into the per-node `playback`
  block of `GET /api/v1/status`, so a client learns it about every node it
  might fail over to — not only the one it asked. The **count** is deliberately
  absent from that payload, which consumers cache; it appears only where it is
  computed live (creation, listing, refusal).
- `pipeline_idle_ms` and `session_idle_ms` ride the same block, so clients stop
  holding private copies of this node's configuration.
- Security review of the whole prefix, four fixes, two findings left open with
  a recommendation. See the plan.

- `410 generation_superseded` on a superseded generation, carrying
  `scope: request`, `node_healthy: true`, `alternative_may_succeed: true`. A
  generation *above* the current one stays `404`. Held since 2026-09-20 and
  released here because a coordinated route break is the right release to
  carry it, rather than holding it for a second flag day.
- The stream token compare goes through `constant_time_equal`, closing the
  first of the two open security findings. The second — `playback/status`
  aggregates visible to any `media_viewer` — is accepted rather than fixed,
  on the record in the changelog.
- `docs/streaming.md` rewritten for the resource model: it described
  one-session-per-bearer, supersession by `POST`, and the retired stream
  route. This is what the clients are briefed from, so it was on the critical
  path rather than hygiene. `max_sessions_per_account` added to
  `docs/configuration.md` and `macha.yaml.example`.

Commits: `9408794` (routes, cap, security), `b821808` (telemetry format),
`4d8312e` (codec), plus the 0.48.0 release commit (version, changelog, `410`,
constant-time compare, docs).

**`max_sessions` must be raised above `max_sessions_per_account` or the cap is
unreachable.** The compiled defaults are 8 node-wide and 32 per account, so on
a default config the node-wide limit always refuses first — and its `429` is
node-scoped, which sends a client walking the cluster instead of telling it to
stop. That is the exact misclassification the account-scoped code exists to
prevent, so shipping the cap without raising `max_sessions` leaves it dead.
The example config and the configuration reference now pair 64 with 32; all
three live nodes run 8.

### Deploy checklist — none of this is done

- [ ] **A current web bundle to `/etc/macha/web` on all three nodes, first or
  with the cutover.** The nodes serve `index-BGrNH6KR.js`, which core reports
  contains no `410` at all in 614,717 bytes. Under this change a superseded
  generation stops being exotic — every regenerate, mode switch and rebuilding
  seek makes one — so the deployed bundle would turn a routine event into
  evidence against a healthy node. **Not the server session's to deploy.**
- [ ] Build once on es-1, ship the tarball, all three nodes together. The
  telemetry format is TEL3 with no compatibility, so a node left behind is
  excluded from gossip rather than misreading it — which is the intended
  behaviour, and the reason the cutover is not rolling.
- [ ] Raise `max_sessions` node-wide on all three nodes, above
  `max_sessions_per_account`, and write both numbers explicitly into each
  config. All three run 8 today, which makes the account cap unreachable. The
  keys reload live and a 0.47.0 binary ignores the unknown one, so this can go
  in before the cutover.
- [ ] Joint test with the clients afterwards, co-ordinated by core. One thing
  worth getting: a mode switch on a moved node, watched from a client, to make
  a real `410 generation_superseded` and see it classified. No amount of
  reading produces that observation.
- [ ] Expect "plays, no sound" on the A85 Direct Play — it has no AC-3 or
  E-AC-3 decoder while claiming both, and it predates all of this.

## P0 — The block cache cannot be observed, and may never have served a read (opened 2026-09-21)

**Not "the cache is broken" — "nothing in this system could tell us if it
were."** The entire observable surface of `PersistentBlockCache` is a function
of writes. `blocks()` is the only accessor, `cache_used` in telemetry is
`blocks() * extent_size` (`src/cluster.cpp:1614-1619`), no test asserts a
read-back from it, and the one log line that would show a hit only fires for
foreground reads taking **≥250 ms** — so a working cache is silent by
construction. A cache that has never returned a byte reports identically to
one working perfectly, on every surface this project has.

It is consulted, it is filled, it is not starved, and on fi-1 it is 2560/2560
blocks full. None of that says it serves reads.

**This is P0 because of where it lands.** fi-1 runs `hosts_extents: false`:
the block cache is the entire reason a storage-less node can serve media. Every
read either hits it or crosses the WAN at 772-3431 ms per 4 MiB stripe. A dead
cache there is not a performance regression, it is the difference between an
edge node and a proxy. One 40-minute window showed 154 foreground reads going
`source=remote`, ~616 MB for one viewer — innocent if that was a first watch,
and currently indistinguishable from a cache that never hits.

Full write-up, evidence and remedy: [the block cache cannot be
observed](2026-09-21-the-block-cache-cannot-be-observed.md).

- [ ] **Falsify it cheaply first**, before any code: read one media file twice
  through FUSE on fi-1 and compare wall-clock. Same speed twice means the
  cache has never served, and the blast radius is every edge-node read since
  it was introduced. One command, and it decides the urgency of the rest.
- [ ] Counters on `PersistentBlockCache` — hits, misses, evictions, entries.
  The materialisation cache already has exactly these and they are what made
  the 2026-09-20 rejoin failure diagnosable. Mirror them.
- [ ] **Surface them on `GET /api/v1/status`, per node** — the per-node
  `cache` block at `src/status_api.cpp:299`, which is `bytes_pair(used,
  capacity)` today, gains hits/misses/evictions/entries. **For every node, not
  only the one answering**: fi-1 is where the cache matters and it is behind
  CGNAT, so the operator will be looking at es-1. Cheap now and not before —
  TEL3 is tagged and length-delimited, so four added fields are additive and an
  older node skips them by length. Monotonic counters are safe on this cached
  payload in a way the live account session count is not: they are diffed, not
  read absolutely. **Do not roll a hit rate up cluster-wide** — it would drown
  the one storage-less node the number exists to expose.
- [ ] A sustained zero-hit, high-eviction cache becomes a reportable
  condition, per the P-1 below.
- [ ] A test that pins a block-cache read-back. Nothing currently fails if
  `get` always returns empty.
- [ ] Only then revisit fi-1 latency, including whether hydration runs at all:
  `enabled: true` with the `read_ahead` and `current_file` engines, and
  **zero** log lines in 40 minutes.

**Sizing is not the problem and a claim otherwise was retracted the same day.**
Measured on fi-1: 1,427 media files, mean 1.19 GB, median 0.67 GB, p90
2.16 GB, max 21.44 GB. A 10 GiB cache holds ~16 titles at the median. The P-1
below is **satisfied** here; do not file this as an instance of it.

## P-1 — A cache must never be smaller than its own working set (opened 2026-09-20)

**This sits at P-1 because it is not a defect in a feature. It is an invariant
the system currently has no way to state, enforce, or even notice being
violated** — and when it was violated on 2026-09-20 the result was a node that
could not rejoin the cluster, burning a core at 100% for 42 minutes, while
Status reported `health: healthy` and `phase: ready`.

**The invariant, stated plainly: a cache whose eviction policy can evict
everything a running operation needs to make progress is not a cache. It is a
mechanism for converting a linear operation into a quadratic one, silently.**

The concrete instance is in the P0 below: one materialisation of this
namespace is ~51 MB, `dht.metadata_materialization_cache_bytes` defaults to
128 MiB, so two fit — and `cur_`/`committed_` are pinned and exempt, so they
*are* those two. A replica catching up therefore ran with **zero usable
cache**: 0 hits against 2 misses per 30 s, 3 evictions, ~228 deltas replayed
per import, 28 B/s of progress. Raising the limit made it 577x faster. But
**the number was never the point.** The point is that nothing anywhere
detected, reported, or refused a cache configured smaller than one unit of the
work it exists to serve.

**Why this generalises, and why it is P-1 rather than a line in the P0.**
Every bound in this system is declared up front as a byte count chosen when
the thing being bounded was smaller: `metadata_materialization_cache_bytes`
(128 MiB against 51 MB objects), and the same shape is worth auditing in
`retained_memory_bytes` and its three reserves, the catalogue and profile
caches, the playback probe and subtitle caches, and the provider response
caches. A budget smaller than one unit of its own work does not degrade
gracefully — it fails **silently and superlinearly**, which is the hardest
failure to attribute and the one this project keeps rediscovering (0.28.3's
quadratic tombstone replay, and now this).

**It is also the shape the Merkle work must not reintroduce.** P-1 below makes
a materialisation small by making it partial; this invariant must still hold
afterwards, for whatever the new unit of work turns out to be. Fixing the
namespace does not retire this.

- [ ] **A cache must be able to say it is in this state.** The counters
  already exist (`materialization_cache_hits`/`misses`/`evictions`/`entries`)
  and they said it unambiguously — 0 hits, 100% miss, evictions exceeding
  entries — but nothing reads them. A sustained zero-hit, high-eviction cache
  is a defect condition and should surface in `cluster.conditions`, not only
  in a diagnostics blob an operator has to know to go and read.
- [ ] **A cache must refuse, or loudly warn, when its limit is below its
  observed unit size.** At minimum one WARN naming the limit and the observed
  object size. A bound that cannot admit one item should be a startup-visible
  error, not a runtime mystery.
- [ ] **Prefer derived bounds over fixed byte counts** wherever the unit scales
  with the library. A fixed default is a guess with an expiry date, and this
  one expired without anybody noticing.
- [ ] **Audit the other declared bounds** listed above against their current
  unit sizes. This is the cheap half and it is where the next instance is
  hiding.
- [ ] **Pinning must be counted against the budget, or excluded from it
  honestly.** `cur_` and `committed_` consumed 103 MB of a 134 MB limit while
  being exempt from eviction, so the *effective* cache was 31 MB against a
  51 MB unit. A budget that reports 134 MB while offering 31 MB is lying to
  whoever sized it.

## P-1 — The namespace does not meet its own scale target (opened 2026-09-17)

Macha is designed for tens of thousands of files and 100 TB+ of media per
cluster. It does not currently do that, and the reason is structural rather
than a bug: `MetadataSnapshot::entries` is a `std::map<std::string, FsEntry>`
holding the whole namespace, the record payload *is* that map serialised, and
the record's identity *is* a SHA-256 over those bytes
(`metadata_hash`, `src/metadata.cpp:1334`). So nothing can be demand-loaded —
the whole structure must be materialised to produce the hash — and every commit
re-serialises and re-hashes the library.

Plan: [namespace Merkle root](2026-09-17-namespace-merkle-root-plan.md).

**Stage A of the plan is done (2026-09-17) and the numbers are now measured on
es-1 and fi-1 rather than derived.** At 1.618 TiB of library (4,808 entries,
424,222 extents) one materialisation is **47 MB** — 26 MB decoded plus a 21 MB
encoded payload resident alongside it — of which 97% is extent references.
That is **15.9 MB decoded per TiB**, so 20,000 files at a realistic average size
is 40-160 TB, i.e. **1.2-4.7 GB resident, on every node, including the Pi-class
ones**. Struct sizes on aarch64 are as assumed (`FsEntry` 72, `ExtentRef` 56,
map node 104+32).

Two corrections Stage A forced:

- **The 128 MiB `materialization_cache_limit_bytes` budget never bounded the
  namespace.** `cur_` and `committed_` are pinned and exempt
  (`src/metadata.cpp:3203`, `:3210-3212`, `:3230-3231`), so residency is
  unbounded by design and the LRU only governs historical materialisations.
  One materialisation equals the whole budget at ~4.6 TiB, not the 9 TB
  estimated — near-term, not target-scale.
- **28.8% of decoded residency was allocator slack, and is now gone.**
  `entry(Reader&)` grew extent vectors by `push_back` with no `reserve`, leaving
  610,567 slots for 424,222 extents. Reserving exactly (bounded by the input's
  remaining bytes, so a forged count cannot size an allocation) cut the decoded
  snapshot from 36.2 MB to 25.8 MB — ~640 MB per node at the 100 TB target, for
  one line, with no format change and no migration. Regression test:
  `storage_metadata/test_decoded_extent_vectors_carry_no_allocator_slack`.

fi-1 is the clearest statement of the problem: it hosts no extents, uses 8.2 GB
of disk in total, and holds a byte-identical 47 MB materialisation describing
424,222 extents of content it does not store.

And a single file write, through `mutate_impl`
(`src/metadata_manager.cpp:1622`), costs four full traversals: a full
`decode_snapshot` (`:1669`), a full `encode_snapshot`, a full `metadata_hash`,
and a full element-wise `entries != entries` comparison (`:228`). The
`before.emplace` deep copy at `:1698-1700` is already skipped, because every
namespace write path uses `mutate_delta`.

The fix is to make the record a root pointer over a content-addressed Merkle
tree, the way `std::optional<ObjectId> catalogue_root` (`src/metadata.hpp:109`)
already works three lines above `entries` in the same struct. That takes a
commit from O(library) to O(log n) per changed path, and only then does moving
extents off the heap buy anything.

**This is not a new discipline for this codebase.** `repair_step`'s comment
(`src/distributed_store.cpp:2132-2136`) diagnoses exactly this pathology in the
object store and records the fix — cursor-based, budgeted, "they never rebuild
complete object vectors" (`src/distributed_store.hpp:238-241`). The namespace
never received it, and `maintenance_objects_cached`
(`src/filesystem.cpp:2392-2455`) still builds the complete ~26-million-id live
vector that `repair_step` is handed (~840 MB transient at 100 TB).

Migration is a re-root, not a rebuild: ObjectIds address content that no
metadata format change touches, so the library survives and only ancestry is
discarded. It is a flag day across every node, and it needs an authority-granting
variant of `recover_from_seed` (`src/metadata.cpp:2097-2131`) built in the shape
of `metadata_branch_floor`/`retention_baseline_complete`
(`src/metadata.hpp:93-103`) rather than by loosening the recovery path. That
interlock is the most dangerous single piece of the work.

- [x] Stage A: real numbers off es-1/fi-1 and `sizeof` confirmation on an ARM
  build. Done 2026-09-17; see "Stage A results" in the plan. One item remains
  open: the live `MetadataReplicaDiagnostics` counters
  (`src/metadata.hpp:415-431`) are reachable only through `GET /api/v1/status`,
  which needs an account holding `view_status`.
- [~] Stage B: Merkle namespace as SM14, readable alongside SM13, not yet
  authoritative. **Started 2026-09-17**: the tree substrate is in
  `src/namespace_tree.{hpp,cpp}` with six green cases, history-independent
  and key-only-chunked, extents addressed from the leaf rather than inlined.
  Measured: one file's stat change rewrites <= 12 nodes instead of the whole
  library; an extent appended to a 4,000-extent file rewrites 3 nodes of 15.
  Still owed: the SM14 record shape and `decode_snapshot` dispatch, a journal-
  style fuzz case, a stat-only read path proven to fetch no extent nodes, and
  the `macha-metadata-dump` mode that runs it over the live es-1 head.
- [ ] Stage C: commit path carries the change set instead of rediscovering it.
- [ ] Stage D: demand-loaded extent nodes, on the `RetainedMemoryLedger`;
  persist `file_media_id`.
- [ ] Stage E: the migration and its interlock.
- [ ] Stage F: the dependent O(N) items now listed under P1 scaling cliffs.

## P0 — es-1 and fi-1 stall metadata RPCs at each other, and it is failing real ingests (opened 2026-09-20, READ-ONLY BLINK ROOT-CAUSED 2026-09-21; the stall itself is not)

**This is breaking production work right now and it outranks everything below,
including the loader-I/O P0 above it.** Agreed with the Android TV client
session on 2026-09-20, which confirmed the viewer-side impact is nil and the
ingest-side impact is permanent.

A 6 GB torrent ingest (`The Day After Tomorrow`) **failed permanently**:

```
ingest failed: metadata write durability floor unavailable:
               too few policy-compatible replicas
```

The mechanism is visible in both journals. Metadata goes **read-only for
about ten seconds at a time**, repeatedly:

```
11:33:39 WARN metadata availability changed state=read-only
              reason="metadata write durability floor lost"  replicas=1/3 required=2
11:33:49 INFO metadata availability changed state=writable   replicas=2/3 required=2
```

With `metadata_min_write_replicas: 2` and exactly two reachable replicas,
**either node blinking takes the whole cluster read-only**, and a commit that
lands in that window dies. The ingest does not retry across it. **16
no-progress RPC cancels in one hour**, and critically they happen in **both
directions** — es-1 stalling on fi-1 (`put_metadata_commit`,
`accept_metadata_commit`) and fi-1 stalling on es-1 (`accept_metadata_commit`,
`get_metadata_heads`).

**Ruled out by measurement on 2026-09-20 — do not re-measure these:**

- **Not fi-1 load.** fi-1 is idle: load 1.11, 97.6% idle, 15 GB of 16 GB free,
  zero WARN or ERROR in its own journal.
- **Not link quality.** 60 pings es-1 → fi-1: **0% loss**, RTT
  123/137/202 ms min/avg/max.
- **Not bandwidth.** Measured **13.8 MB/s** es-1 → fi-1. A 21 MB namespace
  commit (the P-1 whole-library record) should cross in **~1.5 s** against a
  30 s deadline. The bandwidth hypothesis was tested and is wrong.
- **Not a mis-resolved edge node.** `GET /api/v1/status` shows fi-1 correctly
  as `inbound_capable=false`, `hosts_extents=false`, `dialable=false`. es-1 is
  not wrongly dialling it. That hypothesis was tested and is wrong too.

**What is still unexplained, and where to look next.** Both ends healthy, link
clean, 1.5 s of work taking 30 s and being cancelled. Socket state on es-1 at
the time:

- a **`SYN-SENT` to `78.149.248.154:7437`** — the offline third node, dialled
  by bootstrap every ~8 s forever (`bootstrap: connect: No route to host` /
  `Connection timed out`). Harmless in itself but it is a permanent retry loop
  against a node that left the cluster;
- a **`FIN-WAIT-1` to `10.35.1.50:7437` with unacknowledged data**, which is
  fi-1's overlay address. A half-closed socket carrying an unacked byte will
  not trip TCP keepalive (60 s idle, 15 s × 4) because it is not idle — it
  retransmits with exponential backoff instead. That is the shape of a
  30-second no-progress stall on a healthy link, and it is the first thing to
  chase.
- fi-1's *healthy* CONTROL and DATA lanes appear as two `ESTAB` from its CGNAT
  public address `37.136.124.32`, which is expected for an inbound-incapable
  node that dials out.

**Root cause of the read-only windows, found 2026-09-21. The stall was never
the whole story: the cluster had no margin to absorb one.** A peer counts as
live only while it has been observed inside `dead_after`
(`src/membership.cpp:292,315`). The mechanism that refreshes that observation
is `RpcClient::health_loop` (`src/net.cpp:2694`): every `heartbeat` it pings
each peer's CONTROL lane and, on an `ok`, calls `peer_observer_(reply.peer)` →
`members_.observe(peer, true)` (`src/net.cpp:2812`, `src/cluster.cpp:264,305`).
A membership exchange from `NodeRuntime::loop` also observes, but the probe is
the primary path.

**Each probe round is given exactly `dead_after` to succeed in**: the probes
are constructed with `deadline = started + dead_after_` (`src/net.cpp:2755-2758`),
and the abandon message says so — `"health could not be established before
dead_after"`. A probe that *fails* is fine: `next_attempt = now + 50 ms` retries
it for the rest of the window. A probe that **hangs** is not, because
`call_async_known` carries no no-progress deadline of its own, so the one
attempt sat there until the round deadline — and the round deadline is the
liveness budget. **The peer expired at the instant the probe proving it alive
was abandoned, with no retry able to land first, by construction.** That
dropped `online` below `required` (`src/metadata_manager.cpp:99-111`), took
metadata read-only, and killed any commit in the window.

That explains what this item called unexplained: both ends healthy, link
measured clean, nothing actually unhealthy. The apparent six heartbeats of
margin (5 s heartbeat, 30 s budget) never existed for a hung probe.

**Fixed 2026-09-21 (unreleased), in the probe, not the config.** Each probe
attempt now carries its own budget of `dead_after / 3` (floor 100 ms). Past it
the attempt is cancelled and handed to the existing 50 ms retry path, so a
ping that answers on the second or third attempt no longer costs the peer its
membership, and a peer that is genuinely gone is still declared dead at
`dead_after` as before.

**A first attempt at this aimed at the wrong path and is recorded so it is not
repeated.** It read the *membership exchange* as the sole liveness path, gave
`members`/`members_reply` a derived `membership_no_progress_deadline`, and left
`health_loop` untouched — so it would not have fixed the blink. It was dropped.

**Two suite cases on es-1 are load-sensitive and this is now a P0 of its own
(2026-09-21).** A full run of that attempt reported 477/479 —
`hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`
(on `metadata replica set forming: waiting for bootstrap checkpoint survey`)
and `rpc_cluster/test_ingest_torrent_jobs_visible_and_actionable_from_non_owning_node`
— and **a second full run of the same tree reported 479/479**. Both pass in
isolation; the catalogue case passed 5/5, the ingest case failed 1/5 under
`--repeat`, which interleaves in parallel slots. They fail under suite load,
not from any change. Per the operator's 2026-09-15 rule this is P0 work, not a
footnote: see the deterministic-suite plan.

**A methodology note worth keeping.** The two runs above were first read as
"baseline green, change red", which was wrong: the supposedly pristine tree
still carried the change's own two test cases, proven afterwards by their names
appearing in the baseline log (`macha-tests --list` gives 477 for HEAD, and the
baseline run announced 479). **Compare `--list` counts before trusting a
before/after suite comparison on a node**, because rsync into a shared tree is
not a checkout.

**This is the P-1 cache invariant in another currency.** A bound declared as a
fixed number, chosen when the thing it bounded was smaller, that cannot admit
one unit of the work it exists to serve — here one stalled-and-retried
exchange — and fails silently rather than degrading. The audit list in that
P-1 is byte counts; **it should be extended to every timeout that is a budget
for another timeout.** `health_loop`'s round deadline was the instance; the
same question should be put to every other `dead_after_`-sized window.

**Socket evidence above is superseded (re-read 2026-09-21).** The `SYN-SENT` to
`78.149.248.154:7437` was es-1 bootstrap-dialling **gbni-1** (`macnessa`, its
only bootstrap entry) while gbni-1 was down — the ~8 s period is exactly
`connect_timeout_ms: 2500` + `heartbeat_ms: 5000`, not a stuck loop, and it is
gone now gbni-1 is back and `ESTAB`. It was never gbni-2. There is no
`FIN-WAIT-1` in es-1's socket table any more, and **zero no-progress cancels
since the 0.47.0 restart at 21:46:57 on 2026-09-20** (40 that day before it,
247 on 2026-09-19). Progress accounting was audited and is sound in both
directions — `touch()` fires per partial write from the writer and the reader,
odd/even request ids handled symmetrically on the dialled and accepted paths
(`src/net.cpp:1492`, `:1574`, `:3330-3337`, `:4143-4149`) — so a 30 s idle
really does mean zero bytes moved, and that part is still open.

- [x] Root-cause the read-only blink. Done 2026-09-21: a hung health probe was
  given the whole liveness budget, so it expired the peer it was proving alive.
- [x] Regression test for the probe budget. Done 2026-09-21:
  `rpc_cluster/test_a_hung_health_probe_is_retried_inside_the_liveness_budget`.
  The stall fixture *does* reach the probe — `call_async_known` consults it
  (`src/net.cpp:2231-2235`) and `health_loop` dials through that path — so
  holding `MessageType::ping` hangs the probe exactly as the field did, and
  `stalled_calls_for_tests()` counts the attempts. Three attempts inside one
  liveness window with the fix, one without: verified failing against
  `401cd04^`.
- [ ] **The stall itself is still unexplained.** Establish why an exchange
  moves zero bytes for tens of seconds on a link measured at 13.8 MB/s with 0%
  loss. It is now a latency question, not an availability one: the blink is
  fixed whether or not the stall is. Reproduction needs a stall to happen —
  none since the 0.47.0 restart.
- [ ] **Do not treat a no-progress cancel as the bug.** That is discipline 2
  working: the deadline fires instead of waiting forever.
- [ ] Make an ingest survive a transient read-only window. A ten-second blip
  permanently failing a 6 GB job is the user-visible defect regardless of what
  causes the blip, and it is separable from the transport question.
- [ ] **`publish_commit` gathers the durability floor serially**
  (`src/metadata_manager.cpp:751-770`): `store_commit_on` per replica, stopping
  at `required`, no fan-out and no hedge. The slowest of the first `required`
  replicas sets the latency of every metadata commit, and a first choice that
  stalls costs a full deadline before the third node is tried at all. Found
  2026-09-21 while root-causing the above; not yet addressed.
- [x] Stop the forever-dial loop at the offline node. Nothing to do: it was
  ordinary bootstrap of a node that was down, and it stopped when gbni-1
  rejoined.

## P0 — A replica that falls behind cannot rejoin: the materialisation cache is smaller than two snapshots (opened 2026-09-20, WORKED AROUND ON ALL THREE NODES, not fixed)

**gbni-1 returned after three days away, reported `state=online phase=ready`,
and sat 810 generations behind going nowhere** — one core pegged at 99.9% for
42 minutes, no progress logging, no WARN, no ERROR. `health: healthy`
throughout, while roughly 2 TB of its extents stayed invisible to the cluster.

The stack, sampled three times identically:

```
repair_once() -> read_group() -> import_history_from_peer()
              -> MetadataReplica::import_history() -> materialized()
```

**The cause is a cache that cannot hold one useful entry.** One materialisation
of this namespace is **~51 MB**. The default
`dht.metadata_materialization_cache_bytes` is **128 MiB**, so exactly two fit —
and `cur_` and `committed_` are pinned and exempt from eviction
(`src/metadata.cpp`), so they *are* those two. A replica catching up therefore
has **zero usable cache**. Every `materialized()` misses, walks back the delta
chain to a full snapshot because no ancestor is cached, replays it, and is
evicted before the next call can use it.

Measured on gbni-1, 30-second windows:

| | default 128M | with 512M |
|---|---|---|
| cache hits / misses | **0 / 2** | 9 / 3 |
| cache entries | 2 (both pinned) | 3 |
| evictions | 3 | **0** |
| deltas replayed | 457 for 2 reconstructions (~228 each) | — |
| reconstruction time | ~15 s each | — |
| history.log growth | **28 B/s** | **~16,200 B/s** |
| 810-generation rejoin | 3.4 h+ of one core at 100% | **~2 minutes** |

**577x.** Note the failure is silent: `metadata_generation` does not move
during catch-up because the accepted head only advances at the end, so the
only progress signal is `history.log` growing. Same shape as the 0.28.3
quadratic tombstone replay -- a CPU-bound loop that logs nothing.

**What was done, and it is a workaround, not a fix.**
`metadata_materialization_cache_bytes: 512M` was set on gbni-1, es-1 and fi-1
on 2026-09-20 with the operator's explicit authorisation ("development, and
I'd rather not lose 13h or ~2TB of extents -- a manual recovery step is
authorised this time only"). Configs backed up as
`macha.yaml.bak-20260920-matcache` on all three, reasoning written inline.
All three converged at generation 31655 at the time, healthy and writable;
they are at 31663 on 0.47.0 as of that evening.

**The override hides the defect everywhere it is not yet hurting.** 128 MiB was
sized when a snapshot was small; it is now smaller than two of them, so the
cache is structurally useless on any real library and worse at the 100 TB
target. This is P-1's shadow falling on a second subsystem.

- [ ] **Derive the default from observed snapshot size** rather than a fixed
  128 MiB. A cache that cannot hold two of the thing it caches is not a cache.
- [ ] **Pin the import chain's working set for the duration of a walk**, or
  walk forward from a checkpoint instead of backward from each target. This is
  what `ROADMAP.md` already calls "checkpoint-rooted journal-range catch-up for
  a replica that has fallen far behind" -- first observed live here rather than
  predicted.
- [ ] **Make catch-up log progress.** 42 minutes of 100% CPU with no line in
  the journal is discipline 2 unmet: the work is neither observable nor
  bounded.
- [ ] **Make catch-up honour a stop token.** `systemctl restart` on gbni-1
  timed out on SIGTERM and systemd killed it with SIGKILL
  (`macha.service: State 'stop-sigterm' timed out. Killing.`). A node that
  needs SIGKILL to stop is a hazard on hardware that browns out, which is
  exactly gbni-1. es-1 and fi-1 restarted cleanly once caught up, so this is
  specific to the spinning loop.
- [ ] **A node 810 generations behind should not advertise `phase=ready`.**
  Status said healthy while one replica held a three-day-old namespace. Related
  to the aggregation-truthfulness item under P1.

## P0 — The node starves its own control plane with loader I/O (opened 2026-09-19, IN PROGRESS)

**Doing this now, ahead of everything below including P-1.** Finding:
[loader I/O starves control](2026-09-19-loader-io-starves-control-incident.md).
Plan: [apply the governing laws to disk I/O](2026-09-19-governing-laws-on-disk-io-plan.md).

One ordinary ingest on es-1 (0.46.2) took the node to load 11.18 and **91%
iowait, 0% idle**, with single 4 MiB extent writes taking **17.7 s**. Attributed
per process: **macha 34 MB/s write + 9 MB/s read**; qbittorrent-nox 3 MB/s read
and no writes; Plex idle. The node did this to itself.

haproxy recorded **12 `CD--` client aborts clustered at 7.89–8.01 s** across
`catalogue/status`, `catalogue/artwork`, `catalogue/items`, a playback segment
and a session `PATCH`. Requests that are normally sub-millisecond took eight
seconds and the client gave up.

**Re-aimed 2026-09-20 by measurement.** Three harness runs on es-1 (idle
control plus two 8 GB FUSE ingests) settle which law is broken and where:

| vantage | lane | disk | p99 under load |
|---|---|---|---|
| `/api/v1/health` on-box | control | none | **1.2–1.6 ms** |
| `/api/v1/health` at haproxy | control | none | **1.0–2.0 ms** |
| a `web.root` asset on-box | **data** | **NVMe** | **3.3 ms** |
| `/api/v1/manage/unmatched` | data | **sdb1** | **7,808 ms, aborted** |

The web asset is decisive: same lane, same workers, same moment, different
disk — 3.3 ms against 7.8 s. So this is **not** worker starvation, **not** lane
contention and **not** the reactor (0.43.0 closed those; zero `reactor stall`
lines in five days). It is contention for one physical device between loader
writes and interactive reads, arbitrated by nothing.

**Law 3 is not violated** — control touches no disk. **Law 1 is**, on the DATA
backend. Stage 1 is re-aimed accordingly.

Six of the twelve aborts are disk-bound and belong here (artwork ×5, one
playback segment). **`catalogue/items` does not** — it is the in-memory
whole-catalogue copy, i.e. the next P0 down. **`catalogue/status` ×3 does not
either**, and the first reading of it here was wrong: see the bullet below.

Law 3 is enforced in memory (`control_memory_reserve_bytes`), in HTTP threads
(the control lane) and in RPC (the fast-control allow-list). It is enforced
nowhere on the disk, which sits underneath all three:

- `maintenance.max_bandwidth` and the bandwidth fractions bound **maintenance
  only** — repair, rebalance, GC, scrub. Ingest and FUSE publication are
  bandwidth-unbounded by design.
- `background_concurrency` bounds concurrent *operations* (`max(1, nproc/2)`,
  two here). Two are enough to saturate the disk.
- `data_inflight_bytes` bounds bytes in flight, not service time.
- **Nothing measures disk service time.** `grep -niE
  'service_time|io_latency|write_latency|disk_ms' src/` returns nothing.

`DataResourceArbiter` says *"CONTROL does not enter this object"*
(`src/data_work.hpp:95`). That protects control when the pool is the contended
resource, and the disk is contended, shared and modelled nowhere — so control
is not protected by exclusion, only invisible. Note a
`data_control_reserve_bytes` is **the wrong fix** and must not be built:
control never takes a lease.

es-1 sets none of the relevant knobs, but **no available setting would have
prevented this**, which is what makes it structural.

- [ ] **Blocked: four reproduction attempts all failed to saturate the node,
  and the reason is now understood.** Measured iowait fell run over run (52.8%,
  38.1%, 21.0%, 5.4% mean) and no run exceeded load 3.8 against the incident's
  11.18; extent writes peaked at 1.3 s against the incident's 17.7 s. **The
  synthetic load is the wrong shape.** A real ingest reads `/mnt/diskA`
  (`sda`) while writing `/mnt/diskB` (`sdb`), two 9.1 TB disks that on a Pi
  very likely share one controller; my `openssl`-to-FUSE stream read from
  memory and only wrote. **Treat the four PASS results as void** — they
  measure a load the system was never struggling with. A faithful reproduction
  needs a real ingest of content not already in the namespace, and nothing on
  `diskA` qualifies (Greys Anatomy S01-S18 and From S01-S03 are all already
  ingested, so they would dedupe). The three staged torrents are 51%, 24.6%
  and 0% complete.
- [x] Harness built (`run-io-pressure.py`): three vantages, `CD--` counting,
  node vitals on one clock. It falsified two of its own designs before it was
  trustworthy (fork-per-sample measured the TLS handshake; an absolute
  threshold on a WAN vantage failed an idle node) and then falsified stage 1's
  premise. Idle baseline and two loaded baselines recorded.
- [ ] Stage 1 (law 1): `DiskServiceMonitor`, the missing primitive; pressure
  gates **loader/speculative admission on the backend under pressure**, never
  interactive reads. Acceptance needs a probe account with `media_viewer` —
  `anonymous` holds no roles, so every DATA-backed route is a 403 and the
  NVMe probe cannot stand in for one. **Account now exists** (`servertest`,
  all five roles, created by the operator 2026-09-20) and the harness takes
  `--probe-user`/`--probe-pass`. It also now mints its own direct-play session
  via `--media-path` and issues 1 MiB ranged reads at random offsets, which is
  the only probe that reaches the contended disk: artwork (~40 MB total) and
  web assets live in page cache permanently and measured 0.8 ms under load,
  proving nothing. Idle floor with the real probe: health 0.4 ms,
  `catalogue/status` 13.6 ms p99, **viewer read 129.6 ms p99**.
- [ ] Stage 2 (law 1): viewer *latency* as well as viewer *bytes*.
  `data_viewer_reserve_bytes` reserves bytes at admission; a viewer holding
  byte credit still queued behind a 17 s write.
- [ ] **`/api/v1/catalogue/status` walks every artwork id per call** (~418
  here, tens of thousands at target scale) on a route polled every 10 s,
  building a `std::set<ObjectId>` and taking a per-object mutex plus the
  shared `LocalStore::m_` for each. **Corrected 2026-09-20: this is not disk
  I/O.** `LocalStore::has()` answers from in-memory sets for a present object
  and only `stat()`s on a miss. The cost is allocation and contention on the
  very mutex a running ingest holds continuously — which explains the three
  longest aborts better than disk did. Own defect, own fix: cache the count on
  artwork publication/GC, or move the walk to diagnostics.
- [ ] Stage 3 (law 2): hard background floor so pacing can never wedge an
  ingest; pacing must not consume publication retry budgets.
- [ ] Stage 4: service time and paced-admission counters in
  `diagnostics.data_resources`, transition-only logging, docs.

Separate, do not conflate: `put_metadata_commit` to fi-1 takes 5.3–28.4 s and
times out at 30–35 s (24 no-progress cancels in 6 h; metadata read-only
02:50:12–02:53:01). Each commit ships the whole serialised namespace (P-1) off
the same saturated disk — **whether fi-1 is slow or es-1 is too busy to send is
not yet established.**

Also found: **the server logs nothing for a 401/403/400 refusal.** An auth
failure is invisible on-box; haproxy's access log is the only record. And a
`CD--` line carries a substituted `400`, so a 4xx there is a client timeout,
not a rejection — read the termination-state field first.

## P0 — A node accepts inbound RPC before it can answer it (opened 2026-09-20)

**The listening socket opens before `set_inbound_handler` has run, so a peer
that connects inside that window gets an exception instead of a handshake.**
Found while running the suite for the 0.47.0 deploy; not caused by it.

```
INFO node c49856e3c419 listening on 43168 state=recovering
INFO node connection inbound peer=97f6b5a35753 lane=data
uncaught exception: no inbound RPC handler installed
```

`src/net.cpp:1378` (`dispatch_request`) and `src/net.cpp:1898`
(`RpcClient::dispatch_inbound`) both throw when `inbound_handler_` is unset.
Nothing before them refuses the connection politely or defers it; the node has
advertised itself as listening and then fails the first thing asked of it.

**It is load-dependent, not random, which is why it looks like a flake.**
`rpc_cluster/test_metadata_history_checkpoint_recovers_after_crash_between_ack_and_commit`
failed once in a full macOS suite run and passed 5/5 in isolation. Re-running
the whole `rpc_cluster` suite at `MACHA_TEST_JOBS=24 --repeat 6` failed a
*different* case, and a targeted `--repeat 12` of that one reproduced the
exception above 1 in 12. It does not reproduce on es-1's full suite (477 + 10
green there), so a fast, contended machine is what exposes it.

**Why it is a P0 rather than a test problem.** This is the startup path of
every node, and the window is widest exactly when a cluster is recovering
together — a power cut, a rolling upgrade, three nodes coming back at once.
That is the moment peers are most likely to dial in early and least likely to
have a human watching. It is also discipline 1: the node asserts readiness by
listening instead of re-deriving whether it can serve.

- [ ] Do not open the listener until the inbound handler is installed, or
  refuse/defer cleanly until it is. Whichever is chosen, the throw must stop
  being the mechanism, because a peer cannot distinguish it from a node that
  is genuinely broken.
- [ ] Check the same ordering for the other subsystems the listener fronts —
  the promoter and canceller (`set_inbound_transfer_control`) are installed
  separately and have the same shape of gap.

## P0 — The catalogue materialises everything it has (opened 2026-09-17)

The same failure as P-1 in a smaller organ, and — the important difference —
**no format change is required for the core of it.** Plan:
[catalogue demand-loaded shards](2026-09-17-catalogue-shard-demand-load-plan.md).

The catalogue already has the structure the namespace is being given: a root
pointer (`catalogue_root`, `src/metadata.hpp:109`), a manifest of
content-addressed shards (`src/catalogue.cpp:127-129`), a hash selecting a shard
per id (`:131-136`), and a commit that replicates only shards whose id changed
(`:1070-1071`, `:1084-1089`). It then discards the benefit twice: `load_root`
(`:543-571`) merges all 64 shards back into one map, and `commit` re-shards and
re-encodes the *entire* catalogue (`:1062-1072`) purely to discover which single
shard differs. Every read materialises everything; every write traverses
everything, for a 17-byte change.

Eight mutation sites open with `auto current = *current_snapshot();` (`:918`,
`:937`, `:960`, `:1146`, `:1175`, `:1198`, `:1251`, `:1337`). `snapshot()`
(`:829-831`) returns a full deep copy by value. `list()` (`:982-999`) scans and
copies every item and returns the entire filtered set with no paging; `search()`
(`:1001-1020`) scores every item with no index. Estimated ~150-250 MB resident
at 100,000 titles, deep-copied per mutation — but **nothing measures it**: there
is no catalogue equivalent of `snapshot_resident_bytes`
(`src/metadata.cpp:55-91`), which is why that figure is derived from struct
shapes rather than read off a node. Stage A fixes that first.

Because identity is already a root `ObjectId`, a catalogue written by the new
code is byte-identical to one written by the old. Stages B-D deploy by ordinary
rolling restart with no migration.

Two findings worth carrying forward on their own:

- **The shard count is already on the wire and merely refused.**
  `encode_catalogue_manifest` writes it (`:142`), `decode_catalogue_manifest`
  reads it and then throws unless it equals the compile-time constant
  (`:155-156`). Making it growable is a vector and a range check; old manifests
  keep decoding.
- **Profile publication is one commit at a time.**
  `MediaInformationService::publish_one` (`src/media_information.cpp:337-350`)
  uses the singular `put_media_profile` while the batch form
  `put_media_profiles` (`src/catalogue.cpp:932-954`) exists and is used by
  `reconcile_scanner`. Any future field added to `MediaProfile` means a
  per-title backfill: 20,000 commits instead of ~200.

- [ ] Stage A: `catalogue_resident_bytes` modelled on `snapshot_resident_bytes`,
  surfaced in catalogue status. Measure before sizing.
- [ ] Stage B: `commit` carries the changed id set; touch only those shards.
- [ ] Stage C: demand-loaded shards on the `RetainedMemoryLedger`; `get` and
  `media_profile` become one-shard operations.
- [ ] Stage D: batch publication in `MediaInformationService::loop`.
- [ ] Stage E: indexes for `list`/`search` (additive format change). **Agree the
  paging contract with the client sessions first** — see "What the client
  sessions now depend on" below; `list` currently returns the whole filtered set.
- [ ] Stage F: growable shard count.

## P0 — Nodes that cannot accept inbound connections, and edge nodes (business, opened 2026-09-15)

A node behind CGNAT or a non-forwardable NAT must be a full participant
(mount, playback, ingest) while never being connected *to*; and a node that
serves the API and media to its own network, caching what it plays, must be
able to store no media at all. Plan:
[inbound-incapable nodes](2026-09-15-inbound-incapable-nodes-plan.md) --
`network.inbound_capable` and `storage.hosts_extents`, both self-declared,
gossiped and `auto` by default; reverse-dialled lanes over the existing
inbound-route machinery; no relay (extents live only where they can be
fetched from).

**Stages A, B and C shipped in 0.42.0 (2026-09-15), protocol 21.** What is
left: (1) the cluster UAT on fi-1 described in the plan (point Macha at the
peers' public endpoints so its RPC port is genuinely unreachable; expect
`auto` to resolve false within two probe rounds, mount/playback/ingest to
work, no refused dials to it on gbni-1/es-1, `all_known_reachable` to hold
everywhere, and a 20-minute idle DATA lane not to stall); (2) the open
retention question -- the drain test uses unretained objects, so whether a
draining node's retention claims release without special treatment is still
unproven; exercise a FUSE publication from a node that then stops hosting.

## P0 — The test suite must be deterministic (opened 2026-09-14)

**"Known flake" is not a category. It is the name we have been giving the
decision not to diagnose a failure.** Plan:
[the test suite must be deterministic](2026-09-14-test-suite-must-be-deterministic-plan.md).

Six full-suite runs on one laptop during the 0.41.0 work produced seven
failures across six different cases, every one passing in isolation. One of
them was not a flake at all: it was a real regression introduced that
afternoon (0.41.0 put a second plugin in the build's shared plugin directory,
and that test points two Services at it), and it had already been waved past
once in the same session as "the known flakes". That is the cost, concretely.

**First pass done 2026-09-15 (laptop only).** Measurement tooling landed
(`--repeat`, `MACHA_TEST_LOG_LEVEL`, per-run port salt, `TempDir` starts
empty), seven cases classified and fixed -- four test defects, two
infrastructure defects, one product defect (`RpcServer::stop` executed
queued requests during shutdown). Details, rates and what is still
unproven are in the plan. The Pi-only cases remain.

**Two es-1 cases measured 2026-09-21, both load-sensitive, neither yet
diagnosed.** Named here because the alternative is calling them known flakes:

- `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`
  — fails a full-suite run on
  `metadata replica set forming: waiting for bootstrap checkpoint survey`,
  passes 5/5 in isolation.
- `rpc_cluster/test_ingest_torrent_jobs_visible_and_actionable_from_non_owning_node`
  — 1/5 under `--repeat`, which interleaves across parallel slots; 0/10 when
  run one at a time.

Two full runs of one unchanged tree gave 477/479 and then 479/479, which is
the whole problem in one line. **Also on this item: a before/after comparison
on a node must check `macha-tests --list` counts first.** One was read as
"baseline green, change red" when both runs were the same tree — rsync into a
shared source tree is not a checkout, and the count difference was the only
thing that showed it.

Done means: the full suite passes 20 consecutive times on gbni-1 and es-1 at
CI's real parallelism, no case is documented anywhere as expected to fail
sometimes, and a red run therefore blocks a deploy. The two cases with
measured rates on real hardware are the cheapest place to start; the plan says
which and why. The "three load-dependent test flakes" item below is folded
into this and should be deleted, not re-worded, when its cases are classified.

## P0 — Cluster: three live nodes, and what removing a fourth left behind

**All three live nodes run 0.47.0 as of 2026-09-20 evening**, converged at
generation 31663, `replicas=3/3 required=2`, writable, no WARN or ERROR.
es-1 (ramaroja), fi-1 and gbni-1 (macnessa). The 0.43.0 skew on gbni-1 is
closed: it went 0.43.0 -> 0.47.0 in one jump and logged the expected one-off
`persisted telemetry ignored: blob too large`, which is the documented
self-healing discard after a NodeTelemetry field change and does not recur.
gbni-1's earlier rejoin needed the manual cache override in the P0 above; it
would otherwise still be grinding. It also now serves the web client from
`/etc/macha/web`, which the install tarball does not touch (it writes only
`/usr`). Its clock is now capped at
1.5 GHz to reduce unrecoverable brown-outs, which makes any CPU-bound
metadata work on it correspondingly slower. (As of 2026-09-13 the pair was gbni-1 and
es-1 on 0.40.1.) gbni-2 (inverbeg) was removed by the
operator after its sshd stopped accepting key authentication and it could not
be deployed to. The 2026-09-13 storage incident that opened the previous
version of this file is resolved and ledgered in `COMPLETED.md`.

- [ ] **Removing a node is not a concept this system has, and the tombstone
  that removed gbni-2 is a freshness boundary rather than an eviction.**
  `Membership::observe()` says so explicitly: stale gossip cannot recreate the
  association, but "a directly authenticated peer may establish it again". So
  if that machine is ever reachable again and completes a handshake it rejoins,
  and once re-observed with a fresh `seen_unix_ms` ordinary gossip carries it
  back to the other node. Nobody will have done anything; the cluster will
  simply be three again. A real decommission needs three things that do not
  exist: a durable retired state that survives the node returning, exclusion
  from placement, and re-replication of what it held before it goes.
- [ ] **Nothing knows whether removing gbni-2 cost any extents.** `replicas: 2`
  across three nodes means every object that reached its target still has a
  copy, so the expected state is under-replicated rather than unavailable — but
  `min_write_replicas: 1` permits a write to commit with a single copy, and if
  that copy was gbni-2 the extent is gone. Nothing records which objects only
  ever had one replica. 0.40.1 added `diagnostics.repair` so a node now reports
  what *it* cannot source, which is the first half of an answer; the other half
  is the cluster-wide join nobody has built (see P2 diagnostics). Stored bytes
  at removal: gbni-1 737 GB, es-1 1.82 TB.
- [x] **es-1 has no persistent journal — NO LONGER TRUE, verified 2026-09-20:**
  `/var/log/journal` exists and `journalctl --list-boots` shows two boots, so
  the next reboot is diagnosable. (Originally: so reboots cannot be diagnosed.) It
  rebooted at ~2026-09-13 12:41 (the outage this file opened with) and
  `journalctl -b -1` answers "no persistent journal was found". The cause is
  therefore unknowable after the fact, and will be again next time.
  `Storage=persistent` in `journald.conf` is the whole fix. `last` is also not
  installed there.
- [ ] **A node with zero storage capacity reports itself healthy.** gbni-1 ran
  for a day as `state: "online"`, `data storage ready`, `storage_cap=0.0G`,
  because `NodeRuntime::recover_storage` marks the plane ready whether or not
  any backend came online (`cluster.cpp:387`). Readiness should distinguish
  "no backend configured" from "every configured backend is offline". Belongs
  with the "powered-off node is reported online" item under P1 and the
  maintenance-section gap under P2 diagnostics.

## P0 — Playback correctness and poor-network resilience

**Measured from the live journals on 2026-09-20, seven days, all three nodes.
This is what is actually reaching viewers, as opposed to what is reachable in
principle.** Re-run before acting: these are measurements with a date, not
standing facts.

| Signal | es-1 | fi-1 | gbni-1 | What a viewer sees |
| --- | --- | --- | --- | --- |
| `hold_timed_out` | 35 | 78 | 0 | `500`, fatal on media3, no retry |
| `beyond_hold_window` | 3 | 3 | 0 | `500` in under a millisecond |
| `pipeline reclaimed` | 34 | 9 | 0 | generation gone after 60 s idle |
| metadata -> read-only | 3 | 2 | 0 | writes/ingest stall, playback survives |

**113 timed-out holds in a week is the headline, not the seek refusals.** The
refusal path gets the attention because it is the dramatic one, but it fired 6
times; the hold simply running out of time fired 113. Both surface as the same
fatal `500` on media3, which does not retry a 500 on the HLS path, so the
server's hold is the entire retry budget in the system. fi-1 carries more than
twice es-1's share despite hosting no extents.

gbni-1's zeroes are not evidence of health: it was away for three days of that
window and has served little playback since.

Reproduce with:
`journalctl -u macha --no-pager --since '-7 days' | grep -c hold_timed_out`


- [ ] **The bounded-VOD hold is unmeasured on iOS, and its ceiling is now
  known on Android — device evidence 2026-09-13, iOS still unowned.** The
  complete VOD playlist with bounded segment holds shipped in 0.35 and is
  ledgered; what was never measured was what a real player does with a refusal.
  The phone session measured Android and reported:
  - **media3 does not retry a 500 on the HLS path.** It surfaces immediately as
    a fatal `Source error`. So the server's hold is the entire retry budget in
    the system — there is nothing behind it. That should govern how generous
    the hold is.
  - **The tightest deadline on that client is 8000 ms**, on the music path
    (`DefaultHttpDataSource` defaults); the video path allows 10000 ms
    (`OkHttpDataSource` with no timeouts set). Against a 6000 ms hold that is
    2 s of margin. **Raising `segment_timeout` above 8000 ms starts breaking
    the music path**, so treat 8000 ms as a hard ceiling on that knob.
  - **HTTP status never reaches that client's JavaScript** (`PlayerError` is
    `{message: string}`), so `segment_not_ready` and a dead stream are
    indistinguishable there. The 500-over-503 choice is sound for players that
    read status and buys that client nothing.
  - **iOS has never been measured by anyone** and will not be by that session,
    which has only ever run on Android. AVPlayer's time-to-first-byte deadline
    is unknown. Nobody should plan around that number arriving.
  Both the hold and the per-session cap are server config, so acting on real
  numbers stays cheap if an iOS device appears.

- [ ] **A seek past the produced window is refused instantly, and on media3
  that is fatal — measured on an Android device 2026-09-13, and it is a
  consequence of the complete VOD playlist rather than of the hold.** Confirmed
  against current source, not inferred from the report. `public_stream_response`
  has two distinct refusal paths (`playback.cpp:1976-1981`): a segment inside
  `segment_count + segment_hold_window` (8) is **held** for up to
  `segment_timeout` (6000 ms), while one beyond that window is refused
  **immediately** with `beyond_hold_window`, because nothing is working toward
  it. A seek to the one-hour mark of a 2:43 title lands hundreds of segments
  past production, so it takes the second path and is answered in well under a
  millisecond. **Raising `segment_timeout` therefore cannot help this case at
  all** — an important correction, because the device session proposed exactly
  that, and the 8000 ms media3 ceiling made it look affordable.
  What the device measured: seek at 15:57:56, `InvalidResponseCodeException:
  500` at 15:58:00.724, surfaced as a **fatal** `ExoPlaybackException: Source
  error` with **no retry** — media3 does not back off and re-request a 500 on
  the HLS path. So the server's hold is the only retry budget in the system;
  there is nothing behind it. The client then treated the 500 as node failure,
  stopped a perfectly healthy session on gbni-1, recorded the node as failed and
  restarted from scratch on es-1, discarding 15 s of completed transcode for a
  6.2 s gap. That failover behaviour is the client's defect and they have filed
  it, but the refusal that triggered it is ours.
  Cold start, for contrast, never touches any of this: direct -> transcode
  admitted in 1722 ms, first frame at ~2.2 s, then 2:30 of content played with
  zero load failures, because transcode on gbni-1 runs faster than realtime for
  that title and no segment was ever late.
  **The design gap:** a `PLAYLIST-TYPE:VOD` playlist with `ENDLIST` tells the
  player every segment exists, and a native player seeks by requesting the
  segment at that offset — it does not ask the server first. Production is
  strictly sequential from the session's seek origin, so everything outside a
  9-segment window is a promise the server will not keep. The seek-only PATCH
  that *does* reposition production exists and is cheap
  (`HlsVodPlan::reusable_seek`, `video_random_access_points`,
  `seek_segment_seconds` — no reprobe, no index rebuild), but nothing tells a
  client it is mandatory before seeking, and on this client the seek never
  reaches JavaScript at all.
  **Corrected 2026-09-13, same day, by the device session against its own
  source:** the seek that produced this measurement *did* originate in
  JavaScript — their own scrubber, which already tracks the pending seek — and
  the app simply never told the node about it. So the client-side fix is
  available to them and they have taken it: on a transformed generation, PATCH
  the session with the new position before seeking the player. That narrows,
  but does not remove, the case for the server-side fix: a seek from the
  lock-screen or notification media session on the music path never reaches
  their JavaScript, and nothing else covers it. Video has no such path. Weigh
  option 1 as covering that narrower case rather than "clients cannot tell us
  about seeks".
  **Both server-side candidates below are now closed, and the direction is
  settled (operator, 2026-09-13).**
  - **Implicit seek on an out-of-window request: REJECTED.** Inferring seek
    intent from a read position is unsound — a reader legitimately touches
    distant offsets for structural reasons (an AVI's index lives at the end of
    the file and must be read before anything can play), and repositioning the
    encoder on that would mean re-reading the tail of a multi-gigabyte file
    across the network for a seek nobody asked for. Note this is also what
    Jellyfin/Emby do, and their thrash under scrubbing is the prior art for
    why not.
  - **A growing `EXT-X-PLAYLIST-TYPE:EVENT` playlist: REJECTED, permanently,
    and this has been round more than once.** Static media is not an event.
    The file exists in full; a playlist that declines to say so is working
    around the server's own limitation at the client's expense.
  - **The direction is to pre-package.** Every rendition transcoded and
    segmented before playback, which is what commercial VOD does and the only
    shape with no seek problem at all: seeking is free because every segment
    already exists. The open question is not whether but **how to do it
    smartly** — what triggers packaging, which renditions are worth producing
    for a given library and client mix, where the segments live and against
    what storage budget, how it is paced against viewer and loader work under
    the governing laws, and what a viewer sees for a title that has not been
    packaged yet. That is a design piece, not a patch, and nothing above should
    be built in its place.
  Historical, for the reasoning only — **neither is to be built**:
  - **Treat an out-of-window in-plan segment request as an implicit seek**:
    reposition production to that segment's random-access point in the same
    generation and then hold. Segment indices are plan-absolute, so
    repositioning within a generation is coherent and the client's URL stays
    valid. This makes the VOD playlist honest, and is the only option that
    helps a player which seeks natively. Needs a debounce and a one-reposition-
    at-a-time rule, or a deeply prefetching player will restart the encoder
    repeatedly — `note_segment_requested` already exists to drag the authorised
    window and is the natural place for the policy.
  - **Document the PATCH-before-seek contract** and tell all four clients. Cheap
    and immediate, but it cannot work where the player seeks without telling the
    app, which is precisely the measured case.
  **The two refusals are distinguishable at the HTTP layer without parsing a
  body**, which decides how dumb a native transport module can be:
  `segment_not_ready` is **500** with `Retry-After: 1` and
  `Cache-Control: no-store`, while `stream_failed` is **503** with neither. So
  status alone separates "retry" from "dead", and the body's `reason` is needed
  only to tell the four not-ready sub-cases apart (`beyond_hold_window`,
  `hold_timed_out`, `session_hold_limit`, `hold_budget_exhausted`).
  Also recorded, because it disposes of an earlier argument: the 500-over-503
  choice is not merely inert on that client, it is harmful. It cannot read the
  code, so it cannot distinguish "hold, I am building it" from "this generation
  is broken", and its failover treats both as a dead node.

- [x] **gbni-2 serves reads at roughly a sixth of gbni-1 — MOOT since
  2026-09-13: that node is no longer in the cluster.** Kept here rather than
  ledgered because the lesson outlives the node and will apply to the next slow
  one: a node can be healthy by every status field the cluster reports and
  still be unable to serve playback, because nothing measures per-node read
  throughput and `cpu_cores` cannot express it. If gbni-2 ever rejoins, this
  becomes live again and undiagnosed. The measurement: Client-measured raw read rate with no
  encoder in the path (forced Direct Play, then a plain 8 MB range read):
  gbni-2 0.58 / 0.53 / 0.31 MB/s across three unrelated titles, against
  gbni-1 at 3.31 MB/s. Identical across titles, so it is the node rather than
  extent placement or any particular file.
  This is the actual cause of everything attributed to codecs above, and it
  bounds playback directly: a 21.1 GB / 153 min title needs ~2.3 MB/s of
  source reads for realtime, so ~0.55 MB/s caps a transcode at about 0.24x
  no matter how fast the encoder is.
  gbni-2 is the wireless node
  ([[project-cluster-topology]] records the link as flaky), so the first
  question is whether this is simply Wi-Fi throughput or something in the
  read path on that node. Worth separating with a plain network throughput
  measurement between nodes before looking at storage. Note `cpu_cores` does
  not help a client avoid it: all three nodes report 4 cores, and this is I/O,
  not CPU, so the capacity axis the clients just built sees three identical
  nodes.

- [ ] **1. Correct A/V desynchronisation — partially shipped.** Bounded audio
  drift compensation shipped in 0.23.8/0.23.9 (libswresample `async=1` +
  `swr_next_pts()`, verified ±15ms over 8 minutes with no pitch shift); the
  transcode-seek keyframe-snap and rounding fixes shipped in 0.23.10/0.23.11.
  Still open, per the plan doc's own 2026-09-05 progress note: session-relative
  timeline origin; codec delay/priming applied exactly once by one documented
  owner; monotonic DTS/PTS across encoder flush, fragment rollover and
  generation change; bounded correction of malformed inputs; Direct/Remux
  regressions. **The harness prerequisite is met (2026-09-08).**
  `tests/test_transcode_timeline.cpp` drives the real libav pipeline — no
  injected engine — over a synthesized deterministic source longer than 90
  seconds carrying a non-zero audio start, AAC priming and a seek, and
  measures the published fragments back through libav rather than trusting
  the pipeline's own bookkeeping. It gates: where each output stream starts,
  how much media each carries, per-fragment declared-vs-actual duration, the
  accumulated playlist timeline, and a clean finish. Two cases, ~10s total.
  Measured state on that source: start gap 7ms, A/V span gap 53ms over 100s
  — the drift compensation shipped in 0.23.9 holds. It found two real
  defects on its first run (both ledgered under verified defects below).
  What it deliberately does not cover: **pitch**. A resample-ratio change of
  the kind 0.23.8 shipped keeps the timeline honest while changing how the
  audio sounds, so it would pass. That remains a listening test, and the
  harness says so in its own header rather than implying coverage it lacks.
  Direct and Remux equivalents are not built yet.
- [x] **2. Split lightweight status from expensive diagnostics — shipped
  in 0.39.1, deployed 2026-09-13. Ledgered in `COMPLETED.md`.** Kept as a
  numbered stub so the ordering of this list still reads. The question that
  prompted it is a separate open item: see "Status took 10 s" under P1.

- [ ] **4. Make transformed output bandwidth-aware.** Auto negotiation selected
  H.264/AAC but, without a client maximum bitrate, CRF output expanded a roughly
  5 Mbps source to bursts around 7–12 Mbps. Define a conservative poor-network
  default, honour client limits, and prevent a compatibility transcode from
  silently increasing delivery demand.
- [ ] **5. Build useful buffer margin.** Do not run a four-second-fragment
  producer and network delivery only one fragment ahead. Bound startup work,
  produce ahead under viewer priority, and recover from transient stalls
  without restarting or multiplying sessions. Note: `MediaSegmentStore` never
  evicts fragments and disk spill is unbounded (2026-09-05 code audit) — any
  buffer-margin work should fix that bound at the same time, not paper over it.
- [ ] **6. Remove avoidable cold transformed-planning work.** Persist complete
  immutable profiles and remux/seek-index suitability. Admission must use
  stored information, never wait on speculative profiling, coalesce background
  scans, and retain normal media-engine fallback. The "immutable profile hit"
  fast path in `probe_source()` already exists and takes no probe reads; what
  remains is moving a cold miss off the synchronous foreground request path
  (confirmed via 2026-09-05 code audit of `playback.cpp`).
- [ ] **7. Improve viewer source delivery — confirmed safe to start,
  narrower scope than it looks.** Investigated 2026-09-05: real prefetch/read-ahead
  (`ReadAheadHintProvider`, `HydrationScheduler`, `CacheHydrator`) and real
  locality preference (`DistributedStore::owners`/`ranked`, `ReplicaSelector::order`)
  already exist and do not overlap items 1/3/4/5/6 below (different files,
  confirmed by direct code inspection). The actual gap is narrower than the
  original wording: `CacheHydrator`'s worker pool fetches one extent per RPC
  with no coalescing of adjacent sequential stripes into fewer/larger transfers
  and no dedup of redundant full-stripe copies. Extend `hydration.cpp`/
  `distributed_store.cpp`'s existing fetch path rather than building new
  infrastructure.
- [ ] **8. Optimise codec throughput only after phases 1–7 are measured.** Keep
  proven bounded decoder parallelism and memory lifecycle. Evaluate hardware
  acceleration or more parallelism only against corrected transport/timelines.
- [ ] **9. Run the ordered UAT matrix.** Test Direct, Remux and Transcode; start,
  seek and quality changes; LAN and impaired Wi-Fi; cold/warm profiles; one/two
  logical viewers; and concurrent loader work. Gate on bounded A/V drift,
  prompt Status, stable playback, no ambiguous 404, exact session admission,
  bounded RSS/CPU and prompt teardown. This is one of nine exit gates across
  the whole backlog with no recorded result yet (see plan docs); none of the
  other eight block this one.

The completed logical-viewer entitlement work is the foundation: capacity is
tied to one persistent logical viewer/UI session, not each stream generation.
Keep teardown active until UAT proves that seek, quality changes, disconnects,
supersession, failure and failover cannot leak physical encoders or produce
`transcode limit reached` for one viewer.

## P0 — Seamless handover: three pieces left, in the operator's order (opened 2026-09-20)

**Seamless handover is a business P0 and one of Macha's value propositions**
(operator, 2026-09-20). The operator set the order; piece 1 shipped in 0.47.0
and the rest are unstarted.

1. ~~**Speed factor.**~~ Shipped in 0.47.0 as `stream.production`. Core has the
   full brief, including the parked-producer trap, and has confirmed it back.

2. - [~] **Superseded 2026-09-21 by the playback-session resource P0 at the
   top of this file.** This item said a *client-supplied* session key should go
   **in the route as a query parameter**. Both halves were wrong: no identifier
   in this system is client-generated, and the id belongs in the path, not the
   query string. The diagnosis it carried was right and survives — handover is
   impossible because sessions are keyed on the bearer token
   (`logical_session_for(request.session->id)`, `src/playback.cpp:2288`). The
   operator's "no non-standard HTTP headers" rule also survives, and is why the
   stream token stays a path segment. See
   [the plan](2026-09-21-playback-sessions-as-a-resource-plan.md).

3. - [~] **Absorbed into the playback-session resource P0** (2026-09-21): the
   cap ships with that work, because removing one-session-per-bearer without it
   is the DoS. Stated here in the operator's own words, unchanged:
   **Direct-session exemption plus per-account caps.** Direct sessions
   are exempt from supersession but **still counted against a per-account
   cap**. The constraint that governs the design: *"we need to make sure a
   rogue client cannot under any circumstances launch a media DoS against the
   server"* — an exemption must not become a way for one viewer to occupy a
   node. This is law 1's second clause as admission control: not making the
   viewer wait also means not letting one viewer make another wait.

**HELD, not forgotten: `410 generation_superseded`.** The operator asked for
it to be held (2026-09-20) and the code is deliberately back on `404` with a
comment at the site explaining why. It ships only after macha-client-core
releases tolerance — core maps an unrecognised fragment status to `unknown`
and treats `unknown` as endpoint evidence, so shipping first would make a
superseded generation look like a failed node. **Re-applying it is a small
change; the interlock is the release order, not the code.** Ask Core whether
their tolerance has shipped before assuming this is still blocked.

An open question the operator raised and did not settle: whether it is useful
for a client to know that a generation existed *anywhere* rather than on this
node, and whether that would need cluster persistence or is overkill. He was
taking it to core and the clients.

## P0 — Verified correctness defects (found 2026-09-05, code-audit-confirmed)

None of these came from a TODO/FIXME comment — there are none anywhere in
`src/` or `tests/`. Each was independently verified against current source,
not inferred from docs. All are small and isolated; none require design work.

- [ ] **FUSE journal can hold two inodes on one path** (gbni-1: 11240 vs
  7270 on `/TV/Big.Mistakes.S01E01…mkv`, a create-over of a file whose
  previous inode still had pending data). 0.28.3 resolves it at recovery
  instead of exiting, and re-journals the loser, so this no longer stops a
  node starting — what is still unexplained is why the displacing op did not
  clear the old path in the first place (`unlink`/`rename` journal the
  descriptor *before* clearing `current_path` and rely on the op record at
  replay). Find that, and add the hand-built-journal test: 0.31.0's
  `test_fuse_journal_fuzz_every_frame_mutation_still_starts` fuzzes frame
  encoding, not this shape.
- [ ] **A FUSE mount can stop adopting the cluster namespace (live, gbni-1,
  2026-09-06) — root cause fixed in 0.28.2, three related gaps still open.**
  The mount sat two hours behind its own replica: a namespace op published
  before a restart was recovered as "unconfirmed", its effect had since been
  overwritten, and `refresh_namespace_if_stale()` refused every newer view
  while any op was unconfirmed. 0.28.2 retires published ops at recovery
  and confirms live ones by generation (`NamespaceOp::published_generation`),
  and exposes `namespace_refreshed_revision`/`namespace_available_revision`
  plus `FUSE namespace refresh deferred reason=…` so staleness is visible.
  The `stat` 0-bytes-on-two-nodes finding in the next item is plausibly the
  same mechanism (a mount holding a pre-publication entry): rechecked
  2026-09-06 15:50 before the 0.28.2 rollout, all three mounts already
  agreed on 2421711002 bytes after the day's restarts, which is what a stale
  mount (not stale data) predicts. The op that wedged gbni-1 was
  `seq=149 kind=chmod` on an 'Allo 'Allo episode. Still open:
  - [ ] Adoption is also refused while any op is *queued or in flight*
    (`fuse_frontend.cpp` `namespace-queue`/`queue-race` deferrals). An op
    retrying on a retryable backend error (write floor unavailable, peer
    down) therefore blinds the mount to every remote change for as long as
    the retry lasts. Replace the global gate with per-path protection: skip
    only the paths touched by pending ops (`snapshot_path_shadowed()` in the
    recovery path already has the exact rule), adopt everything else, and
    re-run adoption when the pending set changes.
  - [ ] A crash between `apply_namespace_backend()` returning and the
    `namespace_published` journal marker re-publishes the op on recovery
    unless `namespace_effect_confirmed()` happens to see it; a re-published
    `mkdir` gets EEXIST, which is non-retryable, which wedges the queue
    behind an operator skip. Journal the marker before reporting success,
    or treat EEXIST-with-matching-entry as achieved.
- [ ] **`rm -rf` on a FUSE-mounted directory fails with "directory not empty"
  and has no effect, and `stat()` of the identical path returns a different
  size on different nodes (live findings, 2026-09-06) — likely one root
  cause, not two.**
  - Original report: `rm -rf /mnt/machamedia/*` on `corvus-gbni-1` neither
    deletes anything nor reports a sensible per-entry error — it fails
    outright with ENOTEMPTY, which should not be possible for a plain
    recursive delete of files/directories the caller can already
    `readdir`/`stat`.
  - Corroborating finding, found independently while verifying the 0.25.0
    deploy across all three nodes: `stat` on the exact same path
    (`/mnt/machamedia/TV/Chernobyl (2019)/Chernobyl (2019) - S01E01 -
    1.23.45 (1080p BluRay x265 Silence).mkv`) returns the correct size
    (2421711002 bytes) on `corvus-gbni-1` but **0 bytes on both
    `corvus-gbni-2` and `corvus-es-1`**, reproducibly and stably (rechecked
    twice, ~15s apart, unchanged) — not a transient post-restart race.
    Meanwhile every node's own `/api/v1/status` reports the cluster as fully
    converged: identical `metadata_generation` (7552) on all three, quorum
    validated, health "healthy". So the shared/replicated metadata layer
    believes it agrees, but each node's own FUSE-facing view of at least
    this file's attributes does not actually agree — pointing at namespace/
    attribute *projection* inside `FuseFrontend`/`FileSystem` (turning
    replicated metadata into a local `getattr`/`readdir` view), not at
    metadata replication/consensus itself, which the aggregate numbers say
    is fine.
  - This was found by chance while spot-checking a handful of files during
    deploy verification, not a systematic sweep — treat "affects at least
    one file across at least two nodes" as a floor, not a ceiling, on how
    widespread this is.
  - All three nodes were freshly restarted onto 0.25.0 within the same ~90
    minute window this was found in, but nothing in 0.25.0 touches metadata/
    namespace/getattr code (it only added `run_supervised`, the shared
    `macha_core` build, and the subsystem-plugin scaffolding) — a restart is
    much more likely to have *surfaced* a pre-existing local-view bug (by
    forcing every node to rebuild its namespace projection from scratch)
    than to have introduced one, but this has not been confirmed against a
    pre-0.25.0 baseline and shouldn't be assumed either way.
  - Not yet root-caused. First places to look: whatever `getattr`/`readdir`/
    `unlink`/`rmdir` return for entries with in-flight or already-completed
    namespace mutations, and specifically why a freshly-restarted node's
    rebuilt local view would disagree with another node's for the same
    replicated metadata generation (the P1 scaling item on `readdir` being
    O(entire namespace) touches the same frontend code, but this is a
    correctness bug, not a performance one). Needs a minimal repro (a small
    directory tree, known contents, compare `stat`/`readdir` output across
    all three nodes, then delete it) before guessing further.

- [x] **A hard failure in one subsystem takes down the entire macha process
  — CLOSED by 0.41.0 ("FUSE cannot take the node down any more"), ticked
  2026-09-20.** The FUSE mount is a supervised subsystem in
  `libmacha-fuse`; a lost mount is a `faulted` subsystem that remounts, not
  a process exit; `SIGHUP` reload works on a mounted node. Kept unticked
  for five days after shipping, which is the checkbox-maintenance failure
  this rationalisation exists to correct. Historical record follows.
  (Originally: found 2026-09-05 live incident — foundation shipped in
  0.25.0, FUSE/Torrent migration still open.) `FuseFrontend`'s durable-journal
  replay throws `DecodeError` on any unexpected record, uncaught, which
  crashes the whole node — including its unrelated metadata/RPC/API roles —
  not just the local FUSE mount. Lived this directly: a 0.24.3 bug in
  `skip_blocked_namespace_operation()`'s journal bookkeeping (fixed in 0.24.4)
  crash-looped `corvus-es-1` 49 times because journal replay runs in the
  constructor with no isolation. 0.25.0 shipped the mechanism (Phase 0 of
  [subsystem crash isolation via a plugin architecture](2026-09-05-subsystem-plugin-isolation-plan.md)):
  a mandatory `run_supervised` thread-entry guard on every subsystem thread
  (~30 sites), `macha_core` as a shared library, the `Subsystem`/plugin ABI,
  and `SubsystemSupervisor` (`dlopen` + version-checked load + backed-off
  retry + disable-after-N-failures), verified against real fault-injecting
  `.so`/`.dylib` test plugins. 0.28.0 shipped Phase 1: BitTorrent acquisition
  is now a real `dlopen`'d module (`libmacha-torrent`), reached through
  `TorrentService`/`SubsystemRegistry`, absent-or-faulted per node at runtime.
  This item still does not close: `FuseFrontend`'s constructor — the one that
  actually crash-looped es-1 — is exactly as unprotected as it was until
  Phase 2 moves FUSE into its own plugin and off the main thread. Single
  binary, single process throughout, no separate OS processes/IPC (considered
  and rejected).
  **Sized and re-planned 2026-09-14** as two stages in
  [FUSE behind the subsystem supervisor, then out into a plugin](2026-09-14-fuse-supervised-subsystem-plan.md):
  Stage A supervises FUSE in place (closes this P0), Stage B moves libfuse
  into `libmacha-fuse`. The plugin boundary is `fuse_adapter.cpp`, not
  `FuseFrontend`, so the FUSE tests keep linking `macha_core`.
- [ ] **Terminal durability poisoning is never cleared.** `fuse_frontend.cpp`
  sets `durability_poisoned = true` on any exception during the durability
  batch and nothing ever resets it — one transient fsync failure disables all
  writes on that node for the rest of the process lifetime. Needs an explicit,
  deliberate recovery path (even if only "restart the process"), not silent
  permanent lockout.
- [ ] **Unbounded startup wait with no escape.** `wait_for_initial_namespace()`
  (`fuse_frontend.cpp`) busy-waits in 100ms sleeps forever, with no timeout or
  cancellation, and runs from the `FuseFrontend` constructor before
  `fuse_mount`. A node whose metadata replica never becomes available hangs
  indefinitely with only a debug log line to show for it.
- [x] **`rpc_cluster/test_concurrent_reads_during_divergence_produce_one_reconciliation`
  — FIXED 2026-09-15 (0.43.0), a test defect: two Services' maintenance
  loops reconciled the divergence the test had just created. Verdict in the
  deterministic-suite plan, step 3. Ticked 2026-09-20; the body below is the
  diagnosis as it stood.** Originally: fails 40% of the time on aarch64,
  reproducibly, in isolation — found 2026-09-13. This is the flake worth fixing first, because unlike the three
  below it does not need load to reproduce.** Measured on both Pis at
  `--serial` with nothing else running: **4 failures in 10 runs on gbni-1, and
  4 in 10 on es-1**. It has never failed on macOS/clang locally across many
  full-suite runs, so it is aarch64/GCC or simply timing on slower hardware.
  **Not caused by the 0.39.1 status split**, which was the suspicion when it
  surfaced: es-1 was rebuilt with `src/status_api.*` and the three touched test
  files reverted to their 0.39.0 contents and scored *the same* 4 in 10, so the
  behaviour predates that change.
  It fails at `tests/test_rpc_cluster.cpp:2831`,
  `REQUIRE(accepted_heads().size() == 2)` — the setup assertion, before the
  test's actual subject. The test hand-builds two sibling metadata heads at the
  same generation on one node and expects both to still be accepted when it
  looks. Sometimes only one is. The obvious candidate is that the node
  reconciles the divergence on its own between `make_sibling` returning and
  that line — which is precisely what the test then goes on to measure, so a
  race against it is plausible without any product defect. The other candidate
  is that a head is genuinely being dropped, which would be a real bug.
  Deciding which needs someone to instrument `accepted_heads()` over the gap;
  both answers are useful, and a 40% reproduction rate makes it cheap.
- [x] **`hydration_catalogue/test_ingest_pause_resume_and_cancel_still_work_under_a_worker_pool`
  — FIXED 2026-09-15 (0.43.0), a product defect: two concurrent imports both
  created the shared scanner root and the loser's `EEXIST` failed its job.
  Ticked 2026-09-20; the body below is the characterisation as it stood, and
  its `exists` hypothesis was right.** Originally: fails 40-80% of the time
  IN ISOLATION on both Pis — characterised 2026-09-13 during the 0.40.0
  rollout. "Passes in isolation" is false, and has been the
  accepted verdict five times.** This is the measurement the item below asked
  for instead of a sixth sighting, so act on it rather than re-observing it.
  Measured with `--serial --filter`, nothing running but the live node:
  **gbni-1 on 0.40.0, 8 failures in 10; es-1 on 0.39.1, 4 failures in 10.**
  It is therefore **not a 0.40.0 regression** — the unmodified 0.39.1 build on
  es-1 reproduces it, and nothing in 0.40.0 touches ingest. Do not read the
  80%-vs-40% difference as a version effect: the nodes differ in load, disk and
  network, and no controlled comparison was run.
  Every failure is the same shape: the case times out at its full 60 s having
  logged `ingest started workers=4`, one job failing `ingest failed id=…:
  exists`, and two of four copying successfully. An `exists` failure on a
  concurrent import looks like a race between workers over a destination path —
  `ingest.max_concurrent_jobs` and its claimed-set ownership shipped together in
  0.37.0 — and that, not the harness deadline, is the first thing to read.
  One hypothesis was tested and **refuted**: the failing runs also logged
  `subsystem plugin 'libmacha-torrent' build identity mismatch: plugin=0.39.1
  core=0.40.0; refusing to load`, because a freshly built test binary was
  loading the older installed plugin. Installing 0.40.0 so the two matched
  changed nothing — 8 in 10 before, 8 in 10 after. The mismatch is a real
  artefact of building on a node mid-deploy, but it is not this.
  A 40-80% reproduction rate on hardware that is sitting there makes this cheap
  to root-cause, and it is the only one of these flakes that does not need load
  to reproduce.

- [ ] **Three load-dependent test flakes needing a real fix, not another
  isolation-pass shrug — second found 2026-09-08, third 2026-09-13.**
  The third is
  `hydration_catalogue/test_ingest_pause_resume_and_cancel_still_work_under_a_worker_pool`,
  which timed out at its full 60 s under four-way parallel load on es-1 during
  the 0.38.3 and 0.38.5 rollouts and passed in `--serial` isolation at 364 ms
  and 470 ms. Same shape as the two below; noted because the item named two
  tests and there are now three, which starts to look like one shared cause
  rather than three separate races.
  - `hydration_catalogue/test_catalogue_uses_final_state_after_coalesced_metadata_burst`
    has now failed under parallel-suite load and passed in isolation on at
    least five separate occasions across this project's history (three plan
    docs, plus runs on 2026-09-05 and again on 2026-09-08 during the bounded
    VOD playlist work). "Passes in isolation" has been the accepted verdict
    every time; the actual race has never been root-caused. It fails at
    `test_hydration_catalogue.cpp:3827`, a `wait_until(..., 12s)` on superseded
    objects being absent from both nodes' local stores.
  - `invariants/test_status_collects_connected_peer_telemetry_without_client_fanout`
    — **new, 2026-09-08.** Timed out at its full 60 s deadline once during a
    full-suite run, then passed three times in `--serial` isolation at 277 ms,
    275 ms and 526 ms, and passed every subsequent full-suite run that day
    (five or more). Authorship was considered rather than assumed: it was first
    seen on a working tree carrying the phase-1 `wait_object`/playback-route
    changes, but the case builds two `Service` instances and never creates a
    playback session, so neither changed code path is reachable from it. The
    revert-and-reproduce cycle used for the aarch64 hang was deliberately *not*
    run here, because against a flake this rare two green runs on a reverted
    tree are indistinguishable from two green runs without the revert — the
    experiment has almost no power at that sample size, and claiming it settled
    anything would be false precision.

  **Hypothesis worth testing before hunting two separate races (2026-09-08, a
  hypothesis and not a finding).** These may be one problem. Both cases stand
  up multiple `Service`/node instances on real ports, both end in a
  deadline-bounded `wait_until` rather than an assertion, and both fail by
  timing out rather than by asserting anything false. The one failing
  full-suite run measured `wall=81741ms` against 33–37 s for the green runs on
  the same machine that hour — roughly 2.2x slower overall, which is what
  CPU starvation under `effective_parallelism` around 5 looks like. If the
  deadlines in these multi-service cases were sized against an unloaded
  machine, both would be timing bugs in the harness rather than races in the
  code, and root-causing either one separately would find nothing. Check that
  first: it is cheap, and it would explain why five investigations have ended
  in "passes in isolation".

  Do not record a sixth sighting in place of doing this.

## P0 — Security hardening for a network-exposed cluster

Found during the 2026-09-05 code audit. Macha's own `SECURITY.md` already
states the trust model plainly ("Anyone with the cluster key can authenticate
as a node and access cluster data") — these are gaps *within* that accepted
model, on the HTTP surface a client (and potentially the public internet, via
the offsite Spain node) actually talks to. Given the cluster already spans a
home network and an offsite node, this is not a hypothetical exposure.

- [ ] **Wildcard CORS header remains.** `Access-Control-Allow-Origin: *` is
  still sent unconditionally on every response, including mutating ones. Less
  severe now that every route requires a valid session bearer token (a
  malicious page can't drive the API without already possessing one), but
  still not best practice — a page that somehow obtained a token (e.g. one
  leaked to a compromised client) could use it cross-origin undetected. Stop
  sending a wildcard origin on any endpoint that doesn't strictly need it.
- [ ] **The whole HTTP API is reachable from the public internet — and
  anonymous can NO LONGER read the library (headline corrected 2026-09-20;
  it had said the opposite for a week after the body recorded the change).**
  Verified 2026-09-19 on es-1: `anonymous` holds `roles=` empty, and
  `/api/v1/status` refuses it in 0.4 ms with `requires the 'view_status'
  role`. What remains open is narrower: whether `session.allow_anonymous`
  itself stays on, and whether an unauthenticated 7438 should be reachable
  at all. Original finding follows. All three nodes moved to public
  `https://<name>.macha.network` endpoints on 2026-09-12. Verified from outside
  the network: `POST /api/v1/session` with no credentials returns 201, and that
  token reads `/api/v1/catalogue/items`. This is `session.allow_anonymous: true`
  plus the `anonymous` account holding `media_viewer` — correct for a
  television on a LAN, permissive on a public endpoint. **The operator was told
  and chose to keep it** (2026-09-12, "single user alpha"). Revisit before this
  is anything but alpha. To close it: `PATCH` the anonymous account's roles to
  `[]` (no restart, effective next session), or set
  `session.allow_anonymous: false`, or stop exposing 7438 and reach it over
  WireGuard.
  **2026-09-13, asked again and declined for now:** the operator is
  deliberately running with anonymous holding no roles in order to exercise how
  the system behaves in that state, and does not want `allow_anonymous` turned
  off yet. That is a live experiment, not an oversight — do not "fix" it. It
  has a cost: the phone client cannot start a playback session at all while it
  stands, which is blocking the device measurements requested below.
- [ ] **Mixed-version sessions break during a rolling upgrade.** A session
  minted by a pre-0.38 node carries `roles: ["anonymous"]`, which an upgraded
  node refuses with 403 on every route. The session *wire format* is
  compatible; the role vocabulary is not. No compatibility shim exists.
  Operator's call on 2026-09-12, reaffirmed 2026-09-13: "we'll have to deal
  with that for the time being." Upgrade every node promptly, or write the shim
  before beta. (gbni-2, the 0.38.1 node this originally cited, was removed on 2026-09-13;
  no mixed-version node exists today, so this is currently hypothetical —
  until the next node joins on an older build.)
- [ ] **The web client holds a bearer token in JS-reachable storage, and only
  the server can fix it.** Anything in `localStorage`/`sessionStorage` is
  XSS-readable. The node already serves the web client, so a `Secure`,
  `httpOnly`, `SameSite` cookie set on a successful `POST /api/v1/session` and
  accepted alongside the `Authorization` header is same-origin and natural.
  Raised 2026-09-13 alongside the session-TTL question; the operator answered
  the TTL one (30 days stands) and this was deliberately **not** closed with it
  — it is a different question and needs its own decision. Native clients are
  the opposite case: they should hold the token, but in Keychain/Keystore
  rather than plaintext `AsyncStorage`, which is theirs to fix.

- [ ] **Unbounded JSON recursion depth.** `json.cpp`'s recursive-descent parser
  has no depth limit. Combined with the 8 MiB body cap, a deeply nested body
  on any POST/PUT can exhaust the stack. Add a depth limit.
- [ ] **Untrusted length fields drive large allocations before validation.**
  `metadata.cpp` (at least 4 sites), `telemetry.cpp` and `cluster.cpp` each
  read a length/count field off the wire and `reserve()` a container to it
  before reading any of the actual data — e.g. a 4-byte field in `metadata.cpp`
  can trigger a multi-hundred-MB allocation from one small malicious or
  corrupt message. Cap reservations to the remaining message size.
- [ ] **Unsigned identity-reset tombstones accepted from any peer.**
  `membership.cpp` checks only `epoch >= existing` before evicting a roster
  entry and tearing down routes for it.
- [ ] **Unvalidated, echoed `Content-Type` on a bearer-exempt endpoint.**
  `POST /api/v1/catalogue/items/{id}/artwork?mime=` accepts any non-empty
  `mime` query value and `GET /api/v1/catalogue/artwork/{id}` echoes it
  verbatim with `Cache-Control: public, immutable` and no `nosniff`/CSP, on an
  endpoint that doesn't require the bearer token. Validate against an
  allow-list of image MIME types.

## P0 — Structural ingest, metadata and retained-memory safety

Resume the
[structural ingest/runtime remediation](2026-09-02-structural-ingest-runtime-remediation.md)
after the immediate playback correctness blocker. Existing checkpoints remain
valid evidence, but do not prove the end-to-end invariants.

- [ ] **A job being imported still reports `queued` — found 2026-09-10, not
  root-caused.** The head job on es-1 carried `files_total: 1`,
  `bytes_total: 739234786` and a populated `current_file` while its `state`
  read `queued`. That combination should be unreachable: `plan_job()` persists
  `scanning` on entry (`ingest.cpp:1338`) and returns the job to `queued` once
  planning completes (`:1342`), then `import_job()` persists `importing`
  *before* it ever sets `current_file` (`ingest.cpp:1590`-`1605`). So either
  the cluster-aggregated `/api/v1/ingest/jobs` view is merging a stale peer
  copy over the local record, or something resets the state after planning.
  Worth settling because it is why a wedged queue reads as an idle one — the
  operator sees six identical `queued` rows and no indication that any work
  was ever started.

- [ ] **The no-progress counter added in 0.36.9 misses two extent-publishing
  paths, and counts the wrong thing anyway — found 2026-09-10, latent.**
  `FileSystem::write_progress_` ticks in `drain_one_extent()` and `commit()`,
  but `rebuild_step` (`filesystem.cpp:1229`) and the non-pipelined branch of
  `flush()` (`filesystem.cpp:489`) publish durable extents without ticking it.
  A node writing non-sequentially — `rsync --inplace` patching partially
  present files, which is what any resumed import does — can publish at full
  speed while the counter reads flat, and under memory pressure that fails
  healthy work with `EAGAIN` and eventually parks it: the exact failure 0.36.8
  exists to prevent, reintroduced at a different site.
  **Patching the two call sites is not the fix.** The waiter is blocked on the
  shared durable-lower budget, which anyone's release can free, so
  publication's own releases were never the right thing to count; and ticking
  in `rebuild_step` would re-arm every blocked waiter on work that frees no
  memory, masking a genuine deadlock for the length of a long rebuild.
  Move the budget into `RetainedMemoryLedger::acquire()`
  (`retained_memory.hpp:290`), which already owns the loop, the condition
  variable, the deadline and `available_locked()`; the predicate must be **per
  waiter**, not a global release counter, or the deadline becomes decorative on
  a busy node and the original wedge goes undetected again. Re-derive
  `publication_no_progress_deadline_ms` against the corrected counter rather
  than inheriting 30 s. Keep a publication-specific Status field alongside any
  ledger-wide one — its absence is what made es-1 unreadable.
  Not fired to date: `waits.loader` is 0 on all three nodes across 15 hours of
  heavy ingest, because 0.36.9's bound removed the memory pressure the bug
  needs. Full write-up in
  [`2026-09-09-publication-hold-and-wait-plan.md`](2026-09-09-publication-hold-and-wait-plan.md).

- [ ] **The open-writer bound is soft and can overshoot — found 2026-09-10.**
  `runnable_data_locked` tests `writer_cap_reached()` before selecting an
  inode and the worker opens the writer afterwards, so N workers can each pass
  the test at bound-1. Overshoot is up to `commit_workers - 1`; es-1 reported
  `peak_open_publications` 9 against a bound of 8 within a day of the deploy.
  Harmless as configured (9 writers is 108 MB against a 512 MB durable-lower
  budget) and the misleading comments are corrected, but making it exact needs
  the slot reserved at selection time under `data_queue_mutex`, the way
  `reserved_video_transcodes` already reserves a transcode entitlement across
  its admission window.

- [ ] Finish process-wide retained-memory ownership bounds for decoded metadata,
  catalogue/profile state, reconciliation retries, RPC/reassembly, object
  payloads and playback. Retest unexplained idle RSS growth and the fixed
  `hydration executor is stopping` shutdown race.
- [ ] **Confirm the decrypted history/materialisation cache is bounded by
  bytes, not just entry count, and sheds under pressure.**
  `metadata_materialization_cache_bytes` exists as a config default and is
  reported in Status, but no audit or test has watched it actually shed
  end-to-end under memory pressure. (The compaction protocol itself shipped
  in 0.23.6 and the `accept_commit()`-holds-`m_`-across-`persist()` half was
  fixed in 0.24.3; both are ledgered.)
- [ ] **Retention journal grows ~N² over a large import.** Each publication
  quantum re-claims every extent of the file, so a file published in K
  quanta writes K × extents retention entries. Recorded during the 0.32.10
  measurements in [`2026-09-06-self-healing-uat.md`](2026-09-06-self-healing-uat.md)
  and not acted on. The rest of that incident's programme — the serial
  `has_on` loop, the writer-side barrier, the CONTROL put fan-out — shipped
  in 0.26.0 and 0.32.7–0.32.10 and is measured on the real cluster there
  (es-1 retention avg 5,217 ms → 121 ms, no deadline-exceeded since 0.32.7).
  The other follow-ups that run recorded are also still open: present-content
  skip on writer restart
  ([plan](2026-09-06-skip-redundant-replica-writes-for-present-content-plan.md)),
  journal compaction while busy, compact extent encoding, and 9 standing
  conflicts that need a human.
- [ ] **Short-circuit unlink of an in-flight (not yet published) write —
  raised 2026-09-06, during the `has_on` incident.** Confirmed against
  the actual code: `FuseFrontend::unlink()` (`fuse_frontend.cpp:5311`) only
  detaches the pathname from `state_->paths` and journals the op; it does not
  touch the inode's `data_ops`/`durability_pending`/`unconfirmed_data_entry`
  state — nor do rename-over or truncate. `journal_data_abandoned` is reached
  only from `abandon_corrupt_data` and from *recovery*
  (`test_fuse_recovery_abandons_publication_for_file_removed_from_namespace`),
  never from the live namespace path. The
  `refresh_namespace_if_stale()` comment confirms this is deliberate today:
  "Open handles and dirty state retain the detached inode object." So an
  unlinked-but-still-publishing file's data keeps flowing through the full
  pipeline (spool → durability → distributed publish → retention) to
  completion, and only afterward does it become ordinary garbage eligible
  for the `garbage_grace` GC pass — wasted CPU/disk/network for content that
  is already known, at unlink time, to be moot. Wanted behaviour: on unlink
  of a dirty inode with no writable handles, (1) retire its unpublished
  `data_ops` and stop scheduling further publication work for it, (2) reclaim
  its spool bytes immediately rather than waiting through
  durability+publish+grace, (3) ensure the eventual durable history records
  "never existed" rather than "created then deleted," so retention/GC never
  has to process those extents at all, not even later.
- [ ] **Catalogue does not react to a committed unlink — raised 2026-09-06,
  same discussion.** When a namespace unlink is actually durably committed/
  synced (not the short-circuit-unlink case above, which is about aborting
  publication early — this is about the ordinary case where a real file is
  genuinely removed), nothing today notifies the catalogue layer to check
  whether that removal affects any catalogue item. `manage_api.cpp`'s
  `DELETE /api/v1/manage/filesystem` handler calls `hints_.erase_prefix(path)`
  (line ~805) on that one path, but that only clears scanner *hints*, not
  catalogue items, and the ordinary FUSE `unlink()` path
  (`fuse_frontend.cpp:4627`) has no catalogue awareness at all. Wanted
  behaviour: on commit, determine whether any catalogue item's full set of
  referenced files is now empty, and if so remove or update that item —
  **not** a naive "unlink one file -> delete the catalogue item," since (a)
  two namespace paths can reference the same underlying file, and (b) a
  single catalogue item may legitimately reference more than one file (e.g.
  multiple quality variants/parts). The invariant to hold: the catalogue
  (and therefore the Movies/TV UI) must never continue showing an item that
  no longer has any surviving backing file — no phantom library entries.
- [ ] **`DELETE /api/v1/catalogue/items/{id}/metadata` hangs** (separate
  issue, previously filed inside the item above). It hangs because its HTTP handler
  currently performs `repair_once()`, materialises and copies the complete
  catalogue, repeatedly scans all items to discover descendants, walks all
  artwork, and waits for the distributed metadata commit before replying. The
  endpoint must promptly validate the item/revision, durably enqueue one
  idempotent clear job, and return `202 Accepted` with a job/status `Location`;
  retries must recover the same operation. Background execution must use an
  indexed descendant closure and delta-sized mutation, remain below control and
  viewer priority, publish completion/failure explicitly, and preserve atomic
  hierarchy removal plus targeted media rematching. Add large-hierarchy,
  concurrent retry, restart/recovery, conflict, failure and latency regressions;
  neither request handling nor status polling may perform repair, full-snapshot
  copying, remote durability waits or artwork-wide scans.
- [ ] Complete durable namespace batching for mixed create/rename/unlink chains.
  Keep rename a safe singleton until crash/restart proof exists.
- [ ] Make spool backpressure smooth and visibly progressive near its configured
  limit, pacing toward measured publication/drain rate instead of alternating
  full-speed bursts and apparent freezes. **Still reproducible 2026-09-10**:
  under a sustained 6 MB/s import gbni-1's spool sat at exactly
  `max_spool_bytes` (16.00 GB) with rsync throttled to 11 kB/s and its own ETA
  reading `??:??:??`. The mechanism is working as designed — ingest is paced to
  publication drain — but from outside it is indistinguishable from a stall,
  which is precisely what this item is about.
- [ ] Prove large-history, partition/sibling-head, cache-pressure,
  unclean-restart and stale-FUSE recovery, then run a guarded overnight
  four-node rsync UAT. Require bounded RSS/swap/history, automatic rejoin,
  writable metadata, no viewer/control regression and no manual mount cleanup.

Do not tune aggregate ingest throughput around known amplification. Older FUSE
throughput, heap-audit and ownership documents remain detailed evidence but are
absorbed here rather than separate active programmes.

## P1 — OpenAPI description of the HTTP API (operator: wanted soon, 2026-09-13; promoted from P2 2026-09-20)

Promoted because the file's own header called it the largest piece of agreed
but unstarted work while filing it under documentation hygiene. It is not
hygiene: two client sessions hold private copies of a wire format because
nothing publishes one, and the route table already knows which role gates
each route (`service.cpp:199-216`), so the document can say so instead of
leaving clients to discover it with a 403 the server does not even log.

- [ ] **Swagger/OpenAPI description of the HTTP API — WANTED, and soon
  (operator, 2026-09-13, upgrading the 2026-09-07 "optional").** This is now
  the largest piece of agreed but unstarted work in this file, and it should be
  generated from the route table at build time rather than written by hand, so
  that it cannot drift from `service.cpp`. Four client sessions currently learn
  the API by reading `status_api.cpp`/`service.cpp`, which is how two of them
  ended up holding private copies of a wire format. Publish an OpenAPI 3
  document for `/api/v1/*` — session, health, status, catalogue, playback,
  manage, users, ingest — and serve it from the daemon at
  `/api/v1/openapi.json`, with a Swagger UI page. **Generate it from the route
  table at build time**: a hand-written document drifts, and drift here is
  exactly the failure it exists to prevent. Note the route table is also where
  the role gating lives (`service.cpp:199-216`), so the document can state
  which role each route requires rather than leaving clients to discover it
  with a 403.

## P1 — A generation can be reclaimed between its playlist and its first fragment (opened 2026-09-18)

**A robustness question raised by a live observation whose own cause turned out
to be elsewhere.** On the night of the 0.46.0 deploy a client fetched the media
playlist at 01:00:50 (node local), requested nothing at all for the next 60 s,
and the node reclaimed the pipeline at 01:01:50 with `idle_ms=60000`. The first
fragment requests arrived at 01:02:43 -- 53 s after the pipeline was gone -- and
failed until the client gave up and created a fresh session at 01:03:32.

**Do not read that 113 s gap as a real client being slow.** The client session
identified its own likely cause the same night: the run was in a programmatically
created Chrome tab that was never brought to the foreground, and a backgrounded
tab's timers are throttled to roughly once a minute, which is what hls.js taking
113 s to issue its first fragment request looks like. readyState 0, nothing
buffered and no error is that client's documented tab-visibility signature. Not
proven -- the tab was gone before `document.hidden` could be read -- but it is a
previously observed property of the test rig and it has to be eliminated first.
The next run will be in a confirmed-visible tab with `document.hidden` recorded.

Two client-side alternatives were proposed and both are ruled out, so they are
not re-proposed here: the past-the-frontier stall (the node never refused a
fragment -- no `segment_not_ready` line exists in the window, at DEBUG, with
DEBUG on), and a seek-on-`canplay` wait in the client's handover path (that
transition was direct play to remux, and the handover requires both sides to be
managed HLS, so it was never eligible and the client's log shows the teardown
path instead).

The question below survives that regardless, and both sessions agree it does: a
client on a weak link could take that long between manifest and first fragment
for honest reasons, and what the node does then is worth knowing.

**What is established.** No `playback stream refused` line exists anywhere in
the window, at DEBUG, with DEBUG on: the node never held or refused a fragment,
so this was not a past-the-frontier stall. The playlist itself was correct and
complete -- `MediaSegmentStore::playlist()` writes one EXTINF per planned
duration plus `ENDLIST`, all 482 of them. The seek fields were right
(`984818 + 3182 == 988000`). The node restarted clean immediately before this
and logged no WARN or ERROR.

**What is not established, and it decides ownership.** Whether a fragment
request arriving after idle reclamation revives the pipeline or simply fails.
If it revives, that run's 01:02:43 requests should have succeeded and something
else broke them. **If it does not, then any client that takes more than 60 s
between its manifest and its first fragment loses its generation with no way
back** -- and a client that is slow to start is not doing anything illegal, even
if the client that exposed this was slow for a reason no viewer will ever hit. Read the reclamation path in `src/playback.cpp` (the log line is at
`playback pipeline reclaimed after stream inactivity`) against the segment route
before touching either.

Also unexplained, and on the client's side of the boundary: its media element
sat at `currentTime` 988.00 -- the absolute title position, not the 3.182 s
offset into the generation -- with `readyState` 0 throughout. That alone does
not explain the silence, because a client trying to load at local 988 s would
have asked for a fragment immediately and the node would have logged the
refusal.

- [ ] Determine whether a segment request revives a reclaimed pipeline. That is
  the fork; do not change a timeout before answering it.
- [ ] Have the client report the gap between `hls-manifest-parsed` and the first
  `hls-fragment-loading` **from a confirmed-visible tab**. Over 60 s there makes
  it a real client shape rather than a throttled harness.

## P1 — `look_ahead_ms` lies to a session that was already running when the config reloaded (opened 2026-09-20)

**Found by the Web Client session asking whether a value it had measured
could move, not by a failure.** The answer is worse than "it can move": the
reported figure and the generation's actual behaviour move independently.

`session_json` computes `look_ahead_ms` from the live configuration
(`src/playback.cpp:1768`, `config.max_ahead_segments *
config.segment_duration`), and `PlaybackManager::reconfigure`
(`src/playback.cpp:2987`) updates both knobs on a `reload_config` — its own
comment says "playback timing appl[ies] to subsequent sessions immediately".

But a running generation's producer gate is not live. `MediaSegmentStore`
takes `max_ahead_segments` as a constructor argument and stores it in
`impl_->max_ahead` (`src/media_segments.cpp:212`); there is no setter and no
`reconfigure`. The `cv.wait` predicate at `src/media_segments.cpp:146` uses
that construction-time value for the life of the generation.

**So after a SIGHUP that changes `max_ahead_segments`, an in-flight session
reports a frontier its own producer will not honour, and nothing on the wire
says so.** A client that re-reads the field per session — which is exactly
what the 0.45.0 documentation tells it to do, and what it must do — gets a
number that is authoritative for new generations and wrong for this one.

**The harm is a viewer wait taken unknowingly.** Arriving beyond the real
gate does not refuse; production is sequential, so the node encodes its way
there at roughly real time while the viewer waits. That is a lawful bounded
exception to law 1 *when the client chose it*. Here the client declined to
choose it, on the node's own figure.

**A second divergence, same root.** `segment_hold_window` is read live at
request time (`src/playback.cpp:2136`) while the producer gate is not, and the
comment at `src/playback.cpp:1757` states the invariant they are supposed to
maintain: "segment_hold_window is deliberately the same distance, so a request
inside this window is one production is authorised to reach and a request
outside it is one nothing is working toward". After a reload of
`max_ahead_segments` alone, that sentence stops being true, and the refusal
boundary and the production gate are set by different generations of the
configuration.

**Note what is already correct**, because it shows the shape of the fix:
`stream.production.producer_parked` (0.47.0) is derived from the store's own
`impl_->max_ahead`, so it tells the truth about the running generation while
`look_ahead_ms` beside it does not.

Options, in preference order:
- [ ] Report `look_ahead_ms` from the generation that will serve it, not from
  the config — the store knows its own `max_ahead` and `target_duration`, and
  `producer_parked` already reads them. Falls back to config only where there
  is no store (direct play, pre-pipeline).
- [ ] Make the gate live: give `MediaSegmentStore` a setter and notify the
  condition variable. Larger change, and it silently retimes a generation
  under a viewer, which is the thing law 1 dislikes.
- [ ] Decide the knobs are not live for playback at all and say so in
  `reload_config`, alongside the `streaming_restart_required` set that already
  exists for exactly this reason.

**Scope is narrow and the silence is the problem.** It needs a `reload_config`
that changes these knobs with sessions in flight — not an everyday event. But
there is no log line, no PATCH, and no field that reveals the disagreement, so
a client cannot detect it and neither could we from a capture.

**Whichever option is taken, the fix has to be announceable, and that is a
requirement rather than a courtesy.** Raised by the Web Client session: a
client that learns to distrust `look_ahead_ms` will keep distrusting it after
it becomes trustworthy. Clients are already being advised to treat it as
advisory for a generation they did not just create, and that advice does not
expire on its own. So landing the fix silently leaves the field correct and
unused. Say which option landed, in the CHANGELOG and to the client sessions,
and give them something to test against.

**This is a harder case than the absence rule** (see "What the client sessions
now depend on"). An absent field is visibly absent and one branch handles it.
This is a number that looks answered, sitting on the same object as
`producer_parked`, which is answered correctly. Nothing on the wire says which
of the two is current. A convention for absence does not help a client here,
which is why the fix has to be on this side.

## P1 — The seek fast path: NOW TAKEN in the field (corrected 2026-09-20); only the remux decline is unobserved

**MEASURED AGAIN 2026-09-20 AND THE HEADLINE CLAIM IS NO LONGER TRUE. The
fast path is being taken.** Seven days of es-1's journal: **506 `seek
fast-path` lines against 7 `seek fast-path skipped`**, concentrated on
2026-09-17 and 2026-09-18, and still firing today. Every one of the 7 skips
gives `reason=preferences-changed`; none give a `reseek_hls_vod` refusal. So
"every seek pays a full probe/VOD-planning pass while the viewer waits" is
stale and must not be quoted from this file again -- it was true when written
and is not now.

What survives is narrower and still worth doing: the 7 `preferences-changed`
skips are unexplained, and the remux `reseek_hls_vod` bounds below have still
never been *observed* refusing -- which now reads as "no evidence it fires"
rather than "hidden behind a path nothing reaches". Re-measure before
spending anything on it.

**The lesson is the reason this correction is written in rather than the line
being edited.** A backlog entry recording a measurement is only true as of its
measurement, and this one was being read as a standing fact three releases
later. Anything here that says "zero X in the journal" needs a date and needs
re-running before it is acted on.

Original entry follows, with its claim now disproved:

Found by the seek work completed in 0.46.0, not fixed by it. Across a whole day
on es-1 there were **zero** `seek fast-path` lines: every seek pays a full
probe/VOD-planning pass while the viewer waits. The client's seek `PATCH` sends
preferences that compare equal to the stored record — `subtitle_language` is a
plain `std::string` defaulting to `""`, so the client's `""` matches, and
`optional_int` maps a null `subtitle_stream` to `nullopt`, which is what a
subtitles-off session already holds — so `seek_only` should be true.

The remaining branch is `reseek_hls_vod` declining. For remux it re-runs
`indexed_plan` over the keyframes from the new position, and that rejects a plan
whose longest fragment or tail exceeds 90 s. Those bounds are whole-file: a
sparser GOP anywhere else in a long title rejects a seek point that would play
perfectly well. The transcode branch of that same function already carries a
comment warning the check "can spuriously reject an otherwise perfectly usable
seek point if any other part of a long file has a sparser GOP"
(`src/media_engine_common.cpp`) — that warning describes the remux branch's
behaviour and was never applied to it.

Not asserted as the cause: it is the branch that remains, not a proof. 0.46.0
added the diagnostic that settles it — `reuse_seek_session` and
`reseek_hls_vod` now name the failing precondition (`plan-not-reusable`,
`source-duration-unknown`, `segment-length-unknown`, `keyframe-density-bounds`,
`no-prepared-vod-plan`, `direct-mode`), and a non-seek-only PATCH logs
`seek fast-path skipped` with `preferences-changed` or `media-override`. Read
those off a node before changing a bound.

Measured cost while it is broken: 147 `session-update` calls in 34.7 s, 34.68 s
of cumulative server time, ~4.2/s on a node also serving viewers, none of them
individually slow (235.9 ms mean, 1,407.3 ms for the first).

**Superseded by measurement, 2026-09-18 afternoon. The premise of this item was
wrong and the heading has been corrected.** Eight hours of es-1 journal on
0.46.0:

- **11 `seek fast-path` successes.** It is taken, routinely. The "zero across a
  whole day" observation that opened this item does not hold today.
- **2 `fast-path skipped`, both `reason=preferences-changed`.**
- **Zero `keyframe-density-bounds` declines.** The reason I predicted would fire
  has never fired once.

Every success carried `seek_offset_ms=0`, i.e. they were transcode sessions;
`indexed_plan`'s whole-file bounds live on the remux branch and no remux seek
went through `reseek_hls_vod` in the window at all. So the remux half is still
**unobserved rather than answered** — but "probably our bounds" had no evidence
behind it and now has one piece against it. Do not change a bound on it.

The likeliest explanation for the change is the traffic mix (today's sessions
were transcode), not the 0.46.0 code: the transcode branch of `reseek_hls_vod`
never consulted `indexed_plan` before the change either. Stated as the likelier
of two, not as established.

**And the cost this item was chasing is not here.** A 13,433 ms `session-update`
reported by the client as a ~13 s viewer freeze, and attributed by them to a
full re-plan materialising Cues, was none of that:

```
16:22:38  seek fast-path requested_ms=1500000 seek_ms=1500000 seek_offset_ms=0
16:22:38  pipeline start mode=transcode
16:22:38  pipeline seek timing stream_info_ms=2 container_seek_ms=39
16:22:50  first fragment ready elapsed_ms=11672
```

The fast path was taken, the container seek cost 39 ms, and 11.7 s went into
encoding the first fragment of a 4K HEVC-to-H.264 software transcode on a Pi.
Seek latency on transcode is encoder throughput, not planning. Any further work
on the fast path should be justified on its own terms, not on that freeze.

**First live sample, es-1, 2026-09-18 right after the 0.46.0 restart — and it
does not say what the hypothesis above predicts.** The line read
`seek fast-path skipped ... requested_ms=988000 reason=preferences-changed`:
the fast path was not declined by `reseek_hls_vod` at all, it was never
attempted, because `prefs == old->preferences` was false. **Inconclusive, not a
refutation**: that particular session was created `mode=direct` and the PATCH
carried a seek *and* a move to `mode=remux`, so the preferences genuinely had
changed and the log is correct. What it does establish is that
`preferences-changed` is reachable on the live path, so the whole-file bounds
are no longer the only remaining branch. A pure seek PATCH — same preferences,
new position — is the sample that settles it, and core has now located its own
casing boundary (`MachaPlaybackResolver.update()` logs the camelCase object one
statement *above* `wirePreferences`, so the earlier empty-string evidence was
never the wire) and confirms the real body is
`{"preferences":{"subtitle_stream":null,"subtitle_language":""},"seek_ms":...}`,
which compares equal here. So both branches remain open and both are now
instrumented.

- [ ] Capture a **pure** seek PATCH on es-1 or fi-1 and read its reason: either
  `preferences-changed` again (then the comparison is wrong and core's body is
  not what arrives) or `keyframe-density-bounds` (then the whole-file bounds
  are the cause).
- [ ] Decide the re-seek bound on that evidence. Changing `indexed_plan`'s 90 s
  fragment and tail bounds is a separate decision with its own evidence and it
  must not be made by guessing from here.

## P1 — Cluster connectivity, status and operations

- [ ] **Spool usage is not in polled Status (requested 2026-09-14).** The
  numbers exist — `FuseFrontendDiagnostics` carries `spool_bytes`,
  `spool_limit_bytes`, `spool_publish_rate_bytes_per_second`,
  `spool_throttle_waits` and `spool_throttle_wait_ms` — but 0.39.1 moved the
  whole `diagnostics` block off `/api/v1/status` onto its own endpoint, so
  nothing a client polls reports how full the spool is. That is the wrong
  side of the split for this particular number: local write-back admission is
  paced against it, a spool at its ceiling is what a wedged publication
  pipeline looks like from outside (es-1, 2026-09-09), and it is one atomic
  read with no locks and no namespace walk.
  Put the small always-true summary — used, limit, and the publish rate —
  in the cheap always-present part of the status response, next to the
  `subsystems` block, which is there for exactly this reason (0.25.0: "it
  costs nothing to compute and is exactly what an operator needs promptly").
  Leave the per-counter detail in `diagnostics`. Read it through
  `SubsystemRegistry::fuse()` like the other FUSE-facing status does since
  0.41.0, so a node with no mount simply omits it rather than reporting
  zeroes that look like an idle spool.

- [x] **`GET /api/v1/status` took 10 seconds once — RETIRED 2026-09-20 on the
  condition this item set itself:** the 0.43.0 reactor removed worker
  starvation and it has not recurred in five days (zero `reactor stall`
  lines on es-1 since 2026-09-15). The 2026-09-19 incident produced slow
  status responses again, but with a *found* cause (loader disk I/O, the P0
  at the top of this file), not this one. Diagnosis preserved below.
  (Originally: observed by the operator 2026-09-13, cause not found, and the
  obvious suspects are eliminated.)
  What was ruled out by reading the code and measuring the live nodes, so that
  nobody spends the time again:
  - **It is not the handler computing.** `status_response` does no I/O and
    makes no network call; every field is an atomic, an in-memory snapshot or
    a short-held mutex. Ten seconds is a *wait*.
  - **It is not connectivity or UPnP.** Status copies a cached
    `PublicConnectivityStatus`; only `POST /status/connectivity/check` probes.
    Confirmed against the journal — no connectivity or UPnP activity at all in
    the two hours around the observation.
  - **It is not the FUSE or metadata diagnostics providers.**
    `FuseFrontend::diagnostics()` is ~60 relaxed atomic loads and `noexcept`;
    `MetadataManager::cluster_status()` is atomics.
  - **It is no longer the diagnostics locks**, because 0.39.1 moved them off
    the polled route entirely. That is the experiment: if it recurs now, the
    cause is not inside the handler.
  **The remaining hypothesis, untested: HTTP worker starvation.** The API has
  16 workers and a 15 s `keep_alive_idle_timeout`. A worker that has answered a
  request and is waiting for the next one on a kept-alive connection blocks in
  `recv_before` for up to that long, pinned. `queue_has_backlog()` exists to
  close a connection rather than keep it alive when others are waiting, but it
  is only consulted *between* requests — never while a worker is already
  blocked waiting. Ten seconds sits inside that 15 s window, and three client
  families each holding connections would do it. There was exactly one
  established connection per node when measured, which is consistent with an
  intermittent fault under client load rather than a standing one.
  **The five-second test that tells them apart**, next time it is slow: hit
  `/api/v1/health` and `/api/v1/status` on the same node. Health touches two
  atomics and is ungated, so *both slow* means the request never reached a
  handler and it is the worker pool; *health fast, status slow* means it is
  inside the handler and deserves gdb stacks.
  Instrumentation was offered and not built: per-section `elapsed_ms` in the
  diagnostics assembly, plus slow-request logging in the HTTP layer. For an
  intermittent fault that is what converts "saw it once" into an answer.
  **2026-09-15:** the worker-starvation half of this is what the P0
  [HTTP server reactor plan](2026-09-15-http-server-reactor-plan.md) removes
  (idle keep-alive no longer costs a thread; control routes get their own
  lane), and that plan builds the slow-request log. If it recurs after
  Stage A ships, the cause is inside the handler and deserves gdb stacks.

- [x] **`test_storage_data_credit_reserves_viewer_headroom_and_control` hangs
  on aarch64 — NO LONGER REPRODUCES: the full suite was green on es-1 at
  0.45.0 (468 + 10), 0.46.1 (475 + 10) and 0.46.2 (476 + 10), which includes
  this case. Ticked 2026-09-20; nobody recorded which change fixed it, so if
  it returns, bisect the 0.36.x-0.45.0 range. (Originally: pre-existing on
  HEAD, confirmed not from the 0.36.0 work, 2026-09-08.)** The case times out at its full 60 s deadline on both
  gbni-2 and es-1, in the full suite and in `--serial` isolation, on a build
  of current HEAD. It passes in 372/372 on macOS (arm64, AppleClang) and
  passes in 52 ms on gbni-1's older build tree (2026-09-07 04:10), so it is
  both platform- and revision-sensitive: something between that build and
  HEAD broke it on aarch64/Linux. Authorship was established rather than
  assumed — reverting `media_segments.cpp`, `media_engine.cpp` and
  `test_framework.cpp` to HEAD on gbni-2 and rebuilding reproduced the hang
  identically, so the 0.36.0 changes are not the cause. The delta therefore
  falls in the 0.34.x/0.35.0 line.
  The hang is early: the captured output stops after `node metadata ready
  generation=1`, before any RPC result, and no `REQUIRE` failure is printed,
  so the body blocks rather than asserting. The case covers DATA credit and
  viewer headroom reservation — governing-law-1 territory — so a genuine hang
  there is worth root-causing rather than filing as flake. It is *not* a
  flake: it reproduces serially, every run, on two separate machines.
  Note the live cluster has been running affected code since 0.35.0; 0.36.0
  neither introduces nor worsens it.
- [ ] **A powered-off node is reported `state: "online"` (live, 2026-09-08).**
  While gbni-1 was physically dark — no ICMP response, incomplete ARP entry,
  SSH `Host is down` — both surviving nodes' `/api/v1/status` listed it as
  `"state": "online"`. es-1's own roster entry in the same document carried
  `live_age_ms: 14061248` (~3.9 hours) while also labelled `online`. This is
  direct live corroboration of the aggregation half of the Status
  truthfulness item below: the per-sample freshness fix (0.23.3) is working
  in that the age is reported honestly, but nothing folds that age into the
  `state` the aggregate advertises. A node that has been unreachable for
  hours should not read as `online` to an operator or a failover client.
- [ ] **`metadata_quorum_validated` and `metadata_replica_set_validated`
  report `false` with a fresh timestamp (2026-09-08, all three nodes,
  post-0.36.0).** Observed alongside `metadata_quorum_available: true`,
  `metadata_availability: "writable"`, `health: "healthy"`, `conditions: []`
  and three replicas online with generations converging — i.e. the cluster is
  demonstrably fine. The `*_validated_at_unix_ms` values were only ~20 s old,
  so validation is running and returning false rather than never running.
  Either the flag means something narrower than its name suggests, or it is
  wrong; either way an operator reading Status cannot currently tell.
  Possibly the same aggregation gap as the item above. Not a deploy blocker,
  not yet diagnosed.


- [ ] Support multiple advertised endpoints per durable node (LAN/WAN,
  IPv4/IPv6 and configured/discovered), multiple bootstrap candidates,
  reachability-aware racing/fallback, expiry and deduplication by node identity.
  Integrate UPnP/external-IP discovery. Test poor Wi-Fi, partitions, NAT without
  hairpin, endpoint changes, simultaneous dial and commit interruption.
- [ ] Revisit UPnP/external-address discovery sources generally (currently only
  wired to the RPC port's `network.advertise`/`connectivity.advertised`); when
  this happens, fold in the 0.23.7 per-node advertised API endpoint
  (`catalogue.api.advertised_host`/`advertised_port`, `nodes[].api_host`/
  `api_port`) added for any-node Direct Play failover, which today is a static
  config-only override with no UPnP/external-IP probing of its own.
- [ ] Make client/API failover preserve one logical operation: reuse session
  idempotency keys, reconcile ambiguous POST results, fail over endpoints, and
  never turn transient transport loss into a misleading 404 or duplicate lease.
- [ ] **Unconfirmed, flagged during 0.24.0 joint client/server live testing:**
  the client's cluster discovery advertised a node as `toms-macbook-pro.local`
  rather than `127.0.0.1`, and a request to that `.local` hostname sat pending
  (possibly slow mDNS resolution, possibly a CORS mismatch for that specific
  origin). Not investigated — surfacing here since it may be the same
  advertised-endpoint-discovery gap as the item above, or a distinct client-
  side resolution issue.
- [ ] **Correct aggregated Status truthfulness and freshness — partially
  shipped.** 0.23.3 fixed the *freshness* half (a stale sample now reports
  explicit `unavailable`/`recovering` state rather than fabricated zero or
  stale-as-live data) — confirmed directly against `status_api.cpp`. **Still
  open:** the *aggregation* half. This was independently reproduced live
  *after* 0.23.3 shipped (`2026-09-03-fuse-terminal-recovery-loop.md`): node
  200 reported `state: online` while genuinely recovering at generation 0, and
  separately reported ES-1 as green at generation 0 in its own aggregate view
  while ES-1's own endpoint reported healthy/writable at generation 4507.
  **2026-09-09: measured on the live cluster and split into three causes in
  [node telemetry visibility](2026-09-09-node-telemetry-visibility.md).**
  Causes 1 (a stale sample blanked its whole `runtime` block) and 2
  (`metadata_generation` preferring a membership record carrying 0 over a
  live sample) are fixed there. **Cause 3 also shipped, in 0.36.7**: gossip
  was sent only after 2 s free of foreground *and* read-ahead work and then
  only onto an idle writer, so a node went invisible exactly while busy or in
  trouble. Both gates are gone, gossip runs every tick on the SPECULATIVE
  class at `network.telemetry_interval_ms` (10 s), and a demand-driven wake
  publishes sooner but no more than once a second. **Still open:** audit
  how a node folds a peer's telemetry into its own aggregate response — this
  is a different code path from the per-sample freshness fix. This is likely
  the same underlying gap as playback P0 item 2's Status-latency investigation
  above; resolve together rather than tracking twice.
- [ ] Add optional display-only `node_name` at `.nodes[].node_name`; configure
  `Corvus GBNI-1`, `Corvus ES-1`, `Corvus FI-1` and `Corvus MacBook Pro`.
  (`Corvus GBNI-2` dropped 2026-09-20: that node left the cluster.)
- [ ] Complete hard-kill stale-FUSE recovery proof and automatic clean rejoin.
- [ ] **Torrent session health does not reach the HTTP API.** 0.37.2 fixed the
  bind defect and made libtorrent alerts visible in the journal, but
  `torrents/status` still says nothing about listen endpoints or DHT, and
  `TorrentJob` carries `peers`/`seeds` as bare counts with a free-text `error` —
  so a client cannot tell a dead session from a slow swarm. The macha-client
  team asked for exactly that on 2026-09-10: session health with structured
  warning codes; per-job trackers, stall durations, connected-vs-candidate
  peers, structured errors. It needs new `TorrentJob` fields, a persistence-shape
  change and the cluster RPC bridge to carry them, so it is real work rather
  than serialisation.

- [ ] Diagnose faulty torrent/ingest independently so it does not obscure
  convergence and runtime measurements. **2026-09-10: largely answered** by
  the `repair_once()` mutation-mutex item under P0 structural ingest — the
  torrent/ingest subsystem was not itself faulty, it was the most visible
  victim of a node-wide metadata stall. What remains here is the narrower
  original ask: enough per-subsystem signal to tell those two apart without a
  gdb backtrace.
- [ ] Diagnose `ingest failed: metadata acceptance certificate durability
  floor unavailable` failures on torrent ingest once the torrent has
  downloaded, which are also unaccountably slow. **Probably the same root
  cause** as the `repair_once()` item under P0 structural ingest: that exact
  string is thrown by `ensure_accepted_head_durable()`
  (`metadata_manager.cpp:889` and `:899`), which `mutate_impl()` calls at
  `:1675` while holding `mutation_mutex_` — so an ingest commit reports it
  after waiting out whatever else held that mutex, and "unaccountably slow" is
  precisely what a caller queued behind a peer RPC under that lock looks like
  from outside. Re-check this once that item lands rather than diagnosing it
  separately.
- [ ] **No TSan run has ever been made against the concurrency-heavy
  subsystems** (`net.cpp`, `metadata.cpp`, `playback.cpp`). The sanitizer build
  shipped 2026-09-08 (`MACHA_SANITIZE`, whole-tree, with deadlines auto-scaled
  10x under TSan), so this is now cheap — it is simply that nobody has run it.
  The ~47-thread hand-reasoned lock ordering that the 2026-09-05 audit called
  its biggest process gap remains unexercised by a sanitizer. ASan+UBSan across
  the full suite was green as of 2026-09-08. Note CI was **declined** by the
  operator, not deferred: every regression gate is a human running
  `./run-tests.sh`, and nothing runs TSan periodically unless someone does.

## P1 — Scaling cliffs (found 2026-09-05, not yet urgent at current 2-node/home scale)

These are real, verified, O(N) or worse patterns that cost nothing today and
will not stay that way. Listed here rather than P0 because nothing currently
observed ties a live symptom to them — but the worst one (`readdir`) is a
plausible contributor to "the mount feels slow with a big library" if that's
ever reported, and is worth fixing opportunistically rather than waiting for
that report.

**Reprioritised 2026-09-17.** Two of these are no longer "not yet urgent": the
whole-catalogue deep copy and the live-object vector are the same failure as
P-1 above, in two other subsystems. They now have plans of their own —
[namespace Merkle root](2026-09-17-namespace-merkle-root-plan.md) and
[catalogue demand-loaded shards](2026-09-17-catalogue-shard-demand-load-plan.md)
— and are marked below. The rest stand as written.

- [ ] **`readdir` is O(entire namespace).** `fuse_frontend.cpp` iterates *all*
  paths under the global namespace mutex, taking each inode's mutex, for every
  directory listing — same pattern duplicated in `rmdir` and twice in
  `rename`. `FileSystem::NamespaceIndex` already has a parent→children index
  the frontend doesn't use for this. This is the single worst scaling property
  found in the codebase audit. *(2026-09-17: independent of storage format and
  fixable now; listed as Stage F work in the P-1 plan but does not wait on it.
  `FileSystem::readdir`, `src/filesystem.cpp:1633-1642`, shows the shape.)*
- [ ] **SHA-256 plus a heap allocation inside a `std::sort` comparator.**
  `placement.cpp`'s `fallback_score()` allocates and hashes twice per
  comparison, and `StoragePool::ranked()` — hit on every put/get/has/valid/
  remove — sorts using it.
- [ ] **`MetadataManager::node_info()` and `replica_nodes()` are O(n²) roster
  copies per metadata read** (a full roster copy per call to `node_info()`,
  called once per id inside `replica_nodes()`).
- [ ] **`CatalogueHintQueue` is a linear `find_if` over the whole map at 8
  call sites**, with no id→path index — O(N²) per scan.
- [ ] **Whole-catalogue deep copy on every single mutation.** 8 sites do
  `auto current = *current_snapshot();` (a full catalogue copy) then re-shard
  and re-encode all 64 shards for a single-item change. *(2026-09-17: promoted
  and planned —
  [catalogue demand-loaded shards](2026-09-17-catalogue-shard-demand-load-plan.md).
  Unlike the namespace, this needs no format change: the catalogue is already a
  root pointer over content-addressed shards, and even the shard count is
  already on the wire.)*
- [ ] **`StagingArea::reserve()` runs a full recursive directory-size walk
  under its own mutex**, called every 500ms per active torrent and on every
  `GET /api/v1/ingest/status`.
- [ ] **`MediaInformationService::source_for()` linear-scans the entire
  filesystem snapshot** computing a media ID per entry, per lookup, on every
  publication and hint.
- [ ] **`admit_deferred()` is O(all inodes) with a per-inode lock, called
  after every publication quantum.**
- [ ] **`RetainedMemoryLedger::request_shedding_locked` linear-scans every
  live allocation under the global mutex** — exactly under memory pressure,
  which is the worst time to do it.

## P2 — Catalogue and media model

- [ ] Make negotiation representation-aware. One Macha work identity may
  reference several immutable files/assets, each with a persisted profile.
  Select the best Direct representation, then cheapest correct Remux/asset
  combination, then cheapest suitable Transcode while respecting quality,
  stream flags, availability, locality, seek cost and resource limits.
- [ ] Replace provider-shaped public IDs (`tmdb:*`) with opaque Macha work IDs.
  Keep provider IDs internal, preserve aliases during migration, and keep work
  identity separate from immutable content hashes.
- [ ] Update clients to consume immutable profiles and send a useful bandwidth
  ceiling plus a persistent logical-viewer/session identity.
## P2 — Diagnostics and repeatable proof

- [ ] Record reproducible local and four-node benchmark recipes without brittle
  default-suite wall-clock thresholds.
- [ ] Keep diagnostics bounded, snapshot-based and disabled by default when
  they perturb viewer behaviour; never instrument per packet or fragment on a
  critical thread merely to diagnose a P0.
- [ ] **Status has no maintenance section at all — found 2026-09-10.** The
  diagnostics object exposes `convergence`, `data_resources`, `data_store`,
  `filesystem`, `metadata`, `retained_memory`, `rpc_server` and
  `rpc_transport`, and nothing for the maintenance loop: no work remaining, no
  queue depth, no pass progress. So "has background maintenance finished?"
  cannot be answered from the API — only inferred from `DIAG high thread CPU
  name=macha-maint` lines in the journal, and from GC reclaim messages. That
  is discipline 1 of the self-healing plan (background work whose progress is
  observable) unaddressed for the one subsystem that runs continuously. It is
  the same shape of gap the publication counters closed in 0.36.9.

## P2 — An optional plugin publishing cluster state to MQTT (operator, 2026-09-20)

- [ ] **Publish cluster state to MQTT from an optional plugin.** Home
  automation and dashboards want node and cluster state pushed rather than
  polled, and nothing in the product offers that today: the only ways to read
  state are `GET /api/v1/health` (unauthenticated, but three fields and
  nothing about the cluster) and `GET /api/v1/status` (everything, but behind
  a bearer token and the `view_status` role, which is why T.O.M.S is polling
  health on a 15 s timer for a dashboard card).

  Optional and a plugin, like FUSE and torrent, so a node that does not want a
  broker does not link one. Off unless configured.

  Constraints that are not negotiable, because this is a publisher on a node
  that serves viewers:
  - **Law 3.** Publishing is control-class work, not viewer-class. A broker
    that is slow, unreachable or backed up must not delay anything, must not
    accumulate unbounded state, and must not turn into a second way for the
    node to make itself unreachable. Snapshot, publish, drop on backpressure.
  - **Law 4.** A misconfigured or hostile broker must not be able to leave the
    node in a state it cannot recover from by itself. No blocking connect on
    startup, no retry loop without a bound.
  - **Discipline 4.** Compose the payload from the same snapshot Status
    already builds. Do not add a second, divergent view of cluster state, and
    do not instrument anything new on a hot path to feed it.
  - Publishing cluster topology to a broker is an **authorisation decision**,
    not a convenience: `/api/v1/status` is behind `view_status` for a reason,
    and MQTT would be a way around that gate. Decide explicitly what a topic
    may carry, and default to the narrow set.

  Open questions for whoever picks it up: retained messages and Home Assistant
  discovery, per-node topic namespacing, whether the payload is the Status
  document or a deliberately smaller projection, and whether it publishes on
  change or on a timer.

## P2 — Code health and error-handling consistency (found 2026-09-05)

- [ ] **The node-scoped session refusal carries no failure axes, while the
  account-scoped one does** (found by the web client session via core,
  2026-09-21, verified against source). `ResourceLimitError` answers
  `http_error(429, "resource_limit", e.what())` with no `FailureAxes` at all,
  whereas the account cap at `src/playback.cpp:3290` states `scope`,
  `node_healthy` and `alternative_may_succeed`. The comment at
  `src/playback.cpp:3287` says `scope: request` is what tells an axes-reading
  client not to walk — this is the other half of that sentence: **until the
  node-scoped path carries axes too, codes are the only complete signal**, and
  a client reading axes rather than codes gets nothing on the node-scoped
  refusal.
  Smaller than it looks today, and the reason is worth keeping: core does not
  read the axes at all, it keys on codes
  (`ACCOUNT_SCOPED_FAILURE_CODES`, one string, `resource_limit` deliberately
  excluded with the reason written beside it), so every client going through
  core is insulated. Only a client reading axes directly is exposed.
  Related trap, same source: the cap refusal and the `410` carry **opposite**
  `alternative_may_succeed` — `false` and `true` — on the same `scope:
  request` and `node_healthy: true`. Both are correct; the pair misleads
  anyone reading the axes as a set.

Not urgent, but real debt worth chipping away at opportunistically. No design
work needed for any of these.

- [ ] **Duplicated logic that can silently drift.** `fixed_vod_durations` is
  defined byte-identically twice (`media_engine.cpp` and
  `media_engine_common.cpp`); `fuse_frontend.cpp`'s `load_journal` reimplements
  `scan_fuse_journal_frames` inline instead of calling the shared function that
  exists specifically to be the single implementation of torn-tail semantics;
  `net.cpp`'s `PeerConnection` and `Session` are ~450-line near-duplicates;
  three near-identical `MediaInput` implementations exist in `playback.cpp`,
  `media_information.cpp` and `media_catalogue.cpp`; `catalogue_api.cpp` has
  private `json_escape`/`url_decode` duplicating `Json::dump()`/
  `http_url_decode()`.
- [ ] **Empty `catch (...) { }` blocks that discard exceptions with no
  log at all** — concentrated in `net.cpp` (29 as of 2026-09-08, up from 21)
  and `storage_pool.cpp` (10).
  The `storage_pool.cpp` cluster in particular (backend I/O failures in
  `has`/`remove`/`list`/`older_than`/`rebalance_step`/`scrub_step`) will make
  diagnosing a flaky physical disk very hard in the field.
- [ ] **Inconsistent error-signalling contracts.** `DistributedStore::put(span)`
  throws on quorum-unavailable while the sibling overload `put(id, span) -> bool`
  returns false for the identical condition; `LocalStore` mixes bool-for-no-space/
  throw-for-I/O-error/optional-for-absent/noexcept-swallow-everything across its
  own methods. `catalogue_api.cpp` catches `std::exception` and returns 503 for
  what are actually 400-shaped client errors (malformed JSON, unknown kind, bad
  artwork id, invalid `If-Match`) — contrast the correct 400/404/409 ladder in
  `manage_api.cpp`.
- [ ] **Confirmed-dead wiring, worth deleting or finishing, not leaving
  half-connected.** Re-verified 2026-09-08; two of the original five claims
  no longer hold and were dropped (`StorageLock` is used at `cluster.hpp:77`;
  the scanner's `request_media_profiles` is invoked from
  `catalogue_api.cpp:441` via `service.cpp:494`). Still true:
  `PlaybackManager`'s own copy of `request_media_profiles`
  (`playback.cpp:668`) is stored and never called; `status_api.cpp:479`
  hardcodes `metadata_replica = true` for every node regardless of actual
  role; three status fields (`ffmpeg_available`/`ffprobe_available`/
  `ffmpeg_version`, `playback.cpp:2388`) are hardcoded false/false/empty and
  marked "kept for one release" with no tracked removal date; two vestigial
  config knobs (`recovery_commit_workers`, `foreground_commit_workers`) are
  parsed, range-validated and logged but gate nothing.
- [ ] **Tracker list is stale.** 111 tracker errors in five minutes were
  measured on gbni-2 before it left the cluster; `coppersurfer.tk` and others
  have been dead for years, and the list is shared, so the surviving nodes carry
  it too. DHT carries the torrents, so this is noise rather than breakage — but
  it buries real tracker failures, which is what makes it worth a few minutes.

- [ ] **Test-only hooks are live branches in the production hot publication
  loop.** `suspend_loader_for_tests` and `fail_publication_once_after_spool_bytes_for_tests`
  (`config.hpp`) are real conditionals compiled into the shipped binary, not
  behind a test-only build flag. Low risk today, but worth gating out of
  release builds since they're reachable via ordinary config.
## P2 — Documentation hygiene (found 2026-09-05, backlog-adjacent but not code)

- [ ] `TODO/2026-09-03-playback-resilience-and-av-sync-plan.md` cites
  `TODO/2026-08-31-cluster-any-node-playback-failover.md` as tracking a
  client-side fix — that file does not exist anywhere in the repo. Either
  create it with the actual current tracking location or fix the reference.
- [ ] `TODO/COMPLETED.md` is ledgered in patches rather than continuously.
  The 2026-09-08 pruning pass and the 2026-09-13 rationalisation pass each
  moved their own entries across in full, but the 0.24.1–0.35.0 range is still
  only partly represented — the self-healing programme (0.29.0–0.32.0), the
  subsystem-plugin foundation (0.25.0/0.28.0), the playback/streaming work
  (0.32.x–0.34.0) and SPA serving (0.35.0) are recorded in `CHANGELOG.md` but
  not in the ledger. Lower value than it looks: `CHANGELOG.md` covers that
  range properly, so this is tidiness, not lost information.
- [x] **Done in 0.46.3 (2026-09-19/20).** `docs/streaming.md`, `README.md`
  and `VALIDATION.md` were rewritten; `ROADMAP.md`'s "stale voters" was missed
  in that pass and corrected on 2026-09-20. Originally: these four each
  contain claims contradicted by shipped code or by each other (keep-alive
  described as absent when it shipped in 0.23.3, a stale "0.19 metadata
  availability" README headline at 0.23.11, `ROADMAP.md` referring to "stale
  voters" from a voter model abolished in 0.19, and `VALIDATION.md`'s build
  command not matching `CONTRIBUTING.md`/`tests/TESTING.md`). None of these
  are load-bearing for engineering decisions right now, but they will mislead
  the next person who reads them at face value.
- [ ] `TODO/namespace-publication-and-metadata-efficiency.md`'s own checkboxes
  contradict its header ("Phases 0-5 ... complete" while Phase 4 shows all 16
  items unticked and Phase 3 shows 6 of 13 unticked, despite both phases having
  separate docs and a `COMPLETED.md` entry recording them as done). Reconcile
  the checkboxes with the ledger so this file stops contradicting itself.

## What the client sessions now depend on (settled 2026-09-13)

**Mobile walks and charges healthy nodes on any mid-stream player error, and
has done all along** (core, 2026-09-21). This is not a contract and not a
request; it is a standing client defect the server session needs to know
about, because it shapes what node-health evidence from a mobile viewer is
worth. macha-client-rn has no status-to-kind mapping at the player layer at
all — playback errors arrive through expo-video's `statusChange` as a message
string with no code — so on `status === 'error'` the provider calls
`failoverSource` unconditionally, picks another node, and **records a failure
against the node it left**. A routine superseded generation therefore costs a
healthy node a mark in that client's ranking.

`410`'s axes (`node_healthy: true`, `alternative_may_succeed: true`) exist to
prevent exactly this and mobile cannot read them. **0.48.0 does not cause it
and does not worsen it** — mobile is equally blind to the `404` it gets today,
and core verified there is no status-dependent branch anywhere on that path —
but the release makes it legible. Two consequences worth holding:

- **Do not read a mobile client's endpoint-failure record as evidence about a
  node.** It may be a seek that regenerated, not a fault.
- The fix is client work, scheduled by core. The seek case is usually already
  invisible (`repositionTo` repoints before refetching, and
  `errorBlamesEndpoint` declines failover while a seek is outstanding), so the
  exposure is mid-stream errors that are not seeks.

Negotiated with the `@machafoundation/core` session and relayed by it to the
web, Android TV and mobile clients. None of it exists anywhere else in this
repository, and a server change that breaks one of these breaks four clients at
once. Recorded here because the conversation that settled them was
cross-session and will not be in the next session's context.

- **`GET /api/v1/health` is the liveness contract.** No token, no role, works
  during recovery. `200 {"status":"ok"}` when serving, `503` with `starting` or
  `failed` when not, and the HTTP status carries the same answer as the body.
  Core probes it every 10 s for latency ranking, failover and the endpoint
  pre-save gate. It must stay unauthenticated. **Corrected 2026-09-18: it does
  carry `version`, and that is intended** -- it has since 0.42.1, and reading a
  node's running version without a token is how every on-box check and every
  deploy verification is done. The rule it still keeps is the one that matters:
  no node id, no topology, nothing about the cluster. Anything beyond "is this
  node serving, and what is it running" needs `/api/v1/status` and
  `view_status`. The code comment at `src/service.cpp:238-245` still claims no
  version and is stale in the same way this entry was.
- **An old node answers `401`, not `404`**, to that route, because
  authentication happens before routing. Core falls back to
  `/api/v1/catalogue/status` on *any* answer that is not a liveness answer,
  which retires itself once no node needs it. This fact is why a 404-only
  fallback would have fired on every node except the one that needs it.
- **`view_status` gates the Status screen and nothing operational.** It is in
  core's `UserRole` and `USER_ROLES` with a test pinning the order. Health,
  ranking, failover and the connection gate are all indifferent to it.
- **A role-less session gets `403` from `/api/v1/status`, never a reduced
  payload.** There is no reduced-view code path; the gate is above the handler.
  Two client reports of "200 with an empty roster" were gbni-2 (ungated 0.38.1)
  misattributed to gbni-1 — see the endpoint-attribution note below.
- **`/api/v1/status` no longer carries `diagnostics`** (0.39.1). Core never
  typed that block, so it is unaffected; a client whose Status screen reads
  diagnostics needs `/api/v1/status/diagnostics`.
- **`GET /api/v1/session` needs a session and no role**, and re-checks
  `credential_generation` on every request, so it catches revocation and not
  only expiry. A `401` there can mean the account changed underneath the token;
  a client must not tell a viewer their session merely timed out.
- **A PATCH naming `mode` clears `video`, `audio`, `max_height` and
  `max_bitrate`** (`playback.cpp:248`). Unchanged since 0.34.0 and confirmed
  against 0.39.1. Android TV has been told to delete its local rule; if they
  can produce a refusal from a body containing `mode` and nothing else, that is
  a real regression and should be treated as one.
- **A role-less session learning no cluster membership is correct**, per the
  operator: such a session sees only the endpoint it was configured with.
- **`GET /api/v1/users` answers under `items`**, like every other collection,
  from 0.40.0. Single records from `POST`/`PATCH` stay bare. Core has accepted
  either key since its 0.8.0, so no client needed a release.
- **`stream.look_ahead_ms` on the playback session payload** (0.45.0) is how far
  past the fragment it last requested a client may arrive and still find media
  already produced: `max_ahead_segments` x `segment_duration_ms`, `null` for
  direct play. Added because neither knob was on the wire or in the
  configuration reference, so a client could only hardcode 8 and 4000 and
  under-run against a node configured differently. Clients bound themselves
  against this rather than against the defaults. It follows `reconfigure()`, so
  it is read per session rather than cached across a node's lifetime.
- **`seek_ms`, `seek_offset_ms` and `seek_requested_ms` on the playback session
  payload** (0.46.0), on create and on every `PATCH`, all milliseconds on the
  title's timeline. `seek_ms` is where the generation's media begins, which is
  exactly what it has always meant; `seek_offset_ms` is how far into that
  generation the requested position sits; `seek_requested_ms` is the request the
  server honoured after clamping to `[0, duration - 1 ms]`. The invariant is
  `seek_ms + seek_offset_ms == seek_requested_ms`, exactly, in integer
  milliseconds, with no tolerance and no rounding slack, and the offset is never
  negative. The server does not move a position a client asked for and does not
  substitute a mode a client asked for: transcode and direct are frame-accurate
  with a zero offset, remux takes the last keyframe at or before the request and
  publishes the remainder. The offset is zero exactly when the mode can be
  frame-accurate, so a client wanting a cheap aligned seek asks for a position
  that already is a keyframe. Core types all three as `number | undefined`
  because an older node omits them, and deletes its `activationPosition`
  undefined branch — the invariant makes that state unreachable.
- **A node reports the playback budgets it enforces** (0.46.2) on the per-node
  entries of `GET /api/v1/status`, in a `playback` object beside `runtime`:
  `startup_timeout_ms` and `segment_timeout_ms`. They are each node's statement
  about itself, relayed like `load1` and `cpu_cores`; no node computes a
  cluster-wide figure, because telemetry carries no peer's streaming
  configuration and it would be inventing one. A client that needs a worst case
  across candidates composes it itself, since only the client knows which nodes
  those are. **Absent means the node cannot say** -- an older node, or one with
  streaming disabled -- and must never shorten a client's own budget, nor be
  filled in from another node's figure. Deliberately not on the session
  payload, unlike `look_ahead_ms`: these bound the request that creates the
  session, so a client cannot learn them from the response it is timing out on,
  and a node it has never used would never report them. Agreed with the core
  session on 2026-09-18 after it showed that a `playback/status` placement
  could not ride its existing health probe without regressing latency ranking
  for role-less sessions.
- **`pipeline_idle_ms` bounds how long a client may hold a generation before
  first requesting media.** A transformed session whose stream has been idle for
  `streaming.pipeline_idle_ms` has its physical pipeline reclaimed; the logical
  session and its entitlement survive, but the producing pipeline does not. The
  default is 60,000 ms, the configured minimum is 10,000, and the live value is
  already reported by `GET /api/v1/playback/status` as `pipeline_idle_ms` -- so
  a client with a standby or handover window must read it rather than assume 60 s,
  exactly as it must for `look_ahead_ms`. Core's standby windows (8 s transcode,
  30 s otherwise) sit inside the default, but that relation was implicit until
  2026-09-18 and nobody had written it down. **What is NOT yet settled is what
  happens to a fragment request arriving after reclamation** -- see the P1 above;
  until that is answered no client should treat a reclaimed generation as
  recoverable.
- **`session_unused_idle_ms` means "never served a stream object", not "idle".**
  Asked by the Android TV session on 2026-09-20 after a paused **direct-play**
  session survived 4.5 minutes against a 120,000 ms `session_unused_idle_ms`.
  That is correct and deliberate. A session carries a `stream_served` flag set
  the first time it serves **any** stream object -- playlist, fragment,
  subtitle or a Direct Play ranged body -- and never cleared, across seeks and
  quality changes. Once set, the session gets the full `session_idle_ms`
  (30 min default); only a session that has *never* fetched media is held to
  the short clock. Direct play sets it explicitly, with the reason in the code:
  "a single ranged body can outlive several idle windows without another
  request, so this must count as having been used." Both clocks measure from
  the session's last interaction of any kind, so a client still polling or
  PATCHing is never evicted by either. The short clock exists to stop a session
  created and abandoned from holding a transcode entitlement, not to reap
  paused viewers.
- **A fragment past the look-ahead is refused, not missing.** `500
  segment_not_ready` with `Retry-After: 1` and `Cache-Control: no-store`, logged
  as `reason=hold_timed_out`; never a `404`, because the playlist has already
  promised the object exists and a `404` invites an intermediary to cache the
  absence. Retrying is correct and succeeds as production advances. Production
  is sequential, so asking for a distant index does not skip the fragments
  before it -- where the gap exceeds the look-ahead, a new generation seeked to
  the arrival point is cheaper than making the current one catch up (measured
  2026-09-17 on es-1: 1.92 s cold start against ~9 s of catch-up for 28 s).
- **A signed artwork URL is stable for up to 24 hours and valid for 24-48.**
  From 0.40.0 `exp` is quantized to a bucket of the TTL, rounded up to the
  bucket *after* next: `(now / ttl + 2) * ttl`. The invariant is "always between
  one and two TTLs", not "between 24 and 48 hours" — the bucket *is* the TTL, so
  reconfiguring `artwork_capability_ttl` moves the bound with it. With the
  default 24 h TTL this lands on a UTC midnight, because the bucket is a
  multiple of 86,400,000 ms from the epoch and the epoch is itself a UTC
  midnight. The minimum remaining validity is 86,400,001 ms, one millisecond
  *over* `max-age=86400`, so a cached copy can never outlive the signature that
  names it. Measured live: 418 refs, 418 identical URLs.
  The artwork `id` has always been the SHA-256 of the bytes and is the stable
  identity clients should key an image cache on; the signed URL is transport.
  The route is bearer-exempt with a valid signature, so a payload's `url` works
  in a bare `<img src>` with no headers.
- **Session lifetime is 30 days from mint, and that is decided** (operator,
  2026-09-13): counted from creation rather than last use, so a session expires
  even under daily use. No sliding expiry, no refresh tokens. Do not re-raise it
  as a defect.
- **`/api/v1/status/diagnostics` gained a `repair` block** in 0.40.1
  (`unsourceable_objects`, `unsourceable_sample`, `local_unreadable_objects`).
  No client types it today.
- **Record which host served a measurement before reporting it.** Two separate
  client reports today were gbni-2 attributed to gbni-1, and the retracted
  DTS/TrueHD investigation in September was the same mistake at larger scale.
  The node is in the URL; there is no excuse for losing it.

## Cluster and repository state as of 2026-09-20

Facts a new session needs before touching anything. None of this is a task.

**Nodes — three are live, all on 0.47.0** (2026-09-20 evening), converged at
generation 31663, `replicas=3/3 required=2`, writable.

es-1 is `10.34.1.50` / `ramaroja.macha.network` (failure domain `spain`),
behind haproxy terminating TLS on 443 with no upstream proxy; it also hosts
Plex and qbittorrent-nox, which share its disk. **It is the build node** — it
has `zlib1g-dev`, which fi-1 does not, so fi-1 cannot build even though it has
more RAM.

fi-1 is `root@10.35.1.50`, an edge node behind CGNAT (`inbound_capable` false,
hosts no extents) that is nonetheless a full metadata replica. **Port 80 on
fi-1 is not Macha** — it is the T.O.M.S web app, which answers
`/api/v1/health` with a `200` and an HTML body. Macha is on **7438**. Anything
probing fi-1 must check `service == "macha"` in the response, not the status
code, or it will report Macha healthy while Macha is down.

gbni-1 is `10.44.1.50` / `macnessa.macha.network` (`test-lab`), back in the
cluster and current. It serves the web client from `/etc/macha/web`. **Never
build on it**; it browns out under load, has no remote power control, and
needs someone on site if it does not come back. Its clock is capped at
1.5 GHz. SSH as `root@`, not `tom@`.

With `metadata_min_write_replicas: 2` and three replicas there is now one
node of margin: **any one node going down leaves metadata writable; any two
takes it read-only** (reads and playback continue; FUSE writes, ingest and
catalogue mutations stall). That margin is new — it did not exist while
gbni-1 was away. It went read-only 5 times across es-1 and fi-1 in the seven
days to 2026-09-20.

**gbni-2 is defunct and expected to stay that way for some months** (operator,
2026-09-20). Do not include it in a deploy, do not wait for it, do not treat
its absence as an incident.

**gbni-2 (inverbeg) was removed from the cluster**, not merely unreachable.
Verified in both nodes' own `known-nodes.bin` **while the cluster was two
nodes**: each listed one known node — the other — plus a tombstone for
`[inverbeg.macha.network]:7437`, `stale=68836f3e0a8b…`, epoch 1, at 18:47:42Z,
propagated to both. That roster description predates gbni-1 rejoining on
2026-09-20; the tombstone is the part that still holds. It was removed because its sshd began offering password
authentication only (`Authentications that can continue: password`) with the
host key unchanged — the same machine, reconfigured — so it could not be
deployed to and was four releases behind. **See the first P0 item: this is a
freshness boundary, not a decommission, and the node rejoins if it ever
completes a handshake again.**

**All three nodes now carry `dht.metadata_materialization_cache_bytes: 512M`**
(2026-09-20, operator-authorised manual step; backups
`macha.yaml.bak-20260920-matcache`). It is a workaround for the P0 above, not
a fix, and a fresh node will hit the 128 MiB default again.

**Versions deployed.** All three nodes run **0.47.0** (verified 2026-09-21:
es-1 answers `/api/v1/health` 200 with `"version":"0.47.0"` unauthenticated,
macha restarted 21:46:57 on 2026-09-20). The paragraph that stood here said
0.46.2 and was already stale when the section above it was rewritten the same
evening — check `/api/v1/health` rather than trusting this line.

**Repository.** `main` and `develop` are both at `cd35f22`, tagged `0.46.3`
(annotated), both pushed. `0.46.0`, `0.46.1` and `0.46.2` are lightweight
despite the convention; left alone rather than rewritten, like the 105 older
ones.

**Branching convention (confirmed by the operator 2026-09-13).** Work on the
long-lived `develop`; releases are tagged on `main`; bare semver; annotated
tags; never name a branch after a version; the version bump goes inside the
release commit. `work-0.38.2` was deleted when this was adopted. Only `main`
and `develop` exist.

**Pushing is allowed when asked**, and only when asked — the operator said
"Yes, pushes are ok" on 2026-09-13 after an earlier "you must never push".
Never push unprompted; never open a PR unless told.

**Live account roles (read off es-1 with `macha-users list`, 2026-09-19).**
Ten accounts: `tom`, `root`, `jonny`, `rnclient` (every role); `ciaran`,
`bryan`, `troy` (manager, importer, media_viewer, view_status); `webclient`,
`tvtest` (media_viewer, view_status); `anonymous` (**no roles**, generation
2). `view_status` **is** on disk for every real account now, contrary to the
2026-09-13 note. **The roles-less
anonymous account is a deliberate experiment**, not an oversight — the operator
is exercising how the system behaves as a registered-users-only deployment. It
means the manage and status APIs refuse an anonymous session, which has already
cost one diagnosis; if something "does not work" from a client, check the role
before reading code.

**The HTTP layer has a slow-request log (0.43.0, `slow_request_threshold_ms`)
and nothing else.** There is still no access log, and — found 2026-09-19 —
**no line at all for a 401, 403 or 400 refusal**, so an auth failure is
invisible on-box and haproxy's access log is the only record. A `CD--` line
there carries a substituted `400`: read the termination-state field before
believing a 4xx.

**Open cross-session threads as of 2026-09-20.** The Android TV RN client
session is actively play-testing against es-1 and has agreed the priority
order above (stalls first; it confirmed the read-only windows cost a viewer
nothing and an ingest everything). Two things are owed in that direction: it
is driving the reaped-session finding — a session reaped on fi-1 came back as
a **video transcode on es-1** for a stream the television had been copying
natively, costing the cluster a transcode slot — and it has taken an action to
log the exact recovery request body, because the server does not log request
preferences anywhere and that is the only way to settle whether core asked for
the transcode. The server side is already cleared: the one documented
substitution path (`media_engine.cpp:1770`, remux→transcode on an unusable
keyframe index) logs at INFO and **that line is absent**, so the server did
not downgrade the mode. A second `Macha Server` session was asked to
deconflict and had not replied by the end of this session.

**A test account exists:** `servertest` / all five roles, created by the
operator 2026-09-20 for harness probing. `anonymous` still holds no roles, so
every gated route 403s without a token — and note credentials nest under a
`credentials` key in `POST /api/v1/session`; a flat `{username, password}`
body is silently treated as an **anonymous** request and returns 201, which
cost this session a wrong-turn diagnosis.

**Build once, ship the artefacts.** Both nodes are aarch64 Debian 13 on glibc
2.41. A `-j2` build on gbni-1 takes ~25 minutes; staging with `DESTDIR` and
shipping a 3 MB tarball takes about a minute. Sync with plain `rsync` and
**never `--delete`**. See `project-cluster-deployment` in session memory.

**Run the test suite on a node, not only locally.** A clean macOS/clang build
is not evidence: 0.38.0 shipped two defects only GCC caught. **Expect it to be
green** — the full suite passed on es-1 at 0.45.0, 0.46.1 and 0.46.2, and a
red run is P0 work, not a footnote (operator, 2026-09-15). The sentence that
stood here until 2026-09-20 told the reader to *expect* a named case to fail
as a "known pre-existing flake": that case was fixed on 2026-09-15 and the
category was abolished the same day. One platform split is real and recorded:
`test_durability_barrier_rederives_placement_after_peer_restart` fails on
macOS/clang and passes on the nodes.

**Commit messages carry no attribution trailers**, by standing instruction. All
refs were scanned on 2026-09-13 and are clean. `CLAUDE.md` in the repo root is
the operator's and is deliberately untracked.

## Deployment rule

Build once, ship the artefacts. Build on es-1 (or fi-1), run the full suite
there, stage with `DESTDIR`, and ship the tarball — `bin/macha`,
`lib/macha/libmacha_core.*` and `lib/macha/plugins/` together, never the
executable alone. Verify `uname -m`, `ldd --version` and the tarball hash on
each target. Never compile on gbni-1. When a release adds a `NodeTelemetry`
field, upgrade every node together rather than rolling. (Until 2026-09-20 this
said "build nodes in parallel", contradicting the practice recorded in every
deploy note above it; it was also a checkbox that could never be ticked.)
