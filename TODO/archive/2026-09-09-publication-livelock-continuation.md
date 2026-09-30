# Continuation: es-1 publication never completes

Date: 2026-09-09 (evening handover)

Written for a fresh session picking this up cold. Two defects behind the
recurring es-1 livelock were found and fixed today and are deployed; a third
is unfixed, well characterised, and is where to start. Read the "already
refuted" section before forming a hypothesis — three plausible ones are
already dead, with measurements.

## Cluster state as of handover

All three nodes are up, `healthy`, metadata writable, generation ~25752, and
serving playback (a `mode=direct` session was created on es-1 in 47 ms while
it was wedged, so governing law 2 is intact).

- gbni-1 `10.44.1.50`, gbni-2 `10.44.1.51`, es-1 `10.34.1.50`, all `root@`.
- All three run the working tree as built this evening: committed `7d00d2e`
  (0.36.7) **plus uncommitted** reassembly-reserve and no-progress-deadline
  work. The tree is green (380 + 8 tests) but **not committed**.
- **Config drift to undo:** es-1 has `fuse.publication_inflight_bytes: 96M`
  added to `/etc/macha/macha.yaml` (backup:
  `macha.yaml.bak-20260909-inflight`). It was an experiment, it did not help,
  and the other two nodes do not have it. Revert unless it earns its place.
- Both `import-all.sh` loops were stopped this morning and were not restarted.

## What is fixed and proven on the cluster

**1. Reassembly starvation (was the whole "telemetry cannot cross" story).**
`MessageAssembler` could not get a lease to reassemble an inbound frame, threw
`process retained-memory RPC reassembly saturated`, and killed the channel —
~1/second on es-1. Every peer channel died 1–2 s after connecting, so requests
re-dialled constantly (~25 new connections per 70 s) and telemetry, the only
consumer that never dials, appeared to vanish. Fixed with a bounded
reassembly reserve in `retained_memory.hpp`
(`runtime.reassembly_memory_reserve_bytes`, default 32 MB).

Measured before → after on es-1: saturation events ~1/s → **0**; canonical
connections oscillating 0↔2 → **stable**; peer telemetry age climbing past
700 s → **1.2–4.7 s**.

**Placement matters and is load-bearing.** The reserve sits *below* the
control/viewer waiter gate and *above* the loader gate and durable-lower
budget. An earlier attempt (`ec33a78`, reset away this morning) put it above
every gate; that starves publication completely on a node receiving from two
peers, which is why it was dropped. Putting it above the *viewer* gate also
breaks governing law 2 outright. `test_retained_memory_reassembly_reserve_is_bounded_not_absolute`
pins all of this, including that a queued viewer outranks reassembly.

**2. Publication waited with no deadline at all.**
`fuse_frontend.cpp` built its publication `DataWorkContext` with no deadline,
so `data_work.hpp`'s wait took the `cv_.wait(...)` branch and blocked forever.
All eight commit workers sat in
`data_loop → replay_data_quantum → WriteHandle::write → ensure_buffer_memory →
RetainedMemoryLedger::acquire`, holding memory and waiting for more. Nothing
failed, so the 0.30.0 retry-and-park discipline could never see it — which is
the gap `ACTIVE.md` describes in words as "the work never fails, it simply
never completes".

Fixed with `fuse.publication_no_progress_deadline_ms` (default 30 s), driven
by the existing `data_publication_quanta` counter so progress by any worker
re-arms the window for all of them. Confirmed live: es-1 now logs
`FUSE async data publication retry inode=… error=write extent retained-memory
admission made no progress within budget retry_in_ms=250 attempts=1`.

## The open defect

**Publication accumulates leases it never releases, and no budget setting
changes that.** es-1, steady state, over hours and across restarts:

| signal | value |
|---|---|
| `owners.publication` | **515,899,392**, byte-identical across restarts and config changes |
| `owners.fuse_operation` | 19,462,464 |
| sum vs durable-lower budget | 535,361,856 / 536,870,912 = **99.7%** |
| `reclaimable_bytes` | 0 |
| publications started / completed | 137 / **0** |
| `data_publication_bytes_confirmed` | 0 |
| `parked_publications` | **0** |
| `spool_bytes` / publish rate | 8,589,783,970 / **0 B/s** |

