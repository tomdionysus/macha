# Proposal: ebooks in Macha

**Status:** Proposal  
**Date:** 24 September 2026  
**Scope:** Macha server and modern web/phone clients. The Samsung TV client is outside this feature.

## Summary

Treat ebooks as another class of immutable media in Macha. The server stores the original book files in Macha's distributed filesystem, groups related formats under one library entry, enriches the entry with book metadata and cover art, and offers three ways to use it:

- Read an owned book in Macha's browser or phone UI.
- Download its original file to the user's device, with an explicit format choice when several are available.
- Browse and download books from a compatible reading app or e-reader through an authenticated OPDS catalog.

Start with EPUB and PDF for reading, with MOBI/AZW3 supported by the browser renderer where feasible. Preserve every imported original. Format conversion and deeper device integration can follow once the basic library and delivery paths are reliable.

## Goals and boundaries

### Goals

1. Make a personal collection pleasant to browse by title, author, series, language, and format.
2. Match imported books to useful metadata and edition-appropriate covers without silently overwriting the user's choices.
3. Let a user read on a modern desktop or phone browser, including basic navigation and remembered position.
4. Make ordinary file download prominent and predictable. A downloaded file belongs to the user and can be opened by another app or transferred to a device.
5. Publish the user's accessible books through OPDS for clients that support catalog acquisition.
6. Keep Macha's storage and read planning responsible for serving large files. Book-specific indexing and metadata belong above the filesystem layer.

### Out of scope for the first release

- Reading ebooks on the Samsung TV client.
- DRM removal or reading DRM-protected commercial files.
- An ebook store, lending, or discovery of books the user does not own.
- A promise that a mobile web page can directly write to every USB-attached e-reader. Device and browser support vary.
- Automatic conversion of every format at import time.

## User experience

A book's detail page shows its cover, title, authors, series position, description, language, publication/edition data, and available formats. The main actions are **Read**, **Download**, and **Open in another app** where the platform supports sharing. If multiple formats exist, Download asks which original file to save. Unsupported formats can still be stored and downloaded.

On import, Macha shows the information extracted from the file immediately. If an external match is confident, it proposes added metadata and a cover. Ambiguous results appear as a short list of editions, with a clear **Keep file metadata** choice. Users can edit the final record and lock chosen fields against later automatic refreshes.

The browser reader offers a contents list, page/scroll modes where supported, typography and theme settings, search, and a saved position. Start with progress per user and per book format; add highlights and notes after position recovery proves robust across client versions and layout changes.

On a phone, Download uses the operating system's ordinary file handling: save to Files/Downloads, then use the share sheet or an ebook app. For Kindle, guide the user to download an EPUB and use Amazon's Send to Kindle workflow. Do not describe Kindle as a generic OPDS client. For a reader with an OPDS-capable app, the user can subscribe to Macha's catalog and acquire a book there. A desktop user can download and copy a compatible file by USB, including through Calibre if they use it.

## Data model

Separate bibliographic identity from bytes. A useful initial model is:

| Entity | Purpose | Selected fields |
| --- | --- | --- |
| Work | Groups editions of the same intellectual work | local ID, canonical title, authors, series |
| Edition | Identifies a language/publication/version and suitable cover | local ID, work ID, ISBN-10/13, language, publisher, date, external IDs, cover reference |
| Asset | An imported file, never replaced by metadata changes | Macha file/manifest ID, edition ID if known, format, byte size, content hash, embedded metadata, import source |
| User state | Personal reading state | user ID, asset ID, renderer location, updated time, preferences |
| Metadata provenance | Records and protects field choices | field, value, source, retrieved time, user-override flag |

The work/edition distinction matters: a translation, illustrated release, or revised edition can have a different cover and content despite a similar title. An asset may remain **unmatched**; it must still be usable. Several assets can represent the same edition (for example EPUB and PDF), while identical bytes need only one stored copy under Macha's existing deduplication model.

Keep provider identifiers (`openlibrary_work`, `openlibrary_edition`, `google_volume`, ISBN) as mappings, not as Macha's primary keys. Retain the original embedded cover and metadata even when displaying an external result. Never rewrite an uploaded book merely to change catalog metadata.

## Import and matching pipeline

