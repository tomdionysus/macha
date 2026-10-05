# High availability without the homework

Macha is happiest when it has at least three nodes in places that are unlikely
to fail together. The nodes form one cluster of equal peers: when one fails,
the others carry on as they are, with nothing to promote.

For a straightforward highly available setup, use this policy on every node:

```yaml
dht:
  replicas: 2
  write_copies: 2
  metadata_write_copies: 2

network:
  advertise: 10.0.0.11       # this node's reachable address
  failure_domain: site-a     # what could fail together

bootstrap:
  - 10.0.0.12:7437
  - 10.0.0.13:7437
```

Change `advertise` and `failure_domain` for each node. Keep the `dht` policy
and cluster key the same everywhere.

## What those settings buy you

- `replicas: 2` asks Macha to keep two authoritative copies of each DATA
  object. It does not copy the whole library onto every node.
- `write_copies: 2` means new file data is on two nodes when the write
  returns, whenever two nodes are present to take it.
- `metadata_write_copies: 2` does the same for the control objects a
  catalogue change stores. A namespace change is durable on the node that
  made it when the write returns and is sent to every node present
  straight afterwards. With fewer nodes present, a write returns on the
  copies it has and repair delivers the rest; nothing is refused for lack of
  peers.
- `failure_domain` tells Macha which nodes share a likely outage. Nodes on the
  same power supply, disk shelf, host, or site should normally use the same
  value. Placement prefers copies in different domains.

With three nodes in three failure domains and the policy above, one node can
normally disappear without taking the library offline or stopping new writes.
Clients may use any reachable node; that node can fetch DATA from another
replica when it does not hold the object locally.

## Bootstrap is discovery, not authority

Give each node two or more bootstrap addresses when possible. They are simply
ways to meet the cluster, so no bootstrap node becomes a permanent leader or
single point of failure. A returning node can reconnect through any reachable
peer, and a new empty node adopts the established cluster state before doing
useful work.

Do not point an empty node at an unrelated cluster, and make sure every member
uses the same cluster key.

## A few practical cautions

- Two copies on two disks in one machine protect against a disk failure, not a
  machine or site failure. Name failure domains honestly.
- A cache is convenient but is not an authoritative replica.
- Enough free capacity must remain in at least two suitable domains for repair
  and continued writes.
- If only one node remains, it keeps reading and writing. Media is readable
  where a copy is reachable, and what the node writes alone exists on that
  node only until a peer is present to take a copy.
- After adding or returning a node, leave the cluster online long enough for
  placement repair to settle before deliberately removing another node.

For the full operational model, see [Cluster and recovery](cluster.md) and
[Durability](durability.md).
