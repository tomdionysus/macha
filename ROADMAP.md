# Roadmap

This file contains only work that is not part of the current contract.

## Distributed cache locality

Promote predictive hydration into a cluster-wide source-selection system. Candidate extents should be ranked by predicted delivery time using locality, observed bandwidth/latency and load. Cache-only nodes should be first-class participants without becoming authoritative storage owners.

## Network reachability

A node that accepts no inbound connections is a full participant (`network.inbound_capable`, 0.42.0): it dials its peers and they ask it for a lane over the session it opened. Two such nodes still cannot reach each other directly, and nothing relays between them. Add local UDP server discovery for media clients.

## Skip-intro / skip-credits analysis

Run low-priority, data-local analysis jobs on otherwise idle nodes. Store discovered intro/credit segments as distributed metadata attached to file/season/series identities and combine per-episode evidence into season/series confidence.

## Live/event streaming

Finite media uses VOD semantics. A future genuinely live/event mode needs an explicit playlist/window contract, target latency, reconnect/catch-up behavior and tests against native HLS clients rather than reusing finite-media assumptions.

## Metadata scale

The namespace can be a content-addressed Merkle tree that the metadata record points at (0.49.0-0.50.0, applied per node with `macha-namespace-migrate`), so an ordinary commit rewrites a leaf and the branches above it rather than the whole library. A reconciliation merge still materialises all three branches before re-rooting the result, so it costs what it did before the tree. Checkpoint-rooted journal-range catch-up for a replica that has fallen far behind remains to be built. There are no voters; every node is a replica.