1. **Ingest and classify.** Identify format from file content where possible, record the original filename and Macha manifest ID, and enforce the usual user permissions.
2. **Extract locally.** For EPUB, inspect the OPF package metadata and embedded cover; extract equivalent available fields from PDF and supported Kindle formats. Normalize ISBNs, names, language codes, and title punctuation, but keep the raw values.
3. **Match by identifier.** Prefer a validated ISBN or an existing provider ID to find the edition. Check author, title, language, and publication details before accepting it.
4. **Match by text.** Without a reliable identifier, search normalized title plus author; score candidate works and editions. A title alone is never enough for automatic selection.
5. **Resolve uncertainty.** Auto-apply only high-confidence additions; surface close candidates and conflicting edition details to the user. Do not replace a good embedded cover simply because a provider returned another edition's cover.
6. **Persist independently.** Store final catalog fields, provenance, and user overrides separately from the immutable asset. Refresh only fields that the user has not locked.

A background queue can perform external lookups after import so the book appears immediately. Bound concurrency, cache successful and negative lookups, and allow a manual **Find metadata** retry. A provider outage must never block reading or download.

### Metadata providers

**Open Library first.** Its Search API returns work and edition information, and its Covers API provides small, medium, and large images by cover ID or other identifiers. Use ISBN lookup for exact editions, then title/author search with edition selection. For a public-facing cover, follow Open Library's guidance to point image URLs at its cover service and link back to the corresponding record. Use cover IDs or Open Library IDs after resolving a match. Its APIs are intended for human-facing, cached discovery, not bulk harvesting or as a high-traffic data backend. Identify Macha in server-side requests and respect its published request limits. [1][2][3]

**Google Books as an optional fallback.** Its Volumes API can search by ISBN or text and return volume details and image links. Keep this provider configurable because API terms, branding/attribution rules, and quotas differ from Open Library's. Do not make a Google key necessary for basic ebook support. [4][5]

Prefer embedded covers when adequate. Use a generated typographic placeholder when neither the asset nor a provider has a suitable cover. Store provider provenance and source URLs; review image use and caching rules per provider before persisting third-party images on the Macha cluster.

## Reading implementation

Use **foliate-js** as the leading candidate for the modern web and phone reader. It is MIT-licensed and supports EPUB, MOBI, KF8/AZW3, FB2, and CBZ; PDF support is experimental and requires PDF.js. Its own README says its API is unstable and its example viewer is incomplete, so pin a tested revision and build Macha's controls around its rendering modules. Use PDF.js directly if that gives a more predictable PDF experience. [6]

Render from an authenticated Macha asset endpoint. A client may fetch and cache the file for local reading; large files and range requests should use Macha's existing read planner. Ensure that packaged book HTML is isolated from Macha's application origin and cannot execute active content or access user credentials. Test import and rendering with malformed archives, very large books, unusual fonts, right-to-left text, and fixed-layout EPUBs. Reject or safely handle unsupported encryption.

Persist a format-specific location token plus a coarse progress percentage. A token must refer to a particular asset/version, since changing files or renderer versions can invalidate it. Restore the location when possible and fall back to the nearest chapter or percentage when not. Offline reading can be added after online reading and state synchronization are stable; it needs explicit device storage controls and a clear way to remove downloaded copies.

## Distribution and device handoff

### Direct download

Serve the selected **original asset** with its correct media type, safe filename, `Content-Disposition: attachment`, authenticated access control, and resumable HTTP range support where applicable. Display format and size before download. The download route should stream through Macha's normal file API; it should not stage a complete duplicate on the web server.

A mobile browser can save a file and hand it to a reading app through normal operating-system facilities. Direct USB transfer from browser to attached hardware is a separate, device-dependent capability; it is not a first-release requirement.

### OPDS catalog

Expose an authenticated, read-only **OPDS 1.2** catalog first for broad compatibility with existing reader apps, then consider OPDS 2.0 JSON alongside it. This is a compatibility choice to validate against actual target apps, not a claim that all e-readers implement OPDS. Provide navigation by recent books, author, series, and search, and an acquisition link per available format. OPDS defines the catalog and acquisition relationships; Macha remains the file source. [7][8]

Start with HTTP Basic authentication over HTTPS only if it works with the target readers and Macha's account model; otherwise use revocable, scoped catalog credentials. Do not place long-lived bearer tokens in cover or download URLs. Test that feed links, cover images, and acquisition links all work in representative clients, especially across redirects and range requests. Apply the same per-user library permissions to catalog enumeration and file acquisition.

