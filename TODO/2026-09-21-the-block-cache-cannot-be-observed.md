# The block cache cannot be observed, and may never have served a read

Date: 2026-09-21

Status: **Open. Found while investigating why fi-1, a storage-less edge node,
served a viewer 154 remote reads in 40 minutes.** No code has been changed.

**The finding is not that the cache is broken. It is that nothing in this
system could tell us if it were.** The entire observable surface of
`PersistentBlockCache` is a function of *writes*. Hits, misses and evictions
are invisible — to telemetry, to the status API, to the logs in the common
case, and to the test suite. A cache that stores perfectly and has never
served a single byte back would report **identically** to one working
flawlessly, on every surface this project has.

That is why the honest title is "may never have worked". Not an accusation —
an admission that the question is currently unanswerable, which is worse than
a known defect because no amount of operating the system will surface it.

## The evidence

**1. The class exposes no counters.** The complete public surface of
`PersistentBlockCache` (`src/persistent_cache.hpp`):

```
reconfigure  enabled  put  get  has  remove  blocks  remember_metadata  metadata
```

`blocks()` is the only observability, and it is a resident count.

**2. Telemetry carries occupancy only.** `src/cluster.cpp:1614-1619`:

```cpp
cache_capacity = cfg_.cache.max_blocks * cfg_.extent_size;
cache_used     = cache_->blocks() * cfg_.extent_size;
```

Both are derived from `blocks()`. `cache_used` answers "how much has been
written", never "how much has been read". A node reporting a 90%-full cache is
telling you nothing about whether that cache is doing its job.

**3. No test asserts a block-cache read-back.** The only cache-hit assertions
in the suite are for a *different* cache — `materialization_cache_hits`, the
metadata materialisation cache (`tests/test_storage_metadata.cpp:2232-2234`,
`tests/test_invariants.cpp:822`). Five test files touch the block cache and
none of them pins that a read was served from it.

**4. The one log line that would show a hit is suppressed in exactly the case
that matters.** `log_playback_read` in `DistributedStore::get_shared` fires
only when the read is foreground **and took ≥250 ms**. A cache hit off local
disk is fast, so *a working cache is silent by construction*. The absence of
`source=cache` in a journal is therefore not evidence of anything — which is
precisely the trap this investigation fell into before catching itself.

## What is known to work

Stated so the open question stays narrow, and because it eliminates the
obvious suspects:

- **The read path consults the cache.** `get_shared` tries local store, then
  `block_cache().get(id)`, then remote, in that order.
- **The write path populates it, including from foreground playback reads.**
  `get_remote` calls `enqueue_fetched(id, bytes, should_own(id))` whenever
  `opportunistic_persist` is set, which is true for `foreground` and
  `read_ahead` frames (`src/distributed_store.cpp:1383`, `:1441`;
  `NodeRuntime::enqueue_fetched`, `src/cluster.cpp:2014`). It reaches the
  cache through a queued `LocalCopyJob` rather than a direct
  `block_cache().put()`, which is why a naive grep for the latter misses it.
- **The write path is not being starved.** `enqueue_fetched` drops on
  retained-memory pressure or a full 256 MB queue, and logs both at DEBUG.
  fi-1 logged **zero** `opportunistic persistence skipped` events in 40
  minutes of live playback.
- **The cache fills.** fi-1 holds exactly 2560 of 2560 blocks, 9.0 GB, with
  entries written seconds before inspection.

So: it is consulted, it is filled, it is full, and it is not being starved.
Every observable says healthy. **None of them says it ever returned a byte.**

## Sizing is not the problem, and a claim to the contrary is retracted here

An earlier pass in this session asserted that 10 GiB "is about one film",
reasoning from an assumed 8 Mbps. That assumption was invented and the
operator rejected it. Measured from the live library on fi-1, 1,427 media
files:

| | |
|---|---|
| total | 1,699 GB |
| mean | 1.19 GB |
| median | 0.67 GB |
| p90 | 2.16 GB |
| max | 21.44 GB |

A 10 GiB cache holds roughly **16 titles at the median, 9 at the mean, 5 at
p90**. Exactly one file in 1,427 exceeds the whole cache.

**So the P-1 invariant — a cache must never be smaller than one unit of its
own work — is satisfied here**, comfortably. This is *not* a second instance
of that failure, and recording it as one would have sent the next reader after
the wrong thing. The arithmetic was right; the input was fabricated. Check the
library before sizing a cache against it.

## Why this matters more on an edge node than anywhere else

