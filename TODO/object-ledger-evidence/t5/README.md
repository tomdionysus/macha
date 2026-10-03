# T5: component conversions

## T5.1: the node's composition root as code (2026-10-02)

`NodeServices` (`src/service/node_services.*`) holds every service built
after local state recovers, as members in dependency order with
constructor references; the run-time root from T2 is removed (decision log,
2026-10-02). Lifecycle recordings rewritten and reviewed. Suites 701/701,
17/17; traces unchanged.

## 0.75.0 on the cluster (2026-10-02)

The first deploy of experiment code since T0 (`3e6d740`; tarball md5
`8269255302fa73e8c158c847a06fa249`, built on fi-1: no warnings, suites
700/701 then the remaining case fixed as a test race, 17/17, 21/21).
Deletion paused: both nodes already run `garbage_grace_ms: 2592000000`
(30 days). Backups `/root/macha-0.74.0-installed.tgz` on both.

- fi-1 installed 11:38:10Z: the 0.74.0 process stopped in ~4 s; services
  ready 14 s after start. Mixed versions (fi-1 0.75.0, gbni-1 0.74.0) both
  online.
- gbni-1 installed 11:46:01Z, staggered for a failover check (operator):
  stopped in ~4 s, services ready 47 s after start (its usual 40-43 s);
  heap-check drop-in kept. **A viewer on the TV saw one 0.5 s stutter
  across the restarts** (operator).
- **The shutdown fix on hardware**: fi-1 restarted at 11:55:10Z with a
  256 MiB FUSE write at its fsync -- the reproduction that was SIGKILLed at
  60 s on 0.74.0 (2026-10-01) -- stopped in 3 s (`Service::stop` 11:55:10 to
  11:55:13), services ready at 11:55:24; recovery replayed 65 journalled
  FUSE operations.

## 0.76.0 smoke test on fi-1 (2026-10-02)

T5.2 to T5.9 (the node's parts moved into the root, mid-restructure), on
fi-1 only, beside gbni-1 on 0.75.0. Suites on fi-1 before install: 703 +
17 + 21, all passing (`3f15aaa`). Tarball md5 `9d02d4d62ef5836317cfb4cce94b4014`;
backup `/root/macha-0.75.0-installed.tgz`. The inter-site link ran at
400-800 ms round trip throughout (no loss); fi-1 was reset by the operator
at ~16:21Z for reasons outside Macha, before the install.

- Installed 16:31:56Z: 0.75.0 stopped in 3 s; 0.76.0 services ready 12 s
  after start; metadata writable at once. Status on both nodes: every
  startup plane ready, both nodes ready, FUSE and torrent subsystems
  running with no restarts; diagnostics answering.
- Reads through fi-1's FUSE mount behave as 0.75.0's on gbni-1: of five
  random films, the same three fail with EIO on both nodes (their extents
  are on neither), and the two that read are local on gbni-1 (0.1 s) and
  fetched over the slow link on fi-1 (20-26 s for 16 MiB).
- Restart 16:49:47Z with the operator watching a film served by fi-1
  (direct play): stopped in under 4 s, healthy at 16:50:02, the session
  back on fi-1 by 16:51:05. **Playback was fine** (operator); the client
  stayed with fi-1 rather than switching to gbni-1.

0.75.0 soak on fi-1 before the install: `fi-1-0.75.0-observation.txt`
(3.3 h window; too short for the threshold comparison).

Repair on fi-1 (0.75.0, 15:47-16:08Z): unsourceable objects rising with
the pull walk (5,323 to 5,662), reachable at a steady 3.04x unsourceable;
the walk had covered ~2.6% of the id space (samples in build/obs/).

### fi-1's disk lost and recovered (0.76.0), 17:38-17:56Z

fi-1's USB data disk dropped at 17:38:03Z (kernel: USB disconnect, ext4
journal aborted, filesystem shut down); Macha marked the backend offline
one second later and kept serving. After the operator's power cycles the
disk was absent at first (Macha up degraded in 16 s, metadata writable),
then present: systemd mounted it at 17:55:47Z and Macha brought the
backend online by itself at 17:56:37Z (probe 48.9 s, accounting
reconciling). No hand on Macha.

Found on the way: while the disk was absent (since ~16:21Z, the earlier
reset), torrent staging wrote 671 MB into `/mnt/diskB/ingest/torrents` on
the SD card, now hidden under the mount. Staging has no equivalent of the
backend's marker check. Product gap, left for after T5; the hidden files
are untouched.

## 0.77.0 smoke test on fi-1 (2026-10-02)

T5.10 (MetadataServer) and T5.11 (StorageServer). Suites on fi-1 before
install: 703 + 17 + 21, all passing (`6901443`). Tarball md5
`a0ef102d7e1a5e9e9b6dc40de9335ea5`; backup `/root/macha-0.76.0-installed.tgz`.

