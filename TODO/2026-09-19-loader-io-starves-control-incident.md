# Loader disk I/O starves control and viewer traffic

Status: found 2026-09-19 on es-1 (corvus-es-1, 0.46.2), live, during an
ordinary ingest. Not a regression — no mechanism to prevent it has ever
existed. In progress now.

## What was reported

Calls from the local client to
`https://ramaroja.macha.network/api/v1/health?_=...` time out
intermittently, while the node is otherwise alive and answering cluster ping.

## What is actually happening

A single ingest (`/TV/Dark Matter/Season 01/…S01E04…mkv`) saturates the node's
disk. Everything else follows from that.

| measurement | value |
|---|---|
| load average (4 cores) | 8.53 – 11.18 |
| CPU | 0.0% idle, **48–91% iowait**, macha 37% |
| macha disk I/O (10 s sample) | **34 MB/s write, 9 MB/s read** |
| qbittorrent-nox, same box | 3 MB/s read, 0 write |
| single 4 MiB extent write, local | 1,457 ms / 9,318 ms / **17,694 ms** |
| swap in/out | ~0 (not memory pressure) |

The node is doing this to itself. The co-tenant processes on es-1 (Plex,
qbittorrent-nox) were measured and are not the cause.

## The client-visible failure

haproxy recorded **12 `CD--` terminations** (client aborted during the data
phase) in six hours, clustered at **7.89–8.01 s**, which is an ~8 s client
timeout firing:

```
9881ms  /api/v1/catalogue/status
8010ms  /api/v1/catalogue/artwork/f6d3c52c…
7994ms  /api/v1/catalogue/artwork/f6d3c52c…
7886ms  /api/v1/catalogue/status
6971ms  /api/v1/playback/stream/…/segment-000000.m4s
5773ms  /api/v1/catalogue/items?type=artist
3719ms  PATCH /api/v1/playback/sessions/…
```

**Attribution corrected 2026-09-20 by measurement — see the plan.** These are
not all the same failure, and the original reading of this list was wrong:

| route | aborts | reads the DATA backend? |
|---|---|---|
| `catalogue/artwork` | 5 | yes -- `catalogue_.artwork(id)` reads the distributed store |
| `catalogue/status` | 3 | **not disk** -- O(artwork) in-memory lookups under `LocalStore::m_`; see below |
| `playback/.../segment` | 1 | yes |
| `catalogue/items` | 2 | **no** -- `list()` copies and sorts the in-memory snapshot |
| `PATCH playback/sessions` | 1 | planning; unclassified |

Six of twelve are disk-bound and belong to this item. **`catalogue/items` is
not**: it is the whole-catalogue deep copy, which is the separate P0 "the
catalogue materialises everything it has". Attributing it here would have sent
the fix to the wrong subsystem.

And **`catalogue/status` doing O(artwork) disk probes on a polled route is its
own defect**, filed separately below.

Requests that are normally sub-millisecond took eight seconds. **Governing law
1 is violated on the DATA backend by the node's own loader work. Law 3 is not
violated**: control touches no disk and held 1.2-1.6 ms p99 through a
reproduction at 86% iowait.

For scale: over the same six hours `/api/v1/health` was 1,165 calls under
10 ms with a 1.66 s worst case, and `/api/v1/status` 629 × 200 with 13 calls
in the 1–5 s band. The failure is a tail, not a floor — but it is a tail the
node manufactures.

## Why nothing stopped it

Law 3 is enforced in three resources and absent from a fourth.

| resource | protected | mechanism |
|---|---|---|
| memory | yes | `control_memory_reserve_bytes` (64 M), beside viewer and loader reserves |
| HTTP threads | yes | separate control lane (`control_workers`) |
| RPC execution | yes | fast-control allow-list (`ping`, `members` only) |
| **disk I/O** | **no** | only `data_viewer_reserve_bytes` (32 M) — a *viewer* reserve, not a control one |

- `maintenance.max_bandwidth`, `busy_bandwidth_fraction` and
  `idle_bandwidth_fraction` bound **maintenance only** — repair, rebalance,
  GC, scrub. Ingest and FUSE publication are bandwidth-unbounded by design.
