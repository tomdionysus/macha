# Self-healing disciplines — UAT record

Companion to `2026-09-06-self-healing-disciplines-plan.md`. One section per
discipline: the failure as it happened on the cluster before, the fix, and
the same scenario replayed live after, with the journal lines that prove it.
Load is `rsync` from `/mnt/diskA` (read-only) into each node's FUSE mount.

## Discipline 1 — re-derive, don't assert (0.29.0)

### Before (2026-09-06, 0.28.3)

gbni-1 publishing *Pulp Fiction* (inode 7666, 13.9 GB spool) had placed 13
extents on es-1. es-1 was restarted at 18:13. From then on, every barrier:

```
object durability quorum unavailable id=… required=1 durable=0
  replica=333c0e3f20fa epoch=47b4f2e8 outcome="remote-refused: storage durability epoch changed"
FUSE async data publication retry inode=7666 error=object durability quorum unavailable before publication
```

~35 attempts/s once the node was otherwise idle; es-1 logged the matching
`barrier refused: epoch changed expected=47b4f2e8 current=09007637` 1,677
times in ten minutes. The bytes were on es-1's disk throughout. Every
restart of any node did this to every in-flight publication elsewhere.

### After (0.29.0)

Scenario: on gbni-1, `rsync --inplace` of
`/mnt/diskA/Movies/12.Monkeys.1995.1080p.BluRay.x264.AAC5.1.mp4`
(2,566,569,827 bytes) into `/mnt/machamedia/UAT/`; once its extents were
landing on the peers, `systemctl restart macha.service` on es-1.

Expected: on gbni-1 `object durability re-derived after incarnation change
reasserted=N absent=0 peers=1`; on es-1 `object durability re-derived after
epoch change present=N/N`; no `quorum unavailable` that is not immediately
re-derived; the file reaches its full size on gbni-2.

Observed, run 1 (19:26–19:44, first 0.29.0 cut): the es-1 restart landed
on a barrier in flight — `outcome="remote-transport: send: Broken pipe"` —
and that cut treated a transport failure as a loss and replayed the UAT
file from its spool. Fixed the same hour (transient vs definitive, see
CHANGELOG). Then, unplanned and on the real workload: *Pulp Fiction* (inode
7666, 13.9 GB, the file that was wedged all of yesterday) reached a barrier
whose batch still named es-1's pre-restart epoch, and gbni-1 logged

```
19:43:13 INFO object durability re-derived after incarnation change reasserted=260 absent=0 peers=1
```

