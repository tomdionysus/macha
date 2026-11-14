# Macha's store calls onto dagda::Node and Context: the callers' side (2026-11-14)

Macha's half of the call-by-call map; Dagda's half (the store's side) and the bar, route and method are in
`../../../dagda/TODO/2026-11-14-macha-store-map.md` and `../../../dagda/TODO/macha-store-map/`. Written against Macha
develop `f0597a4` and Dagda `feature/macha-store-gaps` at about `0d9a4ff`, by six readers, one per section, each row
with file:line on both sides. Verdicts are against **Context** (the route: Macha calls Context only, no shims); GAP is
a call only a `Node` part serves today.

| Section | Rows | SAME / DIFFERS / MISSING / GAP |
|---|---|---|
| [filesystem](filesystem.md) | 31 | 0 / 0 / 0 / 30, one Dannan's (through Node parts: 17 SAME, 10 DIFFERS) |
| [FUSE and hydration](fuse-and-hydration.md) | 32 | 3 / 5 / 3 / 19, one unverified, one Dannan's |
| [catalogue and media](catalogue-and-media.md) | 45 | 9 / 8 / 0 / 28 |
| [metadata and control](metadata-and-control.md) | 38 | 12 / 6 / 0 / 20 |
| [ingest and torrent](ingest-and-torrent.md) | 15 | 3 / 1 / 0 / 8, three belong to other sections |
| [HTTP, status and the root](http-status-and-root.md) | 61 | 13 / 25 / 2 / 21 |
| **All six** | **222** | |

## Checked by the Macha session in code (not only the readers' claims)

- **Port blocker, confirmed:** Macha's `Service` holds `StorageLock(state_path)` first (`service.hpp:72-73`), a
  `flock` on `<state>/.macha.lock` (`local_store.cpp:438-450`); Dagda's node takes `StorageLock` on the same state root
  (`dagda/store/local_store.h:56-62`, Dannan's `StateLock` also `flock`, `identity/node_identity.cpp:24-39`). Two
  `flock`s on separate descriptors conflict within one process, so the node is refused as "in use". One lock, one
  owner: Macha's root rewrite drops its own.
- **Data safety, confirmed in Dagda:** `WriteOptions::cancelled` and `ReadOptions::cancelled` are raw
  `std::atomic_bool*` (`dagda/store/distributed_store.h:158`, `:197`), read by a store worker in the async forms after
  the caller has returned. The read requirements gain a fifth: cancellation through a token the store co-owns.
- **Resolved on the port branch:** `ingest.cpp:1627` catches `StorageUnavailable`, which is Dagda's type since
  `ed1d3fd`; what remains is that Context returns evidence rather than throwing, so the filesystem turns an unlanded
  write into `FsStorageUnavailable` (a rewrite, not a gap).
- **Ruled out:** the catalogue reader's question whether `maintenance_objects` can report complete without the current
  root's shards. `complete` needs `cached_root_ == metadata_root` (`catalogue.cpp:2643-2645`) and the root set always
  holds the snapshot's `catalogue_root` (`metadata.cpp:4247-4250`), whose loop marks the inventory incomplete on any
  failure.

## The largest gaps for Dagda (readers' findings; Dagda checks each against code)

1. **CONTROL through Context:** fetch to local, local get/has, strict local put, an offer that writes no claim; the
   catalogue's manifest and shards and every metadata tree read and commit depend on them.
2. **Blocking forms on the caller's thread with no deadline** (reads and writes), known-id writes, deferred writes
   returning tickets, a many-ticket wait carrying the caller's frame.
3. **Typed outcomes:** a read told cancelled / timed out / unavailable (`FetchAnswer` is ok / not_found / try_later);
   a typed per-ticket loss in barrier evidence (today English in `ReplicaEvidence::why`); a refused commit typed, not
   `MutationEvidence::why` text. Nothing in Macha may match English.
4. `cache_local`, `hydrate`, `locally_available`, the activity clocks, `idle_for`, `reachability_epoch`, capacity,
   extent size, live figures for Status, reload, DATA admission for Macha's own disk I/O (torrent), the device service
   monitor, `retain_data_here`, the cache seed, reference counts, the reachability provider: all Node-only today.
5. **The hosting advert written by two paths:** Macha withdraws it on a failed dial-back, Dagda's storage server puts it
   back at its next refresh (reader: SS:89-102).
6. Performance: Context work on 4 store workers in one FIFO whatever the class; barriers always at loader; a
   transient barrier failure waits to the deadline; no retained-memory lease on reads; `offer_control_on` holds the
   whole missing graph in memory; an extra SHA-256 per extent through Context.

## API changes the port brings (announce to Core and every client before they ship)

From the root and status section: store message `message_timings` keys become `dagda.<name>`; `startup.error` text
(unless Macha keeps its prefixes); `retained_memory.owners` values for the same work; Status figures up to 10 s old if
read from Dannan's surface; artwork GET gains a deadline (a slow read can answer 503). Must not ship without their
rewrites: the `macha/capacity` and `macha/used` adverts, the telemetry feed, the hosting advert, the persisted path
table.

## Macha defects the readers found (fix in Macha first; report to the operator before fixing)

- `reload_config` calls `StorageServer::refresh` while the refresher may be running it: a race on `online_backends_`.
- `reconfigure_local` writes `cfg_` on the SIGHUP thread while other threads read it: a data race on strings and
  vectors, the kind that smashes a heap (a lead for the parked heap P0).
- A reload that adds the first backend never re-resolves hosting.
- Ingest: an unreadable tail extent on resume fails the job instead of blocking it; a refused torrent admission lets
  the disk I/O run without credit.
</content>
</invoke>
<invoke name="Bash">
<parameter name="command">cd /Users/tom/devroot/macha && python3 - <<'EOF'
p="TODO/ACTIVE.md"; t=open(p).read()
old="5. Repair's drop-local checks a claim then removes with nothing held (`:2913-2922`), already Dagda's gap 7.\n"
new=old+("6. From the callers' map (`TODO/store-map-callers/README.md`): `reload_config` races the storage refresher on\n"
         "   `online_backends_`; `reconfigure_local` writes `cfg_` on the SIGHUP thread while others read it (a heap-smashing\n"
         "   class of race: a lead for the parked heap P0); a reload adding the first backend never re-resolves hosting.\n")
assert t.count(old)==1; t=t.replace(old,new)
old2="**Map review done (2026-11-14 night):**"
new2=("**Macha's half of the map written (2026-11-14 night):** `TODO/store-map-callers/` (six sections, 222 rows, index\n"
      "README with what Macha verified). Sent to Dagda for review.\n"+old2)
assert t.count(old2)==1; t=t.replace(old2,new2); open(p,"w").write(t); print("ok")
EOF
git add -- TODO/store-map-callers TODO/ACTIVE.md && git commit -q -m "TODO: Macha's half of the store map, the callers' side, six sections and an index" && git -c core.pager=cat log --oneline -1