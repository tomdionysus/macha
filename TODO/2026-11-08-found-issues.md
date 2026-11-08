# Issues found, 2026-11-06 to 2026-11-08

Everything found while answering the port question, taking the Pi baselines and reviewing the libraries'
work. Each item says how it was found (run, or read only), where it stands, and who owns it. Macha's code is
frozen until the port starts; "after the freeze" means fix it in Macha then, under the operator's rule that a
genuine Macha defect is fixed in Macha first and the libraries match the fix.

## Fixed

| Issue | Fix | State |
|---|---|---|
| A damaged `history.log` length word read as a torn tail, so opening a node cut every later intact frame; a length over 2 GiB sent the node to seed recovery (found by Dagda's S3 reading) | 0.93.5: a frame that does not authenticate means scanning for the next one that does (`9153011`) | develop, pushed; laptop tests only (71 of 71 `storage_metadata`); not tagged, not on the nodes |
| A failed read in that scan, or in the main loop, also cut good frames (found by Dagda mirroring the fix) | A failed read cuts nothing; unread frames are read at the next open (`98b816d`) | as above; the scan's failure has a case, the main loop's is by reading (Dagda tested it in its copy) |
| macnessa's control store full (below) | `storage.metadata.limit` 4G to 8G on both nodes, operator, 2026-11-08 | live; config backups `/etc/macha/macha.yaml.pre-8g-*` |
| Repair credit-limited to a tenth of the link (below) | `maintenance.idle_bandwidth_fraction` 0.10 to 0.5 on both nodes, operator, 2026-11-08 | live; backups `.pre-fraction-*`; measured 13:20Z: 3.6 to 3.9 MB/s each way, against 0.44 (`build/baselines/repair3-*.txt`) |

## Macha, open, after the freeze

Ordered by what they cost a user.

1. **Heap corruption on macnessa** (run, not explained). Four crashes from 17:18Z to 18:11Z on 2026-11-06, all
   inside `malloc`: two SIGSEGV at one libc instruction, two SIGABRT with `corrupted double-linked list (not
   small)`. Three of four came seconds after a recursive delete through the FUSE mount; three of four stacks
   were building the large ingest-job or catalogue-hint JSON, which only macnessa holds. inverbeg ran the same
   workload without a crash. macnessa had about nine times inverbeg's undervoltage events. The cores were deleted
   on the operator's word, 2026-11-08; their stacks had been read; capture stays on. **Operator: left until after the port**, and
   attributed to undervoltage or to the FUSE crashes. The reproducer to try then: 200 small files written and
   deleted through macnessa's mount, repeated, under an ASan build.
2. **`rm` in the FUSE mount with a filter can crash macha** (operator's report, 2026-11-06). Not reproduced
   here; may be item 1.
3. **A full control store is silent and costly** (run). macnessa's `metadata-objects` reached its 4G limit at
   about 21:16Z on 2026-11-06. Every control put was refused (`LocalStore::physical_space_available_locked`),
   namespace writes on macnessa failed (`namespace node could not be written locally`), and two catalogue
   shards were retried several times a second with no backoff: 73,842 warnings, about 18% of a core, which also
   halved repair's credit through `cpu_scale`. Status said `healthy` with no condition throughout. inverbeg was
   at 94%. What filled the store was not measured; the TV import's tree and extent nodes held for CONTROL GC's
   30-day grace is the guess. Fix: a condition in Status when a store refuses puts; back off work that cannot
   complete; measure what the store holds.
4. **Cancelling an ingest job does not stop its worker** (run). `enqueue_catalogue_hints` (`ingest.cpp:1270`)
   checks neither cancel nor stop; the TV job kept rewriting a 5 MB `hints.json` after `cancel` until macha was
   restarted (2026-11-06 15:51Z). Hints not yet submitted were lost, so some imported files may not be catalogued.
   The rewrite per submission is the import-end defect already in `BACKLOG.md`.
5. **Claims on peers are best effort, and nothing restores a lost one** (read, Dagda's trace, citations checked).
   `MetadataManager::replication_loop` drops claims at stop (`metadata_manager.cpp:131`), on a failed call
   (`:136`) and over the byte cap (`:101`); `retain_on` swallows failures (`distributed_store.cpp:840-843`),
   discarded at `node_services.cpp:549`; `ClaimWalk::step` reads only claims a node already has. The data is
   protected by the peer's own head and two 30-day gates. **Window W1**, not run: a peer's unclaimed object past
   its grace, re-referenced by the sender with no touch, is swept before the peer learns the generation; cost one
   replica until repair. Recommended left until after the port; the operator's call.
6. **A corrupt `author.meta` stops every write with no recovery** (read, Dagda): it throws out of
   `reserve_mutation_dot`. Against the self-heal rule.
7. **Replay compares exception text** (read, Dagda): `metadata.cpp:2302`, `:2627`. Against "never match English
   text". (0.93.5 removed the one in `load_history`.)
8. **`accept_loop` spins on a persistent accept error** such as EMFILE (read): `net.cpp:4168-4176`, on its own
   thread.
9. **The TV import filed real episodes under `/Movies`** (read from the job's plan): about 430 episodes probe as
   films (`House MD Season 6 Episode 06`, `S01 - E15`, Star Trek TNG); 1,286 such files are already in `/Movies`.
   Movie folder names such as `Blade Runner ( (1982)` (`media_catalogue.cpp:587`).
10. **`macha-metadata-dump`**: aborts on a state with no `heads.meta` (a seed-recovered node); its `tombstones=`
    counts the inline `garbage`, not `tombstone_batches`, so it reads 0 on heads with batches.
11. **Smaller, read only:** `reanchor_history` erases without subtracting bytes (`metadata.cpp:3523`), no way found
    to reach it; a policy commit may hold the mutation lock through a peer's stall window; the swap at
    `metadata_manager.cpp:1213-1215` cannot fire; a fold may rotate to a new author it did not need (R13, Dagda);
    `set-aside.meta` is unsealed; nothing writes `journal.log` frames now, so its reader and `compact()` act only on
    old files.
12. **Stale comments and docs** (read): `metadata_manager.hpp:201-202`, `:242-243` and `claim_walk.hpp:32` describe
    restoration and a cursor that do not exist; `metadata_manager.cpp:1151-1153`, `:1449-1450`,
    `maintenance.cpp:158-159`, `metadata.hpp:78-79`, `docs/metadata.md:71`.
13. **Tests:** `test_metadata_seed_sibling_replays_after_restart` was retired and the preferred-head rule has no
    named Macha case (Dagda writes one); `test_rpc_cluster:2999` seeks a placement `discover_or_form` no longer
    reads; rpc:3212 can flake to a third run; two weak cases in `test_namespace_tree.cpp` (lines 218, 746).
    `storage_v18/test_retain_data_batches_a_large_publication_within_bounded_time` exceeds its 120 s bound with
    its temporary directory on macnessa's USB disk.
14. **A telemetry demand can be lost for one interval** (read, found through Dannan's `7b8b35f`):
    `NodeRuntime::signal_telemetry_refresh` (`cluster.cpp:958`) notifies without `telemetry_wait_mutex_`, so a
    demand made between the loop's predicate test and its wait (`cluster.cpp:999-1002`) waits for the next
    `telemetry_interval`. Fix: take and release the wait mutex before notifying, as Dannan's `wake_loops`. The
    connectivity loop has no such race. Macha's other ~180 notify sites are not audited.
15. **Seen, not looked into:** `ls /mnt/machamedia` timed out twice shortly after a restart, then answered in ms;
    remux `playback_pipeline_start_failed` ("timed out waiting for first fragmented-MP4 segment") 3 of 13 attempts
    on inverbeg, 1 of 13 on macnessa, during the baselines.

## The live cluster

- **Replication is slow by policy, not stalled.** Repair's credit is `bandwidth x idle_bandwidth_fraction x
  cpu_scale` (`maintenance.cpp:437`): measured 0.43 MB/s from macnessa and 0.64 MB/s from inverbeg against a
  link estimated at 8 to 9 MB/s, with macnessa's `cpu_scale` about halved by item 3's retry loop. inverbeg held
  1.68 TB to macnessa's 6.3 TB on 2026-11-08. Raised to 0.5 that day: 3.6 to 3.9 MB/s measured at 13:20Z,
  nearly every pass gated by credit. A viewer on either node still paces repair to its 5% share.
- **macnessa's NVMe** reports SMART `FAILED` on critical warning 0x02 (temperature) at 0 C, with no media errors
  and 3% used. Read as a broken sensor; not acted on.
- **Undervoltage** on both Pis (`throttled=0x50000`), macnessa about nine times inverbeg. The operator has ruled
  it is not a gate for the port and is not to be raised again.
- **The laptop's `~/.ssh/known_hosts` disappeared** before 2026-11-08; the operator re-added both nodes
  (macnessa ed25519 `SHA256:UGl8FRfNakQF6Eqygn2RTJ5H2b9ArJtQVau9EeN7wqc`, inverbeg
  `SHA256:MP0wFfZjstDq9w6BxpD16YIWzdO+Bq+xpwmGjRWNrlk`). macnessa is reached at 10.44.1.50.

## Dannan, found in review (0.5.0, `df839fe`)

1. **The acceptor could stop for good** (read): a throw in `admit` skipped the re-arm and `end_operation`
   (`server.cpp:135-139`); under a supervised `run()` the node would take no new connections and `stop()` would
   hang. **Being fixed by Dannan**, on `feature/accept-faults` (`8dd73d2`, 2026-11-06), with a timer pause after
   accept errors.
2. **A roster fsync could run on the shared io threads** (read): `Node::observe_direct` reaches
   `SequencedFile::write`. **Being fixed**: the write moves to a worker, an identity reset still durable before
   it answers. Both need a 0.5.1 tag, at the operator's word, not yet given.
3. **The handshake timeout is open** (run): 2 of about 11 full macOS TSan runs, "timed out at the handshake":
   TCP connected, no answer, neither end closed. Not explained by 1 or 2.
4. **Relay bulk transfers fail under Debian TSan**, not on Macha's path. Fixed toward 0.5.1: numbered chunks
   with pipelining (`7e35338`); a close that waited for ever on a chunk lost with a session (found in review),
   bounded (`0463790`, `8b1c46e`, `34e8080`, each bound's test asserting its own counter); a lost wakeup in
   `Node` that held a forced connectivity refresh for a heartbeat (`7b8b35f`); the `shut()` handler fault
   (found in review, `35e9ed7`). All read and passed by this session. Tag waits on macOS and Debian TSan once
   each on a quiet host (the last run was at load 46 on 12 cores) and the operator's go.
5. **Exposures to carry into the port:** logging blocks on whichever thread logs, io threads included; the io
   thread count must be set deliberately, unmeasured; `free_port` can collide across processes in tests.

## Dagda, found in review

Dagda reproduces Macha's behaviour for every Macha item above, as the freeze requires. Its own: it found and
mirrors 0.93.5 and `98b816d`; its two differences to close are a CONTROL claim race and claims refused by
admission; it has run nothing on arm64 and nothing under GCC since S2.
