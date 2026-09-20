# Roadmap

This file contains only work that is not part of the current contract.

## Distributed cache locality

Promote predictive hydration into a cluster-wide source-selection system. Candidate extents should be ranked by predicted delivery time using locality, observed bandwidth/latency and load. Cache-only nodes should be first-class participants without becoming authoritative storage owners.

## Network reachability

Support egress-only nodes and cluster-wide connection requests so nodes behind non-negotiable firewalls can participate without requiring every peer to accept inbound connections. Add local UDP server discovery for media clients.

## Skip-intro / skip-credits analysis

Run low-priority, data-local analysis jobs on otherwise idle nodes. Store discovered intro/credit segments as distributed metadata attached to file/season/series identities and combine per-episode evidence into season/series confidence.

## Live/event streaming

Finite media uses VOD semantics. A future genuinely live/event mode needs an explicit playlist/window contract, target latency, reconnect/catch-up behavior and tests against native HLS clients rather than reusing finite-media assumptions.

## Metadata scale

Namespace mutation cost is now the P-1 item in `TODO/ACTIVE.md` with its own plan (a content-addressed Merkle tree over the namespace). Beyond that, prefer structural changes over more buffering: persistent indexed snapshots, finer file-manifest deltas, and checkpoint-rooted journal-range catch-up for a replica that has fallen far behind. There are no voters; every node is a replica.
