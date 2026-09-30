# P evidence: authoritative presence

Branch `experiment/object-ledger-p`, cut from `experiment/object-ledger-t1`.
In-process only; the cluster measurement is T5's.

## What changed

- **`PresenceIndex`** (`src/storage/presence_index.{hpp,cpp}`), a primitive:
  no threads, no I/O, no lock of its own. Events installed, observed,
  forgotten, pruned, listed and warmed; a three-valued answer (present,
  absent once authoritative, unknown before). It replaces `LocalStore`'s
  `present_loose_` and `pruned_loose_` and adds what they lacked: ids
  forgotten while warm-up runs are not republished by a listing taken
  before the removal.
- **`LocalStore::has()`** answers from the index under the index mutex.
  Once warm-up has listed every object directory without error, a miss is a
  definite no: no per-object lock, no device. Before then, or if the listing
  failed, a miss is asked of the device as before (object lock, `file_size`,
  prune of a zero-byte file). A put publishes presence after its file is
  installed (after the rename), so a put in progress reads as absent and
  has() no longer waits for it.
- **The accounting scan** marks an object present only if its file still
  exists under the object's lock: a remove between the scan's listing and
  its insert is no longer undone. One more stat per loose object, in the
  background scan that already stats each file.
- **Backend offline and re-adoption** needed no code: a deactivated backend's
  `LocalStore` is retired, and re-adoption builds a new one that warms from
  the directory. Tested at the pool.

## Tests (`tests/test_presence.cpp`)

- The primitive against a model written from its header's table: every
  history of up to five events over two ids (14^5 histories, each checked
  after every event), plus the warm-up race cases by name.
- A put blocked mid-write: has() answers absent within 500 ms without
  waiting on the put's lock, and present once it completes.
- A put that fails mid-write is never present; the next put succeeds.
- After warm-up the device is not consulted: an object file removed behind
  the store's back is still reported (the store wrote it), one planted
  behind its back is not.
- Presence exact across a restart (a removed object stays absent).
- A backend renamed away (offline) and back (re-adopted) at the pool.

## Mutation record

`build/claude-p-mutate.py`, filter `presence`, laptop, 2026-09-30:

- installed keeps pruned -> KILLED by presence/test_presence_index_matches_its_model_over_every_short_history
- installed keeps forgotten -> SURVIVED
- forgotten always remembered -> KILLED by presence/test_presence_index_matches_its_model_over_every_short_history
- forgotten never remembered -> KILLED by presence/test_presence_index_matches_its_model_over_every_short_history, presence/test_presence_index_warmup_race_cases_by_name
- pruned not marked -> KILLED by presence/test_presence_index_warmup_race_cases_by_name, presence/test_presence_index_matches_its_model_over_every_short_history
- listing ignores pruned -> KILLED by presence/test_presence_index_warmup_race_cases_by_name, presence/test_presence_index_matches_its_model_over_every_short_history
- listing ignores forgotten -> KILLED by presence/test_presence_index_matches_its_model_over_every_short_history, presence/test_presence_index_warmup_race_cases_by_name
- warmed keeps forgotten -> KILLED by presence/test_presence_index_matches_its_model_over_every_short_history
- warmed not authoritative -> KILLED by presence/test_presence_index_matches_its_model_over_every_short_history, presence/test_presence_index_warmup_race_cases_by_name, presence/test_has_answers_a_put_in_progress_without_waiting_for_it, presence/test_has_does_not_consult_the_device_after_warm_up, presence/test_presence_is_exact_across_a_restart
- authoritative miss unknown -> KILLED by presence/test_presence_index_matches_its_model_over_every_short_history, presence/test_presence_index_warmup_race_cases_by_name, presence/test_has_does_not_consult_the_device_after_warm_up, presence/test_has_answers_a_put_in_progress_without_waiting_for_it
- has asks the device after warm-up -> KILLED by presence/test_has_does_not_consult_the_device_after_warm_up, presence/test_has_answers_a_put_in_progress_without_waiting_for_it
- has waits for the object lock first -> KILLED by presence/test_a_backend_offline_and_readopted_leaves_presence_exact, presence/test_has_answers_a_put_in_progress_without_waiting_for_it
- warm-up never finishes -> KILLED by presence/test_has_does_not_consult_the_device_after_warm_up, presence/test_has_answers_a_put_in_progress_without_waiting_for_it, presence/test_presence_is_exact_across_a_restart
- put does not publish presence -> KILLED by presence/test_a_backend_offline_and_readopted_leaves_presence_exact, presence/test_has_answers_a_put_in_progress_without_waiting_for_it, presence/test_has_does_not_consult_the_device_after_warm_up, storage_v18/test_has_is_a_cheap_presence_check_not_a_decrypt

The survivor (`installed` clearing the forgotten mark) was an equivalent
mutant: the mark is read only by a listing of an absent id, and an
installed id can only become absent again through `forgotten` or `pruned`,
which set the mark anew while warm-up runs. The redundant erase was removed
rather than kept untested; the model and the header's table say so.

## Acceptance

- has() never reports a partially written object: by construction (published
  after the rename), and tested blocked and failed mid-write.
- Never touches the device after warm-up: the planted and removed files.
- Backend offline and re-adopted: exact.
- T1 traces identical: every fixture passes unchanged (632/632 laptop,
  632/632 fi-1).
- Microbenchmarks, fi-1, five runs each against T0's (`../t0/bench-fi-1.txt`):

| operation | T0 median | P median | change |
|---|---|---|---|
| `has`, present | 897 ns | 475 ns | -47% |
| `has`, absent | 4,129 ns | 460 ns | -89% |
| claim walk, per object | 950 ns | 566 ns | -40% |

  The GC sweep has no in-suite benchmark; it calls has() only through
  `prune_unclaimed`, so it cannot have got dearer. Its cluster figure is
  T5's, against T0's `maintenance.gc.per_object_ns`.
- Suites: laptop 632 (+ runtime 17); fi-1 632 + 17 + 21.
- Left for T5 (on the cluster): a backend unmounted under load and remounted;
  quantum-commit claim latency on a saturated disk.