The number is an equilibrium, not a workload figure: workers acquire extent
leases until the budget refuses them, and because nothing confirms, nothing is
released. **Any** budget will fill to ~99.7% the same way.

The log shape is the strongest clue and should drive the next step:

```
18:33:01 retry inode=8592 … no progress within budget  attempts=1
18:33:31 retry inode=8593 … no progress within budget  attempts=1
18:34:01 retry inode=8594 … no progress within budget  attempts=1
18:34:32 retry inode=8595 … no progress within budget  attempts=1
```

A **different inode every 30 s, always `attempts=1`**. The pipeline is not
retrying one stuck file; it walks the backlog giving each new file a fresh 30 s
wait against a full ledger. That is also why `parked_publications` stays 0:
parking needs repeated failures of the *same* publication, and nothing fails
twice.

**Where to look next.** `WriteHandle`'s teardown on the `EAGAIN` path
(`filesystem.cpp`, `WriteHandle::~WriteHandle` and `cleanup()`), and whether a
publication whose attempt failed is destroyed or retained with its leases
still held. The hypothesis to test first: each failed-then-retried publication
leaves its leases behind, so the retry loop *is* the accumulation. If true,
the fix is release-on-failure, not another budget.

## Already refuted — do not re-run these

1. **"It is a network/WAN fault."** There is no WAN concept in Macha; the path
   is two WireGuard hops and an EC2 channel. Path MTU is 1420 against a 1500
   interface, but PMTU is learned correctly (`mss:1368 pmtu:1420` on live
   sockets) and small request/reply traffic was always fine. The millions of
   `TcpOutRsts`/`TCPTimeouts` on es-1 are **system-wide on a busy host** —
   the high-volume sockets are Plex on `:32400`, idle for hours — and are not
   Macha's.
2. **"Telemetry gossip is broken."** It was a symptom. Gossip is no-dial, so it
   was the only consumer that could not paper over dead channels by
   re-dialling. Fixed by the reserve; ages are now 1–5 s.
3. **"Publication's inflight ceiling is too large."** Tested directly:
   `publication_inflight_bytes` 256M → 96M moved
   `data_publication_inflight_bytes` 268,435,456 → 100,663,296 and left
   `owners.publication` **unchanged at 515,899,392**. The held memory is not
   the inflight extents.

## How to observe it

Anonymous session, on-box or remote (`:7438`):

```
TOKEN=$(curl -s -X POST -H 'Content-Type: application/json' -d '{}' \
  http://10.34.1.50:7438/api/v1/session | python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])')
curl -s -H "Authorization: Bearer $TOKEN" http://10.34.1.50:7438/api/v1/status
curl -s -X DELETE -H "Authorization: Bearer $TOKEN" http://10.34.1.50:7438/api/v1/session
```

Read `diagnostics.retained_memory.owners`, `diagnostics.filesystem`
(`data_publications_started/completed`, `parked_publications`,
`spool_publish_rate_bytes_per_second`) and `diagnostics.rpc_transport`.
Thread stacks: `gdb -p $(systemctl show -p MainPID --value macha.service)
-batch -ex "thread apply all bt 14"` — libmacha_core has symbols.

A node with no `web:` section answers **401 at `/`** for an unauthenticated
request. That is the auth gate, not a fault, and it misled an operator today.

## Also open, unrelated to this

- **Two intermittent test failures introduced by the telemetry gossip change**
  (`test_catalogue_uses_final_state_after_coalesced_metadata_burst`,
  `test_bootstrap_joiner_requires_complete_checkpoint_survey`): 2 failures in
  8 runs on the changed tree, 0 in 4 on HEAD, neither reproducible in
  isolation, both deadline-bounded. Received telemetry feeds only the Status
  store, never a control-plane decision, so the interaction is timing rather
  than semantics. Recorded in `7d00d2e`'s commit message.
- **Cataloguer fixes deployed and half-verified.** `Blade Runner 2049` now
  matches `tmdb:movie:335984`; one music track matched through the new
  undecorated-title fallback. Three retried music tracks left the unmatched
  list without appearing in the catalogue — still unexplained, and 58 music
  files remain unmatched.
- **The reassembly reserve's rule-1 ordering has only been weakly tested live**,
  because es-1 has had no viewers competing for the ledger while wedged.
