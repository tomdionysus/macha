# Macha: the no-special-node audit (2026-11-20)

The maintainer, relayed by the coordinating session: "There is to be no concept of a special role or node. Any node may disappear
at any time, have patchy connectivity, may be permanently retired, be slow, or be degraded in some other way. The
purpose of every one of your projects is so that is never the user's problem except in exceptional circumstances."
Read-only, on `port/dannan-dagda` at `92c7d3d`; **nothing moves until the maintainer decides.** "Unverified" marks what was not
confirmed in code.

## 1. Special roles Macha has or assumes

Macha has no leader. One real special role, and three lowest-id tie-breaks.

- **Founder by configuration (the one real role).** A node with `bootstrap: []` may hold genesis as authority
  (`src/cluster/local_state.cpp:20`; `MetadataReplica::adopt_committed_head_locked`, `metadata.cpp:2900`).
  - Only it creates the root and anonymous accounts and writes `initial-root-password` (`src/auth/accounts.cpp:51-75`,
    `users.cpp:683-716`, no fsync).
  - A joiner with an empty user table refuses every route with 403 until gossip arrives.
  - A joiner that sees no peer waits ("waiting for bootstrap peer", `metadata_manager.cpp:1397`).
  - The founder's config becomes cluster policy: root uid, gid and mode, and the extent size. A node configured
    differently refuses.
  - `replication` is rewritten by any node whose config differs from the snapshot (`maybe_reconfigure`), so two nodes
    configured differently would rewrite each other (unverified).
- **Tie-breaks on the lowest active node id.** Each one is retaken when membership changes.
  - Generation-2 formation (`service/maintenance.cpp:292-303`): founders build identical records, so the tie-break
    only saves work.
  - The catalogue scanner's coordinator (`CatalogueScanner::coordinator`, `media_catalogue.cpp:4313`): it alone does
    destructive reconciliation and full scans. **Gap:** it does not check that the lowest node has the scanner enabled,
    so then nobody reconciles. On takeover the new coordinator does no fresh full pass.
  - The torrent tombstone sweeper (`torrent_coordinator.cpp:1174-1186`).
- **Torrent claims: per-job owners with automatic takeover.** Rendezvous rank, a 30 s step per rank, a lease of 10
  minutes' absence, and epochs merged (`TorrentCoordinator::pass`, `:921`, `:1158`). It is Macha's own lease (see the
  duplication audit).
- **Ingest jobs: owned by one node, with no takeover.** Each node keeps its own `jobs.json`, and an absent owner shows
  `unreachable` (`acquisition/cluster_jobs.cpp:248-256`). The sources are host-local, so another node could not take a
  job over anyway.
- Not roles:
  - the metadata write floor (copies are sought, never required);
  - placement (rendezvous over the nodes present);
  - media information and book caches (per node).

## 2. Each failure: what happens, and whether the user sees it

- **A node gone suddenly.**
  - Metadata stays writable alone.
  - A FUSE commit is refused only if no node present holds its bytes. Failures back off from 250 ms to 30 s, then park
    after 100 failures in 30 minutes, and an operator retries or abandons them.
  - **Viewers lose media held only there** (the survey shows `unavailable`/`partial`). With `write_copies` 1 (the
    live config), recent writes are at risk until repair runs.
  - Torrents move after 10 minutes. **Ingest jobs are stranded.**
  - Status shows "degraded: known nodes offline".
- **Patchy connectivity.**
  - Heads diverge and merge. Conflicts stand for the operator to resolve.
  - A head that cannot be fetched is set aside until membership changes (since `b0b037c` the view leaves it). It is
    **dropped after `garbage_grace` (30 days): its unmerged writes are lost with only a WARN.**
  - A torrent can be claimed twice in a split; the epoch merge keeps one.
- **A node permanently retired.**
  - **There is no drain or retire action.** Data whose only copy it held is lost.
  - It stays known, and health stays degraded, until it is forgotten after `garbage_grace` or an operator runs an
    identity reset (`manage_api.cpp:950-993`).
  - If it was the founder, the password file leaves with it. The accounts are replicated; recovery is
    `macha-users passwd root`.
