# Plan: FUSE behind the subsystem supervisor, then out into a plugin

Date: 2026-09-14

Status: refines Phase 2 of
`2026-09-05-subsystem-plugin-isolation-plan.md` into two separately
shippable stages, and closes that plan's first Phase 3 bullet as already
done. Nothing here changes the 2026-09-05 architecture (single process,
`Subsystem`/`SubsystemSupervisor`, `dlopen` with a build-identity stamp); it
changes *what moves where*, and in what order, after sizing the work against
the code as it is at 0.40.1.

Owns the P0 in `ACTIVE.md`: "A hard failure in one subsystem takes down the
entire macha process — foundation shipped in 0.25.0, FUSE/Torrent migration
still open." Torrent is done (0.28.0). This is the FUSE half, and the P0
closes when Stage A ships and passes UAT — Stage B is runtime optionality,
not crash isolation, and the 2026-09-05 plan says so itself ("the plugin half
is not required for that guarantee").

## What the code actually looks like (verified 2026-09-14)

The 2026-09-05 plan sized Phase 2 as "move four files into `libmacha-fuse`".
The split is not where that sentence implies:

- **libfuse3 is already isolated in the executable.** `CMakeLists.txt:305-307`
  puts `fuse_journal.cpp`, `fuse_frontend.cpp` and `fuse_mountpoint.cpp` in
  `macha_core`; `fuse_adapter.cpp` (the kernel op table, `run_fuse`, the
  mount watchdog, `CoveredMountpointGuard`) is compiled into the `macha`
  executable only (`CMakeLists.txt:404-447`), with `fuse_stub.cpp` standing in
  when FUSE3 is absent. `FuseFrontend` — the 6,732-line class whose
  constructor crash-looped es-1 — is pure C++ over `FileSystem` with no
  libfuse dependency, which is exactly why `tests/test_filesystem_fuse.cpp`
  (4,785 lines) can drive it without a kernel mount.
- **The lifecycle is inverted relative to every other subsystem.**
  `run_fuse` (`src/fuse_adapter.cpp:525`) blocks the main thread: it
  constructs `FuseFrontend` (`:537`, where journal replay happens), installs
  libfuse's own SIGINT/SIGTERM/SIGHUP handlers (`:566`), runs `fuse_loop_mt`
  (`:637`), and returns a process exit code (3-8) that `main.cpp:52-61` hands
  to the OS. `main.cpp:41-46` only blocks service signals — and `:64-81` only
  runs the `sigwait` loop with SIGHUP config reload — when there is *no*
  mount. A mounted node therefore cannot hot-reload config at all: SIGHUP
  reaches libfuse's handler and exits the mount loop.
- **Mount loss is a process shutdown today.** The watchdog (`:584-634`) and
  a failing `fuse_loop_mt` both call `filesystem.request_io_cancellation()`
  and `request_shutdown()` (`:627-630`, `:642-644`), then return 8 so systemd
  restarts everything. That is the behaviour the P0 says must not exist.
- **Core reaches FUSE through a weak pointer, not the registry.**
  `Service::attach_fuse_frontend` (`src/service.cpp:128-140`) is fed by a
  `frontend_observer` callback out of `run_fuse`; `blocked_namespace_operation`,
  `skip_blocked_namespace_operation` and `parked_publications`
  (`src/service.cpp:142-155`) lock that `weak_ptr`. Status gets
  `FuseFrontendDiagnostics` the same way (`src/status_api.cpp:322-331`).
  Torrent, by contrast, publishes itself into `SubsystemRegistry`
  (`src/torrent_plugin.cpp:33`) and core looks it up per call.
- **`SubsystemContext` cannot host FUSE yet.** It carries `config`, `node`,
  `ingest`, `registry` (`src/subsystem.hpp:40-45`) — no `FileSystem`, no
  hydrator. `FuseFrontend` needs the first; `run_fuse` registers the
  frontend as a `HydrationHintProvider` with the second (`fuse_adapter.cpp:564`).
- **The supervisor cannot see a subsystem die after `start()`.**
  `SubsystemSupervisor::run_entry` treats an exception from `create()` or
  `start()` as a fault (`src/subsystem_supervisor.cpp:156-159`) and then
  parks on `entry.cv.wait(lock, stop, [] { return false; })` (`:196-197`)
  until told to stop. `subsystem_supervisor.hpp:40-43` records this as
  deliberately deferred until a real subsystem needed it. FUSE needs it:
  mount loss and a dead `fuse_loop_mt` are post-start faults.
- **The constructor can wait forever.** `wait_for_initial_namespace`
  (`src/fuse_frontend.cpp:4515-4528`) spins in 100 ms sleeps with no stop
  token, from `initialise_namespace` (`:4530`), from `State::start`, from the
  constructor (`:5230-5233`). On the main thread that is the "unbounded
  startup wait with no escape" item in `ACTIVE.md`. On a supervised thread
  it becomes worse unless fixed: `Service::stop()` → `subsystems_.stop()`
  would join a lifecycle thread stuck inside `create()`.
- **The fail-closed guard assumes one attempt per process.**
  `CoveredMountpointGuard` (`fuse_adapter.cpp:473-521`) records the
  mountpoint's mode at construction and restores it only on a clean exit
  (`:657-658`). A second attempt after an unexpected loss would record the
  already-fail-closed mode as "original". Production is not exposed: with
  `fail_closed_mountpoint: true` (`macha.yaml.example:185`) the immutable
  flag set at preparation makes `protect()` a no-op (`:509-510`). The
  mode-bit path still needs to be right for a node without it.
- **Discipline 3 changed what "the es-1 scenario" is.** A torn or corrupt
  journal no longer throws on replay: `test_fuse_journal_fuzz_every_frame_mutation_still_starts`
  (`tests/test_filesystem_fuse.cpp:4428`) proves every frame mutation still
  starts. The regression the 2026-09-05 plan asks for ("replay the exact
  corrupt-journal scenario") cannot be written as stated; the constructor
  fault has to be provoked another way (below).
- **Phase 3's first bullet is already done.** Phase 0 wrapped every thread
  entry in `run_supervised` and `foundations/test_every_subsystem_thread_is_run_supervised`
  (`tests/test_foundations.cpp:1168`) scans `src/*.cpp` to keep it that way.
  `cluster.cpp`, `net.cpp`, `http.cpp`, `media_catalogue.cpp`,
  `media_information.cpp`, `status_api.cpp` and `durability_domain.cpp` all
  pass it today. Strike the bullet.

## Decisions

1. **Two stages, shipped separately.** Stage A puts FUSE behind the
   supervisor while everything stays linked where it is. Stage B moves the
   libfuse-dependent code into `libmacha-fuse`. A gets the P0; B gets
   runtime optionality. A is useful on its own and is the one with UAT risk,
   so it goes first and soaks before B.
2. **The plugin boundary is libfuse, not `FuseFrontend`.** Stage B moves
   `fuse_adapter.cpp` and the new `FuseSubsystem` into the module and leaves
   `fuse_frontend.cpp`/`fuse_journal.cpp`/`fuse_mountpoint.cpp` in
   `macha_core`. Reasons: (a) libfuse3 is the native crash surface the plan
   is worried about, and it is already the only thing in the executable;
   (b) `FuseFrontend`'s consumers in core (`Service`, `ClusterStatusService`)
   keep concrete types, so no abstract `FuseService` and no relocation of the
   110-field `FuseFrontendDiagnostics`; (c) `tests/test_filesystem_fuse.cpp`
   keeps linking `macha_core` and constructing `FuseFrontend` directly — the
   biggest hidden cost in the original Phase 2 disappears. The 2026-09-05
   plan's invariant still holds: no `libmacha-fuse.so` on disk means no
   mount capability at runtime, and `fuse_stub.cpp` is deleted.
3. **Mount loss becomes a subsystem fault, not a process exit.** The
   supervisor remounts with backoff and disables after the policy's failure
   budget, exactly as it does for a failed `start()`. The process exit
   codes 3-8 go away; a mounted node's exit status is 0 on signal like any
   other. `std::_Exit(1)` for an unsafe-to-continue startup stall is
   untouched.
4. **Signals are core's, always.** `main()` becomes the unconditional
   sigwait loop. libfuse's handlers are never installed. Side effect worth
   having: SIGHUP config reload now works on a mounted node.
5. **The constructor wait gets a stop token.** `wait_for_initial_namespace`
   is made cancellable as part of Stage A, because Stage A is what makes the
   hang reachable from `Service::stop()`. Bounding it (fault after N seconds
   → `faulted`, retried) is folded in too — the supervisor turns "silent
   forever" into a visible state for free.

## Design

### `FuseSubsystem : Subsystem`

One class, in `src/fuse_subsystem.cpp`. Stage A compiles it into the `macha`
executable next to `fuse_adapter.cpp` (it needs libfuse); Stage B moves both
into the module.

- **`create(context)`** (the plugin entry / builtin factory): declines with
  no instance when `config.fuse.mount_path` is unset — same "not a fault"
  path as `torrent.enabled == false` (`torrent_plugin.cpp:58-65`), so Status
  says `unavailable`. Otherwise runs mountpoint preparation
  (`prepare_fuse_mountpoint`, moved out of `main.cpp:29-32`; idempotent, and
  `unmount_if_mounted` is exactly what a retry after an unexpected loss
  needs) and constructs `FuseFrontend(filesystem, config, stop_token)`. A
  throw here is a construction fault: the supervisor logs, backs off,
  retries, disables — the process does not notice.
- **`start()`**: registers the frontend with the hydrator and the registry,
  then spawns the mount thread under `run_supervised("fuse-mount", ...)`.
  The thread body is `run_fuse` minus signal handlers, minus
  `request_shutdown`, minus exit codes: `fuse_new` → `fuse_mount` → guard
  `protect()` → `fuse_loop_mt` → unmount → destroy. Any failure — mount
  refused, watchdog-detected loss, loop returning an error — calls the fault
  sink (below) with a reason and returns. Mount-loss handling keeps
  `filesystem.request_io_cancellation()` so in-flight FUSE requests fail
  closed; the next attempt's `reset_io_cancellation()` already exists at
  `fuse_adapter.cpp:577`.
- **`stop()`**: withdraws from registry and hydrator, `fuse_session_exit`,
  joins the mount thread, `fuse_frontend->stop()`. Safe to call twice; the
  destructor calls it.
- **Kernel op table**: unchanged. `fuse_get_context()->private_data` keeps
  pointing at the `FuseFrontend`.

### Supervisor: post-start faults

Add to `Subsystem` a non-breaking hook,
`virtual void attach_fault_sink(std::function<void(std::string)>) {}`,
which `run_entry` calls between `create()` and `start()` with a closure that
sets `entry.fault_requested` + `last_fault` and notifies `entry.cv`. The park
at `subsystem_supervisor.cpp:196-197` waits on
`fault_requested || stop`; on fault it stops and destroys the instance and
falls through to the existing backoff/disable path. Torrent and the five
test plugins compile unchanged. This is the "loop-throw fault-injection
case" left open at the end of Phase 1, designed against a real user.

### Supervisor: builtin subsystems (Stage A only)

`SubsystemSupervisor::start` only discovers `.so` files. Stage A needs
`add_builtin(name, SubsystemFactory)` so the FUSE entry can be supervised
while still linked into the executable; the entry runs through the same
`run_entry`, reports under the name `fuse` in Status. Stage B deletes the
call site (discovery finds `libmacha-fuse` instead) and can keep the method
for tests or remove it.

### Context and registry

- `SubsystemContext` gains `FileSystem* filesystem` and
  `HydrationManager* hydration` (or `CacheHydrator*` — whichever
  `add_provider` lives on). Both exist by the time `subsystems_.start()` runs
  at the end of `initialise_services`; `Service::stop()` already stops
  subsystems before hydration/ingest (`service.cpp:739-742`), so the
  destruction order is right without changes.
- `SubsystemRegistry` gains `publish_fuse(std::shared_ptr<FuseFrontend>)`,
  `withdraw_fuse(const FuseFrontend*)`, `fuse()` — the concrete class, per
  decision 2. `Service::attach_fuse_frontend` and `fuse_frontend_` go away;
  `blocked_namespace_operation`/`skip_blocked_namespace_operation`/
  `parked_publications` call `registry_.fuse()` and answer "none" when it is
  null. `cluster_status_.attach_fuse_diagnostics` is installed once in the
  `Service` constructor with a provider that does the same lookup, next to
  `attach_subsystem_diagnostics` (`service.cpp:103`). The `shared_ptr`
  lookup is what makes a restart safe for a caller already inside a handler
  — the lesson from Phase 1.

### `main.cpp`

Loses the mount branch entirely: block signals unconditionally before
`Service` is constructed (so every thread inherits the mask, including
libfuse's workers), construct, `start()`, sigwait loop with SIGHUP reload,
`stop()`, return 0. About 30 lines shorter.

### Fail-closed guard across attempts

Record the mountpoint's pre-protection mode once per process, in the
preparation record `fuse_mountpoint_preparation()` already keeps, and have
the guard restore *that* on a clean stop rather than whatever it observed at
its own construction. After an unexpected loss the directory stays
non-writable until a later attempt stops cleanly. With the immutable flag
(production) none of this fires; it exists for the mode-bit path.

### Cancellable, bounded startup wait

`FuseFrontend(FileSystem&, FuseConfig, std::stop_token)`;
`wait_for_initial_namespace` polls the token and throws `FsError(EINTR)` on
stop, and throws a distinct "metadata not ready within
`fuse.initial_namespace_timeout`" after a configurable bound (default
generous — minutes, not seconds; the 2026-09-06 lesson about elapsed-time
gates applies, so the bound is on *no metadata at all*, not on slow
progress). Either throw is a construction fault to the supervisor. Existing
`FuseFrontend` callers in tests pass a default token.

## Stage A — supervised in place (shipped in 0.41.0)

- [x] `Subsystem::attach_fault_sink`; `run_entry` waits on fault-or-stop and
  recycles the instance through the existing backoff/disable path.
  Regression in `tests/test_subsystem_supervisor.cpp` with a new
  `tests/plugins/test_plugin_faulting_after_start.cpp`: starts cleanly,
  reports a fault from its own thread, expect `faulted` → `restarting` →
  `running`, `restart_count == 1`; N faults inside the window → `disabled`.
- [x] `SubsystemSupervisor::add_builtin`. Unit-tested through the same file
  with an in-process factory.
- [x] `SubsystemContext` + `filesystem`/`hydration`; `SubsystemRegistry` +
  `publish_fuse`/`withdraw_fuse`/`fuse()`; `Service` consumers switched to
  the registry; `attach_fuse_frontend`/`fuse_frontend_` deleted.
- [x] `FuseFrontend` stop token + bounded initial-namespace wait.
  Regression: construct against a `FileSystem` whose metadata never becomes
  ready, request stop, constructor throws within the poll interval.
- [x] `FuseSubsystem` with the mount loop factored behind a small
  `FuseMountDriver` interface (real: libfuse; test: blocks until exit, can
  be told to "lose the mount" or "refuse to mount"). The double is what lets
  the crash-isolation regressions run without a kernel mount, which CI and
  the macOS build have never had.
- [x] `main.cpp` unconditional sigwait; `prepare_fuse_mountpoint` moves into
  `FuseSubsystem::create`.
- [x] Fail-closed guard restores the preparation-time mode.
- [x] Regressions (all through a real `Service` with the test driver, no
  kernel):
  - constructor fault: an unwritable spool directory (EACCES) or an injected
    throw makes `FuseFrontend` construction fail → Status shows
    `subsystems.fuse` = `faulted`, `last_fault` set, `restart_count`
    climbing, the HTTP API and metadata still answer; fix the directory →
    `running` without a restart. This is the es-1 scenario in its post-
    discipline-3 form.
  - mount loss: driver reports loss mid-write → `faulted`, in-flight FUSE
    request gets EIO/EINTR, remount → `running`, the write's publication
    resumes from the journal.
  - stop during startup wait: `Service::stop()` returns promptly while
    `create()` is waiting for metadata.
  - `wait_for_idle`/`parked_publications`/`blocked_namespace_operation`
    endpoints answer 404-shaped "none" while FUSE is `faulted`, not 500.
- [x] `foundations` addition: `main.cpp` contains no `run_fuse` call and no
  conditional signal mask (a one-line scan, same style as the
  `run_supervised` test).
- [x] Docs: `docs/operations.md` "Subsystem plugins" gets a `fuse` row and
  loses "FUSE is still linked into the executable"; CHANGELOG entry;
  `macha.yaml.example` documents `fuse.initial_namespace_timeout`.

## Stage B — `libmacha-fuse` (shipped in 0.41.0)

- [x] `add_library(macha-fuse MODULE src/fuse_adapter.cpp src/fuse_plugin.cpp)`
  mirroring `macha-torrent`: built only when FUSE3 is found, links
  `macha_core` + FUSE3, installs to `MACHA_PLUGIN_INSTALL_DIR`. The macOS
  include-order workaround moves with it. `macha` stops linking libfuse
  (verified: `otool -L build/macha` names no fuse library).
  **Narrower than planned, and better for it:** only `fuse_adapter.cpp` and
  the entry point moved. `fuse_subsystem.cpp` stays in `macha_core` because
  `FuseSubsystem` does not touch libfuse -- it is core logic over
  `FuseFrontend`, the registry and the hydrator -- so keeping it in core
  lets the Stage A tests build one directly with a test driver and needs no
  second copy of anything.
- [x] Delete `fuse_stub.cpp` and the `add_builtin` call; the plugin exports
  `macha_subsystem_entry` returning `make_fuse_subsystem`, like
  `torrent_plugin.cpp:51-71`.
- [x] Tests: `MACHA_TEST_FUSE_PLUGIN` next to `MACHA_TEST_TORRENT_PLUGIN`
  (`CMakeLists.txt:511-515`); one Service-level test that points
  `plugin_path` at the build tree and sees `subsystems.libmacha-fuse` go
  `unavailable` (no `mount_path`) — the dlopen, entry-symbol and
  build-identity path is otherwise covered by the existing supervisor tests.
  The Stage A regressions keep running against the builtin factory through
  the driver double, so they do not need a kernel or a dlopen.
- [x] Deployment: `cmake --install` now carries `libmacha-fuse.so` into
  `lib/macha/plugins/`, and `docs/operations.md` names it as a file that
  must be copied with the binary.
- [ ] **Still open:** nothing mechanically verifies that a deploy shipped
  every file. The operations doc says to hash all of them; there is no
  script in this repo that does, and deployment is hand-run rsync. A node
  that receives `macha` + `libmacha_core` without the new plugin loses its
  mount silently until someone reads Status.
- [ ] **Still open (cluster UAT):** partial-deploy — ship a `libmacha-fuse`
  from a different build and confirm Status shows `disabled` with the
  mismatch in `last_fault` and the node otherwise serves. The refusal path
  itself is covered by `subsystem_supervisor/test_subsystem_supervisor_refuses_a_mismatched_plugin`;
  what is not covered is a real node in that state.

## UAT (cluster, both stages) — outstanding

Rolling, viewers checked first (`feedback-rolling-restart-viewers`). With
gbni-1 dark and gbni-2 unreachable from the current site, es-1 is the only
node available; UAT that needs a peer waits for gbni-1's power.

1. Deploy to one node, confirm `subsystems.fuse` = `running` and the mount
   serves.
2. Provoke a constructor fault on the box the same way the regression does
   (an unwritable spool directory), restart the service: expect the node to
   come up, serve HTTP/metadata/playback, and show `fuse` `faulted` with the
   reason; fix the directory; expect `running` with no restart.
3. `umount -l` the mount out from under it: expect `faulted` → remount →
   `running`, `restart_count == 1`, no process restart in `journalctl`.
4. SIGHUP the mounted node: expect a config reload log line, mount intact.
5. Full-suite `--serial` on the node before trusting the build
   (`project-dev-workflow`).

## Risks and open questions

- **`fuse_loop_mt` teardown under `fuse_session_exit` from another thread**
  is documented libfuse behaviour, but the existing code has only ever
  exited via libfuse's own signal handlers. The driver double does not cover
  this; UAT step 3 and a clean `systemctl stop` do.
- **Restart storms.** A mount that flaps (a broken kernel module, a
  mountpoint that keeps vanishing) will now cycle through the supervisor's
  retry budget instead of taking the process down once. The budget and its
  window are the existing `SubsystemRetryPolicy` defaults; they were chosen
  for Torrent and should be checked against how long a FUSE remount takes.
- **Durability poisoning** (`durability_poisoned`, the `ACTIVE.md` item):
  a supervised FUSE makes "signal a fault and let the supervisor rebuild the
  frontend" the natural deliberate recovery path. Not in scope here, but
  the fault sink is the hook it will use; note it on that item once Stage A
  lands.
- **Plugin reload** stays out of scope, as the 2026-09-05 plan says.

## Sizing

Against Phase 1 (Torrent: ~1,300 insertions / 600 deletions over 29 files,
one release, commit `6f75081`):

- Stage A: about 2 focused sessions plus a UAT cycle. The supervisor fault
  sink, the registry switch and `main.cpp` are mechanical; the driver
  interface and the mount-thread rewrite are the design work; the
  regressions are most of the lines.
- Stage B: about 1 session plus UAT. CMake and install plumbing with a
  working template, one new test, docs.
- Phase 3 remainder: folded into Stage B (deploy hashing, skew UAT); the
  loop audit bullet is struck.
