# Cluster Status telemetry aggregation UAT

Date: 2026-08-30

## Scope

Verify the deployed aggregation correction from every canonical route
perspective while preserving the client contract: any single Status endpoint
must contain the best locally known status of all connected nodes.

## Method

Read-only requests were made directly to:

- `10.44.1.200:7438/api/v1/status`;
- `10.44.1.50:7438/api/v1/status`; and
- `10.44.1.51:7438/api/v1/status`.

No peer fan-out was performed by the HTTP handlers and no mutation was made.

## Result

All three responses agreed:

- cluster health was `healthy` and metadata was writable;
- metadata generation was 1487;
- three nodes were known and all three were online;
- every node card reported `telemetry_freshness: live`;
- every node had numeric storage capacity/used/free values;
- every node had numeric cache capacity/used/free values;
- every node had a numeric online-backend count of one;
- `storage_online` and `storage_known` were both available; and
- `cache_online` and `cache_known` were both available.

The three endpoints returned the same aggregates:

| Aggregate | Capacity | Used | Free |
| --- | ---: | ---: | ---: |
| Storage | 17,593,259,786,240 | 48,044,793,989 | 17,545,214,992,251 |
| Cache | 17,179,869,184 | 5,507,121,152 | 11,672,748,032 |

Telemetry ages in the sampled responses remained within the five-second live
window. Each node reported three known/active peers and two canonical RPC
connections.

The RPC diagnostics also prove the corrected execution path is active:
telemetry handler counts were 63, 69, and 64 on the three nodes, under the
speculative frame class. Metadata active jobs, pending jobs, pending bytes, and
rejected jobs were all zero. Convergence was drained and unscheduled on every
node.

## Conclusion

**Pass.** One Status request now provides complete live telemetry for all
connected nodes. Sampling the other two endpoints confirmed that aggregation
works regardless of which canonical connection direction each node retains.
