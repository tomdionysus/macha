# Subsystem crash isolation via a plugin architecture

Status: primary active programme for the "hard failure in one subsystem takes
down the entire process" P0 item in `ACTIVE.md`. Phase 0 (foundation) shipped
in 0.25.0. Phase 1 (Torrent as a plugin) and Phase 2 (FUSE as a plugin) are
still open — the P0 item does not close until a real subsystem is actually
isolated, not just the mechanism to do so.

Date established: 2026-09-05

This supersedes ad hoc try/catch as the answer to that P0 item. The concrete
incident that forced the question: a 0.24.3 bug in
`skip_blocked_namespace_operation()`'s journal bookkeeping made
`FuseFrontend`'s constructor throw `DecodeError` on replay, uncaught, which
crashed the whole `corvus-es-1` process — metadata, RPC, HTTP API and playback
included, not just the FUSE mount — 49 times before diagnosis. The fix that
shipped in 0.24.4 closed that one instance; this plan addresses the general
case: FUSE, Torrent, and any future optional subsystem must never be able to
take core down, by construction, not by remembering to wrap the right call.

Single binary, single process throughout. No separate OS processes, no IPC
protocol — that was considered and explicitly rejected as the wrong tool for
this problem. `dlopen`'d code shares the process's address space; a native
crash inside a plugin still takes the whole process down exactly as static
linkage would. This plan does not change that residual risk, and doesn't try
to — the existing `std::_Exit(1)` + systemd `Restart=on-failure` idiom
(`service.cpp:252-269`, used today for an unsafe-to-continue startup stall)
remains the correct answer for genuinely corrupted process state. What this
plan actually guarantees is narrower and fully achievable in-process: an
*ordinary* fault in an optional subsystem — an uncaught C++ exception, a
constructor that throws, a hung loop — degrades that subsystem and nothing
else.

The plugin half is not required for that guarantee; it's adopted because the
work already forces a clean interface boundary between core and each optional
subsystem, and turning that boundary into a real `dlopen` boundary converts
"optional" from a compile-time fact into a runtime, per-node, per-file fact
with no rebuild required — worth having since we're drawing the line anyway.

## Confirmed structural findings

- `main.cpp:48-61` runs FUSE on the main thread, outside any subsystem
  lifecycle: `FuseFrontend`'s constructor (`fuse_adapter.cpp:530`, where
  journal replay happens) throwing has nowhere to unwind to except `main()`'s
  outermost `catch` (`main.cpp:84`), which logs and exits the entire process.
- Background subsystem loops mostly have no top-level exception boundary at
  all. Confirmed by direct inspection: `IngestManager::loop`
  (`ingest.cpp:1036`) and `TorrentManager::loop` (`torrent.cpp:1106`) run their
  full bodies with zero `try/catch`. An exception escaping a `std::jthread`
  lambda does not reach any caller's `catch` — it calls `std::terminate()`
  directly, aborting the process before `main()`'s catch block is even
  reachable. `CacheHydrator::loop` (`hydration.cpp:713`) only catches around
  one `future.get()`, not the loop body. At least a dozen more loop threads
  across `cluster.cpp`, `net.cpp`, `http.cpp`, `fuse_frontend.cpp`,
  `media_catalogue.cpp`, `media_information.cpp`, `status_api.cpp` and
  `durability_domain.cpp` have not been individually audited but follow no
  consistent convention, so none should be assumed safe.
- `Service::initialise_services` (`service.cpp:272-378`) already demonstrates
  the correct pattern for *startup*: the whole body runs inside one
  `try/catch`, and a failure there is reported through `startup_failed_`/
  `startup_error_` rather than propagating. Nothing equivalent exists for
  steady-state operation of any subsystem.
- Optionality is compile-time-only today: `MACHA_ENABLE_FUSE`,
  `MACHA_HAVE_LIBTORRENT` and `MACHA_HAVE_MINIUPNPC` (`CMakeLists.txt`)
  decide at configure time whether a capability exists at all, with
  `fuse_stub.cpp` and inline `#ifdef MACHA_HAVE_LIBTORRENT` branches inside
  `torrent.cpp` providing the compiled-in "unavailable" behaviour. Both
  FUSE and Torrent embed a large third-party C/C++ library
  (libfuse3 + kernel driver, libtorrent-rasterbar) with the widest native
  crash surface in the process; `libav`/ffmpeg is the same class of risk but
  is a hard, non-optional dependency of playback and is out of scope here.
- `macha_core` (`CMakeLists.txt:201`) is a `STATIC` library today. Anything
  that links it more than once in the same process (core executable plus a
  plugin, naively) gets duplicate copies of every global/static and class,
  which is a correctness hazard, not a style question, the moment `FileSystem`
  or `DistributedStore` state needs to be the same object everywhere.

