# Suite failures seen while measuring T0 (2026-09-30)

All on the laptop while other projects' builds held the load average at
110-370. fi-1 (GCC, load 1-2) passed every case of all three suites on the
same code: 614 + 15 + 21 (`suites/`).

| case | laptop full-suite runs | repeat in isolation | 0.73.2 vs 0.74.0, interleaved |
|---|---|---|---|
| `media_playback/test_abandoned_transcode_pipeline_is_reclaimed_before_session` | failed run 2 | 14/30 failed, load 160 | 0.73.2: 5+18+14+16 = 53/120; 0.74.0: 10+13+18+14 = 55/120 |
| `filesystem_fuse/test_fuse_durable_journal_accepts_authoritative_data_done_without_published_prefix` | failed run 1 | 30/30 passed, load 160 | not needed |
| `rpc_cluster/test_repair_pass_keeps_its_place_across_generations_and_restarts` | failed run 1 | 30/30 passed, load 160 | not needed |

The playback case fails at the same rate on 0.73.2 (worktree at
`14634ba`), so T0 did not introduce it. Its mechanism is in the test: the
pipeline's idle lease is 50 ms (`streaming.pipeline_idle`) and the test
renews it with requests 20 ms of wall-clock sleep apart
(`tests/test_media_playback.cpp:2070-2076`); on an overloaded host a 20 ms
sleep overruns 50 ms and the pipeline is reclaimed before the next request,
which then does not answer 200. It is a P0 defect in the test's use of real
time, the class T1's injected clock removes; where the fix lands waits on
the spec's open question 5.

The other two passed every isolated repetition at the same load; each
failed once in a full-suite run at load 366, and each is timing-dependent
by construction (a 1 s publication quiet window; peer batch-request
counts). Neither is closed: both are to be measured with `--repeat` on
fi-1, where the suite is the baseline.
