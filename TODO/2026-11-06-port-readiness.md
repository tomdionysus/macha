# Can Macha move onto Dannan and Dagda with no loss? (2026-11-06)

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
