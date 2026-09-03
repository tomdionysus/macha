# Terminal FUSE recovery publication loop

Date: 2026-09-03
Status: implemented, fully tested, deployed, and live reproduction passed
Version: 0.23.1

## Root cause

GBNI-1 mounted FUSE after cold metadata recovery, then restored 64,792 durable
operations across 29 inodes. Inode 922 retained a 183,038,542-byte spool but
its published path was absent from the accepted namespace. Its first replay
therefore failed terminally with `ENOENT` (`missing`).

The data worker correctly stored the terminal error on the inode, but its common
post-run path then marked the generation deferred. `admit_deferred()` did not
exclude poisoned inodes, so it immediately requeued the same impossible work.
Each subsequent attempt failed before doing useful work. The old process emitted
roughly 400 failure lines per second, consumed about 80–91% CPU, retained more
than 700 MiB RSS, and made the otherwise attached mount/API appear unavailable.

## Correction

- `request_data_publication()` now rejects an inode with a terminal backend error.
- `admit_deferred()` clears and excludes poisoned deferred inodes.
- The worker post-run transition explicitly leaves terminal failures
  non-runnable while preserving the inode error, operation journal and spool for
  explicit reconciliation. Retryable failures retain their previous cursor and
  retry behaviour.

The correction deliberately does not silently discard the 183 MB spool or
pretend the affected path published successfully.

## Regression

`test_fuse_terminal_recovery_failure_is_not_readmitted` creates a published
file, leaves a durable spooled write across frontend shutdown, removes the file
from the accepted backend namespace, and restarts FUSE. It proves:

- exactly one terminal publication attempt;
- no pending or active scheduler work afterward;
- no later increase in the failure count; and
- continued access to an unrelated file.

Before the fix the test failed because the error count grew and a worker stayed
active. After the fix:

- focused regression: 1/1 passed;
- filesystem/FUSE suite: 65/65 passed;
- complete core suite: 277/277 passed; and
- runtime dependency suite: 4/4 passed.

## Deployment proof

The complete source tree was synchronised to GBNI-1, GBNI-2 and ES-1 and built
natively in parallel. All three installed Linux binaries had SHA-256
`ed62f1257849689703713d02ae955406d2a2b58c4713a3e357af07a8698e830c`.
Node 200 was rebuilt and installed natively on macOS. All four reported version
0.23.1.

On GBNI-1 the same durable state reproduced `inode=922 error=missing` exactly
once at 22:27:17. A later bounded observation remained at one failure. The FUSE
mount answered `readdir`, the local Status request returned HTTP 200 in under
one millisecond, and no log storm recurred.

The unrelated 19 GB metadata-history startup cost and recovery RSS remain in
the structural P0 programme.

## Status truthfulness evidence

The rollout independently reproduced the active Status TODO. While node 200 was
recovering with metadata unavailable and generation 0, its per-node entry still
said `state: online`. Afterward, node 200's aggregate reported ES-1 green/online
at generation 0 while ES-1's own endpoint reported healthy, writable generation
4507. Cached reachability and telemetry are therefore being presented as live
readiness/current measurement somewhere in the server/client status contract.
