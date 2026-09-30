# Missing artwork and single-copy writes (2026-09-27)

Status: written up, parked. es-1 is offline in Spain and the operator cannot
reach it before November 2026 at the earliest. Nothing here was changed on
the cluster or in code.

## What was reported

The web client (Macha Client session), on the operator's report of blank
cards on macnessa, ran a poster census against gbni-1 at 19:1xZ:

- 301 movies; 296 carry a poster in their catalogue `artwork` list.
- **60 of the 296 (20%) answer 404** `not_found` "artwork not found" on their
  signed URL. Six sampled answer 404 through fi-1 too. The other 236 answer
  200.
- Titles include 2010, A Knight's Tale, Akira, Alien, Apocalypse Now, Avatar:
  The Way of Water, Bill & Ted's Excellent Adventure, Caddyshack II, Casino,
  Chasing Amy, Clerks, Clerks III, Flight of the Navigator, Furiosa,
  Gladiator, GoodFellas, Gravity, Hackers. Id prefixes: `b57f8dc9e332`
  (Flight of the Navigator), `d5cf24790b63` (Gladiator), `bc5f2911e706`
  (GoodFellas), `c1e358e3c95c` (Gravity). A separate id, `af43d2675682`, was
  404 on both nodes in 2.1 s and 2.3 s.
- **All 60 belong to items last updated 2026-09-06 to 2026-09-10** (32 on the
  7th, 17 on the 10th). The 236 served posters span the 6th to the 27th.
- `catalogue/status`: 3032 artwork objects; gbni-1 holds 2562 locally, fi-1
  holds 37.

The client now falls back to the item's backdrop where that answers 200 (40
of the 60 have one); otherwise a placeholder.

## Where the bytes are: not known

Measured by the client, not by me (my on-box reads were not permitted). Two
candidates, and nothing available today tells them apart:

- **es-1 only.** es-1 did the matching in that window (scanner and TMDB key),
  and it has been offline since 2026-09-24.
- **gbni-2 only, or gbni-2 and es-1.** gbni-2 was a DATA node in the same
  window and has been defunct since 2026-09-20.

What the code does establish: a catalogue commit refuses to publish artwork
that has not passed the durability barrier
(`src/catalogue/catalogue.cpp`, commit and `artwork_durability_barrier`), so
each poster held at least `dht.min_write_replicas` copies when it was
committed. They were fetched and stored; they are not "never fetched".

**The check, when es-1 is back:** rerun the census. Whatever still answers
404 was on gbni-2 or is gone.

## Why nothing heals it today

- Repair and prompt replication copy only from a node that holds the object.
- Nothing re-fetches missing artwork from the provider. `CatalogueArtwork`
  stores role, object id and mime type, not the provider URL.
- A rematch does re-download (`src/catalogue/media_catalogue.cpp`, the
  remote artwork loop) and stores by content hash. If TMDB returns identical
  bytes the object comes back under the same id and every card heals; if it
  re-encoded the image, the item gains a second poster with a new id and the
  dead one stays first. The only trigger is
  `DELETE /api/v1/catalogue/items/<id>/metadata`, which is destructive; the
  client was asked not to use it as a repair tool.

## The same failure is happening now: new writes get one copy

gbni-1's `diagnostics.prompt_replication` at 19:2xZ: queued 0, copies 0,
failures 0, **skipped_no_room 687, dropped 687**; fi-1's all zero.
`skipped_no_room` is counted on the holder and means the *destination* had no
room (`prompt_replication_loop`, the gossiped `used + extent_size >
capacity` test). With es-1 offline, fi-1 is the only other DATA node, and its
10G backend is full. So 687 objects written since gbni-1 last started exist
only on gbni-1, the node with recurring power failures.

Repair is not covering them either: gbni-1's repair block read
`passes_completed 0`, `push_examined 576`, `unsourceable_objects 0` (ACTIVE
item 1, the credit gate).

## Artwork serving facts (answered to the web client)

- A node serves artwork from its local store, then its block cache, then a
  remote fetch over the placement owners and then every other ranked node. A
  remote hit is queued for the block cache (fi-1: 10 GiB), which is why
  repeats are fast; `local_artwork_objects` counts the store only, not the
  cache.
- The route passes no deadline, and the DATA lane's
  `data_no_progress_deadline` defaults to 0, so a peer that stays connected
  and never answers can hold a poster request until the connection fails. Not
  reproduced; the code permits it.
- A 404 from one node already means every reachable node was tried. A local
  admission failure (memory or DATA credit) also yields 404, so it means "not
  obtainable now", not proof of absence.

## Options, for the operator

1. **Give fi-1 room** (ACTIVE item 5). Ends the single-copy window for new
   writes regardless of where the posters went.
2. **Re-fetch artwork no node holds, from its provider.** Store the provider
   URL with each `CatalogueArtwork`; when repair finds an artwork object
   unsourceable, re-download it, and replace the entry if the bytes differ.
   The durable fix, whichever node the posters were on. Wire-visible only if
   the artwork JSON gains fields: announce first.
3. **Bring es-1 back** (November at the earliest), then rerun the census.
