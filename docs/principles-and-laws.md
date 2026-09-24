# Macha principles and laws

These principles and laws are shared by every Macha project: the server, the
shared client core (`@machafoundation/core`) and the clients built on it. The
same text, with the same numbering, lives in each repository, and every
repository cites the laws and disciplines by these numbers. They define the
conceptual boundary of the product and the priority contract every component
must preserve. They are constraints on design and implementation, not
performance aspirations.

The numbering is canonical across all projects from 2026-09-24. Before that the
server numbered the first three laws differently (viewer 1, loader 2, control
3); its citations have been renumbered in place, and the laws themselves did
not change.

## Conceptual principles

### Macha is a media system

Macha is a distributed media filesystem and media server, not a general-purpose
distributed POSIX filesystem or cloud entertainment platform. Clients exist to
browse and play the owner's media. They do not add accounts, advertising,
recommendations, social activity, other-viewer activity or a global watchlist.

### Authority and presentation remain separate

The server owns catalogue authority, immutable media identity, durability,
placement and media transformation. The client owns presentation, navigation,
controls, platform capabilities and local playback intent.

**The server serves facts; the client negotiates.** The server states what a
title is, what its streams are and what operations it can perform. Choosing
between Direct Play, remux and transcode is the client's, made from its own
measured capabilities — and it is made in `@machafoundation/core` rather than in
any one client, so every client decides the same way from the same facts. The
server obeys the result; it does not pick on the client's behalf.

The catalogue wire model mirrors the server contract. A client must not infer
cluster truth, invent a parallel server model, transcode media, or treat a local
cache as authority.

### Control, data and cache are distinct

Macha separates namespace/catalogue authority, immutable media DATA and optional
non-authoritative CACHE. Every component preserves this distinction in its own
behaviour:

- control and status operations remain responsive independently of bulk media;
- playback DATA is consumed through the source selected by the server;
- cached or persisted state is an optimization or resumable history, never
  proof of server authority or a live playback lease.

### Playback resources have one owner

In a client, `PlaybackRuntime` is the application-scoped owner of the platform
player and active playback coordinator. The coordinator owns the server session
lease and source generations. The UI presents snapshots and binds surfaces;
route and presentation changes do not create, replace or destroy playback
resources.

New user intent supersedes obsolete work. Play, pause and seeks supported by the
active generation are immediate local transport operations. Server preparation
for another generation must not serialize or disable those controls.

### Work is bounded and event-driven

Demand, resource availability, completion, queue transitions and pressure
thresholds wake work through events. Polling, artificial quiet periods and
unbounded hidden work are not substitutes for explicit ownership and bounded
state transitions.

Retries, buffering, recovery, queues and caches must be bounded. Failure and
degraded states must be visible and actionable rather than becoming indefinite
waiting.

### Compatibility is explicit

Platform-specific code is restricted to capabilities, playback, application
lifecycle and input integration. Compatibility fallbacks must be deliberate and
testable. They must not silently weaken playback ownership, viewer priority or
server authority.

Application modules are eagerly bundled. Ordinary catalogue artwork may be
viewport-lazy, while the logo and core UI assets required for startup are
preloaded or embedded.

## The classes of work

- **Control** is health, membership, metadata coordination, status, session
  traffic, cancellation and shutdown, and the bounded control work needed to
  admit viewer operations. It must stay answerable whatever else is happening,
  because it is how the cluster, the client and the operator find out anything
  at all.
- **A viewer** is someone watching or listening right now: playback startup,
  reads, seeks and transport.
- **A loader** is durable work the user asked for — FUSE publication, ingest,
  acquisition — which must finish but need not finish first.
- **Speculative** work is everything nobody is waiting for: read-ahead beyond
  demand, maintenance, repair, diagnostics.

## Scheduling laws

1. **Thou Shalt Not Make Control Wait.** Cluster membership, health, metadata
   coordination, cancellation, shutdown and the bounded control work needed to
   admit viewer operations must never queue behind or execute inline with bulk
   data work. Control has independently reserved admission and execution
   capacity which lower classes never occupy. Shared physical capacity may be
   used work-conservingly only while an independent control submission credit
   and a bounded completion path remain available. Control's reservation is a
   floor, not a share of the viewer's: it is set aside first, and viewer
   priority operates within what remains. A node that serves viewers perfectly
   while failing to answer `ping` has broken this law, and its peers will
   record it as dead.
2. **Thou Shalt Not Make The Viewer Wait. And no viewer may be allowed to make
   another viewer wait.** Playback startup, reads, seeks, and the control work
   required to serve them have overwhelming priority. No ingest throughput
   improvement is acceptable if it introduces viewer-visible delay, buffering,
   starvation, or latency spikes. The second clause is not a footnote:
   protecting viewers as a class against loader and speculative work is only
   half of the law. One viewer must not be able to consume a shared resource to
   the point where another cannot start, so every per-viewer resource needs a
   per-account bound as well as a component-wide one.
3. **Thou Shalt Not Make The Ingester/Loader Wait, Unless It Would Make The
   Viewer Wait.** In the absence of viewer contention, ingest must use the
   available spool, storage, network, CPU, and publication capacity. It may be
   paced for hard capacity, durability, bounded-memory, fairness, or genuine
   downstream throughput limits, but not by an artificial quiet period or the
   mere existence of another open writer. Its second clause makes it
   subordinate to law 2: loader work yields to a viewer rather than negotiating
   with one.
