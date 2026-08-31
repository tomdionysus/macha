# FUSE publication Phase 4A three-node UAT

Date: 2026-08-31

Result: passed, with an aggregate spool-rate estimator follow-up.

## Workload and topology

- Node `10.44.1.50` ran one large recursive rsync into `/mnt/machamedia/TV`
  and owned the FUSE spool/publication frontend.
- Nodes `10.44.1.51` and `10.44.1.200` were connected storage peers.
- All three nodes reported Macha 0.21.0, healthy membership, writable metadata,
  three nodes online, and the new 8 MiB pipeline diagnostic.
- Node 200 refused SSH but its status API remained directly reachable; this did
  not affect cluster traffic.

## Pipeline and useful throughput

Node 50 reported:

- `data_publication_pipeline_limit_bytes = 8,388,608`;
- `data_publication_peak_pipeline_extents = 2`;
- peak six active publishers and 192 MiB aggregate admitted quantum bytes,
  below the configured 256 MiB bound;
- 9,434,307,426 useful spool bytes read after 395 seconds of process uptime;
- six generations started, two completed and confirmed;
- 943,980,544 bytes committed and confirmed; and
- zero backend failures and zero FUSE request timeouts.

A clean early interval advanced from 2,810,445,824 to 5,223,743,488 useful
bytes in 57.311 seconds: 42.11 MB/s, or 40.16 MiB/s. Across the complete
observed interval, including the deliberately sustained viewer read and its
quiet windows, useful publication advanced by 6,623,861,602 bytes in 324.7
seconds: 20.4 MB/s, or 19.5 MiB/s. The previous Phase 1 UAT measured about
7.72 MiB/s, so this checkpoint demonstrates a material improvement rather than
mere additional queued work.

The two completions included one approximately 899 MiB generation and a small
follow-on generation. Large open files continued making exact quantum progress;
no prefix reread amplification appeared.

## Viewer law

A short cached probe could not prove adapter signalling, so the conclusive test
used a sustained 256 MiB direct FUSE read at an uncached offset. While that read
was active:

- `data_publication_bytes_read` stayed exactly 8,714,404,341;
- `data_publication_quanta` stayed exactly 269;
- no new loader work was admitted across repeated samples;
- control queue maximum remained 199 microseconds; and
- timeouts remained zero.

A separate 4 MiB direct read at another uncached offset completed in 0.37
seconds including SSH setup. After viewer activity ceased, the five-second quiet
window expired and publication resumed. This satisfies “Thou Shalt Not Make The
Viewer Wait” for this phase.

## Communications and resource behaviour

- Node 50 CONTROL queue wait maximum was 199 microseconds.
- Node 51 loader object puts reached 434 ms and its durability barrier reached
  684 ms, but loader queue wait maximum was only 111 microseconds. Ping queue
  maximum was 142 microseconds.
- Node 200 ping/control queue maximum was 282 microseconds.
- No RPC/FUSE timeout or backend failure was reported.
- CPU moved with useful work rather than remaining at three saturated cores.
  Point samples were usually single-digit process percentages, with transient
  loaded samples around 51% on node 50 and 53% on node 51.
- Final RSS observations were approximately 674 MB, 367 MB and 270 MB. This was
  a loaded checkpoint, not a post-drain RSS soak.

## Follow-up discovered

Spool occupancy rose from 10.22 GB to 13.67 GB of the 17.18 GB limit while the
rsync continued. `spool_publish_rate_bytes_per_second` settled at 5.95 MB/s even
though aggregate useful publication was materially higher. The estimator is
updated by completed individual generations and therefore describes roughly one
publisher's completion rate, not concurrent aggregate drain. At high occupancy
this can pace rsync below available aggregate publish capacity.

The aggregate retirement estimator is now implemented and locally verified in
[the correction checkpoint](2026-08-31-spool-aggregate-retirement-rate.md).
Its short deployed UAT remains before the broader Phase 5 admission work.

## Decision

Phase 4A is accepted. Continue with the aggregate rate-estimator correction and
then the shared byte-bounded data executor/per-peer pipeline. A later mixed-size
UAT should verify completion cadence as well as useful-byte throughput.
