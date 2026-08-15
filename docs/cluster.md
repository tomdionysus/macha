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

Each peer pair has one persistent authenticated bidirectional TCP connection. It multiplexes all health, control and object traffic with 64-bit request IDs. Connections are canonical by authenticated node identity, not endpoint text; simultaneous cross-dial deterministically keeps one physical connection and drains the duplicate before closing it.

Protocol v7 transfers logical RPCs as variable-length AES-256-GCM frames. `network.max_frame_size` is an upper bound, negotiated to the lower peer limit during the authenticated handshake; the default is 256 KiB and the allowed range is 4 KiB..4 MiB. Frames are not padded to that size. Storage extent size is independent of transport frame size.

Frame type is the sole source of transport priority: `control` > `foreground` > `read_ahead` > `speculative`. There is no separate numeric priority on the wire. The sender re-runs scheduling after every frame, so health/control and foreground data can pre-empt lower-priority transfers at frame boundaries. Speculative traffic is entitled only to otherwise spare transport capacity. A transfer may be promoted without changing request ID; subsequent frames use the more urgent frame type.

The v7 handshake uses ephemeral X25519 authenticated with HMAC from the shared cluster key and negotiates the frame ceiling. Directional keys are derived with HKDF-SHA256. Server dispatch likewise separates control execution from data work and always chooses foreground before read-ahead before speculative queued data.

Nodes must be mutually reachable at their advertised addresses. There is no STUN, TURN, UPnP or NAT hole punching.
