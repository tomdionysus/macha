# Catalogue and scanner

The catalogue is cluster metadata. Every node converges the complete catalogue snapshot and every referenced artwork object, independently of `dht.replicas`. Catalogue synchronisation runs ahead of ordinary media repair, and catalogue reads synchronise on demand. A node reports `ready: true` only when it has the current catalogue and all referenced artwork locally.

The optional scanner is composed from catalogue providers. Each provider owns the distributed-filesystem roots it is allowed to inspect and applies only its own media parser there. Movies and TV use separate TMDB-backed providers. Music can use multiple metadata providers in priority order: MusicBrainz first, then optional Discogs fallback; album covers come from Cover Art Archive or the matched Discogs release. The scanner itself only schedules traversal, provider work and reconciliation.

Only the lowest active node ID runs a scan, so a normally configured cluster does not make the same provider requests from every node. Enable the scanner consistently on all nodes if you want automatic failover of that role. Already-bound files are identified by their stable `macha:<sha256>` media identity and are not looked up or downloaded again on every pass. Scanner-owned entries are removed when their final media binding disappears; manually-created catalogue records are not garbage-collected by the scanner.

Remote metadata work is scheduled round-robin across enabled media providers rather than exhausting Movies before TV or Music. The request budget is a hard HTTP ceiling. If a pass stops at that ceiling, each provider resumes after its last attempted path on the continuation pass, so a persistent miss in one domain cannot repeatedly starve the others. TMDB show/season misses and successful MusicBrainz/Discogs semantic misses are cached for the provider lifetime. MusicBrainz and Discogs transient HTTP/transport failures open a short circuit instead of being retried for every candidate; if every configured music metadata provider is unavailable, that media-domain pass is deferred to the next continuation batch.

Movie and TV filename recognition uses scored candidate inference rather than a single destructive parse. Candidate generators implement `MediaProbeCandidateGenerator` and receive a `MediaProbeContext`, then emit a `MediaProbe`, score and human-readable evidence. The default pipeline currently includes filename-semantic, yearless-TV, directory-structural, compact-title and legacy fallbacks. Strong evidence such as `S02E04`, an adjacent `(2003)` year, agreement with a `Season 2` directory, or a technical boundary such as `1080p`/`BluRay` outweighs weak release-name evidence. The scanner retains the best few distinct candidates and may try up to five provider interpretations within the existing provider request budget. Adding another naming convention is therefore a new generator, not another global regex that can damage unrelated filenames.

Movie editions such as `Extended`, `Remastered` and `Director's Cut` are classified separately from the provider lookup title. TMDB comparison is intentionally more forgiving than display parsing: apostrophes, `&`/`and`, roman/word sequel numbers and `Volume`/`Vol.` are canonicalised for comparison, while exact-year agreement and provider result rank can resolve aliases such as franchise-number filenames without rewriting the local title.

Music is tag-first but no longer single-hypothesis. Embedded container metadata is read once through libavformat over Macha's distributed read path and supplied to the same candidate interface as path evidence. Embedded APIC/attached-picture artwork is captured during that same read and stored as a `cover` artwork candidate alongside provider artwork; different immutable images are retained rather than collapsed merely because they share the same role. The default music candidates include authoritative embedded tags, a recording-first interpretation using the track artist, embedded tags supplemented by structured path fields, and a pure structured-path fallback. Album artist and track artist are kept separately, so compilation albums can retain `Various Artists` album context while recording-first provider lookup still searches by the performing artist. Each candidate is offered to configured music metadata providers in priority order; Discogs uses authenticated database search and release detail as a fallback rather than replacing MusicBrainz identity when MusicBrainz succeeds. `Artist - Title.ext` is recognised, and exact root-relative `Artist/Album/File` (plus `CD 2`/`Disc 2`) layouts remain useful, but arbitrary nested collection/grouping directories are not promoted to artists merely because of their depth. Missing configured provider roots make a scan partial: available roots are still ingested, but destructive reconciliation is suppressed until every configured root can be traversed. Other filesystem errors abort the pass.

The coordinator also reacts to committed namespace mutations. A metadata-generation change starts/restarts `rescan_debounce_ms` (10 seconds by default). Continuous mutation cannot postpone the pending scan beyond `rescan_max_delay_ms` (60 seconds by default), measured from the first unscanned mutation. Before scanning, the coordinator compares a deterministic namespace-content signature and runs only if files/directories actually changed. Catalogue-only metadata commits are excluded from that signature, so a scan cannot trigger itself. The periodic `interval_ms` scan remains as a safety/convergence pass.

