# Logical viewer transcode admission

Date: 2026-09-03

## Root cause

Transcode admission was derived from currently running media-engine pipelines.
That physical lifecycle is shorter than a player/UI session: seeks and quality
changes replace generations, idle reclamation removes an encoder while keeping
the session reconcilable, and some clients recover with another POST. Those
gaps allowed a different viewer to take the slot and made the original viewer's
ordinary seek/reconfiguration fail with `video transcode limit reached`.
`Idempotency-Key` could not solve this because it intentionally identifies one
unchanged creation attempt, not the longer-lived player.

## Completed implementation

- Added process-local ownerless `LogicalViewerSession` state. A server session
  always has one, and all PATCH generations share it.
- Added the optional `Macha-Viewer-Session` request header, with equivalent
  `viewer_session_id` JSON compatibility. A client key identifies one
  persistent player/UI session on the selected node; it is not cluster-owned
  playback state.
- A replacement POST with the same viewer key is serialized, preserves the
  server session ID/token, starts the new child pipeline, atomically swaps it
  into the session map, stops the old pipeline and removes its generation.
- Video/audio entitlements are acquired at most once per logical session and
  persist across seeks, replacement POSTs, Direct/Remux/Transcode changes and
  physical idle-pipeline reclamation. DELETE or logical-session expiry releases
  them exactly once.
- Pending admission remains reserved, so distinct viewers cannot race through
  the configured limit. Failed startup rolls back only an entitlement newly
  acquired by that failed transition.
- Status `video_transcodes`/`audio_transcodes` now reports logical admission
  entitlements. Added `running_video_transcode_pipelines` and
  `running_audio_transcode_pipelines` for the distinct physical lifecycle.
- Allowed and exposed the new header through browser CORS and documented the
  client contract in `docs/streaming.md`.

## Verification

- Added `test_logical_viewer_keeps_one_transcode_entitlement_across_replacements`.
  It exercises concurrent and sequential replacement POSTs, Direct, Remux and
  Transcode transitions, repeated PATCH seeks, a competing viewer, stable
  identity, single admission accounting, DELETE and exact reuse of released
  capacity.
- Updated idle-pipeline reclamation coverage to prove a reclaimed physical
  encoder reports zero running pipelines without surrendering its logical
  entitlement, and that only DELETE lets another viewer enter.
- Focused playback suite: 24/24 passed.
- Complete core suite: 276/276 passed with eight process-isolated slots.
- Runtime dependency suite: 4/4 passed.

## Client action

Generate one opaque `Macha-Viewer-Session` value when a player/UI session is
opened, preserve it across every creation/retry/seek/mode change for that
player, and discard it when the player is closed. Continue using a distinct
`Idempotency-Key` per logically distinct POST attempt and reuse that attempt key
only for ambiguous retries.

## UAT checkpoint

The complete source tree was deployed to all four nodes. GBNI-1, GBNI-2 and
ES-1 were built concurrently and installed with identical aarch64 binary
SHA-256 `d8a5a49ddb1e1ce84a9d84760741126b834f98bc6412c5a995f703bb5994ab2c`.
All three systemd services report active/running with zero restarts. Node 200
was installed from the verified local build and restarted manually. Its cold
history reconstruction completed normally; all four nodes then reported
healthy, writable, online 4/4 and generation 3904. Every playback status
endpoint exposes the new logical-entitlement and physical-pipeline fields, with
zero sessions at the clean baseline.

A short behavioural UAT is still useful:
with `max_video_transcodes: 1`, repeatedly seek and switch one player through
Direct/Remux/Transcode while keeping its viewer key stable. No request should
return 429, Status should retain one entitlement, and a genuinely independent
viewer should be refused until the first player is deleted or expires.
