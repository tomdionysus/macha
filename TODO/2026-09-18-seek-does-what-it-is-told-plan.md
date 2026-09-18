# Plan: a seek goes where it was asked to go

Date: 2026-09-18

Status: **Agreed and not yet implemented.** The defect is measured on the live
cluster, the response shape is settled with both client sessions, and the
operator has stated the governing rule. Nothing in `src/` has changed.

## The rule

**The server does what it is told.** It does not change the mode a client asked
for, and it does not move the position a client asked for. Where a mode cannot
begin a stream at the exact position requested, the response says so explicitly
instead of relocating the request and reporting the relocation as though it were
what was asked for.

A client that wants a cheap, exactly-aligned seek asks for a position that is
already a keyframe. `video_random_access_points` is on the plan so it can.

## The defect

A remux seek starts *after* the position asked for, by up to 9.3 s, always
forward. `media_vod::indexed_plan` (`src/media_vod.cpp:41-63`) discards every
keyframe earlier than the request and takes the first survivor:

```cpp
if (!std::isfinite(seconds) || seconds + kTimestampEpsilon < requested_seek_seconds ||
    seconds >= duration_seconds - kMinimumDuration)
    continue;
...
result.actual_seek_seconds = keyframes.front();
```

The transcode path does the same thing through
`media_vod::nearest_keyframe_at_or_after` (`:97-105`), called from
`prepare_hls_vod` (`src/media_engine.cpp:1815-1818`) and again from
`reseek_hls_vod` (`src/media_engine_common.cpp:60-64`).

**The content between the request and that keyframe is in no generation at all.**
No client can recover it. For a viewer seek that is a skipped scene; on the
reaped-session recovery path, which rebuilds a generation at the position a
viewer has actually reached, it deletes content mid-playback.

Measured on 2026-09-17 against es-1 (`10.34.1.50:7438`) and fi-1, remux,
`tmdb:episode:1747124`, title duration 3,951,957 ms:

| requested (ms) | server started (ms) | overshoot (ms) | path |
|---|---|---|---|
| 2,027,092.2 | 2,028,903 | +1,810.8 | viewer seek |
| 2,818,000 | 2,822,779 | +4,779 | viewer seek |
| 908,791.054 | 918,085 | +9,293.9 | session recovery |
| 2,926,000 | 2,934,933 | +8,933 | viewer seek |

Confirmed independently of any log: for the recovery case the browser's media
element reported the replacement generation's duration as 3,033.9 s against a
title of 3,951.957 s, a difference of 918.057 s matching the reported start to
within rounding. The generation genuinely begins where the server said, and the
missing ~9.1 s was measured twice by different methods (the client's handover
arithmetic and a 100 ms position sampler that knows nothing of it).

The forward alignment is deterministic: asked for 2,926,000 the node returned
2,934,933 to 147 consecutive requests over 33.3 s. A client bound that rejects a
start more than one segment ahead therefore cannot make progress, which is how
this became a livelock rather than an inconvenience.

## The response shape

Three fields on the playback session payload, flat and additive, present on
create and on every `PATCH`. All are milliseconds on the title's timeline.

- **`seek_ms`** — the baseline: where the generation's media actually begins,
  the first sample the client receives. **This is exactly what the field means
  today**, so no existing client changes behaviour.
- **`seek_offset_ms`** — how far into that generation the requested position
  sits.
- **`seek_requested_ms`** — the position the server honoured, after clamping to
  `[0, duration - 1 ms]`.

The invariant:

```
seek_ms + seek_offset_ms == seek_requested_ms
```

Exactly, in integer milliseconds, no tolerance and no rounding slack.
`seek_offset_ms` is never negative, so a generation always contains the position
asked for and nothing between the request and the stream start can go missing.

`seek_requested_ms` exists because an exact invariant is only useful if a client
can act on it being violated, and without it a client cannot distinguish a
violation from a clamp near the end of a title. Those want opposite handling —
one is a defect worth surfacing, the other is ordinary. Core's phrasing for the
rule it keeps rediscovering: an unanswered question must not read as an answer.

## Per mode

**Transcode.** `seek_ms` is exactly the requested position; `seek_offset_ms` is
always 0. The encoder can start on any frame, so it does. The decoder still
seeks back to the preceding keyframe for pre-roll and discards decoded frames
before the origin (`src/media_engine.cpp:1275-1279`, rebase at `:1418`); on slow
software decode that can add seconds to startup. That is the price of asking for
a non-keyframe and it is the client's to pay.

**Remux.** `seek_ms` is the **last keyframe at or before** the request;
`seek_offset_ms` is the remainder. A stream copy has no decoder and an fMP4
fragment's first sample must be a sync sample, so this is the only split the
container permits. The client attaches `seek_offset_ms` into the first fragment.

**Direct.** `seek_ms` is the request, `seek_offset_ms` is 0. There is no
generation; the client byte-ranges the source.

The offset is therefore zero exactly when the mode can be frame-accurate.

**The mode is never substituted.** A remux request stays remux, including when
its keyframe situation is awkward. An earlier draft of this plan had remux fall
back to transcode when no keyframe at or before the request existed; the
operator rejected that outright, and correctly — it is the same second-guessing
as moving the seek, and it would trade picture quality and CPU for a case the
client did not ask about.

## Edge cases

- **Request out of range.** Clamped to `[0, duration - 1 ms]` before planning, as
  today. The invariant holds against `seek_requested_ms`, which is the clamped
  value, and the client can see the clamp happened.
