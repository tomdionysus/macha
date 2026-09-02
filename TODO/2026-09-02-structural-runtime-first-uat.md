# Structural runtime remediation — first guarded UAT

Status: **failed safely at the retained-memory stop gate**

Date: 2026-09-02

## Test shape

- Four nodes ran Macha 0.22.3 at metadata generation 3002 before load.
- The three Raspberry Pi binaries were byte-identical:
  `8eeecef1cc1aeb2936d104a3e710c2a7d51e5d282a50acbdc5900df4f5e9d64c`.
- One `rsync --append-verify` wrote through the GBNI-1 FUSE mount while one
  playback session ran from GBNI-1.
- The run used the explicit early-stop rules in
  `2026-09-02-structural-runtime-first-checkpoint.md`.

## What passed

- All four nodes remained online, healthy and writable through generation 3004.
- Control, fast-control and DATA pending-byte queues remained at zero in every
  sample; Status remained responsive.
- Viewer admission was active across the nodes and recorded no viewer waits.
- The pending-data overlay behaved as designed: 30,104 read queries examined
  30,104 ranges and copied 30,104 descriptors. Runtime read work was therefore
  one intersecting descriptor per request rather than proportional to the
  roughly 30,000-entry pending journal history.
- GBNI-1 completed one retained publication, accepted the new input, and had no
  filesystem backend failures before the stop.
- Playback admission used a persisted immutable profile with zero profile
  lookup time. The selected pipeline produced its first fragment in 4,976 ms
  and completed session creation in 6,619 ms while ingest was active.

## Failed memory gate

GBNI-1 RSS rose as follows:

- baseline: 498,794,496 bytes;
- approximately 40 seconds later: 915,275,776 bytes;
- approximately 25 seconds later: 992,722,944 bytes.

The confirming interval was about 185 MiB/minute after an initial increase of
roughly 600 MiB/minute. This exceeded the agreed 128 MiB/minute stop condition
across consecutive samples. CPU on GBNI-1 was approximately 3.2–3.4 cores. The
host reported 145,571,840 bytes of swap in use at the final sample. All Macha
services were stopped before GBNI-1 reached the 1.25 GiB hard ceiling.

The spool grew from 10,259,344,778 to 13,677,612,291 bytes. Publication counters
showed seven publications started, one completed and about 3.42 GiB read. The
overlay counters prove that the remaining RSS growth is not the old whole-journal
read-copy amplification.

One transcode session was active. Its fragment store advanced to 44 ready
segments before shutdown; the configured store has a 64 MiB resident limit and
spills older consumed fragments, so fragment residency alone does not explain
the complete RSS increase. Precise attribution is blocked by the still-open
Phase 0 retained-owner diagnostics.

## Shutdown defect exposed

GBNI-2, ES-1 and the Mac stopped cleanly. GBNI-1 aborted during shutdown with:

> `std::runtime_error: hydration executor is stopping`

The exception escaped after the hydration executor began stopping while
playback/FUSE work was still completing. This is a lifecycle correctness bug:
late submission during cancellation must be rejected or completed as a normal
cancelled result, never escape across a worker/service boundary and abort the
process.

## Required continuation

1. Complete Phase 0 retained-memory ownership diagnostics before another loaded
   UAT. Attribute live and peak bytes to FUSE operation histories, durability
   tickets, publication snapshots/cursors and writers, RPC payloads, playback
   fragment stores, hydration, metadata/catalogue state and reconstructible
   caches.
2. Add a regression for concurrent shutdown and late hydration submission, then
   make shutdown exception-safe and idempotent.
3. Proceed to Phase 2 descriptor charging and Phase 3 process-wide retained
   memory governance using the measured owners. Do not add another unaccounted
   subsystem-local queue or generic concurrency limit.
4. Repeat this same guarded UAT only after deterministic owner bounds pass.

The cluster remains deliberately stopped. Do not leave rsync blocked against
the unmounted FUSE path.