## Required invariants

- No ordinary exception thrown by an optional (or any) subsystem's
  construction or background loop may propagate past that subsystem's
  boundary. It must be caught, logged with full context, and turned into an
  observable health-state transition.
- A faulted subsystem is independently restartable in place — destroyed and
  reconstructed — without constructing, destructing, or pausing any other
  subsystem, any RPC/metadata/HTTP/playback thread, or the process itself.
- A subsystem that fails repeatedly within a bounded window stops
  auto-restarting and reports an explicit, operator-visible "disabled" state
  through Status/Manage — never a silent infinite crash-loop, and never a
  silent permanent hang either.
- Whether a capability (FUSE, Torrent) is present is a runtime fact — plugin
  file present/absent/version-matched — not a fact baked into the binary at
  compile time.
- Exactly one copy of every core class and its global/static state exists in
  the process regardless of how many plugins are loaded.
- A plugin built against a different core build than the one currently
  running must be refused at load time, not loaded and left to corrupt memory
  silently.

## Architecture

**`Subsystem` interface** (new `src/subsystem.hpp`, part of `macha_core`):
virtual `start()`, `stop()`, `health()` (returns an enum:
`starting`/`running`/`faulted`/`restarting`/`disabled`), `name()`. Every
optional subsystem implements this instead of exposing its concrete class
directly to `Service`.

**`macha_core` becomes `SHARED`**, not `STATIC`. The `macha` executable and
every plugin link against the one shared object, so `FileSystem`,
`DistributedStore`, `NodeRuntime`, etc. exist exactly once no matter how many
plugins are loaded. Because core and its plugins are always built from the
same source tree, same commit, same compiler flags, in the same CMake
invocation — never distributed or versioned independently — a plain C++
virtual interface across the boundary is safe; there is no need for a
brittle flattened C ABI beyond the one factory symbol needed to bootstrap it.

