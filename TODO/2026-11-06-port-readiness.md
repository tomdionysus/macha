# Can Macha move onto Dannan and Dagda with no loss? (2026-11-06)

> **Superseded for current state by [`2026-11-08-port.md`](2026-11-08-port.md)** (2026-11-08). Kept as the record of how the rulings came about; the libraries cite it.

**Answer: not today. All three sessions agree.** Nobody found an actual loss. Macha has never run
on either library, so most of the question has not been tested.

Asked by the operator: no loss of data, functionality, performance or reliability. These are working
views, from the Dannan and Dagda sessions reading their code and Macha at `9a08846` (Dannan
`feature/macha-parity`, Dagda `c3c09d2`), not the review. No new test was run for them. The Macha
session has not independently checked the libraries' findings.

## By requirement

### Data

| | Dannan | Dagda |
|---|---|---|
| Proven | Placement: 2,704 orders and 26 capacities on Mac, Debian x86_64 and inverbeg arm64. AEAD tamper and replay, sans-I/O only. Importer on a real roster with a tombstone, and on a copy of macnessa's three files | Tag derivation against the GCM spec. Forward, reverse and crash resume on generated v18 states. Dry run over macnessa's real state: 3,809 loose and 314,390 pack records opened, 9 ledgers classified, 0 unreadable, nothing written. Placement vectors. Claim guard and delete marks |
| Built, unproven | Rollback from `*.macha`. Disk key: the two loaders read the same, with no known-answer case | The real rewrite: tags on real records, ledgers re-sealed at real size, every object read back, a reversed disk opened by Macha, time and faults on the USB disks, a crash in a real run. About a million fsyncs, time unknown. A truncated pack record stays a counted failure and reports held |
| Not built | The facade inputs that decide ownership: failure-domain fallback, the hosts-extents filter, capacity from an advert | Any migration of `<state>/metadata/` or the cache seed: the namespace has no path onto Dagda yet |
| Not known | A long-lived roster with resets | Whether the live disks hold anything outside the fixtures' formats |

### Functionality

- **Dannan:** 561 cases on the Mac, plus BBR and fsync-hold on Linux, alone. All 148 retire targets
  exist by name. Three supervisor cases (G44) and a non-std-throw case (G45) are not written. The 44
  journal lines exist, with rewordings, and few have a case.
- **Dagda:** `dagda_tests` 453 on the Mac; 161 of Macha's cases retire against named cases, by reading.
- **Not built, Dannan's side:** the facade: events from `Nodes::watch`, activity clocks, version,
  deadlines at each call site (ruling A, a real audit), and `metadata_notice` read ahead of every
  queue. Dannan has no inline path, and notification plus `fast_control` is an unproven change.
- **Not built, Dagda's side:** heads, H1 to H30, and 95 mapped cases with them; the metadata plane,
  `ordered` registration, head-backed leases, the reference-count diff, head repair, truncation,
  metadata repair; about 28 unpublished figures. Macha's loop until 8c lacks its steps: `Repair::pass`
  and `Collector::pass` bundle what Macha's pass orders and traces separately. Dagda's loop at 8c:
  parking, exact-grace wake, trace.
- **Goldens:** 113 of 494 Status keys and 52 of 83 journal lines are not reached, so byte identity
  there stays unproven even after a port. Mount, playback and load are not captured.

### Performance

**Not known.** No Macha baseline exists, nothing compares the two, and no Dannan figure exists between
two machines.

- Dannan, inverbeg loopback under replication load: 282 to 347 MB/s at the default frame, 103 to 128
  at 4 KiB, loaded ping p99 6 to 9 ms, RSS 177 to 207 MB. Not a baseline.
- Dagda alone, macnessa NVMe: 1M-id trie built in 38.7 s, lookup 12 us; 100,000 4 KiB objects written
  in 7.4 s, get 30 us; dry run about 216 MiB/s. No USB disk, cluster or viewers.
