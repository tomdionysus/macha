# One definition of work class

Status: agreed design, 2026-10-06. Not started. Operator's instruction: "the
API is viewer class work", and one definition of a class, used by every
subsystem, is a principle of the project.

## The rule

A piece of work has one class, decided where the work starts and carried
with it: **control**, **viewer**, **loader** or **background**. The laws
rank them in that order (law 1 control, law 2 viewer, law 3 loader; what the
laws do not name is background). Every subsystem that admits, paces,
schedules or measures work reads that one class. None keeps its own notion
of which work is which.

## What the code does today

Three places class work, each by its own rule, and they disagree:

- **DATA admission** (`DataResourceArbiter`, `data_work.hpp`): a viewer is a
  `foreground` or `read_ahead` frame; a loader is a `loader` frame;
  everything else (`speculative`) is background; `control` never enters.
- **The activity clocks** (`activity_clocks.cpp`): `foreground` marks the
  "playback" clock, `read_ahead` the "interactive" clock, `loader` the
  loader clock; `speculative` and `control` mark nothing. "A viewer is
  present" (the pacer, the pressure gate, `maintenance.foreground_quiet`,
  status's idle-before-persist) means either of the first two clocks moved.
  Only `DistributedStore` marks them, when DATA bytes move. An HTTP request
  marks nothing: a node with someone browsing the catalogue or saving in
  the editor reads as idle, and background work paces as if no one were
  there.
- **The HTTP server** (`http.cpp`, `service.cpp:71`): two worker pools,
  chosen by path prefix in the reactor. Health, status, session and users
  go to the control pool; everything else to the data pool, playback and
  editor and acquisition alike. The pool is law 1's reservation, not a
  class.

And a fourth, implicit: API handlers mark catalogue reads with
`WorkContext(FrameType::control)` to mean "memory-only, refuse cold work"
(`catalogue_api.cpp:496`, `manage_api.cpp:904`), because `may_enter`
derives what work may wait on from the frame. That spells a waits contract
as a class. API-started DATA work (a match staging artwork, validating it,
storing a media index) runs at `speculative`, the background class, because
the scanner's background matching shares those functions and nothing passes
the caller's class down.

`FrameType` (`frame_type.hpp`) is the wire encoding, with values fixed on
the wire, and it is what every one of these reads. It is not the class: it
carries the class plus one hint (`read_ahead` against `foreground`, both
viewers).

## The design

### 1. `WorkClass`, and one mapping from the wire

```
enum class WorkClass : uint8_t { control, viewer, loader, background };
constexpr WorkClass work_class(FrameType) noexcept;
```

`work_class` is the only place a frame becomes a class: `control` to
control, `foreground` and `read_ahead` to viewer, `loader` to loader,
`speculative` to background. `FrameType` stays the wire and transport type;
no consumer compares frame values to decide class again. The arbiter's
`viewer()` and `loader()` predicates, the clocks' per-frame branches and
the cost-budget spec's "classes below viewer" all read `work_class`.

### 2. The activity clocks are per class

`ActivityClocks::note(WorkClass, bytes)`, `idle_for(WorkClass)`,
`viewer_recently_active`: presence and idleness are per class. The
playback and interactive clocks become the viewer clock (every reader
already takes them together). Bytes stay counted per frame for the traffic
telemetry status reports; presence does not depend on bytes, so a request
that moves none can mark it.

### 3. An HTTP request's class is decided once, at the route table

The service's route table says what each route is: the four control routes
(health, status, session, users: law 1, a node saturated serving viewers
still says what is wrong with it) are control-class; **every other route is
viewer-class**. The HTTP server reads that class: the pool follows it
(control pool for control, data pool for viewer), and dispatch marks the
activity clock with it, so a viewer-class request is a viewer present. The
prefix list moves from `http.cpp` to the route table; `http.cpp` stops
classing by path.

The class of a request is the class of answering it. The work a request
starts has its own class: a torrent added from the API is answered as
viewer work and downloaded as loader work; a rescan is requested as viewer
work and run as background work. "The API is viewer-class work" is a
statement about requests, not about what they set going.

### 4. A work context carries class and allowed waits separately

