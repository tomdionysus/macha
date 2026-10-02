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
