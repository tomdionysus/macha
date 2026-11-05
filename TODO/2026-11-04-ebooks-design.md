# Ebooks as a media class

Status: design, for the operator's review (2026-11-04). Supersedes the
general proposal in `docs/macha-ebooks-proposal.md` for the server side; that
document still holds the user experience, the provider references and the
client reader choice.

Audiobooks are a separate media class, designed later. The one thing here
they will share is the per-user media state (section 6), which is designed
to serve them and video resume as well.

## 1. What already fits

An ebook is a small immutable file. Storage, the namespace, replication,
repair, availability and GC do not look inside a file, so none of them
change. The catalogue already groups files under items with a parent chain,
holds several files per item (`media_ids`), carries artwork, locks edited
items against the scanner, and has a scan provider per class with its own
roots, candidate generators and metadata provider. Music is the model:
`MusicScanProvider` with `/Music` roots, `MusicBrainzProvider` behind a rate
gate, and `choose_destination` filing an ingested track under
artist/album. Ebooks follow that shape exactly, with a smaller probe (no
ffmpeg) and no playback session.

## 2. Catalogue kinds

Three new kinds: `book`, `author` and `series`. None has a parent. A book
item carries the edition-level record; its `media_ids` are the formats of
that edition (an EPUB and a PDF of the same book are one item with two
files), which is the proposal's work/edition/asset model reduced to what
the catalogue has: there is no work level, as there is none for films.

A book is not under an author the way a track is under an album, because
the parent chain is one-to-one and books are not: Good Omens has two
authors, and a series can have several (Discworld's novels are Pratchett's,
its later companion volumes not; the Dune and Wheel of Time series each
change author part-way). So a book names its authors and its series by
reference, in order, and the catalogue gains its first many-to-many
relation:

| field on `book` | meaning |
| --- | --- |
| `author_ids` | the authors in credited order; never empty once matched ("Unknown Author" is an author item) |
| `series` | a list of `{series_id, index}`; `index` optional and fractional (0.5 for a novella), since a book can sit in more than one series: Guards! Guards! is Discworld 8 and City Watch 1 |

An `author` or `series` item carries its own title, sort title, synopsis,
artwork and external ids (`openlibrary_author`; series have no provider
id and are Macha's own). Neither carries `media_ids`. Their ids are
derived from the normalised name (`author:pratchett-terry`,
`series:discworld`) so files scanned on different nodes meet at the same
item; a provider match may later rename an author item's title and set its
external id, never its id. Two series of the same name by different
authors are told apart by hand, as two albums of one name are.

Removal follows the album rule: an author or series left referencing no
book is removed when its last book goes, whoever made it, unless it was
edited (locked). A `book` body naming an unknown `author_id` or
`series_id` is `400 reference_not_found` with the id; naming an item of
the wrong kind is `400 bad_reference_kind`.

Listing and search: `items?type=book&author=<id>` and
`items?type=book&series=<id>` filter by reference as `parent` does by
parent, and `search` takes the same two; the order within a series is
the client's, from `index`. The item JSON carries `author_ids` and
`series` as given; it does not embed the referenced items, which the
client fetches or already holds.

New optional item fields, all on `CatalogueItem`, encoded in the shard
format: `author_ids`, `series`, `language` (BCP 47), `publisher`,
`published` (ISO date string); the ISBN is `external_ids.isbn13`, no new
field. `author_ids` is the first case of the approved people-on-items
work; when that lands generally, it is this field under the general name.

Deploy gate: `decode_catalogue` refuses an unknown kind
(`catalogue.cpp`, "bad catalogue kind"), so a node on an older release
cannot load a shard holding a book. Every node is upgraded before the first
book is catalogued; the scan provider is off by default, also because a
configured root that does not exist makes every scan partial and a partial
scan prunes nothing.

## 3. Scan provider and probe

`BookScanProvider`, name `books`, roots `/Books`, config
`CatalogueBookProviderConfig {enabled, roots, openlibrary}` beside `music`
in `CatalogueScannerConfig`. `MediaProbeKind` gains `book`; `MediaProbe`
gains `author`, `series`, `series_index`, `isbn`, `language` and the
Open Library ids.

Accepted extensions: `.epub`, `.pdf`, `.mobi`, `.azw3`, `.fb2`, `.cbz`.
Every accepted file is catalogued and downloadable; embedded metadata is
read from EPUB only in the first release. The other formats are matched from
their path and from the operator's hand matches.

Candidate generators, scored like the existing ones:

- `book-embedded`: the EPUB's OPF package: `dc:title`, every `dc:creator`
  in document order (with `opf:role`/`role` of `aut` kept and editors,
  illustrators and translators dropped), `dc:identifier` (ISBN and UUID),
  `dc:language`, `dc:publisher`, `dc:date`, `dc:description`, the series:
  `calibre:series` with `calibre:series_index`, and EPUB 3's
  `belongs-to-collection` with `collection-type` `series` and
  `group-position` (several allowed), and the cover item. This is the
  high-confidence source.
- `book-structured-path`: `Author/Title (Year)/file`,
  `Author/Series/NN Title/file` and `Series/NN Title/file` below the
  root; an author folder of `A & B` or `A, B` names two authors.
- `book-filename`: `Author - Title.ext`, `Title - Author.ext`, and the
  bare stem.

Reading an EPUB from the namespace: the scanner has `FileSystem&` in
`probe_file`. An EPUB is a zip; the reader fetches the end-of-central-
directory record, the central directory, `META-INF/container.xml` and the
OPF it names, each bounded (1 MiB per member, 16 MiB total), and the cover
image under `max_artwork_bytes`. Nothing else in the archive is read. A
malformed or encrypted archive yields no embedded candidate, never a scan
failure, and the path generators still run. Zip inflate uses the zlib
already linked; the zip and OPF readers are Macha's own (a central
directory walker and a small XML element reader, each a few hundred lines).
The OPF reader is a deterministic primitive and is tested exhaustively:
EPUB 2 and 3, multiple identifiers, no cover, cover by `properties`,
cover by `meta name=cover`, CRLF, BOM, namespaced and unnamespaced
elements, truncated and oversized members.

