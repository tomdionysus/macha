# Client: use immutable media profiles

- Consume `GET /api/v1/catalogue/media/{media_id}/profile` when presenting playback details or constructing playback capabilities/options.
- Cache by immutable `macha:` media ID; treat `202 Accepted` as queued background work and retry after the advertised delay.
- Do not probe media client-side or cache profile data by mutable path.
- Send one stable `Idempotency-Key` for each logical session-creation attempt and reuse it after an ambiguous timeout or disconnect.
- Send one stable `Macha-Viewer-Session` value for the lifetime of one player/UI
  session. Keep that value across POST retries, seeks, quality/mode changes and
  replacement creations; use a different value for an independent viewer.
- Begin normal playback negotiation even when the advisory profile endpoint
  reports pending or unavailable. Do not wait on or probe media client-side.
