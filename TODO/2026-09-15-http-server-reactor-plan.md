# Plan: the HTTP server without a thread per connection

Date: 2026-09-15

Status: Stages A and B implemented in 0.43.0 (2026-09-15); laptop suite
green; not yet deployed or UAT'd on the cluster. See "What shipped" at the
end for where the implementation departed from the text below.

The HTTP API is served by a fixed pool of worker threads, and a worker is
spent on every kind of waiting the server does: a kept-alive connection
sitting idle, a held segment request waiting on the encoder, a slow viewer
draining a multi-megabyte segment over the WAN, a direct-play read fetching a
range from another replica. Only one of the things a worker does -- running a
handler -- is work. Everything else is a thread parked on a socket or a
condition variable, and the pool is sixteen wide. This plan replaces that
with one reactor thread that owns every socket and never waits, a bounded
pool that only ever computes, and explicit continuations for the two routes
that wait on the media pipeline.

## What the code actually looks like (verified 2026-09-15)

- **Accept thread, fd queue, sixteen workers.** `HttpServer::run`
  (`src/http.cpp:236-334`) blocks in `accept()`, pushes the fd onto
  `queue_` and drops the connection if the queue is at
  `max_queued_connections` (`:304-313`). `HttpServer::worker` (`:336-363`)
  pops one fd and owns it until the connection closes. Defaults at
  `src/config.hpp:305-310`: 16 workers, 128 queued, 30 s client I/O timeout,
  256 KB stream chunk, 100 requests per connection, 15 s keep-alive idle.
