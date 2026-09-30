# Phase 3 three-node idle UAT

Date: 2026-08-30

Observation window: approximately 19:21-19:27 BST

## Scope

Read-only observation of the three-node cluster after the Phase 3 convergence-scheduler changes. The purpose was to verify that a stable cluster does not continuously perform metadata, catalogue, hydration, FUSE, or RPC work, and that the scheduler remains event-driven while no convergence demand exists.

Nodes observed:

- `10.44.1.200` (local macOS node, Macha PID 99204)
- `10.44.1.50` (Linux node, Macha PID 36822)
- `10.44.1.51` (Linux node, Macha PID 56150)

All nodes reported Macha `0.20.0`.

## Result

**PASS for the Phase 3 idle/event-driven convergence scope.**

Across the observation window:

- cluster health remained `healthy`;
- metadata remained `writable`, with quorum available and all 3/3 replicas online;
- all three nodes remained online;
- metadata generation remained exactly `1375`;
- each node retained exactly two canonical RPC connections;
- the canonical RPC creation and reuse counters remained zero;
- no metadata-generation churn, RPC connection churn, or convergence CPU storm was observed.

## Stable end-state measurements

| Node | Service CPU | Instantaneous process CPU | Load 1m | RSS | Canonical RPC connections |
|---|---:|---:|---:|---:|---:|
| `10.44.1.200` | 0.256% | 0.0% | 1.396 | 222,777,344 bytes | 2 |
| `10.44.1.50` | 0.126% | 0.0% | 0.000 | 121,389,056 bytes | 2 |
| `10.44.1.51` | 0.064% | 0.0% | 0.000 | 252,870,656 bytes | 2 |

Both Linux hosts reported 100% CPU idle at the final thread snapshot, with all 91 Macha threads sleeping. The local Macha process reported 99 threads and was also sleeping at 0.0% instantaneous CPU.

## Stack-sample evidence

A five-second sample of the local process collected 2,859 samples per observed thread. It showed:

- `macha-maint` parked in its condition-variable deadline wait for all 2,859 samples;
- `macha-catalogue` parked in `CatalogueHintQueue::wait_for_change` for all 2,859 samples;
- `macha-hydrator` parked in its condition-variable wait for all 2,859 samples;
- RPC health, HTTP workers, FUSE brokers, and FUSE data loops waiting rather than performing sustained work;
- no evidence of an active metadata-maintenance or convergence loop.

This is direct confirmation that the stable cluster's convergence paths are asleep and awaiting events.

## Torrent confounder and isolation

During the first part of the window the local node reported approximately 12-13% process CPU while both Linux nodes were effectively idle. File-descriptor inspection and stack sampling attributed this to libtorrent peer/UTP traffic against a completed file under `state/tmp/ingest/torrents`, including network packet handling and piece-serving activity. The convergence, catalogue, hydration, RPC, HTTP, and FUSE paths remained parked during the same sample.

The operator then paused the torrent. The local node immediately fell to 0.372%, then 0.256% service CPU and 0.0% instantaneous process CPU, without any change in metadata generation, health, replica availability, or RPC connection counts. This isolates the earlier CPU use to the known-faulty torrent/ingest subsystem. Torrent behaviour is explicitly deferred and is not a failure of this Phase 3 UAT.

## Observation-tool note

One long-lived monitoring shell reported simultaneous connection refusals for all three HTTP status endpoints. Fresh direct probes succeeded immediately while all processes and listeners remained continuously present. The failures were confined to that monitoring shell and are therefore treated as an observation-tool artefact, not a cluster outage. Subsequent direct probes to all three nodes completed successfully.

## Conclusion and next UAT

The idle acceptance criterion is satisfied: a stable, writable three-node cluster converges and then sleeps. The previous three-core busy-loop behaviour is absent, work is not running continuously on critical communications paths, and no unnecessary metadata generations or RPC connections were created.

The next useful UAT is an active burst test after the remaining Phase 3 integrated burst/acceptance work: inject a bounded set of metadata/topology changes and verify that the cluster performs one coalesced convergence run, at most one follow-up for events arriving during that run, and returns to the idle state demonstrated here.
