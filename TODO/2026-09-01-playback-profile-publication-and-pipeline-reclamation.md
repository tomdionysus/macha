# Playback profile publication and pipeline reclamation

Date: 2026-09-01

Status: implementation and local verification complete; deployment and loaded
playback UAT remain.

## Trigger

The loaded four-node UAT exposed two independent viewer failures on node 50:

- a completed background media scan lost its publication result when catalogue
  metadata changed concurrently, making later admission repeat avoidable remote
  inspection; and
- after unreliable client/network behaviour, two logical sessions retained the
  only permitted video transcode. The existing 30-minute session expiry made a
  lost client `DELETE` capable of denying subsequent playback for far too long.

## Implementation

Media-information publication now retains a completed immutable profile across
transient catalogue/CAS failure. Its event-driven worker retries after a bounded
250 ms timer; it does not rescan the media and does not busy-poll. The legacy
playback-owned asynchronous publisher follows the same preserve-and-retry rule.

Physical transformed pipelines now have an independent configurable inactivity
lease:

```yaml
streaming:
  pipeline_idle_ms: 60000
  session_idle_ms: 1800000
```

Valid current-generation playlist, fragment and subtitle requests renew the
physical lease. Stale-generation and invalid requests do not, so a retry loop
receiving 404 cannot pin an encoder. Reclamation is scheduled by the existing
event-driven cleanup condition variable, never runs while a valid stream HTTP
request is active, stops the physical encoder and removes its generation spill
directory. The logical session remains until `session_idle_ms` for control and
reconciliation. A reclaimed encoder therefore no longer counts against
`max_video_transcodes` or `max_audio_transcodes`.
The retained logical session can be restarted through its normal `PATCH` path;
an idempotent `POST` still reconciles the same logical session rather than
creating another lease.

`GET /api/v1/playback/status` now reports:

- `pipeline_idle_ms`; and
- cumulative `idle_pipelines_reclaimed`.

## Tests

New regressions prove:

- a synthetic first publication conflict retries the already-completed profile
  and succeeds on the second attempt with exactly one media scan;
- valid current-generation traffic renews the physical pipeline lease;
- repeated stale-generation 404 requests do not renew it;
- the cleanup worker reclaims the encoder but retains the logical session; and
- a new transcode is admitted afterward despite a one-video-transcode limit.

Verification on the final local source:

- build: passed;
- focused playback suite: 23/23;
- runtime/libav/configuration suite: 3/3; and
- complete core suite: 248/248, including
  `hydration_catalogue/test_catalogue_sync_search_and_artwork_gc`.

## UAT gate

Deploy the complete source tree and rebuild all four nodes. With rsync active,
start, seek and abandon/reload playback over the normal imperfect Wi-Fi client
path. Verify:

1. completed profiles survive concurrent catalogue generations and subsequent
   admission logs an immutable-profile hit with no probe reads;
2. valid active playback is not reclaimed;
3. after an abandoned transformed stream, `idle_pipelines_reclaimed` advances
   within the configured interval and transcode counts fall while the logical
   session count may remain;
4. a subsequent viewer is admitted without `video transcode limit reached`;
5. the publisher continues useful progress without viewer/control waits; and
6. process RSS drops after the reclaimed pipeline and publication buffers are
   released.
