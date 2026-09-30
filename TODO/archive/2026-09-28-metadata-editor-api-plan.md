# Metadata editor API (proposal A-G): plan and wire shapes

Status: approved by the operator 2026-09-28 ("Prep the API for this now";
client work is his, 2026-09-29). G, F, D, C, A, E and the multi-file fixes
built in 0.67.0; B waits on the operator's choice of fields. Built in the
order below, each part documented as it landed. Proposal source: the web client's 2026-09-25
handover and core's ACTIVE "Matching and metadata editing".

One interface for matching an unmatched file and editing an item, with three
paths (a candidate, a provider search, manual entry), each with parent links
and an artwork choice. Core owns the server interaction; clients build the
screen. Every response carries a snake_case status/code; errors add a message.

## G. Item edits: validated parents, partial update, honest errors

`PUT /api/v1/catalogue/items/{id}` (whole item, as now) and new
`PATCH /api/v1/catalogue/items/{id}` (only the fields present change). Both
honour `If-Match: "rev-N"` as `PUT` does.

- **Files cannot be unbound by omission.** `media_ids` and `artwork` absent
  from a `PUT` body keep their current values; present, they replace. `PATCH`
  touches only fields present.
- **Parents are validated.** `parent_id` must name an existing item of the
  right kind: season under show, episode under season, album under artist,
  track under album; movie, show and artist take none.
  `400 parent_not_found` (with `parent_id`), `400 bad_parent_kind` (with
  `kind`, `parent_kind`).
- **Errors say what they are.** Malformed body `400 bad_item`; unknown item
  `404 not_found`; revision mismatch `409 conflict` (as now); only a genuine
  catalogue/metadata failure is `503 catalogue_unavailable`. Today a
  malformed body answers 503.
- **Hand edits lock by default.** An edit through `PUT`/`PATCH` sets the
  item's metadata lock (`external_ids.macha_metadata_locked = "1"`), so a
  later scan does not overwrite it, unless the body says `"lock": false`.
  *(Open for the operator: lock by default, as proposed.)*

## F. Search by kind and parent

`GET /api/v1/catalogue/search?q=...&kind=movie&kind=show&parent=<id>&limit=`

- `kind` repeated (`movie`, `show`, `season`, `episode`, `artist`, `album`,
  `track`), filtered before `limit`; absent means all; unknown is
  `400 bad_kind`. (Backlog item 9, approved.)
- `parent`: only items whose `parent_id` is that id.
- Needs the HTTP layer to keep repeated query parameters
  (`HttpRequest::query_all`); `query` is unchanged.

## D. Manual entry with parent ids and the lock

`POST /api/v1/manage/unmatched/{id}/manual` accepts existing parents by id in
place of names, so a hand-entered episode lands in the scanner's `tmdb:` show
rather than a duplicate `manual:` one:

- episode: `season_id`, or `series_id` plus `season_number`, or the names as
  today;
- track: `album_id`, or `artist_id` plus `album`, or the names as today;
- a named id must exist and be the right kind: `404 parent_not_found`,
  `400 bad_parent_kind`.
- `"lock"` as in G, default true.

## C. Match to a provider reference

`POST /api/v1/manage/unmatched/{id}/match` accepts `{"ref": "..."}` in place
of `catalogue_item_id`. The server fetches the provider's record, builds the
hierarchy (reusing items that already exist under the same ids), binds the
file and fetches default artwork, exactly as a scan match would.

- `ref` is what A returns: `tmdb:movie:<id>`, `tmdb:tv:<id>` (with
  `season_number`, `episode_number` in the body), `musicbrainz:release:<mbid>`
  (with `track_number`, optional `disc_number`).
- `400 bad_ref`, `404 provider_not_found`, `503 provider_unavailable`,
  `400 not_playable_ref` (a show or release without the episode/track
  numbers).

## A. Provider search

`GET /api/v1/manage/providers/search?q=...&kind=movie|show|album&year=&limit=`

```json
{"status": "ok", "results": [
  {"ref": "tmdb:movie:335984", "kind": "movie", "title": "Blade Runner 2049",
   "year": 2017, "overview": "...", "provider": "tmdb",
   "catalogue_item_id": "tmdb:movie:335984"}
]}
```

`catalogue_item_id` is present when the catalogue already holds that item.
`503 provider_unavailable` when the provider cannot be reached;
`400 provider_not_configured` when none is configured for the kind.

## E. Artwork options and choice

- `GET /api/v1/manage/providers/artwork?ref=...&role=poster|backdrop|still|cover`
  lists `{option_id, role, width, height, language, preview_url}`;
  `preview_url` is the provider's own small image.
- `POST /api/v1/manage/providers/artwork/choose` with
  `{"item_id": "...", "role": "poster", "option_id": "..."}`: the server
  fetches the full image, stores it and makes it the item's artwork for that
  role. (Built under `/manage/providers` rather than the item's catalogue path:
  the provider connections live with the scanner, which the manage API holds.)

## B. Richer probe candidates

`GET /api/v1/manage/unmatched/{id}` probes gain the file's facts beside each
candidate (size, container, duration) and whether embedded artwork was found.
*(Open: confirm the fields wanted.)*

## Multi-file fixes

- per-file probe deadlines: already true since 0.58.0;
- `size` on `GET /api/v1/catalogue/media/{id}/profile` (built). Not
  `container`: for a format naming several containers it depends on the
  file's name, and the profile is cached immutably per media id; it stays on
  session facts;
- manual items prune files gone from the whole namespace (built);
- a `media_id` bound to two items is listed in `GET /api/v1/manage/unmatched`
  `conflicts` (built; a multi-episode file within one season is not one).

## Order

G, F, D (server-local), then C, A, E (provider operations: search, fetch by
reference, list images), then B and the multi-file fixes. Each lands with
tests, docs in `docs/catalogue.md`/`docs/management.md`, and a changelog entry
listing exactly what the API now sends.
