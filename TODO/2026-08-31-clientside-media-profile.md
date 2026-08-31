# Client: use immutable media profiles

- Consume `GET /api/v1/catalogue/media/{media_id}/profile` when presenting playback details or constructing playback capabilities/options.
- Cache by immutable `macha:` media ID; treat `404 profile_not_available` as temporary and retain the existing session-creation fallback.
- Do not probe media client-side or cache profile data by mutable path.
- Send one stable `Idempotency-Key` for each logical session-creation attempt and reuse it after an ambiguous timeout or disconnect.
