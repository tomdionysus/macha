# Verification of the design's three open points

Read from the code at `6d728a9` on 2026-10-05. Nothing was run.

## 1. Do a node's commits form a chain? On the normal path yes; four paths break it.

Holds because:

- `mutate_impl` holds `mutation_mutex_`; one node never authors two siblings at once.
- The sequence is `max(persisted counter, head's own entry) + 1`, written durably before use.
- `publish_commit` stores and accepts locally first, so a crash cannot leave a peer holding an
  accepted commit the author lacks.
- With several local heads `mutate_impl` reconciles first and never authors on one of them.
- Merges join the clock by max and stamp nothing of their own. A merge reserves a sequence only as
  its retention dot.

Breaks:

- (a) The accept loop keeps sending the certificate to peers after the local accept failed, then
  throws. A peer then holds an accepted commit its author does not.
- (b) `recover_from_seed` quarantines checkpoint, journal, history and heads but keeps the sequence
  counter; the node resumes from a seed that may predate its own commits.
- (c) `install_migrated_head` replaces the head set and keeps the counter.
- (d) Partial state loss or rollback under the same node id (point 2).

Changes required:

1. Accept locally first; if that fails, throw before any peer sees the certificate.
2. Persist the highest own sequence this node has had accepted. If the head chosen for a mutation
   carries a lower entry for this node, do not extend the chain: start a new author id at 1.
3. When authoring with several local heads (which the design allows when a merge cannot complete),
   author on the head that carries this node's highest own sequence.

Also: the FUSE mutation identity uses a separate origin whose clock is monotonic but not
contiguous. Entry dots must use the node's author id, never that origin.

## 2. Can a sequence be reused under one id? After a full wipe no; after partial loss yes.

- The node id is 16 random bytes in `<state>/node-id`; a wiped node gets a new one.
- The counter is `<state>/metadata/mutation-sequence.meta`. A missing file reads as 0; a corrupt one
  throws.
- If `node-id` survives and the counter or `metadata/` is lost or restored from an older copy,
  sequences above what the surveyed head carries are reissued.

Covered by change 2 above, plus: a present node id with an absent counter and a head entry for this
id is a new incarnation.

Claim dots use the same counter and the same rule, so the retention release already depends on
points 1 and 2. These are existing hazards for retention as well as for the planned merge.

## 3. Why was the baseline needed?

- An object the node's own head references is protected by the live set alone; claims are not needed
  for it.
- The baseline protects objects this node holds that its own head does not reference and nobody has
  claimed: legacy objects referenced only by another node's branch, and tree nodes written by a
  re-root that a node on the old head would see as unreferenced.
- So the baseline is a cross-branch protection, and under the design that protection is the horizon.

What the sweep reads today:

- DATA bytes go when the object is not live, not tombstone-protected, unclaimed, and older than
  `max(garbage_grace, no_progress_backoff)` by its last local put or touch time.
- Tombstone protection compares the author's wall clock with the local one.
- There is no locally observed "unreferenced since" time. An old object that becomes unreferenced
  without a live tombstone goes on the first sweep that sees it unclaimed. The design needs that
  timestamp added.
- CONTROL uses a root-epoch fence held in memory, advanced only by a catalogue root change, plus the
  same age check.
- Tombstones only delay deletion. Erasing one on age alone breaks nothing; a merge may bring an old
  one back, already mature, to be erased again.
- The inventory is built from `converged()`, the release from `release_head()`; whether these can
  differ was not traced.
