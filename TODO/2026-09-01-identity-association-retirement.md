# Identity association retirement

Date: 2026-09-01  
Release: 0.22.2

## Live diagnosis

ES-1 was healthy as node `333c0e3f20fa0d19261a23172fb7502d` while the
obsolete identity `85ff11c9606a3bb6f028b2cd75e40bb0` remained in durable
status at the same `10.34.1.50:7437` endpoint. The reset tombstone reached the
cluster, but Status continued presenting and counting the obsolete identity as
offline.

On GBNI-1 the reset was operationally applied at 16:02:38. The request did not
complete until 16:03:13 because it synchronously waited 34,706 ms for a 5.77 MB
metadata snapshot mutation. Returning 200 concealed both the already-effective
local recovery action and the unrelated audit latency.

## Corrected contract

The request path now:

1. validates and resolves the reset scope;
2. advances its locally known epoch;
3. applies and durably persists the small local membership tombstone; and
4. returns `202 Accepted` with `audit_state: "queued"`.

A coalescing worker performs peer propagation and the cluster-metadata audit.
Neither network delay nor metadata publication can delay the HTTP response.

Ordinary Status results omit identities whose last observation predates their
matching tombstone. They do not affect node, metadata-replica, health, storage
or cache totals. Direct node detail remains available and reports
`state: "retired"`; a directly authenticated post-reset observation remains
live because the tombstone is a freshness boundary rather than a blacklist.

## Verification

- `test_manage_identity_reset_does_not_wait_for_metadata_audit` blocks the
  metadata mutation owner and requires reset admission in under 500 ms.
- Existing endpoint-, host-wide-, unavailable-metadata and stale-gossip reset
  tests continue to pass with the asynchronous `202` contract.
- `test_status_excludes_retired_identity_from_live_cluster_health` proves the
  normal list and health totals omit the retired identity while its detail
  endpoint remains auditable.
- Focused invariant suite: 39/39 passed.
- Authoritative serial core suite: 252/252 passed.
- Runtime dependency suite: 3/3 passed.

## Live UAT

- Complete source was deployed and version 0.22.2 activated on all four nodes.
- GBNI-1 and GBNI-2 immediately omitted obsolete identity `85ff…` from the
  ordinary status response. They reported four known identities rather than
  five; current ES-1 identity `333c…` remained distinct.
- An explicit repeat reset of the retired endpoint returned `202 Accepted` in
  2.568 ms with `audit_state: "queued"`.
- Its asynchronous metadata mutation completed 312 ms later at generation
  2234. Request latency was therefore independent of audit latency.
- ES-1 exposed a separate hardware/filesystem failure during deployment:
  `/dev/sdb1` repeatedly returned ext4 `EIO` for directory block reads and the
  mount entered `shutdown` state. Macha was stopped after systemd restart 26 to
  avoid repeatedly probing the failed filesystem. On the operator-requested
  later retry the disk responded, Macha started with zero restarts, and all four
  nodes converged healthy and writable at generation 2234. The transient disk
  fault remains operationally significant but is unrelated to this change.
