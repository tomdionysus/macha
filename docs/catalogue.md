# Catalogue and scanner

The catalogue is cluster metadata. Every node converges the complete catalogue snapshot and every referenced artwork object, independently of `dht.replicas`. Catalogue synchronisation runs ahead of ordinary media repair, and catalogue reads synchronise on demand. A node reports `ready: true` only when it has the current catalogue and all referenced artwork locally.

The optional scanner is composed from catalogue providers. Each provider owns the distributed-filesystem roots it is allowed to inspect and applies only its own media parser there. Movies and TV use separate TMDB-backed providers; music uses a MusicBrainz-backed provider, with album covers from Cover Art Archive. The scanner itself only schedules traversal, provider work and reconciliation.

Only the lowest active node ID runs a scan, so a normally configured cluster does not make the same provider requests from every node. Enable the scanner consistently on all nodes if you want automatic failover of that role. Already-bound files are identified by their stable `macha:<sha256>` media identity and are not looked up or downloaded again on every pass. Scanner-owned entries are removed when their final media binding disappears; manually-created catalogue records are not garbage-collected by the scanner.

Movie/TV filename recognition remains intentionally conservative. Music is different: the music provider first reads embedded container tags through libavformat over Macha's own distributed read path, including title, album artist/artist, album, track/disc, date/year and MusicBrainz IDs where present. Missing fields fall back to filename/path inference. `Artist - Title.ext` is recognised, and exact root-relative `Artist/Album/File` (plus `CD 2`/`Disc 2`) layouts remain useful, but arbitrary nested collection/grouping directories are not promoted to artists merely because of their depth. Missing configured provider roots make a scan partial: available roots are still ingested, but destructive reconciliation is suppressed until every configured root can be traversed. Other filesystem errors abort the pass.

The coordinator also reacts to committed namespace mutations. A metadata-generation change starts/restarts `rescan_debounce_ms` (10 seconds by default). Continuous mutation cannot postpone the pending scan beyond `rescan_max_delay_ms` (60 seconds by default), measured from the first unscanned mutation. Before scanning, the coordinator compares a deterministic namespace-content signature and runs only if files/directories actually changed. Catalogue-only metadata commits are excluded from that signature, so a scan cannot trigger itself. The periodic `interval_ms` scan remains as a safety/convergence pass.

TMDB needs an API Read Access Token. Put the token alone in a file readable by Macha. MusicBrainz does not need an API key, but requires a meaningful contact string and is rate-limited by the provider; Macha spaces its MusicBrainz API requests accordingly. Configure only curated media roots:

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
```

Movies download poster and backdrop artwork. TV downloads show poster/backdrop, season poster and episode stills. Music downloads the front album cover. Images are stored as immutable Macha objects and committed with the catalogue, so every active node receives the actual image bytes rather than depending on provider URLs at display time.

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
POST   /api/v1/catalogue/items/{id}/artwork?role=poster&mime=image/jpeg
GET    /api/v1/catalogue/artwork/{sha256}
```

Item mutations support `If-Match: "rev-N"` and return an `ETag`. If `token_file` is configured, clients must send that file's contents as a Bearer token. Keep a remotely exposed API authenticated and firewall-restricted.

Replacing or deleting the last reference to artwork records a committed retirement tombstone. The current catalogue root and all artwork referenced by it are part of the same live-object mark set as filesystem extents, so a still-live reference always wins. After `maintenance.garbage_grace_ms` (24 hours by default) the retirement is pruned and each node's bounded reachability sweep removes any old unreachable authoritative copy. A disconnected node does not need to retain or replay the tombstone forever: after rejoining, current catalogue reachability is sufficient to converge deletion.


Playback is deliberately separate from catalogue mutation. A playback session may be created from a catalogue `item_id`, in which case Macha evaluates every bound `media_id` and chooses the cheapest compatible representation, or directly from a `media_id`. See [Streaming](streaming.md).
