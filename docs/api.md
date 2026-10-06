# API conventions

What holds for every route under `/api/v1`.

## Lists

A list call answers its entries in the order of their key and takes two
query parameters:

- `limit`: entries per page, 1 to 1000. Without it the call answers the whole
  list as one page.
- `cursor`: the `next_cursor` of the previous page.

Every list answer carries `next_cursor`: a string to pass as `cursor` for the
next page, or `null` on the last. A cursor is a position, not a snapshot: an
entry added or removed between pages is seen once or not at all, never
twice. Cursors are opaque. Clients sort for display themselves.

`400 bad_limit` and `400 bad_cursor` refuse a bad parameter.

| list | key |
|---|---|
| `GET /api/v1/catalogue/items` | item `id` |
| `GET /api/v1/catalogue/search` | relevance; the cursor is a position in the ranking, and `limit` defaults to 50 |
| `GET /api/v1/catalogue/hints` | hint `id` |
| `GET /api/v1/manage/unmatched` | hint `id`; `count` is the whole list |
| `GET /api/v1/manage/filesystem` | entry `name` |
| `GET /api/v1/files/<directory>` | entry `name` |
| `GET /api/v1/files?hash=` | `path` |
| `GET /api/v1/ingest/jobs`, `GET /api/v1/torrents/jobs` | job `id` |
| `GET /api/v1/users` | `username` |
| `GET /api/v1/playback/sessions` | `session_id` |
| `GET /api/v1/status/nodes` | node `id` |
