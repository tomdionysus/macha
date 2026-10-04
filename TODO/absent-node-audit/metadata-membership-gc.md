# Audit: metadata, membership, retention/GC, history vs. "any node may disappear, forever"

Scope read in full: `src/metadata/metadata_manager.{cpp,hpp}`, `metadata_server.cpp`, `namespace_control_store.*`, `src/cluster/membership.*`, `src/contract/gates.*`, `src/service/maintenance.cpp`, `src/storage/retention.*`, `src/ledger/retention_ledger.*`, `node_horizon_builder.cpp`. Read in the relevant parts: `metadata.cpp` (merge, replica history, accept/store, checkpoint proof, compaction), `cluster.cpp`, `catalogue.cpp`, `distributed_store.cpp` (retain_data/retain_control/replicate_control), `node_services.cpp` (claims barrier), `auth/accounts.cpp`, `auth/users.cpp`, plus the FUSE/ingest/torrent catch sites needed for question A. Nothing was built, run or modified. All paths below are under `/Users/tom/devroot/macha/`. Anything not traced end to end in code is marked **inferred** or **unconfirmed**.

## Headline

The code separates two regimes:

- **Authoring (commits, merges)** is floor-based: any `metadata_min_write_replicas` reachable, policy-compatible nodes suffice, and there is no voter set. With floor 1 this meets the requirement. The shipped default is **2** (`src/config.hpp:590`), which does not on a two-node cluster.
- **Everything destructive or compacting** is fenced on *every durably-known node being directly reachable*: tombstone removal, claim release, control GC, DATA sweep, claim-row pruning, history truncation. A node that never returns is never forgotten automatically, so all of these stop forever and their state grows without bound. The only escape is an operator identity reset.

Three further hard stops are not bounded by time:

- The write floor cannot be lowered while the floor is unmet (M2).
- The retention baseline roster names nodes that must all be present (M7).
- A stale head without a provable common ancestor wedges reads and writes (M5/H2).

## Summary table

| ID | Area | Operation blocked or strained | Bounded? |
|---|---|---|---|
| M1 | metadata write floor | every metadata commit when fewer than `metadata_min_write_replicas` compatible nodes are active | no: node return, or operator (and see M2) |
| M2 | floor policy transition | lowering the floor; any write after a config change; head exchange between nodes on different floors | operator-only, and not possible while the floor is unmet |
| M3 | claims barrier inside commit | every commit that introduces objects (DATA floor, CONTROL floor, tree-node floor) | no: node return |
| M4 | merge of divergent heads | merge, and all reads/writes on a node holding more than one head, when below floor | no: node return |
| M5 | merge preconditions | merge; then all converged reads and all writes on that node | operator-only in the worst case |
| M6 | `repair_once` | "stable" flag, hence all destructive gates; tombstone gate via `metadata_dirty` | yes for a dead node (`dead_after`); unbounded for a gossip-live but unreachable node |
| M7 | retention baseline roster | claim release, control GC, DATA sweep | no: every roster node must return; no escape hatch found |
| M8 | virgin formation | first formation / join | until a bootstrap peer appears |
| M9 | replica in `recovery_required` | all metadata reads and writes on that node | until a peer with heads is reachable |
| M10 | unreconstructable head repair | that head excluded from reads; merge | until a compatible peer holding it is reachable |
| M11 | read/mutation path RPCs | reads and mutations slowed by a just-vanished peer; sticky remote generation defeats the cache | yes (`dead_after`, or local generation catches up) |
| H1 | history checkpoint | history truncation | no: all known nodes must be reachable and agree |
| H2 | truncation vs nodes outside the known set | merge with a node that returns after reset plus compaction | operator-only (inferred) |
| N1 | known-node set | everything fenced on `all_known_reachable` | operator-only (identity reset) |
| N2 | `all_known_reachable` semantics | destructive work after every restart until every peer is directly authenticated | yes if peers are alive |
| N3 | gossip-derived liveness | dead nodes propagate to new joiners; briefly counted active | partly |
| N4 | identity reset | the only way to forget a node | operator-only |
| G1 | tombstone gate | tombstone removal | no (N1) |
| G2 | control gate | CONTROL claim release, control GC, claim-row prune | no (N1, M7) |
| G3 | data gate | DATA claim release, physical sweep, prune | no (N1, M7) |
| G4 | `generation_current` / `catalogue_complete` | gates, catalogue refresh, metadata cache | yes (local generation catches up or restart) |
| G5 | metadata retry backoff | convergence latency after failure | yes (max 300 s, reset on topology change) |
| R1 | claim release | sole head, complete horizon, gates | no |
| R2 | per-origin state never pruned | small unbounded growth | n/a |
| C1 | catalogue commit | catalogue edits | no: node return |
| C2 | control convergence | none blocked (debt) | yes |
| C3 | catalogue root conflict auto-merge | automatic catalogue merge | until the objects are obtainable |
| C4 | catalogue refresh vs known generation | catalogue view freeze | yes |
| C5 | scanner coordinator | none blocked | yes |
| T1 | torrent claim lease / pin | torrent ownership move | 10 min; pin is operator-only |
| A1 | auth gossip | none blocked | n/a |

---

## Findings

### M1. The write floor refuses every commit below `metadata_min_write_replicas`
1. Location:
   - `src/metadata/metadata_manager.cpp:1696-1700`, `mutate_impl`
   - `:749-752`, `:782-783`, `:835-836`, `publish_commit`
   - `:891-893`, `:902-903`, `ensure_accepted_head_durable`
   - default `metadata_min_write_replicas{2}` at `src/config.hpp:590`; `macha.yaml.example:288` also says 2
2. Conditions:
   - `if (active.size() < need) throw MetadataNotReady("metadata write durability floor unavailable: too few policy-compatible replicas")`, where `active = compatible_replicas(membership.active())`. This is thrown before the retry `try`, so it is not retried inside `mutate_impl`.
   - `publish_commit`: `compatible.size() < required`, then `stored_on.size() < required` ("metadata commit durability floor unavailable"), then `accepted < required` ("acceptance certificate durability floor unavailable"). These are retried up to `retries` (8) with a 1 ms sleep, then thrown.