fi-1 has `hosts_extents: false`. It owns no extents and is never a DATA
fallback holder. **The block cache is the entire reason it can serve media at
all** — every read either hits that cache or crosses the WAN to es-1 or
gbni-1, measured at 772-3431 ms per 4 MiB stripe. On a node that stores media,
a dead cache is a performance regression. On this one it is the difference
between an edge node and a proxy with extra steps.

Observed on fi-1 in one 40-minute window: **154 foreground reads went
`source=remote`**, roughly 616 MB across the WAN for a single viewer. There is
an innocent explanation — a first watch of new content is necessarily all-miss
— and no way at present to distinguish it from a cache that is never
consulted successfully.

## What to do

1. **Counters on `PersistentBlockCache`: hits, misses, evictions, entries.**
   The materialisation cache already has exactly these and they are what made
   the 2026-09-20 rejoin failure diagnosable at all. Mirror them. This is the
   whole fix for the observability gap and everything below depends on it.
2. **The Status API must carry the counts, per node.** See the section below;
   this is half the fix, not a follow-up. `cache_used` beside a hit rate is
   honest; `cache_used` alone is misleading, because it looks like health.
3. **Make a sustained zero-hit, high-eviction cache a reportable condition**,
   per the standing P-1's first bullet. A 100%-full cache with a 0% hit rate
   is a defect state and should say so in `cluster.conditions`.
4. **Pin a block-cache read-back in the test suite.** Write an object, evict
   the local copy, read it, assert it came from the cache. There is currently
   no test that would fail if `get` always returned empty.
5. **Then, and only then**, revisit fi-1's latency — including whether
   hydration is running at all. It is configured `enabled: true` with both the
   `read_ahead` and `current_file` engines and logged **nothing** in 40
   minutes, which is either silence by design or a second dead mechanism. With
   sizing ruled out, prefetch is the remaining lever on WAN latency.

## The counters must reach `GET /api/v1/status`, for every node

Counters that exist only inside the process repeat the mistake in a smaller
room. The numbers have to arrive where an operator already looks, which is the
Status API, and they have to arrive **about every node rather than only the
one answering**.

**Where.** `src/status_api.cpp:299` builds the per-node block today:

```cpp
node["cache"] = bytes_pair(effective.cache_used, effective.cache_capacity);
```

That is the place. It becomes used, capacity, **hits, misses, evictions,
entries** — the same four the materialisation cache already keeps. They ride
`NodeTelemetry` exactly as `cache_capacity` and `cache_used` do now
(`src/cluster.cpp:1614-1619`).

**Why every node, not just the local one.** The same argument that put the
playback budgets on the per-node block in 0.48.0, and it is stronger here.
fi-1 is the node whose cache matters most and it is behind CGNAT — it cannot
be reached directly, and an operator will be looking at es-1. A cache-health
figure that is only ever about the node you happened to ask is no use for the
node you actually need to know about.

**Why a cached payload is the right home for these, when the account session
count was deliberately kept off it.** 0.48.0 set what looks like the opposite
precedent and someone will cite it, so the distinction goes on the record:

- The account session **count** is *instantaneous*. Read stale, it is simply
  wrong, and it moves whenever anyone on the account starts anything from a
  device neither end can see. That is why it appears only where it is computed
  live.
- Cache hits, misses and evictions are **cumulative**. Nobody reads them
  absolutely; they are diffed across two reads. Staleness shifts the window
  slightly, it does not produce a false statement. `entries` is instantaneous
  but bounded and slow-moving, exactly like `cache_used` beside it.

The rule worth carrying: **an instantaneous count must not go on a cached
payload; a monotonic counter is fine there.**

**Why this is cheap now and was not two days ago.** TEL3 is tagged and
length-delimited, and a default-valued field is omitted entirely. Adding four
counters is additive, an older node skips what it does not know by length
rather than misparsing the rest of the set, and absence already means "this
node does not say" to every consumer. Before 0.48.0 the same four fields would
have cost a flag-day cutover.

**Do not aggregate the hit rate cluster-wide.** `src/status_api.cpp:589-665`
rolls cache bytes up across nodes, and the obvious next step is a cluster hit
rate. It would hide the entire finding: two storage nodes serving well would
drown a storage-less edge node at 0%, which is precisely the node the number
exists to expose. Sum bytes if useful; keep hit rates per node.

## A cheap falsification before any code changes

This does not need instrumentation to get a first answer. On fi-1, read one
media file through FUSE twice and compare wall-clock:

- **Second read much faster** — the cache serves reads, and the finding is
  narrowed to "unobservable" rather than "possibly dead".
- **Second read the same speed** — the cache has never been serving, and this
  becomes a P0 with a known blast radius: every edge-node read since the cache
  was introduced.

Worth doing first. It costs one command and it decides how urgent the rest is.
