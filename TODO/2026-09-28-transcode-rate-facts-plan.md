# A node states how fast it transcodes a kind of source

Status: built in 0.70.0 (operator, 2026-09-28: "yes"; persist across
restarts: yes; on-demand measurement: not now). Announced to Core and every
client with the release.

## Why

Measured on fi-1, 2026-09-28 (see the start-progress plan): The Martian 4K
HEVC 10-bit remux transcodes at ~0.33x real time on this node, whatever the
output height (1440p 0.35x, 2160p 0.33x). The software decode is the cost.
A start that fits the budget still drains the buffer seconds into playback.
Nothing tells a client that before it chooses the file, and choosing a file
is the client's decision (capability is the client's problem; the server
reports facts, never picks).

## The fact

What a node has sustained, measured, not a benchmark and not an estimate:
every transform generation already records `produced_ms` and `producing_ms`
with parked intervals excluded (`stream.production`), whose ratio is what the
node could sustain for that generation. Keep those observations, keyed by the
source's decode class, and publish them.

**Decode class** (the inputs that set decode cost, all from the media
profile): video codec, bit depth, and pixel count bucketed by height class
(`<=576`, `<=720`, `<=1080`, `<=1440`, `<=2160`, above). Output height and
encoder are left out on purpose: measured, they barely move the rate for a
decode-bound source. Audio-only transcodes are a separate class keyed by
codec; they are rarely the limit.

**What is kept per node and class**: the last N (16) observations of
`produced_ms / producing_ms` from generations that produced at least a
minimum of media (60 s), each with the node's concurrent transcode count at
the time. Published as the median, the count of observations, and the
concurrency they were taken at. Facts, no smoothing the client cannot see
through.

## Where it is published

Each node's `playback` block in `GET /api/v1/status` (telemetry, so every
node's figures reach every other node and a client can compare nodes before
choosing one):

```json
"transcode_rates": [
  {"codec": "hevc", "bit_depth": 10, "height_class": 2160,
   "rate": 0.33, "observations": 3, "concurrent": 1}
]
```

A class a node has never transcoded is absent: the answer is "not measured",
never a guess. A client matches its candidate file's profile (codec,
bit_depth, height) to a class and reads `rate` per node; below 1.0 means that
node has not kept up with that kind of source.

## Open, for the operator

- Whether to persist the observations across restarts (a small state file) or
  start empty after each restart. Proposed: persist; a restart does not make a
  node faster.
- Whether a node should also *measure* on demand (decode a few seconds of a
  file on request) so a class it has never seen can be answered. Proposed:
  not now; it costs a decode on the node's CPU at the moment a viewer may
  need it, and the observed figures cover the library's common classes
  within days.

## Order

1. Record observations at generation end (and on stop), keyed by class;
   unit tests with a fake engine's production figures.
2. Telemetry field and the Status block; announce the exact shape to Core and
   every client; wait for their checks.
3. Persist, if chosen.
