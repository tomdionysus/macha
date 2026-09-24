# What the four client sessions report against 0.48.0, collated

Date: 2026-09-21, the afternoon of the 0.48.0 cutover

Status: **Analysis. No code changed.** Four client sessions — core, web,
Android TV and mobile — smoke-tested the live cluster within hours of the
deploy and reported sixteen findings between them. This deduplicates them,
separates what is ours from what is not, and analyses the three that are real.

The headline is at the bottom of the triage and it is new: **every
first-fragment timeout today was on `PATCH`. Eight out of eight. None on
create.** That is not a slow node, it is a property of the supersession path.

## Triage

| # | Finding | Sources | Verdict |
|---|---|---|---|
| 1 | First-fragment timeout on large seeks | all 4 + this session | **Ours. Real.** 8 today, all PATCH |
| 2 | `max_video_transcodes: 1` refusals on fi-1 | mobile, TV, web | **Ours, by consequence.** Slot held by abandoned sessions |
| 3 | Node-wide `max_sessions` absent from telemetry | TV | **Ours.** 0.48.0 added 3 of 4 fields |
| 4 | Node-scoped refusal carries no axes; nothing walks on update | web, mobile, core | **Ours.** Already filed; web adds the create/update split |
| 5 | Compiled defaults contradict (`8` vs `32`) | mobile, web, core | **Ours, latent.** Live nodes unaffected |
| 6 | Account cap disables itself silently when unset/zero | mobile | **Ours, latent.** All three nodes carry `32` explicitly |
| 7 | Subtitle manifest states no segment names | core, web | **Ours. Parked by the operator.** For the record |
| 8 | "Nodes run `max_sessions: 8`" | mobile, TV, core | **Stale, and the error was mine.** They run 64 |
| 9 | Remux would not start on fi-1 | mobile | **Not established.** Node was saturated; mobile said so itself |
| 10 | Ten sessions unrefused on fi-1 | TV | **Correct behaviour.** `max_sessions` is 64 |
| 11 | Transcode limit refused cleanly, readable, no teardown | TV | **Working as designed** |
| 12 | Create-path walk works, no healthy node charged | mobile | **Working as designed** |
| 13 | `410` classified, no walk, no node marked | web, core | **Working as designed** |
| 14 | Client claims AC3/EAC3 it cannot decode | mobile | **Theirs.** Being fixed there |
| 15 | Client misreads position on transformed generation | mobile | **Theirs.** Being fixed there |
| 16 | Harness leaked ten sessions | TV | **Theirs**, and it caused some of #2 |

Six findings are ours and real, three of those latent. Four things were
confirmed working, which is worth as much as the defects. One report was stale
because of an error of mine that propagated through three sessions.

## Finding 1: the first-fragment timeout is a PATCH-path defect

**This is the one to fix.** Measured on fi-1 today:

```
first-fragment timeouts since 13:00 : 8
of which method=PATCH               : 8
of which method=POST (create)       : 0
```

Every client saw it and every client described it as a seek problem, which is
correct but incomplete — a large seek *is* a `PATCH`. What none of them could
see is that **creation never fails this way.** A cold pipeline built from
nothing, on the same node, from the same WAN-fetched bytes, makes its first
fragment inside the 15 s budget. A pipeline built to replace a running one
does not, repeatedly.

**Why the two differ, from `src/playback.cpp:2734-2744`:** on the replacement
path the outgoing generation is marked superseded but **is not stopped before
the replacement starts**:

```cpp
auto old_active = active_engine(*old);
if (old_active) old_active->segments()->mark_superseded(true);
try {
    resource_reservation = reserve_resources(*replacement, old->id);
    start_pipeline(*replacement, trace);
    // Keep the replacement reservation until the old physical pipeline
    // is stopped.  Otherwise a third concurrent request could consume
    // the apparent free slot during this handover window.
```