- **A worker owns a connection for its whole keep-alive life.**
  `handle_client` (`:514-532`) loops `handle_one_request` until it returns
  false. Between requests the worker sits in `recv_before` (`:81-103`) for up
  to `keep_alive_idle_timeout` (`:376-377`). `queue_has_backlog()` (`:365`)
  is consulted only *after* a request has been answered (`:477`), never while
  the worker is already parked -- so a backlog of new connections cannot
  reclaim a worker that is idling on an old one. This is the untested
  hypothesis for the 10 s `GET /api/v1/status` in `ACTIVE.md` ("P1 --
  Cluster connectivity", the status item).
- **The handler contract is synchronous and buffered.**
  `std::function<HttpResponse(const HttpRequest&)>` (`src/http.hpp:77`).
  The request body is read fully into memory up to `max_request_bytes`
  (8 MB, `:432-446`). The response is either `Bytes body` or a
  `std::shared_ptr<HttpBodySource> stream` (`src/http.hpp:36-61`), and the
  worker pumps a stream itself: `read()` a chunk, `send_all()` it, repeat
  (`src/http.cpp:497-507`). `send_all` (`:105-115`) is a blocking send under
  `SO_SNDTIMEO`; a slow client stalls the worker for the whole body, one
  30 s socket timeout per `send()` call.
- **Three body sources exist, and they differ in whether a read can wait.**
  `MemoryBody` (`src/playback.cpp:172`) copies from a resident segment
  buffer. `FileBody` (`src/web_api.cpp:72`) is a `pread` on a local file.
  `LogicalBody` (`src/playback.cpp:141`) reads through a `ReadHandle`, which
  may fetch from a remote replica over RPC.
- **Segment holds cost a worker by construction, and the config says so.**
  `SegmentHoldArbiter::try_acquire` never waits (`src/segment_holds.hpp:84`);
  the wait is `MediaSegmentStore`'s condition variable
  (`src/media_segments.cpp:197`, `:310`), reached from the segment handler
  on the worker. `max_concurrent_holds` defaults to 8 with the comment
  "deliberately small because it is rationing a 16-thread worker pool ...
  whoever moves HttpServer to an async runtime should revisit this"
  (`src/config.hpp:498-505`). The 2026-09-08 plan
  ([bounded VOD playlist and segment holds](2026-09-08-bounded-vod-playlist-and-segment-holds.md),
  "Why the global budget exists, and what changes when the server goes
  async") designed the hold as an explicitly admitted resource so that this
  work would replace only the waiting primitive.
- **TLS is not in-process.** `src/config.hpp:288-290`: a TLS-terminating
  proxy sits in front and Macha serves plain HTTP/1.1. The reactor only has
  to do sockets.
- **There is no event loop anywhere to reuse.** The RPC transport in
  `src/net.cpp` is also thread-per-connection: a reader and a writer
  `jthread` per peer session (`:1153-1154`, `:3069-3070`), an accept thread
  (`:3858`), a reader per accepted session (`:4005`). No asio, libuv or
  libevent is linked (`CMakeLists.txt:131-228`: OpenSSL crypto, curl, libav,
  libtorrent, yaml-cpp, miniupnpc).
- **Logging is synchronous under a mutex.** `ConsoleLogger::log`
  (`src/log.hpp:54-63`) takes `mutex_` and writes to the console fd. Under
  journald a full pipe can stall that write.
- **Construction and routing.** `Service` builds the server at
  `src/service.cpp:118-138` with the handler, the capability-URL exemption
  and the session authenticator (two O(1) local lookups). `Service::handle_http`
  (`:225`) routes health, session, status, users, web, then the rest.
- **Four test files drive the server over real sockets:**
  `tests/test_media_playback.cpp`, `tests/test_hydration_catalogue.cpp`,
  `tests/test_invariants.cpp`, `tests/test_session.cpp`. They are the
  behavioural contract for keep-alive, 413, timeouts and the backlog cap.

## Where a thread is spent today

| Waiting on | Bounded by | Costs a thread? |
|---|---|---|
| Idle keep-alive between requests | 15 s idle timeout | Yes, for the whole idle |
| A segment hold on the encoder | `max_concurrent_holds` = 8 | Yes, hence the low cap |
| A slow client draining a body | 30 s per `send()` | Yes, for the whole body |
| A direct-play range from a remote replica | `ReadHandle` deadline | Yes |
| A handler computing | -- | Yes, and this is the only one that should |

A single deeply prefetching native player can take the whole pool; three
client families each holding one kept-alive connection take most of it.
When the pool is gone the node stops answering `/api/v1/health` and
`/api/v1/status`, which is a governing-law-3 violation.

## The design

Three parts, replacing the one thread that does all three today.

### 1. One reactor thread owns every socket

Non-blocking sockets, a readiness loop over `poll()`, and a per-connection
state machine: reading a request, running a request, writing a response,
idle keep-alive. The existing request parser, keep-alive rules, `413`,
`OPTIONS`, CORS headers and `Content-Length` framing move over intact; only
the I/O around them changes.

An idle kept-alive connection costs an fd and a small struct. The idle
timeout becomes a deadline the reactor checks each pass instead of a
`poll()` a thread is parked in. The accept backlog cap becomes a
`max_connections` cap on open connections, which is the number that
actually bounds memory.

`poll()` first, with the readiness set behind a small interface so kqueue or
epoll can replace it if the connection count ever justifies it. At a few
hundred connections `poll()` is fine, it is portable across macOS and Linux
without conditionals, and it matches the codebase's habit of owning its own
sockets rather than taking a dependency.

### 2. A bounded compute pool runs handlers and body reads

The handler signature does not change. `Service::handle_http`, the session
gate, roles, the capability-URL exemption and every route stay exactly as
they are; they run on a pool job instead of on the connection's own thread.
The authenticator runs there too.

Body sources are pumped by jobs, not by the loop. When a connection's
staging window drops below one chunk, the reactor posts "connection N wants
`stream_chunk_bytes` at offset O" to the pool and forgets about it. A worker
calls `HttpBodySource::read` into a buffer owned by the connection's window
and posts "ready" back through a self-pipe (or eventfd on Linux). At most one
read is outstanding per connection, so a body source is never called
concurrently and the existing implementations need no locking.

Two lanes, mirroring the request classes the RPC layer already has
(`RequestClass` in `src/net.hpp:546`): a **control** lane for health,
status, session and users, and a **data** lane for catalogue, playback and
web assets. Each lane is a small pool. Control traffic never queues behind
playback, which is governing law 3 stated as a data structure rather than
as a hope. Whether this is two pools or one pool with a priority queue is an
open decision below; the recommendation is two pools, because starvation of
a lane is then impossible rather than merely unlikely.

### 3. Explicit continuations for the two routes that wait

Segment holds and profile publication are the only handlers that block on
the media pipeline. A handler that would wait returns a deferred result
instead: what it is waiting for, its deadline, and the admitted `Hold`. The
reactor parks the connection. When the segment store publishes (its
`notify_all` at `src/media_segments.cpp:101`), or the deadline passes, the
request is re-queued to the data lane and the handler runs again with the
segment now present, or answers the existing `segment_not_ready` refusal.

This is exactly the seam the 2026-09-08 plan kept: the admission policy is
unchanged, the deadline is a property of the request, the hold is still an
explicitly admitted resource released in a destructor, and only the waiting
primitive is replaced. `max_concurrent_holds` then stops rationing threads
and becomes the memory and fairness bound the config comment says it should
be; its default rises.

## How megabytes reach many players through one thread

The reactor never waits on a client and never copies more than once. The
bytes sit in the segment store and in the kernel's socket send buffer, not
in the reactor.

- **The per-connection pump.** A streaming response gets a staging window of
  two chunks (`stream_chunk_bytes`, 256 KB each). The reactor `send()`s what
  is staged; the kernel takes what fits and returns `EAGAIN` for the rest;
  the reactor records the offset, asks for `POLLOUT`, and moves on. When the
  client drains its receive window the socket becomes writable, the reactor
  sends the next slice, and when the window falls below one chunk it asks
  for the next one. Backpressure flows one way: client TCP window to kernel
  buffer to staging window to producer. A slow client stops being asked for
  anything, at a cost of one fd and 512 KB.
- **Concurrency is interleaving.** One pass visits every writable socket
  and hands each one slice; fifty players mid-segment are fifty slices per
  pass. A pass takes microseconds; a client's window takes milliseconds to
  drain. Fairness follows from the loop shape: a LAN client cannot starve a
  WAN client because nobody gets a second slice before everybody has had
  one.
- **The wire is the bottleneck, not the thread.** A gigabit port carries
  125 MB/s; one core copies into the kernel at several GB/s. The loop is
  idle well over 90 percent of the time with the link saturated. Adding
  threads to the send path would move no more bytes.
- **What limits viewers per node is unchanged:** the encoder (CPU on the
  pipeline threads, which stay threads), upstream bandwidth, and memory for
  resident and staged segments. Today's fourth limit -- sixteen sends in
  progress -- is artificial and goes away.
- **Above gigabit** a single core does start to be the copy bottleneck. The
  answers are `sendfile` for file-backed bodies or a second reactor with
  connections sharded by fd. Neither is needed at this cluster's link speeds
  and neither is precluded by the design; both are out of scope.

## The rule: the reactor may not call anything that sleeps

This is what makes one thread safe. It is guaranteed structurally, closed by
rule where structure cannot reach, and checked at runtime.

**Structurally.** The reactor's per-connection state holds an fd, parser
state, a list of filled chunks (shared byte buffers with offset and length),
deadlines, and an opaque job handle. It does **not** hold the
`HttpBodySource`, the `ReadHandle`, the `Service`, the handler, or the
authenticator. Those live inside pool jobs, and only the pool executes pool
jobs. If the reactor's data cannot name the type, reactor code cannot call
it, and a reviewer can verify that by reading one struct. Every DHT fetch,
remux, transcode, RPC call and disk read therefore happens where it does
today -- on pipeline threads and inside handlers -- and the reactor's entire
vocabulary is `accept`, `recv`, `send`, `poll`, `close`, `memcpy`, and push
or pop on a queue.

**No exceptions for cheap routes.** Health is two atomic loads and it will
be tempting to answer it on the loop. It runs on the control lane like
everything else. The first exception is how the rule dies.

**The three leaks structure does not close:**

- *Mutexes.* The reactor takes exactly one, the job queue's, held for a push
  or a pop and never while waiting for anything. Reactor-owned state is
  touched only by the reactor; handoff is through the queue.
- *Logging.* The reactor logs on startup and on accept failure only. Per-
  request logging, including the slow-request log below, happens from the
  pool.
- *Allocation.* `malloc` can contend on a lock but never sleeps on I/O.
  Every reactor accepts this.

**At runtime.** The reactor stamps the time at the top of each pass. A pass
longer than a few milliseconds increments a `reactor_stalls` counter and
records the longest pass, exposed in diagnostics like the FUSE stale-mount
counters. If the rule is ever broken in future, it shows up on the first
slow disk as a named counter rather than as an unexplained 10 s status
response.

## Body sources under the new pump

| Source | Where the bytes are | Who produces the next chunk |
|---|---|---|
| `MemoryBody` -- a resident segment | RAM, behind a `shared_ptr` | The reactor sends straight from it. No copy, no pool hop. |
| `FileBody`, spilled segments | Local file | Data-lane job, one `pread` per chunk. |
| `LogicalBody` -- direct play | `ReadHandle`, possibly a remote replica | Data-lane job, always. A slow replica delays one viewer's next chunk and nothing else. |

`MemoryBody` is the common case for transcoded HLS: the segment is fully in
memory when the request is admitted, and the `shared_ptr` keeps it alive for
as long as any connection is mid-send, which is the lifetime rule today. The
optimisation of letting the reactor send from resident memory directly (no
job, no copy) is worth doing in the first pass because it is the hot path;
`HttpBodySource` gains an optional "resident bytes" accessor so the reactor
can tell.

A local `pread` of 256 KB is well under a millisecond, and it would be
tempting to run it on the loop. It goes to the pool, because a stalled disk
must stall one connection, not all of them. A dead disk stalls as many
data-lane workers as there are viewers on it, which is the same admission
problem the pool has today but now confined to reads that genuinely block;
the `ReadHandle` deadline already bounds how long such a job can sit, and
`FileBody` gets an equivalent.

A client that seeks or closes produces `POLLHUP` or a reset on the fd. The
reactor sees it on the next pass and drops the body source and the
outstanding job's result. Today the worker only finds out at its next
blocking `send()`, which can be tens of seconds later while it holds the
thread and the segment.

## Stages

**Stage A -- the reactor, the pool, and the pump.** Replace the accept
thread and worker pool with the reactor and two lanes. Handlers unchanged.
Body pump through jobs, with the resident-memory fast path. Stall watchdog
and diagnostics. The four socket-level test files pass unchanged, plus the
new isolation tests below. This stage alone removes three of the four
thread-spending waits and is the one that answers the 10 s status question.

**Stage B -- continuations.** The deferred result type; segment holds and
profile publication converted; segment-store notification wakes the parked
request; `max_concurrent_holds` default raised and its comment rewritten to
say what it now bounds. `HttpResponse` is otherwise untouched.

**Not planned:** `sendfile`, reactor sharding, HTTP/2, in-process TLS,
and moving the RPC transport onto the reactor. The last is a real
possibility and the same design would serve, but it is a separate piece of
work with its own protocol-level consequences, and nothing here should be
shaped to anticipate it.

## Configuration

Under `catalogue.api`:

- `workers` becomes the data-lane pool size; `control_workers` is new and
  small (2). Both keep their meaning as "threads that compute".
- `max_queued_connections` becomes `max_connections`, the cap on open
  connections, default raised (1024) because an open connection is now an fd
  and a struct.
- `stream_chunk_bytes` keeps its meaning; `staging_chunks` is new (2).
- `client_io_timeout`, `keep_alive_idle_timeout`, `keep_alive_max_requests`,
  `max_request_bytes` keep their meaning exactly; they are now deadlines the
  reactor checks rather than socket options a thread waits under.
- `streaming.max_concurrent_holds` default rises in Stage B.

Old keys stay accepted with a deprecation note in the changelog for one
release.

## Diagnostics

Added to the diagnostics endpoint, not the polled status route:

- `reactor_stalls`, `reactor_longest_pass_ms`, `reactor_passes`.
- `connections_open`, `connections_idle_keep_alive`, `connections_writing`,
  `connections_deferred`.
- `staged_bytes` (sum of staging windows).
- Per lane: `queue_depth`, `queue_wait_ms_max`, `busy_workers`.
- The slow-request log the status item offered and did not build: a request
  whose handler took longer than a threshold logs method, path, elapsed,
  and lane, from the pool.

## Tests

The existing four files are the contract and must pass unchanged first.
New, all against real sockets:

- A body source whose `read` blocks forever on connection A; a `GET
  /api/v1/health` on connection B is answered within the control-lane
  budget.
- A handler that blocks forever on the data lane; health on the control lane
  is still answered. Then `workers` data-lane handlers blocked; health still
  answered, and a further data request gets `503` rather than queueing
  unboundedly.
- A client that reads one byte per second from a 4 MB body on connection A
  does not delay a 4 MB body on connection B.
- Two hundred idle kept-alive connections; health answered promptly and
  `connections_idle_keep_alive` reads 200.
- A client that closes mid-body: the body source is released within one
  pass, verified through its destructor.
- Stage B: a segment request parked on a hold is answered within one pass of
  the store publishing that segment, and answers `segment_not_ready` at the
  deadline if it never does.
- The reactor stall watchdog: a test-only hook that sleeps on the loop
  increments `reactor_stalls` exactly once.

## What this does not fix, so nobody expects it to

- **`DELETE .../metadata` hangs.** That handler does far too much and is its
  own P0 structural item. A hung handler still occupies a data-lane worker;
  it no longer takes the control lane with it, which is the difference
  between a slow route and a dead node.
- **A status response that is slow inside the handler.** The health-versus-
  status test in the status item still applies; this work removes the
  "never reached a handler" half of the ambiguity.
- **Encoder throughput and upstream bandwidth.** Real limits, unchanged.

## Open decisions, with recommendations

1. Hand-rolled `poll()` reactor versus a library. **Hand-rolled.** No
   dependency, portable, and the parser already exists.
2. Handlers stay synchronous on a pool, continuations only for the routes
   that wait. **Yes.** This is what makes the work incremental rather than a
   rewrite of every route.
3. Two lane pools versus one pool with a priority queue. **Two pools.**
   Starvation becomes impossible rather than unlikely.
4. RPC transport onto the reactor. **Not in this work.**
5. HTTP/1.1 only, no HTTP/2, no in-process TLS. **Confirmed by the config
   comment; carry on.**

## What shipped (2026-09-15, 0.43.0)

Stages A and B, in one release, with these departures from the text above:

- **Profile publication was never a request path.** `publish_profiles` is a
  background thread that drains a queue; no HTTP handler waits on it. The
  only route converted to a continuation is the segment hold. Two other
  waits inside session creation -- a second create joining an in-flight
  probe, and an idempotent creation joining its owner -- still block a
  data-lane worker, bounded by their own deadlines. They are short and
  rare; converting them is possible with the same `HttpDeferral` shape if
  they ever show up in `data_lane.busy`.
- **Lane classification is a prefix list, not a callback.**
  `HttpServer::set_control_prefixes` takes paths; the reactor decides the
  lane by string comparison. A callback would have been application code on
  the reactor, which is the one thing the rule forbids. `Service` names
  `/api/v1/health`, `/api/v1/status`, `/api/v1/session` and `/api/v1/users`.
- **The pump's size and resident pointer are read on the pool.** `BodyPump`
  is constructed on the lane thread that produced the response, so
  `HttpBodySource::size()` and `resident()` are never called by the reactor
  either.
- **Body reads bypass the lane queue cap.** They are bounded by the staging
  windows of connections already admitted, and refusing one would abandon a
  viewer mid-fragment; `max_queued_requests` applies to new requests only.
- **`sendfile` was not needed for the first pass**, as predicted: the
  resident path covers transcoded fragments, and a `pread` of 256 KB on the
  pool is sub-millisecond.
- **The stall threshold is configurable** (`reactor_stall_threshold_ms`,
  50) rather than "a few milliseconds": a laptop running the whole suite in
  parallel schedules the reactor late often enough that a lower value would
  count noise.
- **One test changed meaning.** The keep-alive-under-backlog case asserted
  the workaround this work removes; see the changelog.
- **Not yet done:** deployment and cluster UAT. What to look at when it is:
  `diagnostics.http.reactor_stalls` staying at zero over a day of viewing;
  `connections_idle_keep_alive` when the three client families are all
  connected; whether `GET /api/v1/status` is ever slow again, and if so,
  whether health is slow with it (it should not be, ever again); and the
  hold counters under a deeply prefetching native player with the budget
  now at 64.