with es-1 answering `object durability re-derived after epoch change
present=260/260`: 260 extents (~1 GB already on es-1's disk) re-stamped in
one probe, none re-sent, publication continued. Under 0.28.3 that batch
retried `epoch changed` forever.

Known remaining cost, seen while deploying the final cut: restarting the
*writer* itself (gbni-1 at 19:48, Pulp Fiction at 99%) recovers the
publication from the spool and re-puts from its last durable checkpoint —
it resumed at ~4.1 GB of 13.9 GB, ~121 extent quorums in the first three
minutes, all for objects the peers already hold. A peer restart now costs
nothing; a writer restart costs a re-send. The fix is the present-content
skip already planned (`2026-09-06-skip-redundant-replica-writes-for-present-content-plan.md`):
check `have_objects` before `put_deferred` on recovery replay.

Observed, run 2 (19:49–19:59, final 0.29.0 on all nodes):
`2001.A.Space.Odyssey…mp4` (2,558,024,294 bytes) rsync'd on gbni-1;
es-1 restarted at 19:49:56, 19:51:03 and 19:52:12 (ready in 6, 8 and 6 s).

gbni-1:
```
19:51:56 INFO object durability re-derived after incarnation change reasserted=60 absent=0 peers=1
19:53:11 INFO object durability re-derived after incarnation change reasserted=4 absent=0 peers=1
```
es-1: `object durability re-derived after epoch change present=4/4
expected=714c3b85 current=1ecb27ac`, its refusal log showing the batch had
accumulated tokens from four dead epochs (`714c3b85`, `bc7f524c`,
`a193421c`, `93d93d9b`) across the three restarts — all re-stamped.

Totals for the run: `re-derived=3 quorum-unavail=2 retries=1 replays=0
re-put=0 ERR=0`; 515 extent quorums; the file at its full size on gbni-2
at 19:59:50 (≈4 MB/s while gbni-1 was also re-sending Pulp Fiction).
One imperfection worth a look later: one `quorum unavailable … transient=yes
outcome="remote-reasserted"` line, i.e. a requirement re-stamped in the same
call still counted as not durable once and cost one extra retry.

**Verdict: pass.** Restarting a peer mid-publication no longer strands the
publication, and it does not re-send a byte.

### Tests

- `storage_v18/test_durability_barrier_rederives_placement_after_peer_restart`
- `storage_v18/test_durability_barrier_reports_objects_a_restarted_peer_lost`
- `filesystem_fuse/test_fuse_durable_journal_recovers_ordered_mutations` (0/40
  after the ENOENT re-derivation; was 1/4 failing)
- `filesystem_fuse/test_fuse_terminal_recovery_failure_is_not_readmitted` (0/10)

## Discipline 2 — one work-item policy (0.30.0)

Three habits in one discipline: a retry with no budget, a wait with no
progress condition, and a startup gate on elapsed time. Each has its own
before/after below.

### 2a. Publication retry — before (2026-09-06 20:59–21:02, 0.29.0 on gbni-1)

Scenario (`/root/uat/d2-before.sh` on gbni-1): `rsync --inplace` of
`Blade.Runner.2049…mp4` (3.3 GB) into `/mnt/machamedia/UAT/d2-before/`;
45 s in, an `nft` rule rejects every outbound connection from gbni-1 to
both peers' RPC port for 90 s, then is removed. The cluster is otherwise
untouched (no peer is restarted).

Observed: for the first 64 s the in-flight barrier waited on the peers
(nothing logged). Once membership marked them dead the publication failed
with `quorum unavailable … outcome="remote-not-sent"` and the writer
retried it on the fixed 100 ms sleep — **a flat 10 attempts/s, 254 retries
of the same inode in the remaining 26 s**, 550 log lines in the window,
peak 21 lines/s. There is no backoff, no budget, no operator signal: had
the rule stayed, so would the loop, forever, at 10/s, holding one of the
publication slots and burying the log. (This is the same loop that ran at
~35/s for a day on *Pulp Fiction* under 0.28.3.) When the rule was removed
the next attempt succeeded and the file published normally.

```
21:01:31 DEBUG object durability quorum unavailable id=63716277… required=1 durable=0 transient=yes replica=333c0e3f20fa outcome="remote-not-sent"
21:01:31 DEBUG FUSE async data publication retry inode=16565 error=object durability quorum unavailable before publication
… ×254, 10/s, until 21:01:57
```

### 2a. Publication retry — after (2026-09-06 21:04–, 0.30.0 on all nodes)

Same scenario (`/root/uat/d2-after.sh`), same file, same 90 s `nft reject`
isolation of gbni-1 from both peers, with gbni-1 configured for a small
budget so the park is reachable inside the window
(`publication_retry_max_failures: 8`, backoff 250 ms → 5 s; the shipped
default is 100 failures in 30 min, 250 ms → 30 s). Two publications were in
flight when the peers went dead: the after-run's copy (inode 18307) and the
before-run's copy, which gbni-1's 0.30.0 restart had re-published from its
spool (inode 16565).

Observed: **16 retries in the whole window instead of 254**, each one
logged with its backoff and attempt count, both inodes backing off
independently, and both parked with one `WARN` each 22.8 s after their
first failure:

```
21:06:27 DEBUG FUSE async data publication retry inode=16565 error=object durability quorum unavailable before publication retry_in_ms=250 attempts=1
21:06:27 DEBUG … inode=16565 … retry_in_ms=500 attempts=2
21:06:28 DEBUG … inode=18307 … retry_in_ms=250 attempts=1
21:06:28 DEBUG … inode=16565 … retry_in_ms=1000 attempts=3
21:06:29 DEBUG … inode=16565 … retry_in_ms=2000 attempts=4
21:06:31 DEBUG … inode=16565 … retry_in_ms=4000 attempts=5
21:06:35 DEBUG … inode=16565 … retry_in_ms=5000 attempts=6
21:06:40 DEBUG … inode=16565 … retry_in_ms=5000 attempts=7
21:06:45 DEBUG … inode=16565 … retry_in_ms=5000 attempts=8
21:06:50 WARN FUSE data publication parked inode=16565 path=/UAT/d2-before/Blade.Runner.2049…mp4 attempts=9 failing_for_ms=22758 error=object durability quorum unavailable before publication
21:06:50 WARN FUSE data publication parked inode=18307 path=/UAT/d2-after/Blade.Runner.2049…mp4 attempts=9 failing_for_ms=22757 error=…
```

89 log lines in the window (550 before). After the rule was removed the
parked inodes stayed parked — that is the point: the fault may have
cleared, but nine failures in 23 s is the operator's call, not a reason to
spin — and the API showed them:

```
GET /api/v1/manage/filesystem/parked-publications
{"parked":[{"inode":16565,"path":"/UAT/d2-before/Blade.Runner.2049…mp4","error_code":5,
  "error_message":"object durability quorum unavailable before publication","attempts":9,
  "failing_for_ms":29606,"parked_for_ms":6848,"pending_bytes":3348105554},
 {"inode":18307,"path":"/UAT/d2-after/Blade.Runner.2049…mp4", … "attempts":9, …}]}
diagnostics.filesystem: parked_publications=2 publication_retries_backed_off=16
```

Then, with both inodes parked, a second rsync (`Ghostbusters.1984…mkv`,
3,697,566,500 bytes) into the same directory at 21:06:57 → 21:10:52; it
published normally while the parked pair sat in the spool — full size on
gbni-2 at 21:16, `data_publications_completed` 2 → 4 — while gbni-2's copy
of the parked `d2-after/Blade.Runner…mp4` stayed at **0 bytes**. One
`POST …/parked-publications/16565/retry` (`204`, log `FUSE data publication
retry requested by operator inode=16565`) at 21:14:33 released the first;
by 21:34 `parked_publications=1`, `spool_bytes` had fallen to exactly the
second inode's 3.5 GB. `POST …/18307/retry` at 21:35 released the second
(a second POST for the same inode answers `409 not_parked`). By 21:50:21:
`parked_publications=0 data_publications_completed=5 spool_bytes=183039231`
(the pre-run baseline), and all three files at full size on gbni-2 —
`d2-before/Blade.Runner…` 3,348,105,554 at 21:22, `d2-after/Blade.Runner…`
3,348,105,554 at 21:42, `Ghostbusters…` 3,697,566,500 at 21:16. Zero
`ERROR` lines for the whole run. gbni-1's UAT-only budget was then removed
and it restarted onto the shipped defaults (ready in 2 s, 21:50:44).

**Verdict: pass.** A publication that keeps failing now costs a handful of
logged, spaced attempts and then a visible parked item that the rest of the
cluster publishes around, instead of an unbounded 10–35/s loop; the
operator can release or abandon it through the API.

### 2b. RPC no-progress deadline

Before (0.29.0): the plan doc's 150–230 s `accept_metadata_commit` stalls
of 2026-09-06 afternoon, and during the before-run above es-1 logged
`RPC stalled (control) peer=377ce5b1bd86 message=members … no_progress_ms=30000;
request remains active while peer health is monitored` — the call had no
deadline of its own; only `dead_after` ending the session freed it.

After (0.30.0), during the after-run's isolation, gbni-2:

```
21:05:03 DEBUG RPC stalled (control) peer=377ce5b1bd86 message=members age_ms=5000 no_progress_ms=5000; deadline_ms=30000
… every 5 s …
21:05:28 DEBUG RPC made no progress for 30000 ms (control) peer=377ce5b1bd86 message=members age_ms=30000; cancelled for retry
21:05:28 DEBUG bootstrap: RPC made no progress for 30000 ms (control) … cancelled for retry
21:05:32 DEBUG peer 10.44.1.50:7437 liveness failure: health could not be established before dead_after
```

and es-1 the same ladder to `deadline_ms=30000` (there `dead_after` won the
race by a few hundred ms and closed the session first). The stall notice
now names the deadline it is counting down to, the call fails with a
distinct transient error, and the caller (bootstrap) retries under its own
policy. The case the deadline exists for — a peer that answers heartbeats
but never answers the call, which `dead_after` can never catch — cannot be
injected on the live cluster without a code hook; it is what
`rpc_cluster/test_rpc_call_fails_after_no_progress_deadline` exercises
(handler parks on a gate, call fails inside 300 ms with the same message).

### 2c. Startup gate

Before (0.28.2, morning of 2026-09-06): gbni-1 crash-looped eight times on
the 120 s elapsed gate while replaying a five-minute journal; unwedged by
hand with `service_startup_timeout_ms: 1800000` in its config.

After (0.30.0): that override is removed from gbni-1's config; the gate is
`service_startup_no_progress_ms: 120000` and elapsed time is not a
criterion (`service_startup_timeout_ms` defaults to 0). gbni-1 booted
0.30.0 in 3 s (`21:03:50 Started` → `21:03:53 local services ready`, after
replaying its 132 MB journal, which 0.28.3 had already made linear), so the
gate is not stressed on today's cluster; the slow-but-progressing case is
proven by `rpc_cluster/test_service_startup_gate_waits_while_recovery_progresses`
(recovery held at the `data-storage` stage for 3 s under a 1.2 s gate with
the progress counter ticking — no kill; ticking stops — killed inside 2 s).

### Tests

- `filesystem_fuse/test_fuse_publication_backs_off_then_parks_for_operator`
- `rpc_cluster/test_service_startup_gate_waits_while_recovery_progresses`
- `rpc_cluster/test_rpc_call_fails_after_no_progress_deadline`
- suite 345/345

## Discipline 3 — recover by resolving (0.31.0)

### Before (2026-09-06, 0.30.0 — the same on every boot since Sep 4/5)

Every start replays the same durable state, so anything recovery refuses
it refuses on every boot. Both writer nodes had been carrying this for
days:

gbni-1, boot 21:50:42 (0.30.0) — 23 `WARN` lines in the first 20 s:
```
21:50:45 WARN FUSE journal recovery accepted data completion without published prefix inode=904 sequence=1496 frame_offset=21387307
… ×20 (inodes 904…1901, 11340, 11351 — the same twenty frames, every boot)
21:50:47 WARN recovered durable FUSE operations pending=702 namespace=0 inodes=2
21:50:47 WARN FUSE async data publication failed inode=922 error=missing
21:50:47 WARN FUSE async data publication failed inode=5806 error=missing
```
`/etc/macha/fuse-operations.log` 131,912,072 bytes (re-parsed on every
boot); `/mnt/diskB/spool/inode-922.spool` 183,038,542 bytes dated Sep 5
11:43 and `inode-5806.spool` 689 bytes — the whole of gbni-1's steady-state
`spool_bytes=183039231`.

es-1, boot 22:03:21 CEST (0.30.0):
```
22:03:25 WARN FUSE journal recovery accepted data completion without published prefix inode=6435 sequence=65909 frame_offset=178282110
22:03:26 WARN recovered durable FUSE operations pending=22997 namespace=0 inodes=1
22:03:26 WARN FUSE async data publication failed inode=2333 error=missing
```
journal 218,767,658 bytes; `/mnt/diskB/spool/inode-2333.spool`
**6,028,175,014 bytes dated Sep 4 17:55** — 22,997 pending write
operations for a file that had left the namespace two days earlier.

Mechanism: the file was written through FUSE and then removed (or renamed
over) cluster-side before its data published. On each boot
`resume_recovered_data` republishes it, `open_write` answers `ENOENT`,
`publication_path_may_still_appear()` correctly says no, and the loop's
terminal branch poisoned the inode "until an operator acts" — with no
operator action defined. The pending operations kept
`durable_pending_operations > 0`, so the journal could never reset and
grew without bound, and its twenty benign done-without-published frames
were re-warned each start.

### After (0.31.0, first boot 2026-09-06 22:52 gbni-1 / 23:53 CEST es-1)

Nothing was done on either node except installing 0.31.0 and restarting.

gbni-1, first boot — 5 `WARN` lines (was 23):
```
22:52:27 Started … 22:52:29 local services ready
22:52:31 WARN recovered durable FUSE operations pending=702 namespace=0 inodes=2 skipped_frames=0 done_without_published=20
22:52:31 WARN dropped FUSE spool generation inode=5806 sequence=2 reason=file is no longer in the namespace
22:52:31 WARN FUSE data publication abandoned inode=5806 last_path=/TV/Altered.Carbon.S02…/[TGx]Downloaded from torrentgalaxy.to .txt unpublished_bytes=689 reason=file is no longer in the namespace
22:52:31 WARN dropped FUSE spool generation inode=922 sequence=1400 reason=file is no longer in the namespace
22:52:31 WARN FUSE data publication abandoned inode=922 last_path=/TV/Allo Allo 1984 Season 4 to 6 …/Allo Allo 1984 Season 5/32 - Allo Allo S5e05 - Enter Denise.mkv unpublished_bytes=183038542 …
```
Immediately after: `/etc/macha/fuse-operations.log` **8 bytes** (was
131,912,072), `inode-922.spool` and `inode-5806.spool` gone (the two
`*.orphan.*` files from Sep 1–2 are untouched, as designed).

es-1, first boot — 3 `WARN` lines (was 3, but different ones):
```
23:53:07 Started … 23:53:11 local services ready
23:53:15 WARN recovered durable FUSE operations pending=22997 namespace=0 inodes=1 skipped_frames=0 done_without_published=1
23:53:15 WARN dropped FUSE spool generation inode=2333 sequence=22997 reason=file is no longer in the namespace
23:53:15 WARN FUSE data publication abandoned inode=2333 last_path=/TV/Ted Lasso/Ted.Lasso.S03.COMPLETE…/Ted.Lasso.S03E12.So.Long.Farewell.1080p.ATVP.WEB-DL… unpublished_bytes=6028175014 …
```
journal **8 bytes** (was 218,767,658); `/mnt/diskB/spool` empty (was
6.0 GB); 22,997 operations retired in one journaled abandonment.

**Second boot of each node: 0 `WARN`/`ERROR` lines**, ready in 2 s
(gbni-1 22:53:53→55; es-1 23:54:13→15), journal still 8 bytes.

What was abandoned, checked against the live namespace from gbni-2's
mount: `/TV/Ted Lasso/` does not exist at all, and `…/Allo Allo 1984
Season 5/` has no `S5e05` file — both trees were removed by the operator
after the writes were accepted and before they published. The abandoned
bytes were writes to files that no longer exist; nothing visible changed.
The `last_path` in the WARN is exactly what an operator needs to confirm
that.

**Verdict: pass.** A condition recovery could not resolve was re-raised on
every boot for two to three days, holding 6 GB of spool and a 218 MB
journal on es-1; 0.31.0 resolved it once, journaled the resolution, and
the next boot was silent.

### Tests

- `filesystem_fuse/test_fuse_journal_fuzz_every_frame_mutation_still_starts`
  — 9 frames × {truncate after, drop, duplicate, corrupt} = 36 restarts,
  the frontend starts every time; mid-journal corruption quarantines the
  tail and counts the bytes.
- `filesystem_fuse/test_fuse_recovery_abandons_publication_for_file_removed_from_namespace`
  — the 922/2333 case: first boot abandons, journal resets, second boot
  reports nothing.
- `filesystem_fuse/test_fuse_durable_journal_skips_unbacked_data_done`
  (was `…_rejects_…`: a marker with no operation behind it is skipped).
- `filesystem_fuse/test_fuse_journal_frame_scanner_exhaustive_tail_model`
  (mid-journal corruption is reported, not thrown).
- `storage_metadata/test_metadata_journal_mid_frame_corruption_truncates_not_reseeds`
  — one flipped byte in the first of two journal frames: journal truncated
  and tail quarantined, checkpoint/history/heads untouched, replica usable.
- suite 348/348.

## Discipline 4 — compact history out of the hot path (0.32.0)

### Measuring first (2026-09-06 23:30, gbni-2, `macha-metadata-dump --stats`)

The plan was written on the morning's premise — 270k tombstones making
15 MB snapshots. The tool built for this discipline says what the
production head (gen 9903) actually was:

```
snapshot: encoded_bytes=2319777 entries=1744 (files=269 directories=1475) extents=36083 tombstones=439 conflicts=116
bytes:    entries=1958969 (of which extents=1768067, paths+attrs=190902) tombstones=24584 conflicts=335749 other=475
conflicts: namespace_entry=49 catalogue_root=67 identical_alternatives=0 distinct_head_pairs=112
per_entry_bytes=1330 per_entry_bytes_excluding_extents=316
```

So: 76% extent tables (the data map — 36,083 × 49 B, not compressible
without a manifest redesign), **14% standing conflicts nobody knew about**,
1% tombstones (GC consumes them after `garbage_grace`). The plan's per-node
retirement log is not justified by the data and was not built; the
acceptance "≤ 1 KB × live entries" is met excluding extents (316 B) and is
not meaningful including them.

What *was* costing: every merge delta carried the whole conflict set
(`history_body=delta history_bytes=335961`, twice in three hours), and 4 of
es-1's last 5 reconciliations were **full frames of 5.4–7.7 MB** each
(`history_body=full history_bytes=7749233`), replicated to every node and
across the WAN — because the merge canonicalises tombstones by ObjectId,
the primary parent's vector was in append order, and DLT5/6 could not
express a reorder, so `metadata_delta()` gave up.

### After (0.32.0, deployed 2026-09-06 23:55–23:57 to all three nodes)

The first ordinary mutation on gbni-1 (one `.srt` rsync'd at 23:58):

```
diagnostics.metadata: conflicts 116 → 10, conflicts_superseded=106, tombstones=439
metadata mutate mode=delta delta_bytes=162 snapshot_bytes=2261657
```

106 of the 116 were decided long ago — 49 media paths one writer had since
republished, 67 catalogue roots the scanner had moved past — and left the
snapshot in one commit. The 10 that remain are genuine: e.g.
`/Music/Avicii/Stories/08 City Lights.mp3` left = 15,601,728 B v5, right =
12,845,056 B v4 (two writers, two different partial generations). The API
lists them with both alternatives; resolving one:

```
POST /api/v1/manage/metadata/conflicts/4fd3a5e9…/resolve?choice=left  → 204
POST … same id again                                                   → 409 not_standing
diagnostics.metadata: conflicts=9 conflicts_resolved=1
/mnt/machamedia/Music/Avicii/Stories/08 City Lights.mp3  15601728 bytes
```

Snapshot on gbni-2 after that: `encoded_bytes=2261698 conflicts=10
conflicts_bytes=277468` (the ten survivors are the big ones — alternatives
with extent tables). A same-content rule (same bytes at the same path,
differing only in version/mtime, as two rsync writers of duplicate media
produce) was added the same night so those never become conflicts again.

_(merge-delta sizes under DLT7 and the reconciliation body type are
measured in the closing run below, which produces the merges.)_

## The demonstrative run (2026-09-07 00:08–00:22, 0.32.0 on all nodes)

Script: `final-run.sh` (session scratch; the shape is below). Two writers
at once, each rsync'ing from its own read-only `/mnt/diskA`:

- gbni-1 → `/mnt/machamedia/UAT/final/gbni-1/`: *Jurassic Park* 2,115,421,676 B
  + *Prometheus* 2,136,029,979 B (local disk, ~8 MB/s publish);
- es-1 → `…/final/es-1/`: *Idiocracy* 1,051,957,331 B + *Dog Soldiers*
  1,373,380,284 B (over the ~50 Mbps WAN);

and five `systemctl restart macha.service` while both were publishing:
gbni-2 at +2:27, es-1 at +4:13, **gbni-1 (a writer) at +5:59**, gbni-2
at +8:13, es-1 at +10:29. Every node was back in 1–2 s
(`Started` → `local services ready`: 00:10:55→56, 01:12:41→43,
00:14:27→29, 00:16:41→42, 01:18:57→59). Both spools drained by 00:21:55,
13.5 minutes after the first byte.

Result, from each node's journal over the whole window:

| | gbni-1 (writer) | gbni-2 (replica) | es-1 (writer, restarted twice) |
|---|---|---|---|
| ERROR / WARN | 0 / 1 | 0 / 0 | 0 / 0 |
| durability re-derived after peer restart | `reasserted=355 absent=0` ×2 | — | `present=355/355` ×2 |
| quorum unavailable / replays / parked / abandoned | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| publication retries (backed off) | 4 | 0 | 0 |
| RPC no-progress cancellations / skipped journal frames | 0 / 0 | 0 / 0 | 0 / 0 |
| metadata mutations | 11, all `mode=delta` | — | 9, all `mode=delta` |
| reconciliation | `history_body=delta history_bytes=277 conflicts=0 standing=9` | same | same |

The one WARN is gbni-1's own restart mid-publication: `recovered durable
FUSE operations pending=16221 namespace=0 inodes=2` — it resumed both
files from its spool and finished them (a writer restart still re-sends
from its last durable checkpoint; the present-content skip plan is the
follow-up, unchanged since discipline 1). All four files are at their
exact source size on gbni-2. Snapshot after the run: 2,339,893 B for 1,753
entries / 37,681 extents (the four new files' extent tables are the
growth), 9 standing conflicts (the operator's), 439 tombstones; history
`frames=108 full=6 delta=102 anomalies=0`.

Against the plan's acceptance list:

- **no wedges** — five restarts, two of them of a writer, zero
  `quorum unavailable`, zero parked, zero abandoned, every node ready in
  ≤ 2 s; before the programme one peer restart stranded every in-flight
  publication forever;
- **no re-sent extents on a peer restart** — 355 extents on es-1 re-stamped
  twice, `absent=0`, none re-put;
- **bounded retries visible in Status** — 4 backed-off retries on gbni-1,
  each logged with its delay, `parked_publications=0`;
- **sub-10 s restarts** — 1–2 s;
- **compact history** — the reconciliation was a **277-byte delta**
  (335,961 bytes, or a 5–8 MB full frame, on 2026-09-06), every mutation a
  delta, and the snapshot's size is its entries.

**Verdict: the programme's four disciplines hold together under the load
and the restarts that produced six P0s in one afternoon on 2026-09-06.**

## The full-library import (started 2026-09-07 02:08, iterating)

The operator's test: both nodes rsync their entire `/mnt/diskA` (gbni-1
6.1 TB / 15,500 files; es-1 2.4 TB / 6,350 files) into the real namespace
roots, concurrently — "the first thing a new user does". Judged on: secure,
replicated, quick to access, stable, good neighbour; and on throughput
against what the hardware could do. Sources are read-only throughout.

### Iteration 1 — died at 30 % of Music on gbni-1 (02:11:45)

`rsync: write failed on ".../03 Eddie, Are You Kidding_.m4a": Resource
temporarily unavailable (11)`, then Movies and TV each died on their first
write. es-1's import was unaffected.

Cause (from `diagnostics.retained_memory` on gbni-1): publication held
**524,288,000 B of the 768 MB process budget** (`owners.publication`),
21,664 small-file data operations were pending, loader-class `waits=30`,
`cancelled_waits=6`. Each FUSE write reserves loader-class memory before it
copies the payload; that reservation waited at the 5 s request deadline and
`reserve_process_memory` turned the timeout into `EAGAIN`. A blocking
`write(2)` must never see EAGAIN; rsync treats it as fatal.

Fix (0.32.1): the three admission waits block until admitted (interruptible
by shutdown → `EINTR`), the request deadline starts after admission, waits
over 5 s are logged. Tests `pending_write_payloads_are_byte_bounded`,
`operation_metadata_backpressures_at_heap_bound`,
`operation_metadata_retirement_wakes_blocked_writer` already specified the
blocking contract and pass.

Also seen in the same window, filed for the next iteration: metadata
mutations of 15–21 s on gbni-1 (`metadata mutate total_ms=21735`) while
es-1's own import saturated the WAN — the write floor W=2 waiting on a
replica behind a congested link; and `slow-fuse op=truncate` bursts from
rsync's create/truncate/rename pattern on thousands of small files.

### Iteration 2 — namespace commits one op at a time; publication starved (02:30–03:30)

After 0.32.1 gbni-1 showed `data_publications_started=114 completed=0`
for minutes with all eight publication threads parked and the full 256 MB
in-flight budget held. gdb: every thread in `replay_data_quantum` waiting on
`namespace_cv` — for the namespace op that names its file. Status:
`namespace_publication_batches=1677` for `namespace_operations_batched=1723`
— **one op per metadata commit**, ~3/s cluster-wide, 1,690 Music ops queued.
`namespace_batch_compatible()` only batched runs of one kind and never
renames; rsync's create-temp / utimens / rename per file never batches.
Reconciliation churn followed (8 merges in 2 min: every one of those
commits raced es-1's).

Fix (0.32.2): identity batches — see CHANGELOG. 73 rsync-pattern ops
recover into one commit (test). Immediate mitigation: gbni-1's import
reordered to Movies → TV → Music so watchable large media is not queued
behind thousands of small-file ops.

Second finding in the same window, from a read-only dry run over the
already-imported Music (`rsync -ani /mnt/diskA/Music/ /mnt/machamedia/Music/`):
**474 files `>f..t......`** — same size, wrong mtime — the next pass would
re-copy them. Cause: the asynchronous data publication committed the
write's timestamp over the utimens rsync had set after the writes
(`commit_file`'s "explicit mtime" heuristic only sees a utimens applied
while the handle is open; the FUSE writer opens after). Fixed in 0.32.2
(explicit committed mtime from the inode; recovery order). The 83
`.d..t` directories are dirs whose rsync run was killed before it set their
times — expected, cheap on the next pass.

Also measured while here (for the next iteration, not fixed yet):
- metadata mutations of 195 s (es-1) and 284 s (gbni-1) — silent windows
  with `retention claim peer=10.34.1.50 error=control RPC deadline exceeded`
  and control-lane `peer closed`/reconnect churn: the control lane shares
  the saturated WAN with the bulk data lane and gets no priority;
- `publish_commit` tries replicas in NodeId order, not proximity — gbni-1
  may try es-1 (WAN) before gbni-2 (LAN) for every commit;
- viewer path via the playback API on gbni-2: session 6–9 s, transcode
  segments at ~0.7 MB/s (CPU-bound), and a mid-file segment request that
  did not return in 90 s for either film (seek); the UI session is
  measuring the real player;
- es-1 logs 25 `media information prune deferred: catalogue unavailable`
  WARNs in one boot minute — one line would do.

### Iteration 3 — a restart was a write outage; status was unreachable (04:17–04:49 CEST, es-1)

After the 0.32.2 restart es-1's `/api/v1/status` stopped answering
(`http=000` after 40 s), its import made no progress and gbni-1's
publications queued on the metadata mutation mutex. Stacks on es-1: all 16
HTTP workers in `LocalStore::has` (via `CatalogueManager::status`), twelve
`put_deferred` threads and the pack compactor in
`LocalStore::wait_for_accounting`, one thread in `LocalStore::scan` walking
`/mnt/diskB/objects` with `getdents64`, and a metadata mutation's retention
step in `LocalStore::get → valid → retain_on` re-reading extents. The
constructor forced a full walk of the object tree whenever packs existed,
and every put waited for it: gbni-1's walk took 4 min 21 s (320 GB); es-1's
had been running 32 minutes (826 GB, disk saturated by the import) and was
still going. Every metadata commit's retention claim also did the full
`valid()` re-read of each extent.

Fix (0.32.3): a clean checkpoint is trusted with packs (dirty is persisted
before the first mutation, so a torn pack tail can only sit behind a dirty
checkpoint); a dirty checkpoint's figure is an estimate writes are admitted
against while the walk reconciles; a local retention claim is `has()`.

After, es-1 restart 04:49:00: `storage accounting restored path=/mnt/diskB
used=828665503434`, `storage backend online /mnt/diskB elapsed_ms=2627
accounting=ready`, `/api/v1/status` **200 in 0.7 ms**. gbni-2 the same
(133 GB store, restored instantly).

What is deliberately left from the programme itself (all filed, none blocking):
- writer-restart re-send (present-content skip);
- journal compaction while busy (journal resets only when idle;
  `max_operation_journal_bytes` bounds it);
- extent tables are 79% of the snapshot — a compact contiguous-extent
  encoding could roughly halve it; not a habit, an optimisation;
- the 9 standing conflicts want a human (two different rips at one path).