- `dht.background_concurrency` bounds *concurrent operations*
  (`max(1, nproc/2)`, so 2 here), which is not a throughput or a latency
  bound. Two operations are enough to saturate this disk.
- `data_inflight_bytes` bounds *bytes in flight*, not service time. A node can
  sit inside every byte budget and still have a disk queue seconds deep.
- **Nothing observes I/O latency.** There is no feedback from measured service
  time into loader admission anywhere in the tree.

Law 2 is work-conserving on purpose: with no viewer present the loader takes
everything. "Everything" silently includes the headroom law 3 needs. The laws
are mediated against each other in memory and in threads; on disk they compete
unmediated, and the loader wins because nothing tells it not to.

es-1 sets none of the relevant knobs, but **no available setting would have
prevented this.** That is what makes it structural rather than a tuning error.

## Not to be confused with

- **A 403 on the status API.** `anonymous` has `roles=` (empty, generation 2),
  so any unauthenticated or anonymous-session call to `/api/v1/status` is
  refused in ~0.4 ms with `this action requires the 'view_status' role`. Real
  and by design, but a separate thing from the timeouts and not their cause.
- **A server-side 4xx.** A `CD--` line in haproxy carries a substituted `400`,
  so `400 GET /api/v1/catalogue/status` in that log is a client timeout, not a
  rejection. Read the termination-state field first.
- **An upstream proxy.** Nothing fronts haproxy; it binds `:443` with its own
  certificate.

## Adjacent, cause not yet separated

`put_metadata_commit` to fi-1 takes 5.3 s / 19.3 s / 28.4 s and times out at
30–35 s, repeatedly (24 no-progress cancels in six hours, first seen
2026-09-18 23:33). Metadata went read-only at 02:50:12
(`replicas=1/3 required=2`) and recovered at 02:53:01.

Each commit ships the whole serialised namespace (P-1), and es-1 must read and
encode it off the same saturated disk. **Whether fi-1 is slow, or es-1 is too
busy to send, is not yet established** and should be separated before this is
treated as a WAN problem. The no-progress cancels themselves are discipline 2
working correctly: the deadline fires rather than waiting forever.

## A separate defect found in the same list

**Corrected 2026-09-20, having first been recorded here as a disk problem,
which it is not.** `CatalogueManager::status()` walks every artwork id in the
catalogue on every call:

```cpp
auto art = cached ? artwork_ids(*cached) : std::set<ObjectId>{};
status.artwork_objects = art.size();
for (const auto& id : art)
    status.local_artwork_objects += node_.local_store().has(id) ? 1 : 0;
```

The first reading of this was "~418 disk probes per poll". That is wrong.
`LocalStore::has()` (`src/local_store.cpp`) checks the in-memory `packed_` and
`present_loose_` sets first and returns from them; it only reaches
`std::filesystem::file_size()` for an object in neither, which for a node that
holds its artwork is the uncommon case. **For present objects this loop does no
disk I/O at all.**

What it does do, per call, on a route a dashboard polls every 10 s:

- builds a `std::set<ObjectId>` over every artwork reference in the catalogue —
  an allocation and a tree insert each;
- takes a per-object mutex **and** the shared `LocalStore::m_` for every one of
  them.

So the cost is CPU, allocation and **lock contention on the same mutex the
ingest is holding continuously** — which is a far better explanation for this
route recording the three longest aborts (9,881 ms, 7,890 ms, 7,886 ms) than
disk ever was, and it points the fix somewhere completely different.

It is O(library) either way: tens of thousands of lock round-trips per poll at
100,000 titles.

The near-miss is still worth recording. The comment immediately above that loop
says these "potentially large artwork walks and backend existence probes must
not hold the snapshot publication mutex" -- so the walk was seen, the
*publication* mutex was protected, and the walk's own contention on
`LocalStore::m_` was not.

Wanted: a cached count maintained on artwork publication/GC, or the walk moved
to the diagnostics endpoint where an O(N) cost is expected. Measure before
choosing -- a run with this route as the harness's data probe is in flight.

## Diagnostic gap found on the way

The server writes **no journal line for a 401, 403 or 400 refusal**. An
authorisation failure is invisible on-box; haproxy's access log is the only
record. A grep of the journal for `forbidden|view_status|403` returns only
coincidental hex inside object ids.