4. **Thou Shalt Not Shoot Thyself In The Foot.** No operation, code path or
   subsystem may leave the node — or the client — in a state it cannot recover
   from on its own. *Added by the server on 2026-09-20 and adopted by the
   clients unchanged.* It is different in kind from the three above: laws 1-3
   decide who goes first, this one decides what may not be done **at any
   priority**. It is a veto over all three and where it conflicts it wins,
   because a component that has destroyed itself serves no viewer.

   "Non-recoverable" is deliberately broad, and means any of these:

   - it needs physical access to fix, which on a remote node means an outage
     lasting until somebody travels;
   - it needs manual state surgery, or an operator who knows an undocumented
     incantation;
   - it loses data that was already acknowledged;
   - it cannot be stopped or restarted cleanly, so the ordinary remedy is
     unavailable;
   - it degrades without bound and offers no path back — a loop that will not
     finish, a queue that will not drain, a budget that cannot admit one item.

   The last is the easiest to ship by accident and the hardest to see, because
   the component stays up and reports itself healthy the whole time.

   Its test, in the server's words: *if this goes wrong on the node furthest
   away, does it come back without me?* The client's version of the same
   question is a television in another room that nobody will relaunch. If the
   honest answer is no, it does not ship in that form.

These laws define priority, not polling. Viewer demand, resource availability,
durability completion, queue transitions and pressure thresholds must wake or
pace work through events.

They also define priority, not exclusion. The strict order is:

```text
control > viewer >> loader > speculative
```

Capacity is work-conserving where safe, but a lower class may have only a
bounded amount of non-pre-emptible work outstanding when a higher class arrives.
The non-zero loader share must still prove continued progress under sustained
viewing.

## Self-healing disciplines

Law 4 comes with five disciplines governing how a component behaves when
something is wrong. They exist because a system whose failures do not fail
loudly produces outages that are only visible one at a time, each hiding the
next. They are cited by number like the laws.

1. **Re-derive, don't assert.** A check that fails against recorded evidence
   probes the ground truth and re-stamps the evidence. It never retries the
   stale assertion, and bookkeeping that could be cheaply re-derived is never
   trusted over the truth it records.
2. **One work-item policy.** Every retried unit of work has backoff, a failure
   budget, a parked state that is visible, and an operator action. No loop
   retries at a fixed interval, and no wait on another component lacks a
   deadline. "Not yet" must never silently become "forever".
3. **Recover by resolving.** Recovery paths do not throw on an inconsistency
   that has a deterministic resolution: they resolve it, log one line, record
   the outcome so the next start does not see it again, and count it. Only a
   genuinely fatal condition may refuse to start. A component that stays up
   with a counter to read beats one that exits correctly.
4. **Compact history out of the hot path.** The size of live state is a
   function of what is live, not of its history. Retirement history and
   resolved conflicts belong in separately compacted structures, so that read,
   merge, replay and transfer cost scales with the library rather than with its
   past. (This one bites the server hardest, in its metadata snapshots.)
5. **A bound smaller than one unit of its own work is not a bound.** A cache
   whose eviction policy can evict everything a running operation needs to make
   progress converts a linear operation into a quadratic one, silently. The same
   holds for any budget: if it cannot admit one item, it does not degrade
   gracefully, it fails superlinearly and without a log line. Prefer a bound
   derived from the observed unit size over a byte count chosen when the unit
   was smaller, make a budget that cannot admit one item a visible error at
   startup, and count pinned or exempt entries against the budget rather than
   reporting a capacity the caller cannot actually use.

## Non-bypassable end-to-end invariant

Classification at the UI, API or playback boundary is necessary but
insufficient. Priority must accompany a request through every resource it can
wait for or occupy:

```text
user intent -> client state -> network request -> server admission
            -> executor -> lock -> buffer/byte credit -> CPU work
            -> physical I/O -> RPC -> source delivery -> media pipeline
```

No function called from bounded work may hide an unbounded subordinate
operation. A lower-priority operation may not hold a shared lock, executor slot,
buffer reservation, network allowance or media resource while waiting for slow
work if doing so can block control or viewer progress.

Priority inversion is a correctness failure, not merely a poor benchmark.
Bookkeeping, artwork, diagnostics, ingest/status activity and speculative
read-ahead must not delay playback startup, transport controls or seeks.

These are software scheduling invariants, not a promise that failed hardware or
an unavailable server has zero latency. When a required resource is unavailable,
the higher class must complete from already-published state where its contract
permits or fail/degrade within an explicit boundary. It must never wait
indefinitely while lower-priority work continues against the same resource.

## Review gates

Any material change is reviewed against these questions. Each repository adds
its own, more specific gates beside them (the server's are in its
`CONTRIBUTING.md`).

- Can control, cancellation, shutdown or error recovery become queued behind
  media, artwork, ingest, diagnostics or speculative work? (Law 1.)
- Can playback startup, transport or seek wait for unrelated bookkeeping,
  caching, read-ahead, presentation or another viewer's work? (Law 2.)
- Is loader work held back by anything other than a viewer or a genuine hard
  limit? (Law 3.)
- If this goes wrong where nobody can reach it, does it come back on its own?
  (Law 4.)
- Is any retry, queue, buffer, cache, recovery loop or in-flight generation
  unbounded, or bounded below one unit of its own work? (Disciplines 2 and 5.)
- Is polling being introduced where an event or owned state transition exists?
- Is cached/local state being mistaken for server authority or a live resource?
- Does a compatibility path silently change the ownership or priority contract?

For a client, also:

- Can a route/render lifecycle accidentally acquire or destroy playback
  resources?
- Does the change preserve eager module loading and immediate core UI assets?

If any answer is uncertain, add characterization or regression coverage before
changing the mechanism.
