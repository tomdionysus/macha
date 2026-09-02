# Memory ownership and lifecycle

Macha treats memory ownership as part of correctness. Any state which can
outlive the stack frame that created it must have an explicit owner and a
testable release rule.

## Required pattern

Every non-trivially scoped allocation or container must answer all of these in
the declaration or immediately adjacent code:

1. **Owner:** which object exclusively controls admission and release?
2. **Identity:** what stable key prevents accidental duplicate ownership?
3. **Bound:** what byte and/or count limit applies? If the bound is external
   (for example the live namespace), which diagnostic exposes its current and
   peak size?
4. **Release:** which success, failure, cancellation, timeout, disconnect,
   replacement, deletion, recovery and shutdown paths release it?
5. **Concurrency:** which lock or immutable shared view protects lifetime? A
   raw pointer is permitted only while its owning view is held.
6. **Proof:** which repeat-cycle test shows that current ownership returns to a
   baseline, and which counter makes failure observable in a live process?

Count-only bounds are not sufficient for variable-sized payloads. Cache
validity is not a lifetime policy. `shared_ptr` describes mechanics, not the
owner. Moving work between queues must transfer ownership without duplicating
or dropping its byte accounting.

## Standard ownership forms

- **Stack/RAII:** preferred for request-local state and external-library
  handles. Every exceptional exit is covered by the destructor.
- **Immutable shared view:** snapshots are decoded once, shared as
  `shared_ptr<const T>`, and copied only by the single mutation owner.
- **Bounded executor:** one fixed worker set owns a byte/count-bounded queue.
  Cancellation either completes the promise or releases the queued task.
- **Bounded cache:** immutable identity controls validity; a separate hard
  byte/count policy controls lifetime. Deletion prunes where practical, while
  eviction always remains safe.
- **Durable state machine:** an object remains owned while durable journal work,
  an open handle, or an active publication references it. One centralized
  quiescence predicate performs final reclamation.

## Current subsystem contracts

- FUSE inode ownership is centralized in
  `State::reclaim_inode_if_quiescent()`. Namespace operations hold an explicit
  durable reference from journal admission through the `done` marker. Handles,
  durability, data queues, provisional publication, unconfirmed metadata and
  spool state are independent owners.
- Metadata history owns disk frame indexes. Reconstruction owns exactly one
  mutable decoded tree and one delta body; only the requested immutable result
  may enter the bounded materialization cache.
- Hydration owns one fixed worker set and at most `max_inflight` queued/running
  tasks. It never creates a thread per object.
- RPC owns queued request payloads in separate metadata, fast-control, control
  and DATA byte pools. Dequeue, cancellation and shutdown transfer or release
  the corresponding byte ownership. Each peer/session writer also owns a
  128 MiB-bounded outbound queue (plus at most its one currently-written
  message), and pending reply identities are count-bounded and erased by reply,
  cancellation, disconnect or shutdown.
- Retention-before-metadata object checks and claim batches use a bounded
  ordinary-control call. Fast health success cannot leave one of these callers
  waiting forever behind a wedged control worker: the `dead_after` deadline
  aborts the exact route and releases its request/promise ownership.
- Playback probe and subtitle caches have independent count/byte ceilings.
  Provider response caches have a conservative resident-byte ceiling.
- Failed media-profile publication owns at most 128 results / 4 MiB in each
  retry owner. Eviction also removes any corresponding flight so the work can
  be rediscovered rather than leaving a false permanent owner.
- Torrent-search acquisition references are advisory capabilities, bounded to
  4096 entries and released by expiry or oldest-expiry eviction.
- Trace-only write overlap history is a bounded window, never a record of every
  write made through a long-lived handle.
- libav input/output contexts, custom AVIO buffers, codec contexts, frames,
  packets, dictionaries, channel layouts and subtitles use scoped owners.
  Destruction covers normal return and every exception path. Curl header lists
  preserve the old owner if an append allocation fails. Raw file descriptors
  used during orphan quarantine are scoped through fsync, rename and failure.

Authoritative indexes are intentionally not caches. Namespace/catalogue trees,
storage and packed-object indexes, retention claims, membership history and
durable ingest/torrent/hint records are bounded by durable user or disk state.
Their owning manager releases them on snapshot replacement, explicit prune or
manager destruction; they must not be evicted merely to satisfy a cache budget.
They still require current/peak byte diagnostics and loaded-library UAT because
an external-state bound can be much larger than available RAM.

## Sanitizer proof

The test runner executes each case in an isolated child. On Linux sanitizer
builds it invokes LeakSanitizer after the case's local owners have unwound and
before `_Exit`, so process isolation cannot bypass LSan's normal `atexit` hook.
The normal scheduler remains parallel; sanitizer or timing failures must be
fixed as lifecycle/concurrency defects, not hidden with `--serial`.

## Review gate

A change introducing a long-lived map, vector, deque, future, callback,
`shared_ptr`, thread, external handle, complete snapshot copy, or queued payload
is incomplete until its ownership answers and lifecycle test are present.
Loaded UAT must show a stable current-memory plateau over repeated fill/drain
cycles; a low final logical cache count alone is not evidence of bounded peak
ownership.