**Plugin contract**: each plugin is a CMake `MODULE` library exporting exactly
one `extern "C"` symbol, e.g. `macha_subsystem_create`, matching a factory
signature declared in a small header core ships (`src/subsystem_abi.hpp`):
takes a `SubsystemContext&` (the narrow set of core references a subsystem
actually needs — `FileSystem&`, `IngestManager&`, relevant config, etc.,
replacing today's practice of handing out whatever concrete internal
reference happens to be convenient) and returns
`std::unique_ptr<Subsystem>`. The symbol also carries a build-identity stamp
(e.g. the core library's git commit / `MACHA_VERSION`) that the loader checks
before calling the factory — a mismatch is refused and logged, not loaded.

**`SubsystemLoader`/`SubsystemSupervisor`** (new, in core): scans the
configured plugin directory for `.so`/`.dylib` files, `dlopen`s each,
resolves and version-checks the factory symbol, and owns the resulting
`Subsystem` behind `run_supervised`. On fault: log, mark `faulted`, apply
backoff, reconstruct in place; after N failures in a window, transition to
`disabled` and stop retrying automatically. A subsystem whose plugin file is
simply absent is reported `unavailable`, not `faulted` — those are different
facts and must stay visually distinct in Status.

**`run_supervised(name, fn)`**: the mandatory entry point for every
subsystem-owned thread, in core or in a plugin. Catches `std::exception` and
`...` at the top, logs, and transitions health — never lets an exception
reach `std::terminate`. This is the piece that fixes the exact 0.24.3 bug
class generally, independent of the plugin work, and should land first since
it's the cheapest, highest-value part of this plan.

**Config**: new optional `plugin_path` field (`Config`/`config.yaml`).
Default, when absent, resolves to the directory containing the running
executable — needs a small new platform helper (`/proc/self/exe` readlink on
Linux, `_NSGetExecutablePath` on macOS; no Windows target exists in this
codebase, so POSIX `dlopen` covers every deployed platform).

**Status/Manage API**: a `subsystems` map (`fuse`, `torrent`, ...) with
`state`, `restart_count`, `last_fault`. This should be built alongside the
existing open P1 item on honest Status aggregation rather than as a second,
parallel status mechanism — same underlying problem ("does this node
honestly report what's actually going on with a piece of itself").

## Concrete migration, one subsystem at a time

**Torrent first** — the cheaper, lower-risk proof of the pattern. `torrent.cpp`
(and its libtorrent linkage) moves out of `macha_core` into a `libmacha-torrent`
`MODULE` target, built only when libtorrent-rasterbar is found (mirrors
today's detection in `CMakeLists.txt:214-220`, now producing a plugin instead
of object files linked into `macha_core`). The inline `#ifdef
MACHA_HAVE_LIBTORRENT` branches inside `torrent.cpp` are deleted — the whole
file only exists at all in a build where libtorrent was found, so the
branches are dead weight once the plugin either exists or doesn't. `Service`
holds a `Subsystem` handle instead of a concrete `TorrentManager&`;
`AcquisitionApi`/`IngestManager` degrade to today's "no BitTorrent
acquisition" behaviour when the plugin isn't loaded, driven by the loader's
`unavailable` state rather than compiled-in stub logic.

**FUSE second** — the larger surface, informed by what Torrent proves out.
`fuse_adapter.cpp`, `fuse_frontend.cpp`, `fuse_journal.cpp` and
`fuse_mountpoint.cpp` move into `libmacha-fuse`, built only when FUSE3 is
found. `fuse_stub.cpp` is deleted outright — plugin absence now means
capability absence, with no compiled-in stand-in needed. `main.cpp` loses its
`if (config.mount_path) { run_fuse(...) blocking on main thread } else {
sigwait }` branch entirely; `main()` becomes a plain signal-wait loop
unconditionally, and FUSE's full mount lifecycle (mount, `fuse_loop_mt`,
unmount, and — critically — the constructor-time journal replay that caused
the real incident) moves inside the plugin's own `run_supervised` thread. A
replay failure now means "this attempt to establish the FUSE mount failed,"
retried with backoff or left `disabled` pending an operator, while core's
signal-wait loop, metadata, RPC, HTTP API and playback are completely
unaffected. Per the earlier decision, a crash-triggered remount is a brief,
visible unmount/remount — acceptable, and far simpler than trying to keep a
kernel mount handle alive across the owning code being torn down and rebuilt.

## Phase 0 — foundation (shipped in 0.25.0)

- [x] Add `run_supervised(name, fn)` and require it at every existing
  `std::jthread`/`std::thread` construction site in `src/` (~30 sites).
  Regression test `foundations/test_every_subsystem_thread_is_run_supervised`
  scans every `src/*.cpp` and fails if a future thread construction bypasses
  it, rather than relying on a one-time manual sweep staying true forever.
- [x] Define `Subsystem`, `SubsystemContext`, and the versioned plugin ABI
  header (`subsystem.hpp`/`subsystem_abi.hpp`). The build-identity stamp is
  project version + git commit (`kBuildIdentity`, `version.hpp.in`), not
  version alone, so a skew that doesn't bump the version number is still
  caught.
- [x] Convert `macha_core` from `STATIC` to `SHARED`. The ODR/visibility
  fallout the plan anticipated was real: `make_libav_media_engine` and the
  embedded-music-metadata pair were link seams relying on `macha_core` being
  static (their real/stub implementations were resolved by whichever
  executable linked them). Fixed by turning them into a runtime-registered
  factory/provider `macha_core` owns, with the real FFmpeg-backed
  implementations registering themselves at static-init time;
  `media_engine_stub.cpp`/`media_metadata_stub.cpp` are gone, their old
  behaviour is now `macha_core`'s own default with nothing registered.
- [x] Build `SubsystemLoader`/`SubsystemSupervisor`: directory scan, `dlopen`,
  version-checked symbol resolution, `run_supervised`-backed lifecycle,
  backoff/disable-after-N-failures policy (configurable `SubsystemRetryPolicy`).
  Verified with real `dlopen`'d fault-injection plugins in `tests/plugins/`
  (a plugin whose `start()` always throws, one with a mismatched build
  identity, one that starts cleanly) exercised through
  `tests/test_subsystem_supervisor.cpp` — not in-process mocks. One thing
  the plan didn't anticipate: `macha_core` itself lives in the same directory
  as any plugin (see its `INSTALL_RPATH`), so discovery must explicitly skip
  it by name or every scan logs a spurious "missing entry symbol" for it.
- [x] Add `plugin_path` to `Config`/`config.yaml` plus the executable-path
  default helper (`current_executable_directory()`, `/proc/self/exe` on
  Linux, `_NSGetExecutablePath` on macOS).
- [x] Add the `subsystems` block to `GET /api/v1/status` (name/state/
  restart_count/last_fault), in the cheap always-present part of the
  response, not the expensive `diagnostics` block, since it costs nothing to
  compute and is exactly what an operator needs promptly. Left as its own
  attach/detach provider pair (matching the existing `fuse_diagnostics_`/
  `convergence_diagnostics_` pattern) rather than folded into the P1
  Status-aggregation-truthfulness item, since that item is about a different
  problem (peer telemetry folding), not this node's own subsystem state.
  Currently always empty — nothing has migrated onto the interface yet.

## Phase 1 — Torrent as a plugin (shipped in 0.28.0)

- [x] Split `torrent.cpp` and its libtorrent linkage into `libmacha-torrent`;
  remove the inline `MACHA_HAVE_LIBTORRENT` branches. The split is not
  engine/no-engine but core/plugin by *dependency*: the job value types, the
  API and wire JSON, the URI sanitisers and the Torznab search client touch
  no libtorrent and stay in `macha_core` (`torrent_common.cpp`), so search
  still works on a node with no plugin. `torrent_manager.cpp` and
  `torrent_plugin.cpp` are the module.
- [x] `Service` addresses Torrent only through `Subsystem`/`SubsystemContext`;
  `AcquisitionApi` handles `unavailable` cleanly (503, and
  `/torrents/status` answers `build_available` from the runtime fact).
  One thing the plan didn't anticipate: `Subsystem` alone is not enough --
  core needs the subsystem's *own* interface too. Hence `SubsystemRegistry`
  and the abstract `TorrentService`, with lookups returning a `shared_ptr`
  so a restart cannot dangle a caller already inside a handler.
- [x] Regression: `hydration_catalogue/test_torrent_failed_ingest_retry_and_pause_intent`
  (real dlopen, Subsystem driven directly),
  `rpc_cluster/test_ingest_torrent_jobs_visible_and_actionable_from_non_owning_node`
  (full Service, plugin loaded from the build tree),
  `hydration_catalogue/test_acquisition_api_without_a_torrent_plugin_reports_it_absent`,
  `subsystem_supervisor/test_subsystem_supervisor_reports_a_declining_plugin_as_unavailable`.
- Learned here, and it applies to Phase 2 as much as this one: **do not
  `dlclose`**. Core holds `shared_ptr`s whose deleter and control block live
  in the plugin, so unmapping on supervisor stop is a use-after-unmap; the
  rpc_cluster test above segfaulted on exactly that before handles were kept
  for the process lifetime.
- Still open from this phase: a *loop*-throw (as opposed to construct/start)
  fault-injection case. `SubsystemSupervisor` has no detection for a
  subsystem's own background thread dying after a successful start (noted in
  `subsystem_supervisor.hpp`); now that a real subsystem has migrated, that
  can finally be designed against one.

