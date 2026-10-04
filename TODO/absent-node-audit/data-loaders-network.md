# Audit: data path, loaders, startup, network vs. "any node may disappear, forever"

Condensed record of the second audit pass (2026-10-04). The full report was delivered in
conversation and not saved; this file holds its findings without the line-by-line citations.
Anything marked inferred was read from code and not reproduced.

## Data path

- At `min_write_replicas` 1 on a hosting node, an absent peer costs a write nothing.
- `put_impl` returns false at once when the active hosting nodes are fewer than the floor, or when
  the last candidate peer fails.
- A stalled peer (connected, not answering) holds the write for the full 30 s budget.
- `retain_data` re-reads and re-puts an object found short. If the only copies are on an absent
  node it fails, and the commit that wanted the claim fails with it. Appends re-claim every extent
  of the file, so appending to a file whose older extents live only on an absent node fails.

## Loaders

- FUSE file publication: retried with backoff, parked after `max_failing_duration` (1 h default),
  and never unparked except by an operator.
- FUSE namespace queue: head-of-line; a non-retryable operation needs an operator skip.
- `fsync` has no deadline.
- Journal `durability_poisoned` is never reset.
- Ingest: dies on a DATA floor failure (`import_failed`); parks on a metadata floor failure and
  retries after `blocked_retry`.
- Torrent publisher retries every 30 s forever.
- Torrent claims for cancelled or pinned jobs of a lost node are stuck; a pin to an absent node
  needs an operator.
- Catalogue artwork fetched while the catalogue is unwritable is dropped.

## Network

- No per-peer down state. A known node is dialled (2.5 s) on every backoff expiry, forever.
- A dead route takes 30-35 s to close.
- One health thread serves every peer.

## Incidental

- `torrent_coordinator.cpp:223-224` calls `std::any_of(node_.membership().active().begin(),
  node_.membership().active().end(), ...)` on two temporaries: undefined behaviour.