The comment is explicit that a handover window exists in which both pipelines
are alive. That is deliberate and it is right — the old generation must keep
serving until the new one can, or a seek would tear the picture down. But it
means **the replacement's first fragment is produced while the outgoing
pipeline is still holding decoder threads, retained memory and, on fi-1, the
WAN link.**

On a node that stores its own extents this is invisible: the second pipeline
reads from local disk and wins its race easily. On fi-1, which owns nothing
(`hosts_extents: false`), *both* pipelines pull 4 MiB stripes across the WAN at
a measured 772-3431 ms each. The startup budget is 15 s. Two pipelines
competing for one WAN link, with one of them seeking to a cold position, is
enough to miss it — and the evidence says it misses it reliably.

**Confidence.** The 8/8 PATCH split is measured and is not an artefact: the
same node served 18 successful creates in the last 30 minutes alone. The
contention mechanism is inferred from the code and the WAN timings, not
instrumented. The way to settle it is a PATCH seek on es-1 or gbni-1, which own
their extents — if the timeout is WAN contention it should not reproduce there.
**That experiment has not been run** and it is the first thing to do.

**Why it is not simply "raise `startup_timeout_ms` on fi-1".** That treats the
symptom and it makes the viewer wait longer before being told the same thing.
The interesting question is whether the outgoing pipeline should be draining
WAN bandwidth at all once its replacement is committed — governing law 2 says
do not make the viewer wait, and here the viewer is waiting behind a generation
they have already abandoned.

## Finding 2: one abandoned session can hold a node's only transcode slot for 30 minutes

Mobile saw seven `resource_limit` refusals in half an hour; TV hit the same
wall and found their own harness had leaked ten sessions; web found a mode
switch refusing itself. All three on fi-1. The measurements explain all of it:

```
session creates on fi-1 since 13:00 : 57
DELETEs                             : 0
creates in the last 30 minutes      : 18
```

**No client deletes its sessions.** Not one, all day, across four client
sessions. With `session_idle_ms` at 30 minutes, fi-1 therefore carries roughly
18 live sessions at any moment, none of them wanted by anybody.

The server is not leaking: the cleanup loop expires them on `touched +
session_idle` and erases them from the map that
`video_transcodes_locked()` counts, so the slot does come back. **The defect is
the interval, not a leak.** With `max_video_transcodes: 1`, a single abandoned
session that once transcoded denies transcoding to the entire node for up to
half an hour.

**The part that is genuinely ours to answer.** A physical pipeline is reclaimed
after `pipeline_idle_ms`, 60 seconds, precisely because no stream request has
arrived — the server has already concluded the session is not being watched.
That same evidence is not permitted to release the transcode entitlement, which
survives it by a factor of thirty. The documented contract
(`docs/streaming.md`) says a logical session keeps its entitlement "through
Direct/Remux/Transcode changes and physical idle-pipeline reclamation, then
releases it exactly once on DELETE or session expiry", and the reason is sound
— an entitlement that evaporated on reclamation would make resume-after-pause
fail against a busy node. But on a node admitting **one** transcode, the cost
of that guarantee is the whole node.

Worth considering: release the entitlement when a pipeline is reclaimed for
inactivity *and* the node is at its transcode limit, reacquiring on resume and
accepting a refusal then. That converts a certain 30-minute outage into a
possible refusal at resume.

Client-side, every client needs to `DELETE`. That is being raised with them
separately and is not a fix for this.

## Finding 3: node-wide `max_sessions` is not on the wire

Found by Android TV against the live cluster and verified here against source.
The per-node `playback` block carries five fields
(`src/status_api.cpp:362-380`, `src/telemetry.hpp:94-113`) and the node-wide
cap is not among them. **0.48.0 added three of the four and missed this one.**

Their statement of the cost is the clearest: a client that wants to tell a
viewer *"another screen on this account is playing"* versus *"this node is
full"* can state the per-account number and cannot state the other.

## Finding 4: the refusal a client can act on is the one that says least

Already filed; the web client sharpened it and asked a direct question.