- **Request exactly on a keyframe.** `seek_offset_ms` is 0 in both modes and the
  seek is cheap. This is the property that lets a client opt into fast seeks.
- **Remux with no indexed keyframe at or before the request.** Baseline 0,
  offset carrying the whole request. A decodable stream's first sample is
  necessarily a sync sample, so a copy can always start at the beginning; the
  index simply did not name it. Mode preserved, invariant preserved, nothing
  lost. In practice unreachable, since a file's first frame is virtually always
  indexed — it exists so the invariant needs no escape hatch.

## What changes

- `media_vod::indexed_plan` (`src/media_vod.cpp:41`): choose the last keyframe
  at or before the request instead of the first at or after, and return the
  offset alongside the baseline.
- `media_vod::nearest_keyframe_at_or_after` (`:97`): no longer used for
  alignment. The transcode paths stop snapping entirely.
- `prepare_hls_vod` (`src/media_engine.cpp:1701`), both branches: set the
  baseline and offset rather than a single relocated seek.
- `reseek_hls_vod` (`src/media_engine_common.cpp:38`), both branches: the same,
  so a `PATCH` seek and a create seek agree.
- `HlsVodPlan` / `PlaybackPlan`: carry the offset and the honoured request
  alongside `seek`.
- `session_json` (`src/playback.cpp:1587`): the three fields.
- `docs/streaming.md`, `docs/configuration.md`: the shape and the invariant.
- `TODO/ACTIVE.md`: the client contract entries.

## Diagnostics that go in with it

Two things this investigation needed and the server could not say:

- **Cue density on a successful plan.** `video_keyframe_seconds`
  (`src/media_engine.cpp:430`) reads the demuxer's index, which for Matroska is
  the Cues — and Cues are not obliged to name every keyframe. So the observed
  spread is the distance between *indexed* entries, an upper bound on the true
  GOP. Only the reject path logs anything today (`:1764-1768`). Log the shape on
  success too: entries, longest gap, median gap. If the measured offsets cluster
  well below the indexed gaps, the Cues are sparse rather than the GOP long.
- **Why the seek fast path was declined.** `reuse_seek_session`
  (`src/playback.cpp:1513`) logs at INFO on success and says nothing on failure.
  Across a whole day on es-1 there are **zero** `seek fast-path` lines, so it is
  never taken — but nothing records whether `seek_only` was false or
  `reseek_hls_vod` declined. Name the failing precondition.

## The fast path is separately broken, and it is probably ours

Not part of this change, but found by it and worth its own item. The client's
seek `PATCH` sends preferences that compare equal to the stored record —
`subtitle_language` is a plain `std::string` defaulting to `""`, so the client's
`""` matches, and `optional_int` maps a null `subtitle_stream` to `nullopt`,
which is what a subtitles-off session already holds. So `seek_only` should be
true and the fast path should be taken. It never is.

The remaining branch is `reseek_hls_vod` declining. For remux it re-runs
`indexed_plan` over the keyframes from the new position, and that rejects a plan
whose longest fragment or tail exceeds 90 s. The transcode branch of that same
function carries a comment warning the check "can spuriously reject an otherwise
perfectly usable seek point if any other part of a long file has a sparser GOP"
(`src/media_engine_common.cpp:56-59`) — that warning describes the remux
branch's behaviour and was never applied to it.

Not asserted as the cause: it is the branch that remains, not a proof. The
diagnostic above settles it. Measured cost while it is broken: 147 `session-update`
calls in 34.7 s, 34.68 s of cumulative server time, ~4.2/s on a node also
serving viewers, none of them individually slow (235.9 ms mean, 1,407.3 ms for
the first).

## Agreed with the client sessions

Both reviewed the shape before it was written and both accepted it.

- **Core** already models a generation this way: `generationLocalPosition` is
  `absolute - session.seekMs`, with `seekMs` already meaning where the media
  begins. `seek_offset_ms` is the number it derives on the next line and can
  stop deriving. Its `activationPosition` returns undefined when the request is
  before the generation's start — the branch that livelocked — and the invariant
  makes that state unreachable, so the branch is deleted rather than tuned. Core
  asked for `seek_requested_ms` and will type all three as `number | undefined`,
  since an older node omits them.
- **The client** confirmed both attach paths already do this: the initial-seek
  listener used by every Continue Watching resume, and the handover path, whose
  `clockOffsetMs` wants exactly a non-zero position. A non-zero offset on a
  session's *first* generation is not a special case for it.
- **The offset is fetched, not watched.** An earlier draft of this plan weighed
  the remux offset as replay the viewer sees. It is not: the client attaches *at*
  the offset, so the pre-roll is never presented. It is data fetched, and with
  hls.js's `startPosition` pointed at `seek_offset_ms` it need not even be
  fetched. That is what settled the question of a transcode fallback against it.
- One caveat the client raised rather than hid: `WebMediaTimeline.establishOrigin`'s
  non-zero branch is written for this case but is the least-exercised path in
  that file, and it has not carried a large offset. It will be watched on the
  first run rather than assumed.

## Out of scope

- The existing remux-to-transcode fallback for a wholly unusable keyframe index
  (`src/media_engine.cpp:1769-1777`). That is remux being unplannable, not a
  seek being moved, and it is guarded by `allow_video_transcode_fallback`.
- `indexed_plan`'s 90 s fragment and tail bounds. They may well be wrong for the
  re-seek path (see above) but changing them is a separate decision with its own
  evidence.
- The client's own acceptance bound, which core has already made sign-safe in
  its `6369f2c` so a backward delta needs nothing from it.