- Installed 18:19:21Z with one playback session on fi-1: 0.76.0 stopped in
  2 s; DATA backend online 18:19:30 (350 GB used of 8 TB); metadata ready,
  services ready and writable 18:19:36 (12 s); healthy 18:19:39.
- Status on both nodes: every plane ready, both nodes ready, subsystems
  running with no restarts, diagnostics answering. gbni-1 (0.75.0) logged
  no "not available" refusals and no warnings or errors after the install.
- Reads through fi-1's FUSE: 16 MiB in 3.8 s and 6.0 s for the two
  readable films sampled earlier.

## 0.78.0 smoke test on fi-1 (2026-10-02, late)

T5.12-T5.14: LocalState recovered in one construction, consumers taking it
directly, and the root owning it (LocalServices: local state, then the
metadata and storage servers). Suites on fi-1 before install: 703 + 17 +
21, all passing (`8a316b4`). Tarball md5 `927dc7fb92fdd3adc08aeb7c9af3d321`;
backup `/root/macha-0.77.0-installed.tgz`.

- Installed 22:42:15Z with one playback session on fi-1: 0.77.0 stopped in
  2 s; listening 22:42:18; DATA backend online 22:42:20 (355 GB used);
  metadata ready, local services built and services ready 22:42:30 (12 s);
  writable 22:42:31; healthy 22:42:33.
- Status on both nodes: every startup plane ready (the root's recovery
  record reads as the node's bits did), both nodes ready, subsystems
  running with no restarts, diagnostics answering. gbni-1 (0.75.0) logged
  no "not available" refusals and no warnings or errors after the install.
- Reads through fi-1's FUSE: 16 MiB in 4.3 s and 8.6 s for the two
  readable films sampled earlier.
- fi-1's data disk had one USB reset with a single failed read at
  21:54:06Z (0.77.0); the device recovered at once and the backend stayed
  online.

## 0.79.0 smoke test on fi-1 (2026-10-02, late)

T5.15: Accounts (user table, sessions, their gossip) as its own part.
Suites on fi-1 before install: 703 + 17 + 21 (`7d9ef27`). Tarball md5
`2d1cc8a37948760007bb469a4cda37ec`; backup `/root/macha-0.78.0-installed.tgz`.

- Installed 23:10:13Z: 0.78.0 stopped in 5 s (2-3 s at the previous
  installs; within the bound); listening 23:10:19; DATA online 23:10:23;
  services ready and writable 23:10:31; healthy 23:10:34.
- A session created on fi-1 (0.79.0) was honoured by gbni-1 (0.75.0) at
  once (HTTP 200); revoking it on fi-1 had gbni-1 refuse it within 6 s
  (401). Status normal on both nodes.

## 0.79.0 on gbni-1 (2026-10-03)

Same tarball as fi-1's 0.79.0 (installed library md5 identical); backup
`/root/macha-0.75.0-installed.tgz`. Installed 08:25Z, healthy at once,
writable at generation 65164; no warnings or errors on either node after.

## 0.80.0 on both nodes (2026-10-03)

T5.16-T5.18: the filesystem without the node, the replica built with its
namespace applier, the contract audit and its fixes. Suites on fi-1 before
install: 704 + 17 + 21 (`3cbe49c`). Tarball md5
`f5228be744aea6dedd9b9789de802da2`; backups `/root/macha-0.79.0-installed.tgz`
on both.

- fi-1 09:44:09Z, gbni-1 09:52:39Z (eight minutes apart, no playback on
  either): healthy and writable at generation 65164; every startup plane
  ready, both nodes ready, plugins running with no restarts; no warnings
  or errors on either beyond fi-1's standing edge-node note and the known
  torrent alert overflow.
- Presence warm-up: control stores in 61 ms (fi-1) and 191 ms (gbni-1);
  fi-1's DATA store (99,694 objects) in 28.6 s.
- A read through fi-1's FUSE of Men In Black 1: 16 MiB from 200 MiB in
  18.4 s while the DATA warm-up was running (a DATA pressure onset at
  09:44:27Z), the same range again at 299 MB/s, then EIO at 216 MiB.
  gbni-1, still on 0.79.0, returned nothing at 216 MiB and 4 of 8 MiB at
  100 MiB: the missing extents already recorded, not this release.

## 0.81.0 against the K table (2026-10-03, 12:34Z to 18:34Z)

T0's six-hour top-up load re-run on both nodes at 0.81.0 (driven remux
and transcode sessions, a 32 MiB FUSE write per cycle, the K6 routes
polled, three restarts per node), compared with the same six hours of T0
(0.74.0, 2026-10-01 14:32Z to 20:43Z) re-reported from T0's logs. Reports
and the comparison are in `k-0.81.0/`. The whole-run baseline in
`t0/baseline.md` is mostly the 31-hour soak, so the top-up window is the
like-for-like figure.

