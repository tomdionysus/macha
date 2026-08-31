# Phase 1C weighted scheduling checkpoint

Date: 2026-08-31

Status: implementation and focused verification complete; deployed on nodes 50
and 51; live FUSE/loader leg passed; genuine-viewer UAT still required.

## Implemented

- Added positive relative `fuse.viewer_weight` and `fuse.loader_weight`
  configuration, defaulting to 95 and 5.
- Classified FUSE open/read/backend traffic as loader/convenience traffic and
  removed its updates to viewer activity clocks.
- Replaced the exclusive playback quiet-window publication gate with bounded,
  work-conserving weighted loader service shared by data and namespace
  publication.
- Loader work can borrow full safe capacity while no viewer is active. Under
  viewer contention it completes bounded quanta and receives a finite non-zero
  share instead of stopping indefinitely.
- Restored the byte-window condition-variable predicate after focused testing
  exposed an incorrect runnable-worker predicate that could spin while the
  in-flight byte window was full.
- Documented the new configuration and interface classification.

## Deterministic verification

Focused local suites passed:

- foundations: 16/16;
- filesystem/FUSE: 50/50; and
- runtime dependencies/configuration: 3/3.

The tests cover default and custom weights, invalid weights, work-conserving
borrowing, finite 95:5 cooldown, non-starvation, viewer arrival during loader
service, FUSE reads not refreshing viewer clocks, and publication progress under
sustained injected viewer activity. The complete suite has not yet been rerun
after this cut, so do not describe it as a full-suite pass.

## Deployment and live evidence

The same source hashes and resulting binary were installed on nodes 50 and 51.
Node 50's existing overnight `rsync --append-verify` was left running.

Before the viewer attempt, node 50 demonstrated simultaneous FUSE acceptance and
publication: publication read 1,393,085,969 bytes in 30.483 seconds (about 43.6
MiB/s) while rsync continued. This proves append verification no longer
manufactures viewer demand and suppresses publication.

During deployment, node 51 was offline. Playback from node 50 then failed with
`extent unavailable`, followed by libav `Input/output error`; repeated attempts
also reached the finite video transcode limit. Node 50 reported only two storage
nodes online and unavailable known durable capacity. This was a real storage
availability failure, not a valid weighted-scheduler result.

Node 51 was rebuilt, installed, and restarted. The cluster returned healthy at
metadata generation 442 with 3/3 nodes and no conditions. After recovery:

- three rsync processes remained active on node 50;
- no new media-read, extent-unavailable, transcode-limit, or timeout log entries
  appeared in the observation window;
- publication advanced 50,122,257 bytes over 20.795 seconds;
- publication quanta and yields each advanced by two; and
- backend failures and timed-out requests did not increase.

## Remaining UAT

After the overnight import, deliberately run genuine Macha streaming playback
while loader work remains runnable. Confirm prompt startup and uninterrupted
playback, continued non-zero loader progress during viewing, and prompt return to
full loader capacity after playback ends. Then rerun the paused aggregate spool
retirement-rate UAT. Do not use a degraded cluster or FUSE reads as the viewer
leg.