- Commit and merge cost cannot be measured until heads exist. `ClassWaits` is unmeasured.
- Where Dannan expects a loss if there is one: 13 shared workers against Macha's per-connection threads,
  under viewers plus ingest, and the strand and timer each required deadline adds on the DATA path.

### Reliability

- **Dannan:** Debian x86_64 and macOS sanitizers clean at 560 to 562; arm64 plain and ASan 562 of 562,
  once. Compose partitions and failover, minutes long. Those runs found two bugs macOS had hidden, so
  it expects more. Nothing has run for hours, across restarts on imported state, over a real WAN or
  under Macha's load. arm64 has no TSan: g++ 14's refuses the kernel's 47-bit address space. 36 relaxed
  atomics are unreviewed for weak memory. No crash matrix beyond durable files.
- **Dagda:** crash cases by forked children; sanitizers clean at `8361c8b` (422 cases), 13 commits ago.
  Current code not run on Linux; the class-waits branch is Mac only. One unexplained `mutex lock failed:
  Invalid argument` in a TSan run. Nothing has served a real node. Instant delete, the 4096-id claim
  parts, the lease fence, the device reserve, v19 sealing and two-part open have no service history.
- **The hardware:** macnessa rebooted itself on 2026-11-06 at about 12:17Z. See `ACTIVE.md`.

## Seams nobody owns

Owners are the Macha session's proposals, not agreed.

| Seam | Risk | Proposed owner |
|---|---|---|
| **Custody and certificates** | **Blocker.** `dagda::Node` refuses a node without custody; Dannan's plan leaves custody off and certificates out. Nobody provisions them on the nodes | Macha's deploy; Dagda specifies the need |
| Adverts: `macha/*` or `dagda/*` | If capacity reads as absent, owners move: data placement | Dagda owns the names; the facade writes them |
| Message numbers 516, 517, 544 | A second registration is refused; no case for the mixed state | Facade removes them at stage one |
| Memory ledger and its build order | Unowned at stage one | Dannan's node |
| Storage key known answer | A mismatch makes the disk unreadable | One test, pinned by both |
| Who writes `startup.data_storage_degraded` | Unsettled at stage one | Facade |

**Torrent, if it leaves the record at 8c:** "`macha-metadata-dump` identical" and S7's byte gate cannot
hold across the change, so they need restating; the 23 live requests need an answer; plugin state on a
generic space makes more than one space a Macha need, which Dagda must replan.

## The gate cannot answer yes as it stands

Dannan's objection, which the Macha session shares: the review reads code and nodes while Macha is
frozen, and no facade can exist without changing Macha, so a truthful review could only say "not known".
A port branch off the live tree, running the mapped cases and goldens on real hardware, is what would
let it conclude. It leaves develop and the live nodes untouched.

## Proposed plan

1. Pi baselines when the TV import finishes.
2. Assign every seam above an owner, in both libraries' plans.
3. Tag Dannan. Build the facade on a Macha port branch. Run the mapped cases and goldens on real
   hardware, and compare with the baselines.
4. Dagda builds heads S2 to S7 and the steps Macha's loop needs.
5. Stage one, then 8c, on that branch against copies of real state.
6. The review: the library sessions after a clear, from code, nodes and the branch's results.
7. Only then the cutover, inverbeg first.

## Decisions waiting on the operator

1. **A port branch before the review?** Recommended yes.
2. **A timed run on that branch?** Not a soak between stages, since it comes before any cutover.
   Recommended yes; it needs spare hardware or a window on the live nodes.
3. **The real migration on a copy?** Previously "I'll decide later". Step 5 and the data answer need it.
   Recommended yes.
4. **Custody's owner:** Macha's deploy? Recommended yes.
5. **Torrent out at 8c, dropping the 23 requests?** Recommended yes.

## The operator's ruling on the libraries' answer, 2026-11-06 afternoon

