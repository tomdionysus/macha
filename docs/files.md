# Files and availability

The namespace is a resource, and every file in it says how much of it the
reachable cluster holds. Both are readable by any signed-in viewer.

## The files resource

```text
GET /api/v1/files/<path>            a file, or a directory with its entries
GET /api/v1/files?hash=macha:<id>   the files with that content
```

The path is the URL path: `GET /api/v1/files/Movies/Film/film.mkv`, each
segment percent-encoded. `GET /api/v1/files/` is the root.

A file or directory is:

| field | meaning |
|---|---|
| `path`, `name`, `type` | `type` is `file` or `directory` |
| `size` | a file's bytes; for a directory, the bytes beneath it at the last survey |
| `mtime_ns` | |
| `media_id` | a file's content identity (`macha:<hash>`), `null` for a directory or a file not yet surveyed |
| `extents` | the extents it refers to |
| `extents_local` | those this node holds |
| `extents_unavailable` | those no reachable node holds |
| `extents_unknown` | those this node lacks, where a node that might hold them could not be asked |
| `availability` | `complete`, `partial`, `unavailable` or `unknown` (below) |
| `surveyed_generation`, `surveyed_unix_ms` | the metadata generation and time of the survey these came from; `null` before the first |

A directory also carries `entries`: its direct children, each in the same
form (without their own `entries`), in no promised order. A directory's extent
counts are the sums of everything beneath it.

`?hash=` filters the collection by content identity and returns
`{"status": "ok", "files": [...], "surveyed_generation", "surveyed_unix_ms"}`:
one entry per path holding that content, none if no surveyed file has it.

Codes: `404 not_found`, `400 bad_media_id` (a hash that is not a `macha:`
identity), `400 bad_request` (`?hash=` on anything but the collection),
`405 method_not_allowed`.

## What `availability` means

| code | meaning |
|---|---|
| `complete` | every extent is held by a reachable node |
| `partial` | some extents are held by a reachable node and some by none |
| `unavailable` | no extent is held by a reachable node |
| `unknown` | not surveyed yet (a file written since the last survey, or no survey since this node started), or some extents could not be decided |

These are facts about extents being **held**, from each node's index of what
it stores. They do not say the bytes read back intact, and "no reachable
node" is not "lost": a node that is down may hold them. The server reports
them and does not act on them: a playback request for a `partial` file is
served as far as its extents allow. What to show or offer is the client's.

The same seven fields (`extents` through `surveyed_unix_ms`) ride on each
entry of `GET /api/v1/playback/media`, so a client choosing a file to play
needs no second call.

## Catalogue items

Every catalogue item, wherever one is returned (`GET
/api/v1/catalogue/items`, `/items/{id}`, `/search`, and the replies to
`PUT` and `PATCH`), carries:

| field | meaning |
|---|---|
| `availability` | the same four codes |
| `availability_members` | for a set, `{total, complete, partial, unavailable, unknown}`: the items beneath it that have files, by code; `null` for an item that is not a set |

- **An item with files** (a movie, an episode, a track) takes the best of
  its files, since any one of them can be played: `complete` if one is
  complete, otherwise `partial` if one is partial, otherwise `unknown` if
  one is unknown, otherwise `unavailable`.
- **A set** (a show, a season, an artist, an album) is judged over its
  members, at any depth: `complete` if every member is, `unavailable` if
  every member is, `unknown` if some are unknown and none is short,
  otherwise `partial`. A set with no members is `unknown`.

These come from the last survey and the catalogue as it stands, with one
lookup per item; a response never waits for either.

## How the survey works

Each node keeps, for every node of the namespace tree, the number of
extents beneath it and how many of those it holds. Tree nodes are content
addressed, so nodes at different metadata generations still agree on what a
subtree is. To learn what the cluster holds, a node asks its peers about
tree nodes from the root down: a subtree a peer holds whole is settled by
its id, and only partial subtrees are descended. The cost follows what
differs between the nodes, not the size of the library, and no extent id is
sent.

The survey runs in the maintenance pass, as background work. It is repeated
when something could change its answer: the namespace, this node losing
something, the membership, or a peer's storage shrinking; and, at no more
than a twentieth of the pass's time, when a peer's storage grows while
something is unavailable. After a restart the first survey waits for the
store's presence index to fill. Requests read the last result and never
wait for a survey.

Repair uses the same result: it does not try to fetch an extent the survey
found on no reachable node, and takes it up again once a survey finds it.
