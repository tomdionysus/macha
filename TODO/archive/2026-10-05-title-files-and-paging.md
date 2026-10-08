# Title files and paging: API design

*2026-10-05. Operator decisions the same day: build server routes for a
title's files; delete by path removes only that path, a separate delete by
content removes every path; unmatch goes straight to the unmatched list with
no automatic rematch; paging on every list call.*

## Title files

Three calls, one per action, each one commit on the answering node.

### Unmatch a file from a title

```
DELETE /api/v1/catalogue/items/{id}/media/{media_id}
```

Unbinds `media_id` from item `{id}`. Every file holding that content is put
in the unmatched list at once (`GET /api/v1/manage/unmatched`), with no
provider lookup: it waits to be identified by hand. If the item is left with
no media, it is deleted, and so is each season, show, album or artist above
it left with no children, whether the scanner or a person made it.

- `200 {"status": "unmatched", "item": {...}, "removed_item_ids": []}` while
  the item still holds other media; without `item`, and with the item and
  the parents removed with it in `removed_item_ids`, when it does not.
- `404 not_found` (no such item), `404 media_not_bound` (the item does not
  hold that media id), `409 catalogue_conflict` (`If-Match` revision changed).

### Delete one file

```
DELETE /api/v1/files/{path}
```

The path is the URL path, each segment percent-encoded, as for `GET`.
Removes that path only. Its content stays bound wherever another path still
holds it. When no path holds it any longer, it is unbound from every item at
once, and items and parents left empty are removed as above.

- `200 {"status": "deleted", "path": "...", "removed_item_ids": [...]}`
- `404 not_found`, `409 not_a_file` (a directory: directories are removed by
  `DELETE /api/v1/manage/filesystem` as now).
- `400 missing_hash` for `DELETE /api/v1/files` without `hash`.

### Delete a file's content everywhere

```
DELETE /api/v1/files?hash=macha:{id}
```

Removes every path holding that content, then unbinds it and removes empty
items and parents as above.

- `200 {"status": "deleted", "paths": [...], "removed_item_ids": [...]}`
- `404 not_found` (no path holds it).

## Paging

Every list call takes `limit` and `cursor`, and answers `next_cursor`:

- `limit`: 1 to 1000. Without it the call answers the whole list, as today,
  so no client breaks.
- `cursor`: the `next_cursor` of the previous page, opaque. A cursor is a
  position in the list's order, not a snapshot: an entry added or removed
  between pages is seen or not, never twice.
- `next_cursor`: a string, or `null` on the last page.
- The order is the list's key: item id, path, hint id, job id, user name.
  Clients sort for display themselves.

Lists: `catalogue/items`, `catalogue/search`, `catalogue/hints`,
`manage/unmatched`, `manage/filesystem` (a directory's entries),
`files/{path}` (a directory's entries) and `files?hash=`, `ingest/jobs`,
`torrents/jobs`, `users`, `playback/sessions`, `status/nodes`.

A list whose total size is useful also answers `count`, the total.