Both library sessions answered the question below with a conditional yes: agreement on numbered
amendments, with the claim restated as "no loss found by the named gates, ruled changes excluded".
**The operator rejected it:** "Amendments aren't acceptable. Development will continue on them with
macha code frozen until they can answer Yes unconditionally, the entire reason for their existence
is this." Relayed to both sessions in those words.

**What "no change" means (operator, the same afternoon, on both libraries' R1):** "Dannan and Dagda
are generalisations of macha's cluster and storage layer. The intent is to devlop them to the point
where they are at least as capable as the macha code they are based on, then replace the macha code
with them. MachaDFS and cluster become systems implemented with Dannan and Dagda as fundamental
libraries. The fidelity point is that after that, Macha should work just as well as it ever did."
The Macha session reads this, and told both libraries it is a reading: the test is Macha's capability
and how well it works, not its internals.

**Who builds the port (operator, the same afternoon):** "R2 should be built by this agent, when the
dannan and dagda agents have finished all tasks and reported their capabilities, and this agent is
satisfied that macha can be ported with no loss of functionality onto their codebases, which will be
frozen during the port." This replaces the proposed plan's order and the library proposals to build
the facade. Told to both.

**The port's order (operator, the same afternoon, answering the Macha session's four questions):**

1. **A defect found during the port:** "The port will pause until the relevant agent has identified
   and fixed the fault, with testing. The port will then continue with the relevant codebase frozen
   again."
2. **The review:** "you are the review." The Macha session's judgement is the gate before the port;
   the library sessions' review after a clear is gone.
3. **Proving it:** "We will convert macnessa and inverbeg store backends and install there to test,
   prove and iterate." The live nodes are the test bed. No spare hardware and no copy of the disks is
   planned, so the libraries' requests for those are answered by this.
4. **Macha's freeze** holds until the port starts, and the port branches off `9a08846`: "yes, because
   the other agents need to see stable code to ensure enough fucntionality to replace the macha
   internals."

**Dannan's capability report (2026-11-06 evening, `../dannan/TODO/2026-11-06-dannan-capabilities.md`,
develop `93d6689`, unpushed, untagged):** no Dannan work left it can do without the port; "I cannot say
Yes", since only Macha running on it can show that. 622 on macOS and 624 on Debian, sanitizers clean, but
one unexplained handshake timeout in one macOS TSan run, and arm64 not rerun since `415c6c4`. Checked by the
Macha session so far: inventory rows, none partial; 268 cited cases, all exist (not read for adequacy).
The timeout stays open: 1 in 10 full macOS TSan runs at this code, not reproduced in 10,635 runs of the
case under 10 parallel copies, not called environmental. Dannan's failure text gains the stage and source
port, on the Macha session's word, so its head moves past `93d6689` by that line.

**Dannan 0.5.0, on `df839fe`, pushed, on the operator's word: the head to review.** Two items open, by
Dannan's report. (1) On Macha's path until shown otherwise: the handshake timeout recurred, 2 of about 11 full
macOS TSan runs, "timed out at the handshake from local port 50040": TCP connected, then no answer, neither
end closed; not explained. (2) Not on Macha's path: relay bulk transfers fail under Debian TSan, one session
dying after about 30 s with no sign of movement. Last TSan runs: macOS 622 passed, 1 failed; Debian 623 passed,
2 failed, no warnings. Its other gates ran once, some before the last test-only commit, per the operator's "test
once". arm64 and gateway results pending. By Dannan's reading, a stalled handshake on a dial is a bounded delay
(`connect_timeout`, then backoff to 4 s, failing to the caller), and bounds match Macha's for pre-auth 8,
handshake 5 s and backoff. Not bounded by reading: an acceptor that stops re-arming. Put to Dannan by the Macha
session: a throw from `admit` before the re-arm (`server.cpp:135-139`), and whether anything can hold the shared io
threads, since Macha's accept and handshakes have their own. **Both real, by Dannan's reading at 0.5.0, not run:**
`admit` can throw, which skips the re-arm and `end_operation`: the process dies, or under a supervised `run()`
survives with no acceptor and `stop()` hangs. And `Node::observe_direct` on io threads can reach
`SequencedFile::write`'s fsync. Dannan is fixing both, on the Macha session's word, the write moving to a worker with an
identity reset still durable before it answers; 0.5.1 needs the operator's word. Neither explains the open timeout.
For the port: set the io thread count deliberately; logging blocks on whichever thread logs. For Macha after
the freeze: `accept_loop` spins on EMFILE (`net.cpp:4168-4176`).
The review proper waits for Dagda's report.

