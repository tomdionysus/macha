# Pi baselines at 0.93.4 (2026-11-06)

What the port is compared against. Taken 16:17Z to 18:15Z on the live nodes, version 0.93.4, one
node at a time, by `build/baselines/baseline.py` and `partition.py`; raw JSON per run, with thread CPU,
frame timings and diagnostics before and after, in `build/baselines/results/`. Medians, with the range
and run count. Both Pis read `throttled=0x50000`.

**Read these first:**

- **inverbeg was still filling** throughout, about 1.3 MB/s in and 0.5 MB/s onto disk, at 1.4 TB
  against macnessa's 5.8 TB; the operator chose not to wait. Its figures carry that load, and repair's
  CPU is why its idle CPU is above macnessa's. A later comparison must state its own replication state.
- **macnessa crashed four times from 17:18Z to 18:11Z**, heap corruption (`ACTIVE.md`): twice in the
  small-file phase, once in the delete after it, once in the catalogue scanner with no baseline
  writing. Its write figures are from two passes, and small files reached 3 of 10 runs. **Every macnessa
  figure comes from a process with a damaged heap or close to one**, so they may not be what a healthy
  0.93.4 gives. inverbeg did not crash.
- **The loader here is a mount writer**, not an ingest job. No viewer was playing beyond these tests.

## Figures

| Measure | inverbeg | macnessa |
|---|---|---|
| Idle CPU, % of one core | 22.5 (17.4-32.6) n=5 | 9.6 (8.6-9.7) n=5 |
| Idle RSS, MB; threads | 1442; 137 | 1329; 128 |
| Mount read, one reader, MB/s | 8.8 (4.6-8.9) n=5 | 74.3 (65.5-94.3) n=5; 71.5 (66.5-82.6) n=5 |
| First byte of that read, ms | 624 (22-726) | 46 (46-60); 51 (39-74) |
| Mount read, four readers, MB/s total | 10.9 (10.2-20.8) n=5 | 81.1 (77.7-86.3) n=4; 78.9 (74.4-88.4) n=5 |
| CPU per GB read, one reader, s | 18.8 (17.7-86.5) | 6.2 (6.2-6.3); 18.9 (9.0-21.7) |
| Peer round trip during reads, ms | 89 | 34 to 46; up to 156 in one pass |
| Mount write, 512 MiB file with fsync, MB/s | 73.8 (55.8-76.3) n=5 | 33.4 (32.4-34.7) n=5; 29.2 (28.6-30.6) n=5 |
| 200 files of 64 KiB, each fsynced, files/s | 1.6 (1.3-1.6) n=5 | 1.3, 1.3, 1.2 (n=3, then crashed) |
| Listing `/Movies`, 1918 entries, ms | 32 (31-35) n=5 | 31 (30-37) n=5 |
| Walk of `/TV`, 7346 entries, entries/s | 17,790 (16,716-20,774) | 17,820 (17,142-20,528) |
| Commit (API mkdir), idle, median of 20, ms | 34.8 (34.4-35.8) n=5 | 38.0 (37.5-41.5) n=5 |
| The same, worst of 20, ms | 115 (38-182) | 65 (43-1265) |
| Commit under a saturating mount writer, ms | 36.2 (35.3-38.3) n=5 | 38.8 (36.1-43.4) n=5 |
| The same, worst of 20, ms | 43 (39-229) | 55 (39-163) |
| Cold start to healthy, s | 40.6 (39.6-44.1) n=5 | 68.7 (62.2-69.7) n=5 |
| Cold start to mount answering, s | 45.4 (44.3-48.9) | 73.2 (67.0-76.5) |
| Peak RSS 60 s after start, MB | 1083 (1068-1113) | 1288 (1124-1704) |
| Stop, s | 2.0 (1.9-2.2) | 1.9 (1.7-7.3) |

Viewer, on the node asked, through the API:

| Measure | inverbeg | macnessa |
|---|---|---|
| Direct: create session, ms | 1736 (533-2981) | 168 (53-267) |
| Direct: first byte, ms | 14 (13-15) | 12 (11-13) |
| Direct: first 8 MiB, MB/s | 15.4 (8.0-216) | 108 (98-218) |
| Direct: first byte after a seek, ms, n=25 | 535 (35-2011) | 46 (32-125) |
| Direct, writer running: create; seek, ms | 1344 (110-2953); 458 (15-1693) | 145 (49-236); 42 (12-107) |
| Remux: create session, ms | 2112 (686-2903) | 397 (106-411) |
| Remux: first segment after create, ms | 7.5 (6.7-8.6) | 8.3 (7.5-9.6) |
| Remux, writer running: create, ms | 1983 (372-3999) | 181 (105-447) |

Remux refusals: `playback_pipeline_start_failed` ("timed out waiting for first fragmented-MP4
segment") 3 of 13 attempts on inverbeg, 1 of 13 on macnessa, not looked into. `copy_not_supported` and
`choice_required` are the test's requests, not failures.

Partition: inverbeg's cluster port dropped both ways for 120 s, 3 commits on each side, 5 runs. Every
commit answered, slowest 0.8 s. Converged 4.4 s (2.9-4.6) after healing; merge `ms=97` in the one line
read. Both nodes still counted 2 online 20 s in, so the rejoin figure says little.

## Macha's own large publication test on the USB disk

`storage_v18/test_retain_data_batches_a_large_publication_within_bounded_time`, run with its temporary
directory on macnessa's `/mnt/diskB`, **timed out at its 120 s bound in all 4 runs read** (a fifth ran,
result not read). No figure comes from it, only "over 120 s". The binary is
`/home/tom/macha/build/macha-tests` as last built there; its version was not confirmed. The suites
are normally run with the temporary directory on the node's root disk, so this does not contradict the
gate.

## Not measured

- **Not available without new code:** direct store put and get on the USB disk, loose and packed; claim
  journal latency by itself. Dagda's own figures for these have no Macha counterpart yet.
- **Not reached today:** GC, scrub and compaction rates: those journal lines are DEBUG and the nodes do
  not log DEBUG, and a scrub is due monthly. `has` and open time at 1.03M objects: only the journal's
  `storage backend online elapsed_ms` (16,861 on macnessa at 15:51Z).
- **One machine's DATA path by itself:** reads on inverbeg are the only inter-node figure, and they mix
  held and fetched extents; `net_rx_mb` per run says how much came over the link.
- **Control I/O under a saturating loader** is shown only as commit latency above.
- **Comparison rule:** unset. Dannan proposes the spread alone, Dagda the spread or 5 percent.
