# Pre-Phase-3 deployment rsync failure observation

Status: failed deployed-build observation; rsync stopped; guarded redeployment requested

Date: 2026-09-02

The operator found the cluster effectively unusable after leaving rsync
running, then stopped rsync. Read-only inspection established that all four
services were still alive on the earlier `0.23.0` deployment and the cluster
had recovered to healthy/writable generation 3331 with 4/4 nodes online. There
was no kernel OOM event in the preceding twelve hours. The failure was resource
and work amplification severe enough to make the cluster appear dead, not a
clean process crash.

Node 50, the FUSE ingress node, retained approximately 1.02 GiB RSS (1.11 GiB
peak) and had used swap on a 4 GiB host. Its 16 GiB spool was still roughly
15.17 GB occupied after input stopped. Status showed 31,161 retained DATA
operations, 30,496 retained publication operations, 128 MiB currently in
flight, 93.9 GB published from 101.9 GB of spool reads, and 20,8772 publication
requests for only 84 started file publications. Remote 4 MiB extent writes
were commonly taking roughly 0.8–2.0 seconds.

The other nodes retained approximately 827–869 MiB RSS, while node 200 reported
approximately 1.48 GB. Logs showed repeated whole-library catalogue scans,
media hints against incomplete files, full metadata fallback/mutation of an
approximately 9 MB snapshot, multi-second to tens-of-seconds metadata work,
and stalled CONTROL/speculative RPCs. ES-1's aggregated peer telemetry also
reproduced the already logged defect where live peers intermittently appear
with metadata generation zero.

This evidence is consistent with the structural remediation plan: bounded
spool bytes did not bound operation/publication ownership, publication caused
large secondary metadata/catalogue work, and independent retained-memory
domains let aggregate RSS grow. The currently deployed binaries predate the
local Phase 2 operation-metadata bound and Phase 3 global ledger/shared-object
work, so this run cannot validate those fixes.

At the operator's request, the next action is a complete-source, parallel
four-node deployment of the locally verified checkpoint, followed by a guarded
rsync UAT. Stop gates remain: rapidly rising RSS, swap growth, loss of API/SSH
responsiveness, non-converging metadata, or viewer/control starvation.

## Guarded redeployment result

The complete source tree was synchronized to all three Linux nodes. The three
Pi builds ran concurrently and produced the identical binary SHA-256
`d8515f7ad728ea8e33d362b9d2e19fc9d0081ff61c60cbe44fcee772670f0ba0`.
The locally tested macOS binary and all three Linux binaries were installed,
then all four nodes were started. Node 200 initially failed because its stale
FUSE cleanup had removed `/Volumes/machamedia`; recreating the configured empty
mountpoint allowed a clean restart. This is operationally noteworthy but was
not the memory failure.

The cluster recovered to healthy, writable, 4/4 online at generation 3337 and
all Status requests returned immediately. The new Phase 2/3 accounting was
active. Node 50 reconstructed roughly 29,000 durable operations with only
16.9 MiB charged operation metadata, no overcommit, and initially restarted at
about 293 MiB RSS rather than the old process's 1.02 GiB. It drained the spool
from 15.17 GB to 12.98 GB and completed two files without new rsync input.

The guarded trend nevertheless failed. During the final 30-second no-input
sample, RSS increased approximately as follows:

- GBNI-1: 517 MiB to 573 MiB (+56 MiB);
- GBNI-2: 247 MiB to 298 MiB (+51 MiB);
- ES-1: 370 MiB to 469 MiB (+99 MiB); and
- node 200: 971 MiB to 1.05 GiB (+84 MiB).

At the same time, the new ledger reported only about 42 MiB live on GBNI-1,
zero on GBNI-2 and node 200, and about 9 MiB on ES-1. There was no ledger
overcommit or admission waiting. This proves the continuing growth belongs to
owners not yet integrated into Phase 3; it is not the already bounded FUSE
operation metadata or shared object/RPC buffers. The repeated approximately
9 MB whole-snapshot metadata mutations, catalogue rescans/hints and
reconciliation visible in the logs are the primary next suspects.

Rsync was **not** restarted. The stop gate was applied and all four Macha
processes were stopped. The Linux units remain in systemd `failed` state solely
because the unsafe processes were deliberately SIGKILLed; `pgrep` confirmed no
remaining process. Node 200 stopped cleanly. Node 50 retains approximately
12.98 GB of durable spool/WAL work for the next controlled run.

## Exact continuation point

1. Leave all services and rsync stopped.
2. Resume Phase 3 at decoded metadata/materialisation ownership. Ownership must
   follow shared snapshots beyond cache eviction and admission must precede
   decode/allocation; post-hoc cache accounting is insufficient.
3. Then integrate catalogue snapshots, media-profile work and remaining
   metadata/reconciliation retry states with the common ledger, with
   reconstructible cache shedding and no blocking on CONTROL/viewer threads.
4. Add a deterministic reproduction in which a durable publication burst
   causes metadata mutation, reconciliation, catalogue discovery and media
   hints. Assert a process-wide retained bound and eventual release across all
   participating nodes.
5. Rerun the complete 275 core and 4 runtime tests. Only then restart the
   services for another no-input backlog-drain UAT. Do not restart rsync until
   that run demonstrates a stable RSS plateau.