**Operator's decisions, that evening:** macnessa's heap corruption is left until after the port. 0.93.5 is
committed and pushed to develop. Dannan's arm64 rerun, tag and push, and real IGD run are all approved.

**For the port, from Dagda's S5:** set `ordered` on the three mutation types; from the later stage the facade passes
Macha's numbers as `NodeOptions::exchange.numbers` and sets `SpaceSpec::unaddressed`; `SpaceSpec::generation_advert`
and `write_copies_advert` are Macha's to name. After a failed history read, `ensure_history_root` may append the
committed head again, a duplicate frame the next open indexes. **Rollback:** Macha's binary comes back only after
`--reverse` has finished; its reader cuts a Dagda-sealed or half-migrated `history.log` as one torn tail.

**Found by Dagda's S5, by reading, for after the freeze:** `MetadataManager::replication_loop` claims on peers are best
effort, not owed: dropped at stop (`:131`), on a failed call (`:136`, logged "deferred"), and over the byte cap (`:101`),
and in memory only, and `retain_on` swallows peer failures, discarded at `node_services.cpp:549`. **Nothing restores a
lost claim** (Dagda's trace, citations read by the Macha session): `ClaimWalk::step` reads only claims this node has;
one returns only through a later commit covering the object. The bytes are protected by the peer's own head and the two
30-day gates. **Window W1, by reading, not run:** a peer's unclaimed object past its grace is re-referenced by the sender
with no touch on the peer, and swept before the peer learns the generation. Cost one replica until repair, not the data.
Kept as Macha's behaviour; recommended left until after the port, the operator's call. Comments at
`metadata_manager.hpp:201-202`, `:242-243` and `claim_walk.hpp:32` are stale. For the port: `known_generation` from
`Exchange::known_generation`; no `Objects::remove` on Macha's paths; the facade sets `write_copies` 1. Tests: the phantom-id hunt at
`test_rpc_cluster:2999` seeks a placement `discover_or_form` no longer reads; rpc:3212 can flake to a third run.

**Torrent (operator, later that afternoon):** moved after all of the port work, shelved until he asks
again. Decision 5 is answered no: torrent and its 23 requests stay through 8c, so the SM19 section, the
`macha-metadata-dump` gate and Macha's one space stand as they are, and Dagda's restated torrent gate
above is not needed.

So: the question stands as asked, and the only answer taken is Yes with nothing attached. Every
amendment below is the library's own work list, not a condition on Macha or on the operator. Macha's
code stays frozen at `9a08846` while Dannan and Dagda are developed against it until each can say
Yes. A library that believes a Yes needs something only Macha or the operator can give (baselines,
hardware, a port branch, a ruling) asks for it by name, once, with its reason; the Macha session
carries it to him. The record of what they answered is kept below because it is their work list.

## What the libraries answered, before the ruling

Asked by the Macha session, at the operator's word, under the recommended answers to decisions 1
to 5: does the plan above, executed, let each library conclude no change in data, functionality,
reliability and performance for its layer? Both answered from their code and this file, no new test
run. Both agreed on amendments, and both rejected the claim as literally stated. Their words:

- Dannan: "No change cannot mean literally none, and no test proves absence. It means: no loss found
  by these gates, except changes Tom has ruled." Agrees "where proves no change means ... no loss
  found by those gates, and every accepted change listed, not a proof of absence."
- Dagda: "Tests can only show no change in what they exercise, so the claim is bounded by the gates."
  "Changes nothing is also not literally true."

They asked for the gate's sentence to be restated as "no loss found by the named gates, every ruled
change listed". The operator's ruling above refuses that.

### The amendments, merged

Dannan's are D1 to D10, Dagda's A1 to A14, as each numbered them. Grouped by plan step.

**Step 1, baselines.** Both extend evaluation section 6 item 4, at least 5 runs each on the same Pi
so the spread is known (D9, A13). Dannan adds: several concurrent DATA transfers; CONTROL ping p50
and p99 idle and during; server queue wait and handler time by class from `frame_timings`; CPU per GB
moved and thread CPU by group; viewer first segment and segment latency with ingest running. Dagda
adds: local put and get on the USB disk, loose and packed; claim journal latency and a publication of
at least 100,000 ids; commit, verify and merge latency once heads exist; GC pass, scrub and
compaction rates; `has` and open time at 1.03M loose objects; I/O latency of control work under a
saturating loader. The rule differs: Dannan "equal or better on every one, within Macha's own
run-to-run range, no other tolerance"; Dagda "within the spread or 5 percent, whichever is larger,
peak RSS at most 10 percent over". Both say the figure is the operator's after the baselines
(decision 7). Where each expects a loss: Dannan, 13 shared workers and a strand per connection, plus
a strand and timer per call on the DATA path, settled by queue wait, CPU and viewer latency under
viewers plus ingest; Dagda, `IoReserve`'s 6 concurrent DATA operations where Macha has no cap, and
more journal syncs from 4096-id claim parts, settled by the local put and publication measurements.

**Step 2, seams.** All six owners accepted by both, with two corrections. Adverts: Dagda's
`StorageServer` writes `dagda/*` itself and the facade writes only hosted fields from 1024, since
placement reads `dagda/capacity`; the importer's `macha/hosts-extents` follows Dagda's name (cross
point 1, below). Message numbers: the facade removes every number Dagda registers at stage one, 23 of
them, not three: 515, 516, 517, 525, 531, 532, 534, 535, 544, 552, 557, 559, 561, 615, 616, 619, 630,
633, 635, 637, 768, 769, 770. Custody: Dagda makes the wrap advert optional (A14), so stage one needs
custody only and no certificate; its spec is custody created in `state_path` from a systemd
credential, root backed up off the node; Dannan adds a case for custody on with certificates off (D5).
Seams added, with proposed owners:

| Seam | From | Owner |
|---|---|---|
| Hosted telemetry 1024 and up against `dagda.*` figures: one writer per figure | Dannan a | facade, Dagda naming |
| The importer's advert name | Dannan b | Dannan, after Dagda names |
| Foreground presence for the arbiter's predicate and the activity clocks after the store moves | Dannan c | Dagda, facade supplying (cross point 2) |
| The observation recorder moves to the node; `service.cpp:560-568` removed; `shutdown` emitted before stop | Dannan d | facade |
| `.dannan.lock` and the layout marker order on a fresh directory (F9) | Dannan e | facade |
| Deadline values per call site (ruling A) | Dannan f | facade; Dagda for its own |
| Relay flow control | Dannan g | parked, recorded as a known defect (decision 12) |
| Macha's metadata takes `LocalStore&` (`namespace_control_store.hpp:20`); stage one gives it Dagda's control store | Dagda a | facade type adapter, or Macha's headers switch |
| `.macha.lock`: Dagda's `Node` takes that name on the state and every store root; the facade must not hold it | Dagda b | facade |
| Rollback after stage one: the reverse covers disks only; Macha's binary and Dannan's state | Dagda c | Macha's deploy |
| `OpenLater` ordering against Dannan's readiness | Dagda d | facade |

**Step 3, the port branch.** Dannan: G44's three, G45 and the custody case written before the tag
(D5); every facade item F3, F4, F7, F8, F10, F11, F18, F21, F22, the call filter and the deadline
audit gets a named Macha case or golden, not "the branch passes", F10 under a saturated control queue
(from Q2c); rollback rehearsed on a copy of real state, import, run, restore `*.macha`, open under the
then-current Macha release, compare Status and roster (D2); importer cases for a long roster, thousands
of nodes to the 65536 bound, resets with port 0 and no stale id, and a re-import from a long-running
fixture cluster (D4); a two-machine Dannan measurement on real hardware (D9). Dagda: retirements run,
not read: the original Macha case over the facade wherever it compiles, beside its Dagda case, before
it is deleted; where it cannot run, assertions compared case by case and said so (A9).

**Before stage one.** One storage-key known answer, Macha's own output as the constant, pinned in
Macha and both libraries, a real copy read through it (D3; Dagda has `DomainStorageKey`'s, Dannan pins
the same value).

**Step 4, heads and the steps.** The heads plan re-read at `9a08846`, written from `b080c38`; H20, H29
and H30 have no case and get one; each of the 95 gap-heads cases gets a session or a named retirement
(A7). `Repair::pass` and `Collector::pass` split into Macha's steps (A8). The `<state>/metadata/`
and cache-seed migration built and tested here, run at 8c (A4).

**Step 5, real state.** A whole-disk run, not a copy of state plus a sample: inverbeg's disk or a full
copy, about 670 GB (A1). macnessa's 4 TB is never rehearsed by the plan, so its first real run is the
cutover; Dagda accepts that only after inverbeg's measured rate and a tested `--reverse` (A2, decision
9). Crash cases on the copy: kill -9 and one real power cut, once each in the object, ledger and marker
phases, each resumed and read back (A3). A read-only `dagda-migrate --dry-run` of macnessa's whole
DATA disk before the cutover (A5). The data gate is not `has` and `get`: a before-and-after
comparison tool, unbuilt, naming every object in DATA, CONTROL and cache by id and plaintext hash,
removed objects staying gone, held tries, claims per id with dots and removed dots, the retention
marker, reference counts and root, sightings, backend markers' node and token, accounting bytes, and
for metadata the head set, generation, root, certificates and every history frame replaying to the
same hash (A6). The million fsyncs are measured there; grouping with `syncfs` only with new crash
cases. A truncated pack record reporting held is 0.93.4's behaviour, matched, so not a change at the
port, and a known gap on both.

