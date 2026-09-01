# Metadata-history bounded-memory UAT

Date: 2026-09-01

Status: active; cold-restart gate passed. Loaded ingest proved the bounded
history representation, but the mixed ingest/playback gate failed on unrelated
foreground-probe and network-admission defects described below.

## Deployment integrity

All three Raspberry Pi 5 nodes were built from the synchronized production
source tree and CMake project metadata. Their installed `/usr/bin/macha`
binaries are byte-identical:

```text
cffe231aa309e9e24c840123476add71e1533a242bd4e3d28e39a7c83e36e4e9
```

All are AArch64 Release builds using GCC 14.2.0 and share the same ELF build ID
`ba4fcc7a0796c27cde616079443e46ecc3ca3602`. The initial checksum discrepancy
was caused by stale CMake/version input on nodes 51 and ES-1; it was corrected
before accepting the deployment.

Deployment rule established from this incident: always synchronize the entire
source tree and complete top-level build metadata before configuring. Piecemeal
remote source updates are prohibited.

## Cold restart and existing-history result

All four nodes report version 0.22.1, ready, writable, four online nodes and
metadata generation 2124. Node 50 recovered its stale FUSE mount and existing
2.13 GB metadata history without manual unmount, OOM or a service-manager
restart. Its old unattributed spool files were preserved as quarantined orphan
files rather than replayed or deleted without proof.

Initial settled measurements:

| Node | Current RSS | History records | History file | Resident history payload | Materialisation cache |
|---|---:|---:|---:|---:|---:|
| GBNI-1 / 10.44.1.50 | 166 MiB | 1,000 | 2,168,523,969 B | 0 B | 45,350,936 B |
| GBNI-2 / 10.44.1.51 | 154 MiB | 1,000 | 2,185,340,303 B | 0 B | 45,350,936 B |
| ES-1 / 10.34.1.50 | 149 MiB | 1,000 | 2,185,327,654 B | 0 B | 45,350,936 B |
| Mac / 10.44.1.200 | 448 MiB | 1,000 | 2,157,564,335 B | 0 B | 113,390,060 B |

Linux CPU settled below 1% after recovery. Node 200 was approximately 3.5%.
The cache limit is 134,217,728 bytes on every node.

ES-1 reports the current four-node view healthy. GBNI/Mac status still includes
an older offline duplicate ES identity and therefore reports degraded; this is
the separately tracked identity-association-reset issue, not a current node or
metadata availability failure.

## Loaded gate

- [x] Restart rsync on node 50 and record sustained namespace/history growth.
- [ ] Prove RSS/cache reach a stable ceiling through multiple generations.
- [ ] Verify reconciliation bodies remain delta-sized under concurrent heads.
- [x] Start playback and perform seeks while rsync continues.
- [ ] Verify viewer/control wait and timeout counters do not regress. This gate
  failed at the transport/API level despite zero scheduler wait counters.
- [ ] Stop/drain load and prove memory and CPU settle without restart.

### Loaded observations and failed mixed-work gate

With rsync active on node 50, the spool grew from about 2.9 GB to 13.4 GB.
Eight publications remained active and advanced from 1.32 GB to 5.55 GB of
source reads. The scheduler recorded cooperative publication yields and no
viewer waits or timed-out FUSE requests. Metadata history remained bounded at
zero resident payload bytes; after the first new generation it held 1,001 disk
records and a 68,022,596-byte, three-entry materialisation cache.

The mixed-work UAT nevertheless failed:

- node-50 RSS grew from 166 MiB cold to about 1.34 GiB anonymous resident
  memory (plus about 62 MiB swap) while eight publications and retained
  playback sessions were active;
- 4 MiB replica writes completed in synchronized bursts and commonly took
  four to seven seconds;
- during one burst, new HTTP connections from node 200 to all three Linux API
  endpoints timed out at two seconds, while node 50's loopback status endpoint
  completed in approximately 0.6 ms. Later the same remote endpoints recovered
  to 0.15--0.45 seconds for the 322 KB catalogue response. Node 200 uses a poor
  Wi-Fi path, so this does not by itself prove loader starvation or server API
  lag; transport attribution remains unresolved. It does prove that the
  end-to-end client/session behaviour is not resilient to an ordinary bad
  network interval;
- playback paused, reloaded and buffered. A seek pipeline needed 1.59--2.80
  seconds for its first fragment;
- a media-profile miss caused synchronous remote probing during session
  admission: 3,358 ms for profiling plus 1,101 ms for first-fragment startup,
  4,708 ms total. This violates the explicit advisory/asynchronous profile
  rule and the viewer invariant;
- the client reported generic 404 failures for playback and catalogue. The
  catalogue collection itself remained present and byte-consistent on all
  three Linux nodes (HTTP 200, 322,260 bytes), so the precise 404 was a
  session/fragment or item-specific response hidden by the client error;
- playback status after the failure showed two retained sessions, one active
  video transcode and two audio transcodes on node 50. Nodes 51 and ES-1 had no
  sessions. Session lifecycle remains part of the separately tracked leak
  investigation.

Stopping rsync stopped new FUSE writes but did not cancel already accepted
spool publication, as designed. The drain/recovery measurement must therefore
continue until those loader publications complete; it must not be mistaken for
an immediate idle transition.

### Remediation checkpoint

The two directly actionable playback defects from this run now have a locally
verified correction: completed profile publication retries without rescanning,
and abandoned physical transformed pipelines have a separate 60-second
event-driven inactivity lease. All 248 core tests and all 3 runtime tests pass.
This UAT remains failed until the complete source is deployed and the mixed
rsync/playback test is repeated; implementation details and the new gate are in
[the playback reclamation checkpoint](2026-09-01-playback-profile-publication-and-pipeline-reclamation.md).