- **A slow node.**
  - Commits ask the nearest replicas first; the replicator and repair follow.
  - DATA admission has a no-progress deadline, and FUSE operations have time limits. The user sees delay only.
  - The RPC stall notice is configurable but set to 0 node-wide (`dannan_bridge.cpp:58-62`; unverified whether
    intended).
- **A degraded node.**
  - A full DATA disk spills to the next backend or node.
  - **A full control store is silent:** health reads `healthy` and work retries without backoff (found-issues 3, open).
  - A corrupt `author.meta` stops writes (found-issues 6). `StoragePool::get` stops at the first empty backend
    (found-issues 15).
  - Over `runtime.memory_bytes`, caches shrink pro rata with a WARN.
  - Clock skew: grace and set-aside expiry run on each node's own clock from its own first sighting. Sessions and
    artwork expiry are unverified.

## 3. What Macha relies on from below

- **Dannan:** membership (`active`, `dead_after`, `forget_after`), the persisted roster, identity resets, per-call
  deadlines, lanes, reserves.
- **Dannan has, but Macha does not use:** `dannan::agree` (CASPaxos per key) and the hybrid logical clock. Macha's
  rule says "Agreement is Dannan's; no lease standing in for one", yet the torrent claim and the lowest-id tie-breaks
  are Macha's own.
- **Dagda:** objects, replication and `write_copies`, repair (being redesigned), garbage grace, admission.
- **Dagda not yet used:** the head space (Macha keeps its own), and its own maintenance (Macha drives it).
- **Open below:**
  - Space lacks `b0b037c`.
  - Heads merge equivalence with Macha is unproven.
  - Nothing restores lost claims on peers (found-issues 5).
  - Nothing drains or retires a node's data.

## 4. What Macha would change, and where each belongs

1. **Formation without a founder** (Macha and Dannan).
   - Genesis by the nodes present, as generation 2 already does, rather than by `bootstrap: []`.
   - Cluster policy (extent size, replication, root mode) set once in cluster state through the API, not per node's
     config. Then no node's config is special and none rewrites another's.
   - The root credential and the anonymous account go with sign-on to Lugh (duplication audit 3e). Until then, they are
     made with genesis and replicated, with no file only one node holds.
2. **Tie-breaks and per-job owners on Dannan's agreement and Dagda's leases.**
   - The catalogue coordinator and the torrent sweeper become an agreed role among the nodes *able* to do the work (a
     capability advert), fixing the scanner-disabled gap.
   - Torrent claims move to Dagda's `lease/`.
   - Placement: Dannan `agree`, Dagda `lease`. Macha keeps no lease of its own.
3. **Retiring a node is a cluster action, not data loss** (Dagda and Dannan).
   - A retire or drain moves every object the node holds to the rest before it is forgotten (Dagda: evacuation in
     repair).
   - The identity is retired cleanly, with health back to `ok`, without waiting 30 days (Dannan).
   - Macha calls it from its management API.
4. **A set-aside branch is never silently dropped** (Dagda heads, with Macha until it moves).
   - Before expiry, whatever this node can read of the branch is merged or kept as a standing conflict. If nothing of
     it can ever be read, a snake_case status code says so in Status.
   - It belongs with the head space in Dagda.
5. **The user never sees a single copy go:** `write_copies` above 1 wherever two nodes exist, as soon as the cluster
   is not degraded. The live config is 1 by the operator's degraded-operation ruling (2026-09-28); revisit after the
   cutover.
6. **Degraded-node defects already on Macha's list:** a full control store (found-issues 3), a corrupt `author.meta`
   (6), and the empty backend on read (15). Each becomes a health code and a recovery, not a stop.
7. **Ingest jobs:** sources are host-local, so a job cannot move. Its record becomes cluster state, so any node can
   show it and cancel it, and a stranded job is reported with a code rather than as `unreachable`.