**The loop gate (A8).** `test_maintenance_trace`'s fixtures unchanged byte for byte: on the port
branch over Macha's loop calling Dagda's split steps through stage one; at 8c the same fixtures over
Dagda's loop; any line that must change goes to the operator as a named exception, never regenerated.

**Before step 6, the review.**
- Goldens widened (D6, A10): neither accepts 381 of 494 keys and 31 of 83 lines as "unchanged".
  Comparison by full path, not name, since by-name makes 381 an upper bound; a run with data, a lost
  copy repaired, claims, GC and a removed disk; a mount; a playback session; an identity reset and a
  peer leaving; UPnP and external IP against the fake or a real gateway; plugin failure paths; load.
  A key or line still unreached is listed and accepted by the operator one by one (decision 11). A
  journal line the capture does not reach needs a named case asserting its level and words, or a
  recorded reason it cannot occur; Dagda's log text is not Macha's, so each store line in section 5 of
  its cutover spec gets a mapping or a listed exception.
- Sanitizers on the final tree with class-waits merged: the suite and ASan with UBSan on Linux
  aarch64; TSan on x86_64 Linux and the Mac (A11). Dannan: the 36 relaxed atomics read and each
  justified or strengthened in a table, 20 repeats of plain and ASan on arm64, and a clang TSan
  attempt on arm64, a stated gap if it does not run (D8).