TMDB needs an API Read Access Token. Put the token alone in a file readable by Macha. MusicBrainz does not need an API key, but requires a meaningful contact string and is rate-limited by the provider; Macha spaces its MusicBrainz API requests accordingly. Discogs database search requires authentication; create a personal token and place only the token in a file readable by Macha. Configure only curated media roots:

```yaml
catalogue:
  scanner:
    enabled: true
    interval_ms: 21600000
    rescan_debounce_ms: 10000
    rescan_max_delay_ms: 60000
    max_artwork_bytes: 16M
    providers:
      movies:
        roots: [/Movies]
        tmdb:
          enabled: true
          token_file: /etc/macha-tmdb.token
          language: en-GB
          image_size: w500
      tv:
        roots: [/TV]
        tmdb:
          enabled: true
          token_file: /etc/macha-tmdb.token
          language: en-GB
          image_size: w500
      music:
        roots: [/Music]
        musicbrainz:
          enabled: true
          contact: https://github.com/tomdionysus/macha
          cover_size: "500"
        discogs:
          enabled: false
          # token_file: /etc/macha-discogs.token
```

Movies download poster and backdrop artwork. TV downloads show poster/backdrop, season poster and episode stills. Music retains embedded cover art and provider front-cover artwork as separate candidates. Images are stored as immutable Macha objects and committed with the catalogue, so every active node receives the actual image bytes rather than depending on provider URLs at display time.

Enable the API locally:

```yaml
catalogue:
  api:
    enabled: true
    listen: 127.0.0.1
    port: 7438
    # token_file: /etc/macha-api.token
    max_request_bytes: 8M
```

Useful endpoints are:

```text
GET    /api/v1/catalogue/status
GET    /api/v1/catalogue/items?type=show&parent=...
GET    /api/v1/catalogue/search?q=expanse
GET    /api/v1/catalogue/items/{id}
PUT    /api/v1/catalogue/items/{id}
DELETE /api/v1/catalogue/items/{id}
DELETE /api/v1/catalogue/items/{id}/metadata
POST   /api/v1/catalogue/items/{id}/artwork?role=poster&mime=image/jpeg
GET    /api/v1/catalogue/artwork/{sha256}
```

Item mutations support `If-Match: "rev-N"` and return an `ETag`. `PUT` can mark an item with the internal `macha_metadata_locked=1` external ID so scanner reconciliation preserves manual descriptive changes while still reconciling live media bindings. `DELETE .../metadata` is the destructive rematch operation: it removes the selected catalogue entity and any descendants needed to release leaf media bindings, while leaving the underlying namespace media untouched. When the scanner is enabled, the serving node requests one explicit scanner pass immediately so those files can be probed and matched again; ordinary recurring scans remain coordinator-owned. If `token_file` is configured, clients must send that file's contents as a Bearer token. Keep a remotely exposed API authenticated and firewall-restricted.

Replacing or deleting the last reference to artwork records a committed retirement tombstone. The current catalogue root and all artwork referenced by it are part of the same live-object mark set as filesystem extents, so a still-live reference always wins. After `maintenance.garbage_grace_ms` (24 hours by default) the retirement is pruned and each node's bounded reachability sweep removes any old unreachable authoritative copy. A disconnected node does not need to retain or replay the tombstone forever: after rejoining, current catalogue reachability is sufficient to converge deletion.


Playback is deliberately separate from catalogue mutation. A playback session may be created from a catalogue `item_id`, in which case Macha evaluates every bound `media_id` and chooses the cheapest compatible representation, or directly from a `media_id`. See [Streaming](streaming.md).

## Cache freshness

Catalogue API reads use a shared decoded in-memory snapshot. Once warm, GET/list/search
never perform distributed metadata validation or reload a catalogue root on the request
thread. Metadata generation announcements and `metadata_cache_ms` expiry instead make
the service control plane converge the catalogue asynchronously. Validation only loads
a new catalogue object when `catalogue_root` changes; an unchanged content-addressed
root keeps the same decoded snapshot, and publication of a replacement is atomic.

If metadata validation temporarily fails after a catalogue has already been loaded,
Macha continues serving that last coherent immutable snapshot and records the refresh error.
`/api/v1/catalogue/status` exposes both `metadata_generation` (the cached catalogue's
validated metadata generation) and `known_metadata_generation` (the newest generation
the node knows exists).
