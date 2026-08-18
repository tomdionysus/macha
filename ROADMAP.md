# Roadmap

Macha is experimental. This file records implementation directions that are useful enough to retain but are not part of the current finite-media playback contract.

## Live/event HLS mode

0.7.x transformed playback originally exposed movies and episodes as a growing HLS `EVENT` playlist. The producer published `init.mp4`, appended media fragments as they became available, and emitted `#EXT-X-ENDLIST` only when the complete source had been processed. Production was bounded by `max_ahead_segments`: client fragment requests advanced a high-water mark and the remux/transcode worker was allowed to remain only a small number of fragments ahead of that demand.

That mechanism is still useful for future genuinely live or event-shaped media, but it is not appropriate as the finite movie/episode VOD contract. 0.8.0 therefore replaces it for finite media with a complete immutable VOD manifest while retaining lazy fragment generation.

### What was unexpected

Testing the 0.7.x EVENT implementation with native browser HLS exposed behaviour that was not anticipated when the demand-driven producer was designed:

- the native player treated the end of the growing finite playlist as an event/live edge and periodically reacquired close to that edge rather than consuming every fragment sequentially;
- fragment requests consequently skipped groups of already-advertised middle segments and appeared to the application as unexplained forward playback jumps;
- because each newer request also advanced the producer high-water mark, playlist growth and native-player edge selection formed a positive feedback loop: newer request -> more production -> newer event edge -> newer request;
- reaching the current event edge could leave the player waiting/stalled for further playlist growth, which presented as playback locking until a seek created a new generation;
- these effects remained after transport and local-storage stalls had been removed, and occurred within one server playback generation without a client seek request; one captured generation requested `2,3,4 -> 10,11,12 -> 18,19 -> 20 -> 26` as the playlist expanded;
- a small or otherwise unusable first fragment could trigger native-player recovery behaviour immediately, making edge chasing more aggressive and obscuring the distinction between a mux/timestamp defect and EVENT playlist semantics.

The behaviour is not necessarily incorrect for an actual live/event presentation. The mistake was coupling those semantics to a finite VOD source and then using client position at the moving edge as producer back-pressure.

### Requirements before EVENT returns

A future EVENT/live implementation should be an explicit media mode, not an alternate representation of ordinary VOD. At minimum it should:

- decouple producer progression from the largest fragment index most recently requested by a native player;
- define whether the playlist is append-only EVENT or a sliding live window, including media-sequence/discontinuity rules;
- define target live latency and client catch-up behaviour instead of inheriting browser defaults accidentally;
- keep health/control and storage scheduling independent of media edge progression;
- treat playlist reload cadence, missing/late fragments and producer end as first-class protocol state;
- test native HLS implementations for edge selection, reconnect, pause/resume and seek-to-live behaviour;
- retain the current bounded memory/spill model without allowing a client request pattern to make production run away through an entire source.

The 0.7.x request logs are useful regression fixtures: any future EVENT mode should make non-sequential edge requests expected and harmless rather than allowing them to redefine playback time unintentionally.

## Metadata after 0.9.0

0.9.0 removes whole-namespace network and durable-write amplification from ordinary mutation by using deterministic delta CAS plus an encrypted local journal. Three scaling costs remain deliberately visible rather than hidden behind another abstraction:

- the canonical `MetadataSnapshot` is still re-encoded to derive every successor hash;
- changing one very large file carries that file's complete extent manifest in the delta;
- stale-node/read-repair still exchanges a complete snapshot rather than a range of journal deltas.

If those become material, the next metadata work should be structural rather than another transport tweak: field/extent-manifest deltas for large `FsEntry` values, checkpoint-rooted journal range catch-up for stale replicas, and eventually a persistent indexed snapshot representation that does not require rebuilding the complete canonical byte stream for each mutation. Quorum ordering, mutation IDs and committed-checkpoint recovery should remain unchanged.

## Physical small-object packing after 0.10.0

0.10.0 garbage collection operates on logical content-addressed `ObjectId`s and the current local representation remains one encrypted `<sha256>.obj` file per object. `extent_size` is a logical chunk ceiling, not a fixed physical allocation unit, so GC does not need to repack partially dead 4 MiB containers.

If large catalogues, subtitles or other future metadata create enough tiny objects for per-file filesystem overhead to matter, packing should remain a node-local `LocalStore` representation detail. The logical `ObjectId`, `ExtentRef`, DHT ownership and replication model should not change. A local index can map `ObjectId -> pack/offset/length`, while GC continues to mark individual object IDs. Pack compaction should be driven by reclaimable bytes/utilisation versus rewrite cost, with a minimum useful reclaim threshold, rather than by a fixed number of dead objects.

## macFUSE Unicode pathname correctness

0.10.2 fixes the measured macOS pathname-identity failure at the adapter boundary rather than migrating Macha namespace metadata. macFUSE high-level lookup is mounted with `norm_insensitive`, while `readdir` presents stored names in Unicode Normalization Form D as required by macFUSE/Finder. Persisted namespace strings remain byte-preserving, so existing NFC or NFD names written by earlier Macha versions stay valid. `log_level=all` records incoming path bytes and both stored/emitted dirent bytes for future platform-specific diagnosis.

The original reproductions covered both a file (`01.Clannad - Na Buachaillí lainn.mp3`) and an accented directory tree (`Café del Mar ...`); regression coverage must continue to treat both cases as part of the same mount contract.
