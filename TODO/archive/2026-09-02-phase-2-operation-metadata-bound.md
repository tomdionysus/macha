# Phase 2 operation-metadata bound

Status: locally complete; not deployed

Date: 2026-09-02

The durable FUSE spool already bounded payload bytes, but its in-memory
`DataOp` history, per-chunk SHA-256 arrays and a simultaneous publication
snapshot could grow independently. Tiny writes and truncates could therefore
consume a large heap beneath a mostly empty spool.

Each new durable operation now reserves a conservative retained-heap charge
before it is accepted. The charge covers vector allocation slack in both the
authoritative history and one publication snapshot. Ownership transfers to the
inode when the operation becomes visible and is released exactly when journaled
publication/abandonment retires it or the inode is reclaimed.

`fuse.max_operation_metadata_bytes` defaults to 64 MiB. At the bound, admission
wakes publication and waits on durability or retirement events. It does not
poll, exceed the bound, return a fabricated disk-full error, or discard
acknowledged work. Journal recovery reconstructs the same ownership charge; if
an operator lowers the limit below recovered history, recovery remains valid
and new work waits while the backlog drains.

Status exposes current, peak, limit and wait counters. Two deterministic tests
prove hard saturation/cancellation and successful event-driven wakeup after
retirement. The complete `filesystem_fuse` suite passed 64/64.

This closes Phase 2. Loaded UAT remains deferred until Phase 3 supplies the
process-wide retained-memory ceiling.