- The unexplained `mutex lock failed: Invalid argument`: the same first-run options repeated at
  least 50 times with each child's stderr kept; found, or carried into the review as open, never
  closed (A11).
- The timed run (D7, A12), decision 2 answered with a named pair of machines. Dannan: at least 72
  hours, two Pi 5s on Debian 13 over the real or a shaped path, viewers and ingest, restarts on
  imported state, a kill -9, a deliberate partition, the arm64 build; pass is no thread fault, no
  unexplained close, RSS flat after warm-up, stall and no-progress counts no worse than Macha's over
  the same load; spare Pis preferred, since macnessa's reboot and disk drop make a failure there
  unattributable; compose stays the x86_64 fault test. Dagda: at least 7 days per stage and until
  every event has occurred, both Pi 5s or identical hardware with real USB disks, covering a partition
  and healing, restarts under load, USB disk loss and return, a power cut, GC removing under a short
  grace, instant delete under claims, a full scrub, compaction and rebalance, a 100,000-id
  publication. Both call the length a judgement, and Dagda notes the operator ruled no soak between
  stages (decision 8). Both: a run shows a problem, not its absence; what they stand behind is no
  worse than Macha over the same exposure.

**Torrent out at 8c (Dagda).** SM19 keeps an empty torrent section, so the forms stay and the drop
of the 23 requests is one explicit commit. Gate: every frame before that commit replays byte for byte
to the same hash and S7's vectors hold; the head after it dumps identically in every section but
torrent. Macha stays at one space through 8c, so Dagda's S order is unchanged and only S4's `Space`
must allow more than one; if plugin state must land on a space at 8c, a second space joins Macha's
path and the plan comes back to the operator.

