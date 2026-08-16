# Cluster, recovery and transport

## Node replacement and recovery

`state_path` contains identity and control-plane state. Deleting it means **this is a new node**, even if it starts on the same machine and storage paths.

Committed namespace checkpoints are therefore retained on every active node, not only metadata voters. If an old voter is permanently lost and a fresh replacement joins, the surviving nodes can reconstruct the voter set from an agreed committed checkpoint. The replacement then walks the recovered live-object set and pulls the extents it should own.

This is intentionally not a partition escape hatch. Recovery waits until apparently-active peers are either reachable or age out under `dead_after_ms`, requires the surviving committed checkpoints to agree, and requires fresh replacement node(s) for the missing voter seats. If an old voter quorum still exists, normal quorum recovery wins.

Losing only `storage` is simpler: namespace and node identity survive, and normal object repair restores missing replicas. Losing `state_path` is replacement, not disk failure.

## Metadata and split brain

Namespace metadata is a versioned encrypted CAS record held by a configured voter set. Reads and mutations require majority evidence from that set.

For three voters:

```text
3 healthy       quorum 2; read/write
2 healthy       quorum 2; read/write
1 healthy       no mutation quorum
```

A metadata minority fails rather than inventing a second history. Read-only access may fall back to the last valid local snapshot when quorum is unavailable; mutations do not.

The current metadata voter set and data replica count are persisted in the namespace. Replica counts may be changed on a coordinated whole-cluster restart; the old voter majority commits the new policy, then ordinary repair converges existing objects to the new data replica count. `extent_size` remains fixed for the lifetime of the namespace.

## Transport

A peer pair uses up to two persistent authenticated bidirectional TCP lanes. `CONTROL` carries heartbeat/health, membership and other small protocol operations. `DATA` carries object payloads plus prioritised metadata snapshots. DATA is lazy: ordinary cluster formation establishes CONTROL, and the second lane appears when a node needs object traffic or user/background metadata work.

Each lane is canonical independently by authenticated `(NodeId, lane)`, not endpoint text. Simultaneous cross-dial deterministically leaves at most one connection for each lane and drains duplicates before closing them.

Protocol v9 transfers logical RPCs as variable-length AES-256-GCM frames. The authenticated handshake includes the lane and negotiates `network.max_frame_size` to the lower peer limit. The default is 256 KiB and the allowed range is 4 KiB..4 MiB. Frames are not padded to that size and storage extent size is independent of transport frame size. v8-and-earlier peers are intentionally incompatible.

On DATA, frame priority is `foreground` > `read_ahead` > `speculative`; scheduling is reconsidered after every frame. Transfer-local promotion and cancellation notifications remain on DATA because request IDs are scoped to that lane. Health and membership never share a TCP byte stream with object payloads, so bulk retransmission/head-of-line blocking cannot directly delay liveness traffic.

The v9 handshake uses ephemeral X25519 authenticated with HMAC from the shared cluster key. Directional keys are derived with HKDF-SHA256. Server dispatch separately services control and data work, with foreground chosen before read-ahead before speculative queued data. User-originated metadata mutations use read-ahead priority on the DATA lane, while background metadata repair uses speculative priority; health and membership remain on CONTROL.


Nodes must be mutually reachable at their advertised addresses. There is no STUN, TURN, UPnP or NAT hole punching.
