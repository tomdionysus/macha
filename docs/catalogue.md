# Catalogue and scanner

The catalogue is cluster metadata. Every node converges the complete catalogue snapshot and every referenced artwork object, independently of `dht.replicas`. Catalogue synchronisation runs ahead of ordinary media repair, and catalogue reads synchronise on demand. A node reports `ready: true` only when it has the current catalogue and all referenced artwork locally.

The optional scanner walks configured roots in the distributed filesystem and populates this catalogue automatically. TV and movie metadata comes from TMDB. Music metadata comes from MusicBrainz, with album covers from Cover Art Archive. Provider integration is behind a small interface rather than built into the scanner.

Only the lowest active node ID runs a scan, so a normally configured cluster does not make the same provider requests from every node. Enable the scanner consistently on all nodes if you want automatic failover of that role. Already-bound files are identified by their stable `macha:<sha256>` media identity and are not looked up or downloaded again on every pass. Scanner-owned entries are removed when their final media binding disappears; manually-created catalogue records are not garbage-collected by the scanner.

Filename/path recognition is intentionally simple and conservative. Typical forms are `Show/Season 02/Show.S02E05.Title.mkv`, `Movie.Title.2024.mkv`, and `Artist/Album/01 - Track.flac`; `CD 2`/`Disc 2` music directories are also recognised. Unrecognised files are ignored. Embedded audio/video tags are not parsed in 0.7.0. A failure to read any configured scan root aborts that pass rather than treating the missing root as an empty library.

TMDB needs an API Read Access Token. Put the token alone in a file readable by Macha. MusicBrainz does not need an API key, but requires a meaningful contact string and is rate-limited by the provider; Macha spaces its MusicBrainz API requests accordingly. Configure only curated media roots:

```yaml
catalogue:
  scanner:
    enabled: true
    interval_ms: 21600000
    roots:
      - /Movies
      - /TV
      - /Music
    max_artwork_bytes: 16M
    providers:
      tmdb:
        enabled: true
        token_file: /etc/macha-tmdb.token
        language: en-GB
        image_size: w500
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

Replacing or deleting the last reference to artwork records its object ID in committed metadata garbage. After `maintenance.garbage_grace_ms` (24 hours by default), every node removes its local copy; a disconnected node performs the same deletion after it rejoins. A still-live reference always wins.


Playback is deliberately separate from catalogue mutation. A playback session may be created from a catalogue `item_id`, in which case Macha evaluates every bound `media_id` and chooses the cheapest compatible representation, or directly from a `media_id`. See [Streaming](streaming.md).