**Changes the claim must exclude**, by ruling already given (both): the wire and the flag day,
Dannan's state formats, the `/status/diagnostics` thread rows, per-pool DIAG lines, the
`MEMORY OVER LIMIT` wording if the facade does not map it, and section 5.3 of the evaluation: layout
v19 and ledgers under custody, the 256 MiB control reserve, durability levels and evidence, instant
delete, per-domain grace, the read-failure and degraded shapes, the future tombstone and unkeyed delta
rules, repair diff only, CONTROL GC out of the catalogue, read sharing. **Not ruled, Dagda:** the
device reserve, the lease fence and the 4096-id claim parts are additions; each is ruled a change or
removed before the claim (decision 10).

### Cross points between the two, settled between them

1. **Adverts.** Dagda's `StorageServer` writes `dagda/capacity`, `dagda/used` and `dagda/extents`
   itself once the store is on Dagda; the facade writes only hosted fields from 1024. At the first
   cutover there is no Dagda `StorageServer`, so the facade writes the three itself until stage one,
   then stops, or placement reads nothing (Dannan). `dannan-import-macha` writes `dagda/extents` as
   `Bytes{1}`, present only on a hosting node (read by presence, `availability.cpp:125`), and
   `dagda/capacity` and `dagda/used` as u64 from the roster's figures; a peer's own server replaces
   all three when it next advertises (Dagda).
2. **The foreground predicate after the store moves.** Dagda owns it: `Node` already wires the
   arbiter's predicate from Dannan's activity clock over the quiet window, and the facade's part is
   noting Macha's foreground and loader work on those clocks. Dannan's correction, sent to Dagda:
   Dannan's clocks count every inbound request in `Node::handle` (F8) where Macha's count only object
   bytes served, so they cannot feed the predicate unchanged; it needs Macha's meaning, or the
   viewer's traffic class alone.
3. **Run length and tolerance.** Each said its agreement would stand under the other's figures.
   Moot under the ruling: there is no tolerance and no accepted exception.
4. **Locks.** `.macha.lock` is held by Dagda's `Node` only; `dannan-import-macha` takes both locks
   while it runs, with every node stopped, and leaves the empty files behind.
5. **A2.** Dagda withdrew "after a tested reverse" as a condition, since the reverse not being a gate
   is the operator's ruling; "macnessa only after inverbeg's measured rate" stays in its own plan.

### The libraries' requests to the operator, after the ruling

Each asked once, with its reason, as the ruling allows. Nothing here is agreed.