Differences in the runs: today's six restarts fell in the first 80
minutes (T0's were spread); both load drivers started in the same second.

| criterion | fi-1 | gbni-1 |
|---|---|---|
| K1 claim walk p50/p99 (us) | 3/29 to 1/14 | 4/87 to 2/15,359 |
| K2 GC per object p50/p99 (ns) | 2,815/28,671 to 2,303/21,125 | 4,095/851,967 to 4,607/91,859 |
| K3 claim barrier p50/p99 (us) | 4,607/840,588 to 4,607/24,575 | 360,447/2,097,151 to 425,983/2,883,583 |
| K4 repair bytes per minute | 3.19 MB to 0.16 MB | 1.79 MB to 0.76 MB |
| K4 push examined per minute | 13.6 to 2.31 | 1.35 to 0.33 |
| K4 loaded repair step p50 | 0.33 s to 7.3 s | 0.04 s to 0.33 s |
| K6 `status` p50/p99 (us) | 639/3,839 to 639/3,327 | 959/12,287 to 895/6,655 |
| K6 `torrents/jobs` p50/p99 | 767/6,655 to 3,583/20,479 | 7,679/81,919 to 575/5,119 |
| K6 `catalogue/items` p50/p99 | 212,991/9,437,183 to 229,375/1,310,719 | 589,823/2,097,151 to 491,519/2,097,151 |
| K6 `catalogue/status` p50/p99 | 12,287/45,055 to 7,167/32,767 | 36,863/5,767,167 to 13,311/720,895 |
| K7 `first_fragment` p50/p99 (us) | 8.4 s/15.0 s to 6.3 s/14.7 s | 0.59 s/14.7 s to 0.46 s/14.7 s |
| K7 `create` p50/p99 | 9.4 s/23.1 s to 10.5 s/31.5 s | 0.59 s/4.7 s to 0.52 s/6.3 s |
| K8 FUSE publication per minute | 9.81 MB to 8.51 MB | 11.61 MB to 10.25 MB |
| K9 RSS median/max | 1,379/1,970 MB to 1,463/2,264 MB | (T0 not in window) to 1,196/1,913 MB |
| K10 shutdown | 8.2, 7.5, 5.0 s, one killed to two killed, 20.5 s | 2.2, 5.9 s, one killed to 50.1, 1.4, 11.0 s |

Read against the kill criteria:

- **Same or better**: K2, K3 (fi-1 much better at p99), K6 except one
  route, K7 first fragment, health and status latency.
- **Worse, unexplained**: K4. Repair moved a twentieth of the bytes on
  fi-1 and under half on gbni-1, and a loaded repair step takes about
  twenty and eight times as long. Repair's own code is unchanged since
  T0; what it runs beside (the ledger, the restructure, the lock
  wrappers) is not. fi-1's 0.75.0 observation had the loaded step at
  0.49 s, so the change is between 0.75.0 and 0.81.0, or in the load.
  gbni-1's claim-walk p99 (15 ms) and fi-1's `torrents/jobs` are also
  outside T0's hourly spread.
- **Worse, a known mechanism**: K8, about 12% lower on both. Each node
  held a background DATA slot across its put to the other, so with both
  writing all four slots waited on each other until the 120-second
  no-progress abandon ("DATA credit wait abandoned", 13 times on fi-1,
  18 on gbni-1; one extent put took 232 s). In T0's code too, where the
  top-up log shows the same long cycles (fi-1 8 of 112 over 300 s, gbni-1
  5 of 132; today 3 of 37 and 2 of 46), but the journals no longer cover
  T0 to confirm the warning. Fixed in 0.82.0, with a test that hangs on
  the old code.
- **Worse, resilience**:
  - **Shutdown**: fi-1 was killed at systemd's 60 s on two of three
    restarts and took 20.5 s on the third; gbni-1 took 50.1 s once. Each
    stalls after "FUSE main loop returned cleanly" and before outbound
    calls are cancelled, that is, inside the services' stop, with a
    top-up write in flight each time. T0 had two of six killed (the fsync
    wait fixed on 2026-10-02 stalled earlier, before the main loop
    returned); 0.76.0 to 0.79.0 stopped in 2 to 5 s without this load. Not
    yet diagnosed: no stacks were taken.
  - **A crash**: gbni-1 died of SIGSEGV at 17:21:22Z inside `malloc`
    (glibc's heap check), on the catalogue scanner's thread saving hint
    state, twenty seconds after a transcode pipeline started. The heap
    was corrupted by something else; the core does not say what. gbni-1
    runs the heap check because of the unexplained corruption of
    2026-09-28, whose core was on the media-information thread. Core:
    `/mnt/diskB/crash/core.macha-catalogue.695744.1791048075` on gbni-1.
    Restarted by systemd; services ready 46 s later.
- fi-1's playback errors (seek 503 after about 15 s, `media input read
  failed`) are the missing extents, as at T0 (161 of 580 seeks then, a
  similar share now).
