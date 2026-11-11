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
| Repair slept for hours with work left (found 2026-11-10 from the observations: maintenance wakeups 0 for up to 31 h on both nodes, each sleep starting as a viewer arrived) | 0.93.6: a repair step that yielded looks again after an interval; the busy wake and the share's turn read the pass's stale start state, and a credit past an extent set no deadline, so nothing else woke it. Test `maintenance_trace/test_repair_resumes_after_yielding_to_a_viewer_that_arrived_mid_step` fails before and passes after | laptop: the case, maintenance_trace 11/11, storage_metadata 71/71; macnessa gate 755 + 20 + 21; deployed 2026-11-10, inverbeg 20:16Z, macnessa 20:18Z |
| macnessa's control store full (below) | `storage.metadata.limit` 4G to 8G on both nodes, operator, 2026-11-08 | live; config backups `/etc/macha/macha.yaml.pre-8g-*` |
| Repair credit-limited to a tenth of the link (below) | `maintenance.idle_bandwidth_fraction` 0.10 to 0.5 on both nodes, operator, 2026-11-08 | live; backups `.pre-fraction-*`. 3.6 to 3.9 MB/s at 13:20Z was one burst: averaged over the next 53 h inverbeg gained 0.8 MB/s, because of the sleep above |

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
14. **Lost wakeups** (read: every notify site in `src/` audited 2026-11-08, after Dannan's `7b8b35f` found one in
    its `Node`; each notifier and waiter below read by this session unless marked *agent*). The defect: a waiter
    tests its predicate under mutex M and blocks; the notifier changes what the predicate reads without M and does
    not take M between the change and the notify, so a change landing between test and block is missed. The fix
    everywhere is one rule: change the state under M, or take and release M before notifying (Dannan's
    `wake_loops`); wait for a stop through the `stop_token` overload; a predicate must read what its notifier
    changes; a time-based predicate needs a timed wait.
    - **Stop can hang until systemd's kill** (plain wait whose predicate reads the jthread stop token; `request_stop`
      and notify outside M): `~DistributedStore` local writer (`distributed_store.cpp:1185-1187` vs `:3159`);
      `RpcServer::stop`'s four worker pools (`net.cpp:4140-4148` vs `:4498`, `:4518`, `:4539`, `:4628`);
      `~DurabilityDomain` (`durability_domain.cpp:75` vs `:218`); the FUSE durability worker
      (`fuse_frontend.cpp:5310` vs `:2355`); FUSE spool admission (`stopping` set at `:5279`, notify `:5283`, vs the
      untimed wait `:1272`; a broker write parked there holds `stop()` at the broker join, `interrupt_waits` does not
      reach it); the RPC session reaper on a double miss (`net.cpp:4401`, `:4695` vs `:3455`), narrow.
    - **Work stalls until an unrelated wake:** torrent alerts (`torrent_manager.cpp:159-161` vs `:1274`; libtorrent
      fires its notify only on an empty queue turning non-empty, so a miss is not repeated); the torrent resume
      listener (`:155`, called from `ingest.cpp:937`; the predicate's ingest read *agent*); FUSE deferred-retry due
      times (`fuse_frontend.cpp:2910` vs `:3923`); an fsync on a parked inode the operator abandons (`:3467` vs
      `:2262`; at stop `waits_interrupted` rescues it), and `backend_error` set with no notify at all (`:2316`,
      `:2992`, *agent*); hydrator wakes (`hydration.cpp:720-722` vs `:842`); prompt-replication retries, a
      time-based predicate on an untimed wait (`distributed_store.cpp:1237`); a `PeerConnection` writer after its
      reader fails lingers until `close()` (`net.cpp:1714` vs `:1539`); an extent submitter misses cancellation
      until the next pop (`filesystem.hpp:615` vs `filesystem.cpp:1472`, *agent*).
    - **The notify can never satisfy the predicate** (not a race, every time): media-information requests on an
      idle service (`media_information.cpp:177`, `:193` vs `:508`; the predicate never reads the hint queue, so a
      request waits for the next metadata generation, `maintenance.cpp:1401`, which masks it on a busy cluster);
      `TorrentCoordinator` API wakes (`torrent_coordinator.cpp:180`, predicate constant false: every request waits
      for the 2 s pass); hydration fetch completion (`hydration.cpp:611`, noticed at `config_.interval`, *agent*).
    - **Bounded:** telemetry demand, one `telemetry_interval` (`cluster.cpp:958` vs `:999-1002`); a dial request,
      one heartbeat, during which the requester's reverse dial may time out (`net.cpp:2733-2735` vs `:2830`); FUSE
      write and metadata admission against stop and a new durable prefix, 200 ms (`fuse_frontend.cpp:5284`,
      `:5285`, `:2347`, *agent*); startup readiness, at most 1 s (`service.cpp:542-553` vs `:471`).
    - **Unsettled:** the plain `namespace_cv` wait in `replay_data_quantum` (`fuse_frontend.cpp:3514-3523`) against
      stop, if `namespace_loop` can exit without its final notify; the DATA arbiter seeing `local_copy_cancelled_`
      with no notify (`data_work.hpp`, bounded by the no-progress window if non-zero); cancels into
      `retained_memory.hpp:263`; `finish_loader_admission`/`wait_for_loader_admission`, possibly dead code.
    - Clean: about 150 sites. The connectivity loop is clean. These may bear on past slow stops; not shown.
15. **Found by Dagda in Macha's code, 2026-11-08 and 09** (Dagda's reading and tests, cited lines not re-read here;
    the operator ruled fixed in Dagda now, in Macha after the freeze):
    - **A pack compaction can lose an object** (Dagda pinned it with a test that failed first). A packed put's record is in the
      active pack but not yet indexed; compaction counts it dead, can take the active pack as its victim and unlink it,
      and the object is gone while the held ledger says held. On a remove, the same gap brings a deleted object back.
      Macha `local_store.cpp:821-867`, `:1694`, `:1700-1714`, `:1842`. Dagda's fix: compaction waits, under the pack lock,
      for every written record to be indexed.
    - **`StoragePool::get` stops at the first backend's empty answer** (`storage_pool.cpp:591-610`), so a copy on a
      second backend is not read. Open in Dagda too.
    - **A sweep pass forgets deferrals from its earlier slices**: `gc_step` reports `deferred` for its own slice only
      (`storage_pool.cpp:1030-1083`), and the loop reads it from the slice that completes the pass
      (`maintenance.cpp:1079-1087`), so the pass ends "complete; waiting for an event" with immature garbage still on
      disk until the next event. Garbage is late, never wrongly deleted.
16. **`namespace_service/test_a_stale_tombstone_in_a_batch_is_erased_once` ran past its 30 s wait once**, in a full
    laptop run of the port branch at load 5 (2026-11-11, `build-port`); alone it passed in 7.3 s, and it passed in two
    earlier full runs of the same code and in macnessa's 0.93.6 gate. A laptop under load gives no timing verdict;
    reopen as P0 if it fails again, above all on macnessa.
17. **Seen, not looked into:** `ls /mnt/machamedia` timed out twice shortly after a restart, then answered in ms;
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
