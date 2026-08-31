# Phase 1D.3 DATA resource headroom UAT

Date: 2026-08-31

Status: objective loaded UAT passed; subjective playback quality awaits the
operator's confirmation. Phase 1D remains open.

## Deployment

- Built the Linux binaries natively and conservatively with `-j2` on nodes 50
  and 51.
- Installed and restarted one node at a time. Both installed binaries had
  SHA-256
  `7e14825908800c242ec2b3772129fd43a68d2273755beb17d9d80f6b5defcdbd`.
- Each node returned to `ready`; the cluster was healthy, writable and 3/3
  before the loaded observation began.
- Node 200 was already exposing the new `diagnostics.data_resources` contract
  during the run and remained connected.

## Starting load

Node 50 recovered five active spool publications with 15,842,003,189 bytes of
the 17,179,869,184-byte spool occupied. There was no live `rsync` process, but
the recovered spool and continuing accepted work supplied sustained loader
pressure.

The first loaded sample showed five active publishers, 40 MiB peak DATA use and
25--40 MiB current DATA use. Publication initially paused at 82,051,072 bytes
read while the spinning storage continued slow physical reads. It subsequently
resumed without intervention.

## Viewer and loader coexistence

- Viewer admissions increased from 0 to 24 on both Linux nodes.
- Viewer waits remained 0 and cancelled waits remained 0.
- On node 50, loader admissions continued from 19 to 1,307 during the same run.
- One publication completed, 696,597,597 bytes were committed, and publication
  yielded 158 times.
- Net spool occupancy fell from 15,842,003,189 to 14,493,977,752 bytes, a
  reduction of 1,348,025,437 bytes, despite occupancy briefly increasing again
  as more input was accepted.

The node-wide admission ceiling was respected:

- node 50 peak: 41,943,040 bytes;
- node 51 peak: 16,777,216 bytes;
- configured capacity: 134,217,728 bytes;
- non-borrowable viewer reserve: 33,554,432 bytes.

The live workload did not fill the 96 MiB lower-class portion, so this run did
not independently force a loader to wait on the new arbiter. The deterministic
RPC/storage saturation regression remains the direct proof that a viewer and
CONTROL request pass when that portion is full.

## Communications and convergence

- All three nodes remained healthy and online with two canonical peer
  connections each.
- All three converged to metadata generation 885.
- Filesystem timed-out requests: 0.
- Observed RPC failures: 0.
- DATA cancellation failures: 0.
- Status and CONTROL sampling remained responsive throughout the loaded work.

## Remaining performance evidence

Node 50 read 4,757,994,589 publication bytes to commit 696,597,597 bytes during
the observation, about 6.8 times as much read work as committed output. The
active recovered handles included old-generation append-tail/materialisation
work, and physical reads on spinning storage were at times slow. This is not a
failure of the new capacity invariant, but it is evidence that the already
tracked overlapping-writer/changed-range reconstruction work remains necessary.

## Verdict

The objective resource and communications gate passed: viewer-class work was
admitted without waiting, loader work retained progress, CONTROL remained
responsive, bounds held, and the cluster converged without timeout or churn.
The operator should confirm whether playback start and seeks were subjectively
acceptable. That confirmation closes this UAT cut; it does not close the
remaining per-device, durability-ticket or read-amplification Phase 1D work.