`WorkContext` gains `Waits allowed`, defaulting from the class (control:
none beyond locks; viewer, loader, background: all). `may_enter` reads
`allowed`, not the frame. A catalogue read from the API is then what it is:
viewer-class, waits none, "memory-only or refuse". The overload of
`FrameType::control` as "must not wait" goes.

### 5. API-started DATA work carries the request's class

The match path (`reconcile_scanner`, `stage_artwork`,
`stage_artwork_deferred`, `put_media_index`, the commit's artwork
validation) takes the caller's `FrameType`; `ManageApi` passes the
request's (viewer), the scanner passes `speculative` (background). The
same for `put_artwork` from the editor. Behaviour change: artwork a person
stages from the editor is admitted as viewer work, ahead of loaders and
never refused for disk pressure; the scanner's stays background. Retention
claims on commit keep their lanes (`retain_control` on the control lane by
law 1; DATA claims as the caller's class).

### 6. The pacer sees a viewer when one is there

Consequence of 2 and 3, not a separate change: with a viewer-class request
marking the clock, `maintenance.foreground_quiet`, the pressure gate and
`repair_share` pace background work down while someone browses or edits,
as law 2 intends. Under "pace, never gate" nothing stops; the loader takes
what the viewer is not using. Measure the effect on repair throughput with
a viewer browsing (stage 3 numbers are the baseline).

## What gets deleted

- `DataResourceArbiter::viewer()` and `loader()` as frame predicates.
- The playback/interactive split in `ActivityClocks`.
- `HttpServer::set_control_prefixes` and `control_route`; the reactor
  reads the class the route table gives.
- `FrameType::control` as the way an API read says "do not wait".

## Not changed

- `FrameType` and its wire values; the RPC lanes (`control`, `data`) in
  `net.cpp`, which are transport, keyed by frame as now.
- The RPC server's classing of inbound frames: a peer's viewer reading from
  this node arrives as a `foreground` frame and is viewer-class here, which
  the one mapping preserves.
- Playback and FUSE: already `foreground`/`read_ahead`, so viewer-class
  through the same mapping.
- The cost-budget scheduling spec's six questions. This design supplies the
  class definition that spec assumes; it decides nothing the spec leaves to
  the operator.

## Tests

- `work_class` exhaustively, one case per frame.
- The arbiter's admission and pressure decisions keyed by class (existing
  tests, adapted).
- A viewer-class HTTP request marks the viewer clock; a control-class one
  marks the control clock, not viewer; neither moves DATA bytes.
- The route table: the four control routes and nothing else are control.
- A match from the API stages artwork with a viewer frame; the scanner's
  with a background frame (count the frame on the store's put).
- A catalogue read with waits none refuses a cold load rather than waiting
  (existing warm-read test, restated in the new terms).

## Order of work

1. `WorkClass` and `work_class`; arbiter and clocks on it; status callers
   updated. No behaviour change. With it, an audit: every DATA admission
   (`DataResourceArbiter::acquire` and `try_acquire`) listed with the frame
   it is given, confirming no control frame reaches one. Control's guarantee
   is separation from DATA work, not priority within it (law 1, "a floor,
   not a share"); a control path that entered the arbiter would queue
   behind a running transfer. Candidate to check: the peer validity check
   (`have_valid_objects`, `storage_server.cpp`) admits with the request's
   frame, since validity means a decrypt.
2. The route table classes requests; the server follows it and marks the
   clock. Behaviour change: API activity is a viewer present.
3. `WorkContext::allowed`; API reads become viewer-class, waits none.
4. The request's class passed down the match and artwork paths.
5. `docs/principles-and-laws.md` gets the definition (class, what it is,
   which frames carry it); `docs/configuration.md` where it names classes.
   No API or wire change, so nothing to announce.
6. Measure repair pacing with a viewer browsing, against the stage 3
   baseline; record in COMPLETED.

One to two days. Steps 1 and 3 are mechanical; 2 and 4 are the ones with
behaviour to watch.

## For the operator

- The four control routes stay control-class, above viewer, by law 1. That
  is the one place "the API is viewer-class work" is not literally so;
  confirm or overrule.
- Artwork staged from the editor becoming viewer-admitted (step 4) is the
  one DATA-side behaviour change; say if the editor's writes should stay
  background.