3. Lost: every metadata commit. That covers FUSE namespace operations and file publications, catalogue edits, conflict resolution, torrent request writes, tombstone stamping/erasure (`maintain_garbage_metadata`), policy transitions and the baseline commit.
4. Protects: durability. The comment at `:92-94` says "The write policy is a durability floor, not a voter set: any metadata_min_write_replicas reachable replicas may accept a mutation." The hazard is a commit that exists on fewer than W nodes being lost with them. It is not a split-brain fence; branches are allowed and merged.
5. Bounded: no. It ends when enough compatible nodes are active again. The apparent escape, setting the floor to 1, is blocked by M2.
6. Accumulation: caller-side queues grow (FUSE spool/journal, ingest jobs in `metadata_unavailable`, torrent intents journal). Bounds are those callers' concern; see answer A.

### M2. Lowering the floor needs the old floor; nodes on different floors cannot exchange heads
1. Location:
   - `metadata_manager.cpp:1350-1429`, `maybe_reconfigure`, especially `:1401-1405`
   - `:1730-1732`, `mutate_impl`
   - `:721-744`, `acceptance_floor_for`
   - `:196-218`, `compatible_replicas` / `require_metadata_policy_match`
   - `:100-119`, `publish_replica_state`
   - `:1497-1500`, `discover_or_form`
   - `src/metadata/metadata_server.cpp:80-93`, `MetadataServer::accept_commit`
   - `src/metadata/metadata.cpp:3243-3288`, `acceptance_matches_record_policy_locked`
   - `metadata.cpp:1901-1917`, the merge's `scalar_merge`
2. Conditions:
   - `transition_floor = std::max(configured, persisted); if (compatible.size() < transition_floor) return initial;`. The transition is silently not committed. Then `mutate_impl` throws `MetadataNotReady("metadata write-floor transition is not durably accepted")` because `snapshot.metadata_write_replicas_required != need`.
   - `compatible_replicas` drops any peer whose advertised `metadata_write_replicas_required != local config`.
   - `MetadataServer::accept_commit` returns false for any head whose snapshot floor `!= min_write_replicas_`.
   - `publish_replica_state` reports `read_only` on `local_policy_mismatch`.
   - Before a protocol-20 policy exists, `require_metadata_policy_match(active)` throws `MetadataNotReady("metadata write-floor policy mismatch peer=...")` if any active peer differs.
   - The merge throws `std::runtime_error("metadata cluster policy diverged: ...")` if both branches changed a policy scalar differently.
3. Lost:
   - An operator who sets the floor from 2 to 1 on the survivor of a two-node cluster gets a node that is still read-only. The transition commit needs max(2,1) = 2 compatible active nodes.
   - If nodes are reconfigured one at a time, the reconfigured node treats the other as incompatible (not counted, not surveyed, its heads refused) until both carry the same value and are both present.
4. Protects:
   - Comment `:1366-1369`: "Until a durable protocol-20 policy exists, every active peer must agree on the write floor, or two incompatible cohorts could each establish authority from the same genesis."
   - Comment `:735-736`: "Policy transitions are certified at the strongest policy on any parent edge: this covers lowering the floor and merges with an older sibling."
   - Hazard: a node unilaterally weakening durability for history certified at W=2.
5. Bounded: operator-only, and the operator action cannot take effect while the absent node stays absent. No code path found that forces a floor-lowering transition with fewer nodes than the old floor.
6. Accumulation: as M1.

### M3. The claims barrier runs inside every commit and has its own floors
1. Location:
   - `src/service/node_services.cpp:398-413` (the `publication_retention` callback), called from `metadata_manager.cpp:1860-1865`, `:1311-1318`, `:2091-2094`
   - `src/cluster/distributed_store.cpp:845-1000`, `retain_data`
   - `:1002-1018`, `retain_control`
   - `src/metadata/namespace_control_store.cpp:36-38`, `ControlNamespaceNodeStore::put`
   - `metadata_manager.cpp:1823-1826` (tree commit) and `:1268-1276` (merge commit)
2. Conditions:
   - `retain_data`: for each object, at least `min_write_replicas` (default 1, `config.hpp:591`) active hosting nodes must both hold it and durably record the claim. An object found "short" is re-read (`get`) and re-`put`. If its only copies are on the absent node, it returns false and the barrier throws `MetadataNotReady("DATA retention floor unavailable before metadata publication")`.
   - `retain_control(control, dot, metadata_write_replicas_required)`: `if (active.size() < required) return false`, then `MetadataNotReady("CONTROL retention floor unavailable ...")`.
   - Tree node write: `if (replicas_->replicate_control(id, node) < required_) throw MetadataNotReady("namespace node could not reach the metadata durability floor")`.
   - The barrier is not inside the `try` at `:1867`, so its throw leaves `mutate_impl` at once.
