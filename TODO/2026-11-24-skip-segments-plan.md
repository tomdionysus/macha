# Skip intro and credits: plan

The maintainer, 2026-11-24: plan accepted with three decisions. Fingerprints by Chromaprint; the work is Macha's (no
stack placement question); segments on their own route per media.

## What the server gives

Data and codes only; each client presents its own skip button.

```
GET /api/v1/catalogue/media/{media_id}/segments
{"status": "ok", "segments": [
  {"kind": "intro",   "start_ms": 61200,   "end_ms": 151800,  "source": "fingerprint"},
  {"kind": "credits", "start_ms": 1262000, "end_ms": 1318400, "source": "chapter"}]}
```

- `kind`: `intro`, `recap`, `credits`, `preview`. `credits` also marks where an "up next" prompt may show.
- `source`: `chapter`, `fingerprint`, `manual`.
- No segments is an empty list, not an error. A media id the node cannot place answers as `/profile` does.
- An API addition: announced to Core and every client before it ships (`CLIENT-CONTRACTS.md`).

## Where segments come from, in precedence order

1. **Manual**, on the manage API (`manager` role): set or clear a media's segments. Overrides everything.
2. **Container chapters.** Matroska and MP4 chapter titles mapped to kinds: intro, opening, OP; recap, previously;
   credits, end credits, ending, ED; preview, next time. Read in the profile probe (`AVFormatContext::chapters`).
3. **Fingerprints shared within a season** (episodes only). Chromaprint over an opening window (the first 10 minutes
   or quarter, whichever is less) and a closing window (the last 6 minutes). Each episode is compared with its
   season's siblings; the longest audio run it shares with at least two siblings, at least 15 s, is its intro (or its
   credits in the closing window). Each episode's fingerprint is an immutable DATA object kept for later matching.
4. **Films**: chapters only at first; a picture and audio credits heuristic is a later option.

## How it fits

- Derived data per media id, like the profile and the keyframe index: segments in a catalogue shard map beside
  `media_profiles`, fingerprints as objects beside `media_indexes`. One definition of a segment, used by every route.
- Background work, paced by the laws: after the media's profile, yielding to viewers and the loader; never a gate.
- Once per media id across the cluster, scheduled as profiles are.
- A season whose episodes change (added, removed, re-matched) has its matches redone; unchanged fingerprints reused.
- Audio only, at a low sample rate, one file at a time; no video decode.

## Phases, a branch each

| Phase | Delivers | Main tests |
|---|---|---|
| 1 | Segment storage, the `/segments` route, chapter segments | title mapping; storage round trip; route answers |
| 2 | Chromaprint (new direct dependency); season matching for intros | alignment on synthetic fingerprints, exhaustive; generated clips sharing an intro |
| 3 | Credits by fingerprint; the "up next" point | as phase 2, at the end |
| 4 | Manual overrides on the manage API | precedence; roles |
| 5 | Film credits heuristic (optional) | after measuring 1 to 4 on the real library |
