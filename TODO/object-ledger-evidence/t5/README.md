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