## Phase 2 — FUSE as a plugin

- [ ] Split `fuse_adapter.cpp`/`fuse_frontend.cpp`/`fuse_journal.cpp`/
  `fuse_mountpoint.cpp` into `libmacha-fuse`; delete `fuse_stub.cpp`.
- [ ] Simplify `main.cpp` to an unconditional signal-wait loop; move mount
  lifecycle fully inside the plugin's supervised thread.
- [ ] Regression: replay the exact corrupt-journal scenario that crashed
  `corvus-es-1` and confirm the process survives with FUSE `faulted`/retrying
  instead of exiting.
- [ ] Regression: `kill`-equivalent fault injection mid-write/mid-mkdir,
  confirm clean unmount/remount and no core impact.

## Phase 3 — hardening and UAT

- [ ] Audit and convert the remaining unguarded thread loops
  (`cluster.cpp`, `net.cpp`, `http.cpp`, `media_catalogue.cpp`,
  `media_information.cpp`, `status_api.cpp`, `durability_domain.cpp`) to
  `run_supervised`, even though they're core, not optional — the same
  uncaught-exception-terminates-the-process hazard applies to them today and
  this plan builds the primitive that fixes it for free.
- [ ] Deployment: extend the install step to ship `libmacha_core`,
  `libmacha-fuse`, `libmacha-torrent` alongside `macha`; confirm the existing
  byte-identical-hash verification across nodes covers all shipped files, not
  just the executable.
- [ ] UAT: partial-deploy simulation (mismatched plugin vs. core build) to
  confirm the version stamp check refuses to load rather than silently
  running with a mismatched ABI.

## Risks and open questions

- **ODR/shared-state discipline** is the sharpest technical risk. Converting
  `macha_core` to `SHARED` and getting Phase 0 fully working before either
  subsystem migration is what surfaces this early, rather than discovering it
  once two plugins are already relying on it.
- **Plugin reload is not in scope as a live operational feature yet.**
  Reconstructing a `Subsystem` object in place (Phase 0's core mechanism)
  requires only that the subsystem stopped cleanly; actually `dlclose`ing and
  loading a *new* build of a plugin's `.so` while the process keeps running
  is a further step (attractive for shipping a FUSE-only fix without a full
  node restart, given the existing byte-identical-hash deploy discipline) but
  depends on the subsystem having fully quiesced first and is deferred until
  Phases 0-2 are proven.
- **Version-skew across a partial deploy** is a new failure mode this
  architecture introduces that didn't exist with one static binary — mitigated
  by the build-identity stamp check in the plugin contract, but worth
  explicit UAT coverage (see Phase 3) rather than assuming it works.