**Dannan** ("My answer today is No, and I carry on until it is Yes. I will not say Yes on anything I
cannot show."):

- R1. **A ruling on changes already ruled.** Dannan's wire and flag day, Dannan's state formats, the
  `/status/diagnostics` thread rows, per-pool DIAG lines, and the journal lines reworded where
  Dannan's field differs. Do these count as change? If they do, the answer can never be Yes while
  Dannan keeps its own wire. Dannan can remove the journal, DIAG and thread-row differences by
  reproducing Macha's, with effort, if he wants that; the wire and formats cannot both stay Dannan's
  and be unchanged. One word on each.
- R2. **A port branch**: may Dannan build the facade itself in a clone of Macha at `9a08846` kept
  outside `../macha`, under `../macha-port`, with `../macha` frozen and untouched? Without Macha
  running on Dannan nobody can show function, performance or reliability.
- R3. **Macha's baselines on the Pis**, with Dannan's Q4 list, as soon as the import allows.
- R4. **Two machines for a timed run** under Macha's load: spare Pi 5s, or a window on the live
  nodes. Length is his, or Dagda's 7 days.
- R5. **A Dannan tag and a push of develop** when it says it is ready; Dagda's ClassWaits merge
  waits on it.
- R6. **The real IGD run**: which machine, and his word. Dannan's UPnP has only met a fake gateway.

Dannan's state after the ruling (15:00Z, `feature/macha-parity`, 568 on macOS, not rerun on Debian or
arm64): done are G44's three, G45, custody without certificates, the long roster to 65536 with resets,
the storage-key known answer against Macha's compiled output, the relaxed-atomics table by reading,
and the importer on `dagda/extents`. In hand, in order: relay flow control (taken back from parked),
the journal table with a case or a reason per unreached line, the inventory's partial rows. Its own
"everything I can do alone is done" line to the operator was wrong and it has corrected it. It holds
that without R1 and R2 the answer cannot reach Yes however much of the list is finished.

**Dagda** ("the answer today is No: Dagda cannot yet answer Yes. A1 to A14 are now Dagda's plan, heads
first."). The device reserve, the lease fence and the 4096-id parts need no ruling: Dagda makes each
inert on a Macha node (the reserve off by option, claim parts no finer than Macha's own journal
frames, no fence on Macha's paths), so they are not changes. The arbiter's predicate will be fed in
Macha's meaning, object bytes served. Torrent work is on hold until the 8c ruling.

- R1. **A ruling on changes already ruled.** Layout v19 and ledgers sealed under custody, the 256 MiB
  control reserve, the read-failure and degraded shapes (already in Macha), repair diff only,
  CONTROL GC out of the catalogue. On-disk format or internal, ruled by him; some cannot be undone
  without reversing "one layout". If they do not count, Yes means "no change he has not himself
  ruled"; if they do, Dagda must know before building on them.
- R2. **The real data**: the whole-disk run on inverbeg's real disk or a full copy of about 670 GB,
  with a stop of inverbeg if in place, and a read-only dry run over macnessa's whole DATA disk. A
  state copy and a sample cannot say every object survives or how long a million fsyncs take on USB.
- R3. **Hardware or a window for a timed run**: two Pi 5s with USB disks, or the live nodes, with
  viewers, ingest, a partition, a disk pull and a power cut. Nothing Dagda has built has served a
  real node.
- R4. **A port branch** off `9a08846`, develop and the live nodes untouched, holding the facade and
  the rewritten cases; and who writes it, the Macha session or Dagda, given Dagda's standing rule
  that `../macha` is read only for it.
- R5. **The Pi baselines**, as agreed, including Dagda's Q4 list when convenient.

**The same four asks from both**, in different words: whether his own ruled changes count against
the Yes; a port branch, and who may write it; the Pi baselines; hardware or a window for a timed run.
Dannan adds a tag and push at its word and the real IGD run; Dagda adds the whole-disk run.