3. Lost:
   - Any commit introducing or re-affirming objects.
   - A merge commit whose merged namespace references extents this node cannot find on `min_write_replicas` active nodes. A merge that brings in the other branch's files can be refused because that branch's data lives on a node that has since gone (inferred from `retain_data`'s short-object path; not reproduced).
   - Appends: `node_services.cpp:334-340` re-claims every extent of the file, so appending to a file whose older extents live only on an absent node fails.
4. Protects: `node_services.cpp:305-307`: "An entry missed here never gets liveness evidence and can be collected while still referenced." `metadata_manager.hpp:173-175`: "every object the new head refers to is durably claimed first".
5. Bounded: no; ends when a holder returns.
6. Accumulation: reserved mutation sequences are consumed; staged tree nodes and catalogue shards become orphans and wait for a GC that is itself fenced (G2).

### M4. Divergent heads cannot be merged below the floor, and then nothing can be read through the converged path
1. Location: `metadata_manager.cpp:1193-1195`, `read_group`; `:1592-1609`, `read_record`; `:1710-1725`, `mutate_impl`.
2. Conditions:
   - `if (compatible_replicas(nodes).size() < need) throw MetadataNotReady("divergent metadata heads await reconciliation; write durability floor unavailable")`.
   - `read_record` falls back to the local committed record only if `heads.size() == 1`.
   - `mutate_impl` rethrows when `local_heads.size() > 1`.
3. Lost: with W=2 and a node holding two heads when its peer disappears, `snapshot_view()` / `converged()` throws and all mutations fail. Only `available_snapshot_view()` (the last decoded cache) still serves.
4. Protects: the merge is itself a commit, so it needs the floor (M1 hazard).
5. Bounded: no; node return.
6. Accumulation: none beyond M1.

### M5. Merge preconditions that can make convergence impossible
1. Location:
   - `metadata_manager.cpp:1202-1212`, `read_group`
   - `:1221-1222`, `:1243`
   - `:1165-1166`
   - `src/metadata/metadata.cpp:1909`, `:1967`, `:2104`, `merge_metadata_snapshots_over`
   - `metadata.cpp:3593-3661`, `history_common_ancestor_locked`
   - `metadata_manager.cpp:316-363`, the rootless healer in `import_history_from_peer`
2. Conditions and results:
   - `!common` gives `MetadataNotReady("divergent metadata heads have no known common ancestor")`.
   - `!base_materialized` gives "metadata common ancestor cannot be reconstructed".
   - `!left_materialized || !right_materialized` gives "metadata merge head cannot be materialized".
   - Single head not materialisable gives "selected metadata head cannot be materialized".
   - Policy scalars changed on both sides gives `std::runtime_error("metadata cluster policy diverged")`.
   - Conflict records diverged gives `std::runtime_error("metadata conflict state diverged for id ...")`.
   - Tree merge needs tree nodes via `ControlNamespaceNodeStore::for_reading`, which pulls from peers with `ensure_control_local`. A missing node fails the merge (exact exception type from `merge_tree_backed_snapshots` not traced).
   - All of these are retried on every `read_group` call. Nothing parks them.
3. Lost: the merge. Because the node now holds more than one accepted head, converged reads (`read_record` fallback needs exactly one head) and every mutation (`:1722-1723`) are lost on every node that imported the second head. This is a cluster-wide write wedge once the heads spread through `discover_accepted_heads`.
4. Protects: a three-way merge without a base cannot distinguish "deleted here" from "created there". Hazard not stated in a comment.
5. Bounded: the healer (`:320-361`) fetches missing ancestors from the head's owners and witnesses, so it clears if some reachable node still has the connecting history. If nobody does (H2), no code path resolves it. Operator-only, and no tool for it was found. `plan_causally_dominant_metadata_repair` / `plan_conflict_preserving_metadata_repair` (`metadata.cpp:2178-2246`) refuse tree-backed snapshots, and their caller was not located.
6. Accumulation: the 30 s `unacceptable_head_retry_at_` map (bounded by head count); log volume.

### M6. `repair_once` demands that every active node accept the head; failure clears "stable"
1. Location: `metadata_manager.cpp:1989-2033`; `src/service/maintenance.cpp:407-451`; `:497` (`garbage_due = !busy && !metadata_dirty`); `:706-709`.
2. Condition: `if (converged < active.size()) throw MetadataNotReady("metadata accepted-head replication incomplete")`. Maintenance then calls `note_replica_validation(false, ...)`, so `status().stable` is false; `metadata_dirty` stays set; retry follows the G5 backoff.
3. Lost: `metadata_stable`, so the tombstone, control and data gates all shut. `garbage_due` is false while `metadata_dirty`. Non-destructive repair continues (`:425-435`).
4. Protects: comment `:2019-2022`: "Convergence is replication, not head replacement: every active node is offered the head and proof". Stability means all active replicas hold the head.
5. Bounded:
   - Yes for a dead node: it leaves `active()` after `dead_after` (30 s).
   - Not bounded for a node kept "active" by gossip but not reachable from here. `Membership::observe` refreshes `seen` on any newer gossiped record (`src/cluster/membership.cpp:232-236`). In a non-transitive partition (A reaches B, B reaches C, A cannot reach C), A counts C active and `repair_once` fails every time.
   - Unconfirmed: whether `node_.call` can relay through B, and whether this also hits two inbound-incapable nodes.
6. Accumulation: none direct. It feeds the G1-G3 accumulation.

### M7. The retention baseline needs every node on a fixed roster to be active and at the head
1. Location:
   - `metadata_manager.cpp:2039-2112`, `repair_once`
   - `:1374-1393`, roster construction in `maybe_reconfigure`
   - `src/metadata/metadata.cpp:1918-1931`, merge
   - `src/metadata/namespace_tree.cpp:1166-1169`, where migration sets `retention_baseline_complete = false`
   - `src/contract/gates.cpp:16-18`, `:91-92`
2. Conditions:
   - `all_participants_online` is false as soon as any id in `snapshot.metadata_participants` is not in `active`. The baseline commit is then never attempted.
   - The roster, when empty and the baseline is incomplete, is built from legacy `metadata_voters`, every `node_status` id, every `mutation_sequences` id and the currently active peers. That is every node that ever wrote metadata or reported status.
   - A merge unions the rosters and ANDs the flag.
   - Gates: `control_gate` reports "destructive GC not enabled"; `data_gate` reports "retention baseline incomplete".
3. Lost: CONTROL and DATA claim release, control GC, the physical DATA sweep, and claim-row pruning. Tombstone collection does not read this flag (G1).
4. Protects:
   - Comment `:2064-2068`: "A namespace migrated from SM12 or re-rooted onto the tree has no retention baseline. Establish it only once every durable participant is at this head; the publication guard claims every reachable object before the baseline commit is accepted. Until then destructive mark/sweep is fenced in Service."
   - Hazard: sweeping objects that are live but were never claimed.
5. Bounded: no. A roster member that never returns blocks it forever. Identity reset does not edit the roster. `mutate_impl` forbids changing `metadata_participants` (`:1777-1782`). No tool that edits it was found (`tools/namespace_migrate.cpp:211` only reads it).
6. Accumulation: all garbage objects, all claims, and all superseded tree nodes and catalogue shards, without bound.
7. Applicability to the live cluster: unconfirmed. The cluster's namespace was re-rooted onto the tree, so this depends on whether the baseline commit landed before es-1 went away. `gate.data` conditions or `metadata_inspect` would show `baseline=0/1`.

### M8. Virgin formation waits for peers
1. Location: `metadata_manager.cpp:1494-1537`, `discover_or_form`; `maintenance.cpp:380-399`.
2. Conditions:
   - `write_active.size() < need` gives "metadata replica set forming: need N ...".
   - Bootstrap configured and `active.size() == 1` gives "waiting for bootstrap peer".
   - `!survey.complete` (any active peer failed `get_committed_metadata`) gives "waiting for bootstrap checkpoint survey".
   - Policy mismatch with any active peer throws.
   - A virgin non-lowest-id node defers to the founder and retries on `metadata_retry_backoff`.
3. Lost: startup of a node at generation 1 or below. A fresh node configured with a bootstrap peer cannot form alone. FUSE `wait_for_initial_namespace` (`src/fuse/fuse_frontend.cpp:4441-4462`) throws ETIMEDOUT after `initial_namespace_timeout`.
4. Protects: comment `:1487-1488`: "A joiner may form a virgin namespace only once every active bootstrap peer has shown no durable post-genesis history exists." Hazard: a joiner inventing a second namespace.
5. Bounded: until a bootstrap peer is reachable. It does not affect established nodes.
6. Accumulation: none.

### M9. A replica in `recovery_required` cannot serve or write until a peer supplies heads
1. Location: `src/metadata/metadata.cpp:2275-2277`, `:2384-2405`, `:3208-3214`; `metadata_manager.cpp:1602`, `:1660`, `:1704-1709`; `maintenance.cpp:429-432`.
2. Condition: `recovery_required_` is set when primary state failed authentication (the node is seeded from the cache) or when a protocol-20 checkpoint has no acceptance certificate. The `read_record` fallback is refused while it holds. `retention_release_view` returns nothing. Maintenance rethrows and skips the whole pass.
3. Lost: all metadata reads and writes on that node, and all maintenance.
4. Protects: `metadata.cpp:3209-3210`: "Without a certificate a write-floor checkpoint is not authority: keep it and force peer recovery rather than invent a legacy head."
5. Bounded: until any peer holding accepted heads is reachable. If the peers are gone for good, nothing ends it; no tool found.
6. Accumulation: none.

### M10. Unreconstructable-head repair needs a compatible peer that can serve the full record
1. Location: `metadata_manager.cpp:445-498`; `metadata.cpp:4147-4178` (the head is excluded from `accepted_heads()` during its cooldown); `maintenance.cpp:991-997`.
2. Condition: `peers.empty()` returns 0 ("no peer is reachable"). Otherwise each compatible active peer is asked with `get_metadata_history_record`.
3. Lost: while flagged, the head is hidden. Callers see fewer heads, so a node may operate on its other head. If it is the only head, `accepted_heads()` is empty and `read_group` throws "metadata accepted-head set is empty".
4. Protects: hazard not stated beyond "excluded from reads pending live repair".
5. Bounded: until a peer that can materialise that hash is reachable. If the only holder is gone, it never ends.
6. Accumulation: none.

### M11. Read and mutation paths make synchronous peer RPCs
1. Location: `metadata_manager.cpp:841-862`, `discover_accepted_heads` (sequential `node_.call` per active compatible node); `:1063-1143`; `:272-281`, `cached_record`; `:1710-1712`; `hpp:63-65` (`mutation_mutex_` "Held across replica RPCs and metadata commits"); `src/cluster/cluster.cpp:846-872`, `merge`.
2. Conditions:
   - A cache miss (TTL 250 ms, `config.hpp:633`) runs `read_group`, which surveys every active compatible peer.
   - A peer that has just died stays in `active()` for `dead_after` (30 s). Each survey waits on it.
   - `cached_record` returns nothing while `node_.remote_metadata_generation() > cache_->generation`. `remote_metadata_generation_` is a process-lifetime max over gossiped `NodeInfo::metadata_generation`, including entries relayed for nodes no longer present.
3. Lost: nothing permanently. Reads through `snapshot_view()` and mutations (serialised under `mutation_mutex_`) are slowed for up to `dead_after` after a node vanishes. If the vanished node had advertised a higher generation that nobody else holds (possible with floor 1), every read is uncached and every mutation re-surveys until local generation reaches that number or the process restarts. That second consequence is inferred from the code, not observed.
4. Protects: freshness; comment `:301-303`.
5. Bounded: yes.
6. Accumulation: none.

### H1. History truncation requires every known node reachable, single-headed and agreeing
1. Location: `metadata_manager.cpp:964-1061`, `attempt_history_checkpoint`; `:906-929`, `discover_accepted_heads_required`; `metadata.cpp:3120-3143`, `:4704-4737`; `maintenance.cpp:999-1007`.
2. Conditions, each a silent `return` retried on the next maintenance pass:
   - `local_heads.size() != 1`
   - `!node_.membership().all_known_reachable()`
   - any participant in `membership.all()` fails or mis-answers `get_metadata_heads`
   - any participant has other than one head, or a different head
   - any participant fails to durably ack the proposal
   - The epoch is `sha256` of the sorted `membership.all()` ids, so any roster change invalidates an in-flight proposal.
3. Lost: history truncation. `history.*` grows by one frame per commit forever. Thresholds are 256 records or 64 MiB, but they only make the attempt due.
4. Protects:
   - Comment `:977-978`: "Only with exactly one local accepted head and every known participant directly reachable, as for destructive object GC."
   - `maintenance.cpp:983-985`: "Metadata ancestry is re-rooted only once a durable cluster-wide proof shows every durably-known participant's ancestry floor at the same accepted head."
   - Hazard: an absent node may hold a head whose common ancestor with ours lies below the truncation point, which makes the merge impossible (M5).
5. Bounded: no. Only by the node returning or by identity reset (N4).
6. Accumulation:
   - The history file and in-memory `history_` index are unbounded.
   - Materialisation walks delta chains back to the nearest full record, so reconstruction cost grows.
   - Remote participants compact only when they themselves run a successful round (`compact_history_if_safe` is called only at `:1057`).
   - Note: `compact_history_if_safe` itself checks only thresholds and single-head. It does not check that `checkpoint_proof_` is committed. Its one caller does.

### H2. Truncation only considers the current known set; a node outside it that returns can wedge the cluster (inferred)
1. Location: `metadata_manager.cpp:986-997` (participants = `membership.all()`); `metadata.cpp:3167-3192`; `metadata.cpp:3090-3094`; `src/cluster/membership.cpp:247-265`.
2. Scenario: node C is removed from the known set by identity reset, or is simply unknown to the proposer. The rest converge and compact history to floor F. C later returns holding head E that is older than F's recorded parent, or divergent from below F.
   - `read_group` imports E from C and accepts its certificate.
   - `accepted_head_is_ancestor_locked(E, F)` cannot prove ancestry. The proof rule only says the floor is an ancestor of later generations.
   - `history_common_ancestor(E, F)` is empty unless E is exactly F's stored `previous`.
   - Result: "divergent metadata heads have no known common ancestor" on every node that imported E (M5). C itself imports F as a rootless full record and is in the same state.
3. Lost: merge, converged reads and all writes, cluster-wide.
4. Protects: nothing; this is the gap in the protection. There is no staleness or epoch check on a returning node. `membership.cpp:212-215` says a reset "is a freshness boundary, not a blacklist: only a direct observation or gossip seen after the reset re-establishes the node."
5. Bounded: operator-only. No tool found.
6. Additional observations:
   - `load_checkpoint_proof` keeps the proof only if `proof.floor_hash == committed_.hash`. After a restart with commits past the floor, the proof-based ancestry rule is gone.
   - Participants ack a proposal without consulting their own roster (`record_checkpoint_ack` checks only the single head). The roster that counts is the proposer's alone.
   - Not reproduced; derived from reading the code.

### N1. A known node is never forgotten automatically
1. Location: `src/cluster/membership.cpp`. `nodes_` is erased only in `apply_identity_reset` (`:256-260`) and at load for reset-matched entries (`:85-95`). It is persisted at `state/membership/known-nodes.bin` (`cluster.cpp:210`). `dead_after` only moves a node out of `active()` (`:288`, `:311`).
2. Condition: none removes a node by age. `seen_unix_ms` is stored but never used for expiry.
3. Lost: everything fenced on `all_known_reachable` (G1, G2, G3, H1).
4. Protects: `membership.hpp:54-57`: "every durably-known node authenticated directly within dead_after (gossip does not count), so a node loaded from disk fences GC until contacted."
5. Bounded: operator-only (N4).
6. Accumulation: roster capped at 65,536 nodes / 16 MiB (`:17-19`); otherwise see answer C.

### N2. `all_known_reachable` requires direct authentication of every known node within `dead_after`
1. Location: `membership.cpp:316-325`; consumers at `maintenance.cpp:706` and `metadata_manager.cpp:982`.
2. Condition: `item.second.direct_seen && now - *direct_seen <= dead_`. The exception: when neither side is inbound-capable the pair is skipped ("Neither side can be dialled: not a fault, no fence").
3. Lost: all destructive work after each restart, until every peer has been directly contacted; and permanently if any is gone.
4. Protects: as N1.
5. Bounded: yes if the peers are alive.
6. Note: the skipped pair is still included in `attempt_history_checkpoint`'s participants (`:986`), where it must answer RPCs. Two inbound-incapable nodes that know each other would pass the reachability test but fail the survey, so history is never truncated. Unconfirmed: depends on whether calls can ride an existing route or relay.

### N3. Liveness and roster by gossip
1. Location: `membership.cpp:202-245`, `observe`; `cluster.cpp:795-803` (the `members` reply sends `members_.all()`); `cluster.cpp:846-889`.
2. Behaviour:
   - A node first learned by gossip is inserted with `seen = now` and persisted. Every new joiner therefore inherits every dead node any peer still remembers, and is fenced by it (N1).
   - A known node is counted active for `dead_after` whenever a gossiped record carries a newer `seen_unix_ms` than the stored one. After a restart this can briefly re-activate a dead node, because the persisted `seen_unix_ms` is only rewritten on association change (`:236`, `:243-244`).
3. Lost: transiently, `repair_once` (M6) and floor arithmetic count a node that is not there. Commits then fail at the store step rather than at the count.
4. Protects: hazard not stated.
5. Bounded: 30 s per occurrence. Roster inheritance is permanent.
6. Accumulation: none.

### N4. Identity reset is the only "forget", and it is operator-only
1. Location: `src/api/manage_api.cpp:613-660` (`POST /api/v1/manage/identity-associations/reset` and `/nodes/{node_id}/identity-association/reset`); `membership.cpp:247-265`; `cluster.cpp:997-1029`; applied from metadata at `metadata_manager.cpp:222-224` and `metadata_server.cpp:30-34`.
2. Behaviour: keyed by endpoint (host, optional port, optional stale id). It erases matching roster entries and suppresses gossip older than the reset. Propagation goes only to currently active peers, plus a metadata audit record that itself needs the write floor (the handler comment says the reset "must not depend on metadata whose convergence the stale identity may fence").
3. It does not edit `metadata_participants` (M7), `mutation_sequences`, `node_status`, or claim origins.
4. A node reset out and later directly observed is re-admitted unconditionally (H2).
5. Reset tombstones are kept forever (cap 65,536).

### G1. Tombstone gate
1. Location: `src/contract/gates.cpp:38-55`; facts built at `maintenance.cpp:497`, `:706-729`; action at `:734-760` and `:135-196`.
2. Conditions, in order: `garbage_due` (`!busy && !metadata_dirty`); not `rebuilt_inventory`; `stable(facts)` = `reachable && metadata_stable`, with reasons "not every known node reachable" / "metadata not stable"; `catalogue_complete(inventory)`.
3. Lost: removal of matured tombstones (`snapshot.garbage`), stamping of legacy tombstones, removal of stale tombstones.
4. Protects: `maintenance.cpp:501-504`: "Destructive maintenance is deliberately opportunistic: degraded clusters retain garbage. Reclamation is enabled only after every durably-known node has been directly reached by this process and metadata repair has validated/converged that complete replica set." Specific hazard for tombstones not stated. Inferred: an absent node's branch could still reference the object, and the tombstone is what protects it from the orphan sweep during grace.
5. Bounded: no (N1). Dropping also needs a metadata commit (M1) through `maintain_garbage_metadata`.
6. Accumulation: `snapshot.garbage` grows by one `GarbageRef` per retired object, carried in every full snapshot, and unioned on every merge (`metadata.cpp:2129-2143`). Unbounded.

### G2. Control gate
1. Location: `gates.cpp:57-79`; `maintenance.cpp:794-826`.
2. Conditions: `gc_due`; not rebuilt; `destructive_gc_enabled` = `stable && release_view && retention_baseline_complete`; `catalogue_complete`; an inventory exists; `generation_current` (inventory generation >= `known_generation`).
3. Lost: CONTROL claim release (`release_unreferenced`), `control_gc_step`, `prune_unclaimed(control)`.
4. Protects: `maintenance.cpp:803-805`: "Destructive retention release is fenced by direct reachability of every durably-known node. A partition may continue to accumulate causal tombstones/claims, but it cannot reclaim authoritative bytes."
5. Bounded: no (N1, M7).
6. Accumulation: every commit on a tree-backed namespace writes new spine nodes, and every catalogue commit writes new shards and a manifest. Superseded ones are never removed. Control claims are never released. Unbounded.

### G3. Data gate
1. Location: `gates.cpp:81-110`; `maintenance.cpp:833-943`.
2. Conditions: as G2, spelled out, each with its own reason string ("no sole accepted head for retention release", "retention baseline incomplete", "inventory generation N behind known M").
3. Lost: DATA claim release, the physical sweep (`local_.data().gc_step`), `prune_unclaimed(data)`.
4. Protects: as G2.
5. Bounded: no.
6. Accumulation: every deleted or overwritten extent's bytes stay on disk; claims and claim rows stay. Unbounded, so the disk eventually fills.

### G4. `generation_current`, `catalogue_complete` and the sticky remote generation
1. Location: `gates.cpp:22-28`; `metadata_server.cpp:66-70`, `known_generation`; `src/catalogue/catalogue.cpp:1690`, `:1779-1782`, `:761-762`, `:823-829`.
2. Condition: `known_generation = max(local, remote gossiped max)`. While remote > local, `generation_current` is false, the catalogue head is "not current" so `catalogue_complete` is false, and the catalogue's `repair_once` returns before adopting anything (`:761`).
3. Lost: all three gates and catalogue refresh, while a higher generation is known but unobtainable because its holder left.
4. Protects: comment at `maintenance.cpp:762-766`: "never sweep the control store with such a stale set".
5. Bounded: yes. It clears when local generation reaches the number or on restart (the max is in-memory).
6. Accumulation: none.

### G5. No-progress backoff
1. Location: `maintenance.cpp:21-26`, `:416-424`, `:300-306`, `:267-270`.
2. Behaviour: a failed metadata repair is retried at `metadata_retry_backoff`, doubling from `clamp(no_progress_backoff, 5 s, 30 s)` up to `no_progress_backoff` (default 300 s). It resets on a topology change, a remote epoch change, a new convergence demand or a local generation change.
3. Strain only. With a permanently failing repair (M5, M6), the node re-surveys every 5 minutes forever. It is never parked.
4. Bounded: yes.

### R1. Claim release
1. Location: `src/storage/retention.cpp:647-705`, `release_unreferenced`; `:173-200`, `apply_release_locked`; `:158-171`, `apply_add_locked`; `metadata_manager.cpp:1659-1683`, `retention_release_view`; `src/ledger/node_horizon_builder.cpp:26-67`; `src/ledger/retention_ledger.cpp:50-55`; `maintenance.cpp:774-788`.
2. Conditions:
   - Release needs `release_head()`: not `recovery_required`, exactly one accepted head, materialisable.
   - The release horizon is published only when `complete`: every catalogue root in `metadata_catalogue_root_set` (including conflict alternatives) and every namespace tree node must be readable, pulling from peers if not local.
   - The G2/G3 gates must be open.
   - The release itself is purely local and causal. A claim `(origin, seq)` is removed only if the head's `mutation_sequences[origin] >= seq` and the object is not in the head's live set. An add is ignored if `removed[origin] >= seq`.
3. Needs from other nodes: nothing for the arithmetic. This is the right shape for the requirement. `retention.hpp:21-24`: "A removal can therefore erase only claims causally visible to the metadata mutation that produced it; a concurrent branch re-affirmation survives." The reachability fence is added on top by the gates, not required by the claim algebra.
4. Bounded: no, via the gates. Also blocked while a catalogue root conflict's alternative objects exist only on an absent node (horizon incomplete).
5. Accumulation: `data_` / `control_` claim maps are in memory and in checkpoint shards. Shard decode caps at 1.5M objects per shard and 64 MiB per shard (`retention.cpp:33`, `:303`). An unbounded pile of unreleased claims would eventually exceed those and fail `compact_if_needed` or load with "retention checkpoint shard too large". That last step is inferred from the constants.

### R2. Per-origin state is never pruned
- `MetadataSnapshot::mutation_sequences` (one entry per node that ever committed), `node_status`, `identity_resets`, and per-object `adds` / `removed` origin maps are joined by max and never erased. No `erase` on these exists in `src`.
- A permanently lost node's ids live forever in every snapshot.
- Small, but unbounded with node churn, and it feeds the M7 roster.
- `metadata_branch_floor` is encoded, compared and reset (`metadata.cpp:1927`, `metadata_manager.cpp:1558`, `:2079`), but nothing reads it for a decision. It is vestigial.

### C1. Catalogue commit needs the metadata floor for its control objects
1. Location: `src/catalogue/catalogue.cpp:1164-1171`, `commit`; `:579-581`.
2. Condition: `replicate_control(...) < required` (= `metadata_min_write_replicas`) throws `CatalogueUnavailable("catalogue shard|manifest could not reach metadata durability floor")`. The metadata mutate is then subject to M1 and M3.
3. Lost: catalogue edits, with HTTP 503 `catalogue_unavailable` (`src/api/catalogue_api.cpp:733`, `manage_api.cpp:1096`). No retry or queue on the server.
4. Protects: comment `:1164-1165`: "A commit may reference a control object only once the metadata write floor holds it."
5. Bounded: no.
6. Accumulation: staged shards are orphaned and wait for the fenced control GC.

### C2. Control convergence wants every active node
- Location: `catalogue.cpp:613-685`.
- `replicate_control(...) < active_nodes.size()` leaves `error_code_ = "converging"` and retries in 5 s.
- Comment: "missing replicas are convergence debt, not an invalid root."
- Not a block; active set only.

### C3. Automatic catalogue-root conflict merge needs all three roots loadable
- Location: `catalogue.cpp:707-726`, `:583-611`.
- `load_root` throws `CatalogueUnavailable` if a manifest or shard cannot be made local.
- If the other branch's shards live only on the node that has gone again, the conflict stands, the catalogue stays at the base root, and the release horizon is incomplete (R1).
- Even when loadable, `merge_catalogue_snapshots` returns nothing for same-item collisions. Those conflicts need an operator (`resolve_conflict`), as do namespace-entry conflicts.

### C4. Catalogue view freezes while a higher generation is known but not held
- `catalogue.cpp:758-762`. See G4.

### C5. Scanner coordinator
- `src/catalogue/media_catalogue.cpp:2695-2700`: the coordinator is the lowest id among `membership().active()`.
- There is no lease and no election. A vanished coordinator is replaced after `dead_after`. In a partition each side has one.
- Only "destructive reconciliation" and scheduled rescans are the coordinator's (`:3899-3900`, `:3975`). Nothing blocks.

### `control_gc_step` epochs
- `catalogue.cpp:1791-1857`. The epoch is local (root-change sequence observed by this node) and needs nothing from peers.
- An unreferenced object is removed only in a later root epoch than the one in which it was first seen, and only if not claimed (`ledger_.retained`) and older than grace.
- A node whose catalogue root never changes never orphans anything: that is activity-bound, not node-bound.
- `control_gc_unreferenced_epoch_` is in memory and bounded by the control object count.

### T1. Torrent ownership (outside the area; included for question E)
- `src/torrent/torrent_coordinator.hpp:31-33`, `.cpp:853-874`.
- A claim lapses 10 minutes after its owner leaves `active()`; then other nodes claim by rank, 30 s per rank.
- A request pinned to an absent node is not claimable by others (`.cpp:938-940`) until an operator patches the pin.
- Writes need metadata (M1). While metadata is unwritable, intents are journaled locally (`hpp:108-109`).

### A1. Auth
- `src/auth/accounts.cpp:160-250`, `src/auth/users.cpp:106-117`. Pure gossip over `membership().active()`, best-effort broadcast, re-announced every 30 s, last-writer-wins by version.
- Nothing waits on or is refused for an absent node.
- User tombstones are kept forever and the whole table is gossiped.
- A long-absent node can resurrect a deleted user only if its record's version is strictly higher than the tombstone's. Inferred from `incoming_wins`.
- Sessions gossip only `recent(gossip_ttl_, 64)`. Whether an old session revocation reaches a late returner was not traced (unconfirmed).

---

## Answers

### A. One of two nodes down, `metadata_min_write_replicas = 2`
The remaining node can commit nothing at all. `mutate_impl` throws `MetadataNotReady` at `metadata_manager.cpp:1699-1700` before doing anything, and `publish_replica_state` reports `read-only` (`:118-119`). The survivor's own maintenance still succeeds at `repair_once` (single head, replicated to itself), so the state is stable but not writable. What each caller does:

- **FUSE namespace operation**: journaled and retried. `MetadataNotReady` is not an `FsError`, so `retryable_backend_error` returns true (`src/fuse/fuse_frontend.cpp:193-215`). Backoff runs 50 ms to 5 s. Past the budget it is reported blocked but "keeps retrying at the ceiling" (`:3168-3197`; policy at `src/config.hpp:141`). It is never dropped; only an operator may skip it.
- **FUSE file data publication**: retried with backoff 250 ms to 30 s (`config.hpp:136`). When the retry budget is exhausted the file is parked (`fuse_frontend.cpp:3989-4034`) and is only re-admitted by an operator through `manage/filesystem/parked-publications` (`:6526-6544`).
  - `RetryPolicy::max_failing_duration` defaults to 1 hour (`src/retry_policy.hpp:24`). `publication_retry` is built with four arguments, so that default applies unless config overrides it at `config.cpp:339`.
  - On that reading, an outage longer than an hour parks every pending file and they stay parked after the node returns. I did not trace whether anything else unparks them.
  - What the writing application sees at `write()` / `close()` time was not traced.
- **Catalogue edit**: HTTP 503 `catalogue_unavailable`; no retry (C1).
- **Conflict resolution API**: HTTP 503 `metadata_unavailable` (`src/service/service.cpp:318-322`).
- **Ingest job**: blocks as `metadata_unavailable` and retries after `blocked_retry` (`src/acquisition/ingest.cpp:1243-1259`).
- **Torrent actions**: intents journaled locally and published later. `add` returns 503 `metadata_unavailable` with cluster scope (`torrent_coordinator.hpp:60`, `.cpp:290`, `:513`).
- **Maintenance's own commits** (tombstone erase or stamp, baseline): throw and are swallowed at `maintenance.cpp:1055`.
- **Changing the floor to 1** does not help while the peer is down (M2).

### B. Lone node, floor 1, partitioned
- **Can it author?** Yes. `active` is {self}, the floor is met, the commit is stored and accepted locally. Two conditions apply:
  - Objects must be claimable on `min_write_replicas` active holders (M3; default 1, itself).
  - For tree and catalogue nodes, `replicate_control >= 1`.
  - Reconciliation is explicitly not a prerequisite (`:1713-1725`).
- **Is the merge automatic on heal?** Yes in the normal case.
  - `repair_once` and every `read_group` survey heads, pull history (`import_history_from_peer`), and accept the certificates.
  - The lower-hash-primary rule makes every reconciler mint the identical merge commit (`:1255-1264`).
  - The merge is three-way against `history_common_ancestor` and is published at the floor.
- **What is not automatic**: genuine same-path or same-item conflicts. They keep the base value visible and stand as conflict records until a later mutation rewrites the subject (`prune_superseded_conflicts`) or an operator resolves them (`metadata.cpp:2078-2080`, `:2106-2127`; C3).
- **What can make the merge impossible**:
  - "no known common ancestor" (history compacted below the divergence point: H2)
  - "common ancestor cannot be reconstructed" / "merge head cannot be materialized" (a delta chain whose base is missing: M5, M10)
  - tree nodes of either side not obtainable
  - the merged head's objects not claimable (M3)
  - the floor unavailable (M4)
  - policy scalars changed differently on both sides, or conflict records diverged (`std::runtime_error`)
  - a peer on a different configured floor (M2: its heads are never even surveyed)
- **How long is history kept?** Forever, until a checkpoint round succeeds (H1). There is no time-based truncation.
- **Does truncation consider absent nodes?** It considers every node in the persisted known set: all must be reachable and at the same sole head. It does not consider nodes outside that set (reset-out or never learned): H2.

### C. Is a never-returning node ever dropped automatically?
No (N1). Only an operator identity reset removes it (N4). Until then:

Blocked forever:
- tombstone removal (G1)
- CONTROL claim release, control GC and control claim-row pruning (G2)
- DATA claim release, physical reclamation and DATA claim-row pruning (G3)
- history truncation (H1)
- the retention baseline, if the node is on the migration roster. That one is not released even by a reset (M7).
- the `cluster_stable` observation event (`maintenance.cpp:710-718`)

Growing forever:
- `snapshot.garbage`, and therefore snapshot and merge size
- every deleted or overwritten DATA object's bytes
- superseded namespace-tree nodes and catalogue shards
- claim maps and their checkpoint shards (hard caps noted in R1)
- the metadata history file and index
- the per-origin entries in R2

Kept trying forever:
- the heartbeat loop dials it every `heartbeat` (`cluster.cpp:1083-1096`), with debug-level logging
- the new-joiner inheritance in N3

Not blocked: authoring, merging among the nodes present, non-destructive repair and re-replication, catalogue edits, auth.

### D. A node returning after a long absence
- **What it does**: reconnects, is observed directly, and its heads are surveyed by peers and vice versa. If its head is an ancestor of the current one, it adopts the current head (history pushed or pulled; full-record fallback at `:873-878`). If it authored anything, the heads are siblings and merge (answer B).
- **Can it resurrect deleted entries?**
  - Not through the merge when the common ancestor is known. An entry deleted on the main branch and untouched on the returning branch has `l == b` and takes the deletion.
  - If the returner modified an entry that was deleted elsewhere, the result is a conflict with the base (pre-delete) value re-installed and visible until resolved (`metadata.cpp:2078-2080`).
  - If the ancestor is not known, the merge does not happen at all (M5/H2) rather than resurrecting.
- **Does it need tombstones that may have been dropped?** Namespace deletions are not carried by tombstones; they are carried by the three-way merge base, i.e. by history.
  - Object tombstones (`garbage`) are merged by plain union (`metadata.cpp:2129-2143`). The returner re-introduces tombstones the others already erased. The comment says "an extra tombstone is safe"; they are re-collected later. A dropped tombstone is not needed for correctness.
  - What it needs is history back to the common ancestor. That is guaranteed only while it stayed in the known set, because that is what blocks truncation.
  - Objects it still references are protected the same way: nothing is reclaimed while it is known-absent. If it was reset out, bytes may be gone. A conflict's base entry or its own stale entries may then reference extents nobody holds (inferred).
- **Is there a staleness check?** None found.
  - There is no maximum age, no epoch on rejoin and no comparison of its head against a floor.
  - The identity reset only filters gossip older than the reset; direct contact re-admits it (`membership.cpp:212-215`).
  - The implicit protection is that the gates stay shut while it is known-absent.
- **Its own GC while stale**:
  - It needs all its known nodes directly reachable, and `generation_current` shuts its gates as soon as it hears a higher generation.
  - Its claim release uses only its own head's clock.
  - It can delete local copies of objects it considers garbage. Newer claims for such an object cannot exist on its store, since it was away.
  - That window looked safe on reading; not tested.

### E. Quorums, majorities, leaders, leases
- **No majority or quorum in the live path.** The only majority arithmetic is legacy: `metadata_voters.size() / 2 + 1`, used as the floor for snapshots that predate the write-floor policy. It appears in `metadata_manager.cpp:725-726`, `:1358-1360`; `metadata.cpp:3248-3249`; and the legacy `dht.metadata_replicas` config key at `config.cpp:201-203`. A legacy-rooted namespace therefore needs a majority of its old voters' count to commit its first transition.
- **Unanimity over the known set** (stronger than a quorum): history checkpoint (H1) and the three gates (G1-G3). There is no fixed proposer; any node may propose, so no role holder can block.
- **Unanimity over a roster**: retention baseline (M7).
- **Unanimity over the active set**: `repair_once` (M6), catalogue control convergence (C2, soft), virgin formation survey (M8).
- **Leader-like roles**:
  - the virgin founder is the lowest active id (`maintenance.cpp:386-398`); followers have a timed retry if it vanishes
  - the catalogue scanner coordinator is the lowest active id (C5); it fails over after `dead_after`
  - the torrent claim owner (T1): 10-minute absence lease, then others claim; a pin to an absent node blocks until an operator changes it
- **Locks**: `mutation_mutex_` / `reconciliation_mutex_` are process-local. No distributed locks or leases found in the audited area.

### F. Anything keyed on a fixed node count or roster
- `metadata_min_write_replicas` (default 2) is an absolute count, not a fraction, and is not reduced when membership shrinks (M1). It is also stamped into every snapshot (`metadata_write_replicas_required`) and into every acceptance certificate (`required`), and used as a compatibility key between nodes (M2).
- `min_write_replicas` (default 1) is the absolute DATA claim floor per object (M3).
- `replication` (default 3) is the placement target; it is `min(replication, nodes)` on the write path (`distributed_store.cpp:237`), so it adapts.
- `metadata_participants` is a fixed roster for the baseline (M7). The code calls it "a migration roster, not authority" (`:1374-1376`), but it is a hard gate on destructive GC.
- `HistoryCheckpointProof.participants` and `epoch` are the full known roster at proposal time (H1).
- Legacy `metadata_voters` are a fixed voter list with a majority floor until cleared (`:1362`, `:1407`).
- The persisted known-nodes set is the roster for `all_known_reachable` (N1, N2).
- `MetadataAcceptance.replicas` (witness ids) is evidence only. Witnesses are used as extra history sources (`:1098-1109`) and nothing requires them to be present later.
- `mutation_sequences` / claim origins are keyed by node id forever (R2). They are not gates, except as input to the M7 roster.

## Not covered or unconfirmed
- Whether `node_.call` can reach a peer that is active only by gossip (affects M6, N2).
- `merge_tree_backed_snapshots` failure modes when a tree node is missing (`namespace_tree.cpp` was not read in full).
- The callers of the manual repair planners at `metadata.cpp:2178-2246`.
- FUSE behaviour as seen by the writing application; whether anything other than the operator unparks a parked publication.
- `src/ledger/availability.cpp` and `predicate_query.cpp` were not read.
- Whether the live cluster's baseline is complete (M7).
- H2 and the sticky-generation part of M11/G4 are derived from reading the code, not reproduced.