Ingest: `probe_host_media_candidates` runs the same generators over a host
file, and `choose_destination` gains a `book` case:
`/Books/<First author>/<Title>/<file>`, with the existing unknown-folder
fallbacks ("Unknown Author"). The path carries one author because it is a
path; the item carries them all.

The scanner creates the `author` and `series` items a matched book names
when they do not exist, in the same commit as the book, as it creates an
artist for an album. A later book naming the same normalised name joins
them.

Media profile: a book has no streams, so the ffmpeg profile path is never
entered for it. `request_media_profiles` skips media whose catalogue item is
a book, and `GET /api/v1/catalogue/media/{id}/profile` answers
`422 profile_not_applicable` with `kind: "book"`. Playback session creation
on a book media is `400 not_playable_media`. The direct-play reader is not
used for books (section 5).

## 4. Metadata provider

`OpenLibraryProvider`, name `openlibrary`, no key, behind a gate like
`MusicBrainzGate` (one request a second; Retry-After honoured). Lookup
order: ISBN (`/isbn/{isbn}.json`, exact edition), then an Open Library
edition or work id the probe carries, then `search.json?title=&author=`
scored on title, author, language and year. A title alone never matches
automatically; it goes to the unmatched list as a music track with no tags
does. Covers come from `covers.openlibrary.org` by cover id, stored as
artwork DATA like TMDB posters; the embedded cover wins when it exists and
the provider's is kept as an artwork option.

