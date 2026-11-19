# The mixed-version window, as analysed before the cutover plan changed (2026-11-19)

The maintainer ruled afterwards that both nodes stop together for the cutover, so no window exists. This is the record of the
analysis and the smoke test, kept for a later rolling upgrade across a library's wire change.

## The analysis

From live-b's stop until live-a runs 0.95.0, the nodes run different stacks: live-a 0.94.2 (Dannan 0.5.3,
Macha's store, v18 disk), live-b 0.95.0 (Dannan 0.9.2, Dagda's store, v19 disk). live-b is down for its migration
(Dagda's estimate for live-a's disk is about 22 h; live-b's is smaller, unmeasured), during which live-a serves
alone, single-copy writes (degraded operation is accepted). What happens once live-b is back on 0.95.0 depends on
the store answer still awaited from Dagda and on a two-version smoke test:
- **Transport (Dannan, from code, 2026-11-19; never run):** 0.5.3 and 0.9.2 share the wire (message, codec, record
  encoding identical; versions 1 and 2), and 0.9.2 admits a peer with no agreement table, so live-a is admitted.
  Two conditions: live-b keeps the cluster key and gets no certificate (a certificate without `use_cluster_key`
  offers only wire v2 and the pair refuse each other); live-b's advertise-with-deadline reports live-a `missed`
  until gossip carries the record (harmless unless Macha waits on it). Not checked by Dannan: the client's dialling and
  relay changes, the scheduler. **Run by Dannan 2026-11-19** (build-1, `dannan-node` 0.5.3 and 0.9.2 from their tags,
  loopback, 60 s each, logs in `../dannan/build/mixed-version-2026-11-19/`): with the cluster key on both and custody
  on 0.9.2, either side dialling, both lanes up, each lists the other active, echo on control and data both ways, no
  session ended; with a certificate alone on 0.9.2 they never became peers. Not covered: telemetry contents, advert
  pushes, Macha's and Dagda's types; those are this plan's smoke test.
- **Routes (checked here):** each hosted type's lane and frame rules must agree, or the receiver ends the session.
  Macha's own types are unchanged. Five store types differ between 0.94.2's registration and Dagda's: `delete_object`
  and `have_valid_objects` (0.94.2 any frame, Dagda not control), `bool_reply` (0.94.2 control lane, Dagda both
  lanes), `object_reply` (0.94.2 not control, Dagda any frame), `have_control_objects_reply` (0.94.2 control frame
  only, Dagda any). By the default frames the senders use, the requests stay within both, but a reply on the other
  lane or a promoted request would end the session. Dagda's own types (768 to 774) are unknown to 0.94.2 and not
  policed there. Dannan, 2026-11-19: the check applies to replies too, on both the sender's sealer and the receiver's
  opener, and a refusal ends the session (every call on it fails). A reply travels in its request's lane and class.
  The likeliest failure: 0.95.0 calls 0.94.2 in a class 0.94.2's reply rule forbids (`object_reply` to a request sent
  in the control class, `have_control_objects_reply` outside the control class, `bool_reply` to a data-lane
  request), and 0.94.2's own sealer ends the session. Not traced: Dagda's call sites' frame classes, promotion.
- **Store (Dagda, from code, 2026-11-19; never run): they must not mix for long.** What lines up: type numbers and
  protocol-23 payloads, Dagda serves every store type 0.94.2 sends, a principal-less put or claim is the sender's own,
  object ids, placement salts. What does not: (1) capacity adverts (`macha/capacity`, `macha/used` against
  `dagda/capacity`, `dagda/figures`), so each reads the other's capacity as 0 and ranks owners differently;
  (2) Dagda sends its own types to 0.94.2, which answers 501: its deletes (`remove_objects`) never take effect on
  live-a, its repair push (`offer_objects`) fails every pass, its pull of many falls back to single gets (works);
  (3) Dagda appends its writer to every put and claim, and 0.94.2's handlers end with `Reader::finish()`, which refuses
  trailing bytes (`src/codec.cpp:109`, checked here): **every put and claim from live-b to live-a fails**.
  Macha's metadata types are unchanged and do cross; reads across (`get_object`) work. Not checked: trie_diff
  answers across v18 and v19 ledgers. `dagda-migrate` is purely local.

**The two-version smoke test (2026-11-19, the maintainer's go; fresh test nodes, a few MB, nothing from the live nodes):** node A
Macha 0.94.2 (`main`, Dannan 0.5.3) on build-1 founding the cluster, as live-a; node B the 0.95.0 deploy tarball
itself (Dannan 0.9.2, Dagda 0.3.0, test custody from a credentials directory) on build-2, bootstrapping to A, as
live-b; replicas 2, write copies 1, metadata write copies 1, `collect_garbage` false. About 9 minutes mixed: each
wrote 8 files (1 to 6 MiB) through its FUSE mount, each read the other's back (16 of 16 sha256 match), each deleted 2,
then 3 minutes idle. Result: both online and healthy throughout, each lists the other `online` with its version;
metadata writable with `replicas=2/2`; no WARN or ERROR line on either (accounts' first-start notice aside); transport
connections made once and reused about 500 times on each, so no session ended; server rejections 0; prompt
replication 0 failures both ways; both data directories hold the same 19 large objects, so every write reached both
nodes. A answered 26 requests of types it does not know (Dagda's 768 to 774, answered 501 as Dagda predicted) without
any session ending. Settled by Dagda from code afterwards: B's prompt copies to A carry no writer, so they are
protocol 23's payload exactly and succeed (the 0 failures is real); B's write fan-out and its claims always carry its
writer, so those to A fail on the trailing bytes (500 from 0.94.2's dispatch, logged by Dagda at DEBUG only; the
server's `rejected_jobs` counts queue refusals, not handler errors, which is why it read 0). With write copies 1 the
writer's own copy meets the write, so nothing surfaced. During the window, then, live-a holds copies of live-b's
new objects with no claim from live-b; with `collect_garbage` false nothing collects them, and once live-a runs
0.95.0 its claims are its own. The window stays minutes. Not exercised: a promoted request, deletes reaching the other
node's store (garbage collection off). Logs in `build/smoke/logs/`; configs and scripts in `build/smoke/`.

**The plan for the window (recommended; the maintainer's choice at the go):** keep the mixed time to minutes. live-b migrates
while live-a serves alone on 0.94.2 (single-copy writes, accepted). live-b starts on 0.95.0 and is proved serving
(step 8), and live-a is stopped at once for its own migration, with live-b serving alone on 0.95.0 meanwhile.
What was written to only one node during either migration is unreachable from the other until both run 0.95.0 and
repair converges them (degraded operation, accepted). `maintenance.collect_garbage` stays false throughout, so
nothing is reclaimed on a partial view. The alternative, live-b left stopped until live-a's turn, is ruled out:
both would be down at once. The smoke test still runs first, to show the minutes of mixing end no session and crash
neither node.

