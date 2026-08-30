# Status disk-usage availability correction

Date: 2026-08-30

## Live reproduction

Three sequential rounds were captured from the deployed local, node-50, and
node-51 Status endpoints while all three nodes were online and healthy at
metadata generation 1439.

Every endpoint reported its own storage usage plausibly with
`telemetry_freshness: live`. The same endpoint reported remote cards as
`telemetry_freshness: unavailable`, but still emitted numeric fields such as:

- `used_bytes: 0`;
- `free_bytes: capacity_bytes`;
- `storage_backends_online: 0`;
- zero-valued cache capacity/usage.

The result was deterministic in the raw API. A client viewing different node
endpoints could therefore alternate between a plausible self measurement and
an apparently empty remote disk. The client was not inventing the zero; the API
made missing optional telemetry numerically indistinguishable from a genuine
zero measurement.

## Cause

Status correctly constructs the cluster roster from authoritative membership,
then enriches it with optional best-effort telemetry. The serializer did not
preserve that distinction for storage/cache numbers: when telemetry was absent,
it serialized default or membership values through the same numeric byte-pair
shape used for a coherent telemetry observation. Cluster aggregates likewise
summed missing measurements as zeros.

Membership is sufficient to keep a connected node visible, online, and eligible
for metadata durability. It is not sufficient to fabricate a coherent current
used/free/cache measurement when optional telemetry is absent.

## Correction

- Storage and cache byte objects now include `available`.
- Without current, stale, or persisted telemetry, per-node `used_bytes` and
  `free_bytes` are `null` and `available` is false.
- Membership-known storage capacity remains visible; unknown cache capacity is
  `null`.
- `storage_backends_online` is `null` without telemetry.
- Aggregate storage/cache usage is unavailable if any included node lacks a
  measurement, instead of silently treating that node as empty.
- A coherent measurement containing a real zero remains numeric with
  `available: true`.
- Persisted telemetry is no longer overwritten by less coherent membership
  storage numbers while cards are assembled. Endpoint, failure-domain,
  liveness, metadata-generation, and role authority remain membership-owned.

No polling, sampling, gossip expansion, metadata mutation, or durability work
was added.

## Tests

`invariants/test_status_uses_membership_without_telemetry` now proves both sides
of the contract in one deterministic test:

1. an online peer without telemetry remains present and online, but storage
   usage/free, cache measurements, and online-backend count are unavailable;
2. after a coherent peer telemetry observation reports a genuinely empty disk,
   zero usage/free-space arithmetic is numeric and explicitly available.

Verification completed before the repository-wide run:

- focused Status regression: 1/1 passed;
- complete `invariants` group: 35/35 passed;
- FUSE/convergence Status integration: 1/1 passed.

Repository-wide verification then passed:

- complete default suite: 204/204;
- runtime dependency suite: 3/3;
- `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc` passed
  explicitly within the complete run in 6.650 seconds.

## Deployment/UAT boundary

After deployment, query all three Status endpoints sequentially. Each endpoint
should continue to show its self card as live. A remote card with unavailable
telemetry must show null usage rather than zero; if gossip supplies telemetry,
the same card may become live/stale/last-known with numeric values. A genuine
measured zero must remain numeric.