A user's catalog URL should be easy to copy, with setup instructions that distinguish OPDS-compatible apps from Kindle's Send to Kindle path. Do not expose a public catalog by default.

### Conversion

Preserve original formats. If a user requests a format the collection lacks, a later server-side `ebook-convert` integration could create a derived asset on demand, with the input manifest ID and conversion settings recorded for cache invalidation. Make conversion opt-in and show failures; converted output may lose styling or unsupported features. Avoid converting at import merely to fill every possible format.

## Integration with Macha

- **Filesystem:** book assets use normal manifests, immutable extents, deduplication, multisource reads, and normal deletion/GC rules. Metadata updates should not rewrite book bytes.
- **Media catalog:** add ebook work/edition/asset records alongside existing media types. Book identifiers and user edits belong in metadata, not filenames.
- **Web client:** add Books browsing, detail pages, reader route, download action, and metadata correction UI. The Samsung build can omit Books entirely.
- **API:** expose format lists, book metadata, match candidates, user edits, read state, downloads, and OPDS feeds. Keep authorization consistent across all routes.
- **Indexing:** extract import metadata and optionally full text in bounded background jobs. Full-text indexing is a later feature and should not delay first availability of an asset.
- **Observability:** track import failures, unmatched books, provider response/cache rates, download errors, reader failures, and OPDS acquisition failures without logging book text or private reading positions.

A possible route shape (illustrative, not a required API contract):

```text
GET   /api/books?query=...
GET   /api/books/{book_id}
GET   /api/books/{book_id}/metadata/candidates
PATCH /api/books/{book_id}/metadata
GET   /api/books/{book_id}/assets/{asset_id}/download
GET   /api/books/{book_id}/progress
PUT   /api/books/{book_id}/progress
GET   /opds/v1/catalog.xml
```

## Delivery sequence

| Stage | Deliverable | Acceptance check |
| --- | --- | --- |
| 1. Library and files | Import EPUB/PDF, extract local metadata and covers, browse books, download originals | A user can import an ebook and retrieve byte-identical content from another device; malformed files fail cleanly |
| 2. Matching | Open Library lookup, candidate selection, field provenance, edits and overrides | ISBN match selects the right edition; ambiguous title matches require a choice; provider outage does not block use |
| 3. Reader | EPUB reader on desktop and phone, PDF viewer, per-user progress | Resume after refresh/device switch at the same chapter or nearest safe position; large books remain usable |
| 4. Device delivery | Authenticated OPDS 1.2 with search and acquisition; phone download/share instructions | At least two independent OPDS clients can browse and fetch only authorized titles; Kindle EPUB handoff is documented |
| 5. Extensions | More formats, OPDS 2.0, offline reading, optional conversion, annotations | Each is enabled only after device and format tests establish a clear need |

## Decisions to settle during implementation

1. Whether Macha represents one local **book** as a work, an edition, or a user-managed grouping. The proposed model supports all three but the first UI must choose a primary view.
2. Which mobile and hardware reader apps to use as OPDS acceptance targets. This will determine the authentication and feed compatibility details.
3. Whether provider cover URLs are displayed directly or proxied/cached under each provider's rules.
4. Whether reading position is shared across different formats of the same edition; the initial proposal keeps it per asset to avoid inaccurate jumps.
5. Which ebook files Macha will allow into the browser renderer versus offer only for download.

## References

[1] Open Library API usage guidance: https://openlibrary.org/developers/api  
[2] Open Library Search API and work/edition data: https://openlibrary.org/dev/docs/api/search  
[3] Open Library Covers API: https://openlibrary.org/dev/docs/api/covers  
[4] Google Books API: https://developers.google.com/books/docs/v1/using  
[5] Google Books branding guidance: https://developers.google.com/books/branding  
[6] foliate-js README and license: https://github.com/johnfactotum/foliate-js  
[7] OPDS 1.2 specification: https://specs.opds.io/opds-1.2  
[8] OPDS 2.0 specification: https://specs.opds.io/opds-2.0  
[9] Calibre Content server and OPDS usage: https://manual.calibre-ebook.com/server.html and https://manual.calibre-ebook.com/faq.html  
[10] Amazon Send to Kindle supported formats: https://www.amazon.co.uk/gp/help/customer/display.html?nodeId=G5WYD9SAF7PGXRNA