The asymmetry, stated once: the **account**-scoped refusal — where walking is
pointless, since every node answers identically — carries full axes, states
limit and live count, and publishes its limit for every node. The
**node**-scoped refusal — the one case where walking is right — carries no axes
at all and its limit is on no payload anywhere.

**The web client's question, which needs the operator:** should the refusal
carry different axes on the *update* path than on the *create* path? Mobile
confirmed the create-path walk works correctly today. Web reports that nothing
walks on update, so a viewer's explicit mode choice is silently not applied.

A proposed answer, not yet decided: **yes, and the difference is real.** On
create, "this node cannot" means try another node. On update the session
*lives* on this node, so walking means abandoning it and rebuilding elsewhere —
a failover, not a retry. The useful alternative on the update path is a
different *instruction* against the same node: remux rather than transcode, a
lower height. So the update-path refusal wants `alternative_may_succeed: true`
with nothing that reads as "walk", while the create-path refusal keeps its
node-scope. The web client has offered to match the client to whatever is
decided.

## Findings 5 and 6: two latent config traps

Both real, neither affecting these three nodes, both worth closing because the
next node built will hit them.

**The compiled defaults contradict each other.** `src/config.hpp:502` is
`max_sessions{8}` against `:540` `max_sessions_per_account{32}`, and
`reserve_session_slot` checks node-wide first
(`src/playback.cpp:1402-1410`). A node that does not set these in YAML can
never reach its own per-account cap. Three separate sessions found this
independently, which is a strong argument for fixing the **defaults**, not
just the example config.

**The account cap disables itself silently.** It is enforced inside
`if (config.max_sessions_per_account)`, so unset or zero means the refusal
never fires — and a node that never refuses is indistinguishable from a client
that handles the refusal correctly. A cap test can pass having tested nothing.
All three live nodes carry `32` explicitly, verified against the pre-change
backups, so the cap phase is not affected.

## The stale report, and whose fault it was

Three sessions reported that the live nodes run `max_sessions: 8`, making the
account cap unreachable, and mobile correctly escalated it as a blocker on the
cap test. **The nodes run 64.** I raised them from 8 before the cutover and
then continued to describe the 8-vs-32 problem as live, so my own superseded
claim propagated through core to two further sessions and produced a blocking
instruction that was already false when it was issued.

Recorded because the failure mode is worth more than the fact: a server-side
statement about cluster configuration is trusted absolutely by every client
session and none of them can check it. Correcting it here is not enough; it has
to be corrected *to them*.

## What is confirmed working

Four things, from independent sessions, and they cover most of what 0.48.0
changed:

- **`410 generation_superseded` is classified correctly** by the web client —
  no cluster walk, no node marked, classification via
  `playbackFailureKindForStatus` rather than a status list.
- **The create-path walk works.** Mobile had a create refused by fi-1 with
  `resource_limit`, walked to macnessa, and succeeded. Node-scoped code, walk
  correct, no healthy node charged.
- **The transcode limit refuses cleanly**, reaching the viewer as a readable
  sentence without tearing playback down.
- **All three nodes serve 0.48.0 and TEL3 is on the wire** — the new per-node
  playback fields were read back off `GET /api/v1/status` by Android TV, which
  is the field-level proof the cutover did not take.

## Ordered next steps

1. **Run the PATCH seek on es-1 or gbni-1.** One experiment, and it separates
   "supersession is too expensive" from "fi-1 cannot do this over the WAN".
   Everything about finding 1 depends on the answer and nothing should be
   changed before it.
2. **Decide the entitlement question** in finding 2 — should reclamation
   release a transcode entitlement when the node is at its limit?
3. **Decide the create/update axes question** in finding 4; the web client is
   waiting on it.
4. **Put node-wide `max_sessions` on the telemetry block** (finding 3) and
   give `ResourceLimitError` its axes. Both additive, neither needs a flag day
   under TEL3.
5. **Fix the compiled defaults** (finding 5), since three sessions found it.
6. **Tell the client sessions the nodes run 64**, which has not been done.