External ids: `openlibrary_work`, `openlibrary_edition`, `isbn13`,
`isbn10` on a book; `openlibrary_author` on an author. Open Library's
author records give the canonical name and alternate names, which fill
the author item's title and aliases; its series data is thin and
inconsistent, so series come from the file and the path, never from the
provider. Provider refs for the manage API: `openlibrary:edition:<olid>`
and `openlibrary:work:<olid>` (a work needs no numbers; a work match takes
the work's first English edition unless the probe has an ISBN). Search
kind `book` maps to (`books`, `openlibrary`, `MediaProbeKind::book`) in
`search_providers`' source table. Artwork roles: `cover` only.

Google Books is a later optional provider with a token file, like Discogs.

## 5. Serving the bytes

Macha has no route today that serves a file's bytes outside a playback
session. Books need plain HTTP range reads of the original file, for the
reader in the client and for download, so this adds one:

```
GET /api/v1/catalogue/media/{media_id}/content
GET /api/v1/catalogue/media/{media_id}/content?download=1
```

Viewer class, bearer token, `Range` honoured (the single-range parser and
`Content-Range` code in `playback.cpp:229-320` moves to `http/` and is
shared), `Cache-Control: private, immutable` with the media id as `ETag`,
the file's media type from its extension (`application/epub+zip`,
`application/pdf`, ...), and `Content-Disposition: attachment` with a safe
filename when `download` is given. The read goes through the same
`ReadHandle` the direct player uses and is paced as viewer work; the file
is never staged whole. A media this node cannot read is `404 not_found`
with the item's availability already telling the client where else to go.

The route is not book-specific: it serves any media id. That is deliberate
and is announced as such.

## 6. Per-user media state

Macha keeps no per-user state beyond accounts; a reading position is the
first. It is designed once for every class: a position in a book, a resume
point in a film, later a position in an audiobook.

One record, `UserMediaState`:

| field | meaning |
| --- | --- |
| `user_id`, `media_id` | the key; per file, not per item, as the proposal decided |
| `position` | an opaque client token: an EPUB CFI, a PDF page, a video time in ms |
| `progress_permille` | 0 to 1000, for lists and "continue" rows |
| `finished` | set by the client |
| `updated_unix_ms` | last-writer-wins |

Records live in cluster metadata as their own CONTROL collection beside the
catalogue, keyed by (`user_id`, `media_id`), merged last-writer-wins on
`updated_unix_ms` with the node id as the tie-break: a commutative merge,
no claims, no leases. The torrent-request collection is the nearest
existing shape. It is committed through `mutate_delta` like every other
metadata change and so waits for the tree-native branch: a new collection
is written in the tree form from the start, never inline.

Routes, under the user:

```
GET    /api/v1/users/me/media                       the caller's states, paged
GET    /api/v1/users/me/media/{media_id}
PUT    /api/v1/users/me/media/{media_id}            body: position, progress_permille, finished
DELETE /api/v1/users/me/media/{media_id}
```

A `PUT` older than the stored state (by `updated_unix_ms`) answers
`200` with the stored one and `status: superseded`. Clients throttle
writes (the reader saves on chapter change, pause and close, and at most
every 30 s); a commit per write is fine at that rate. A `user_id`'s states
are removed with the user.

Video resume moves onto this record when it exists; the playback session's
`start` stays the client's choice and is not read from it by the server.

## 7. API and docs

`docs/catalogue.md`: the kinds, the reference fields and their rules (a
parentless kind that others reference is new to the catalogue), the new
fields, the content route, the profile answer for a book. `docs/api.md`: the `book` search
kind and the Open Library provider refs. A new `docs/users.md` section for
media state. Codes stay primary; no English matched anywhere.

Every change here is announced to Core and every client session before it
ships, per the standing rule. The content route and the user media state
are the two clients will build on; the kinds and fields are the ones they
must not choke on (an unknown `kind` in a list must be skipped, not fail
the list; Core is asked to confirm).

## 8. Out of scope here

- The reader itself and the download UI: the client's, with foliate-js as
  the proposal's choice.
- OPDS: a presentation of the same catalogue and content route in Atom
  XML with its own authentication. Designed after the reader works, when
  there are target apps to test against.
- Conversion, full-text indexing, annotations, Google Books.
- Reading on the TV client.

## 9. Order and size

1. Kinds, fields, scan provider, EPUB reader, path generators, ingest
   destination, the profile answer, the content route, docs, client
   announcement. The largest piece; the EPUB reader and the content route
   are the only new mechanisms.
2. Open Library provider, search, refs, artwork options. The size of the
   MusicBrainz provider.
3. User media state. Small, after the tree-native branch merges.
4. OPDS, if wanted.

Gate for each stage: the laptop suites, then macnessa's, then a deploy to
both nodes before the provider is turned on.
