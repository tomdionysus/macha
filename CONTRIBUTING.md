# Contributing

## Read first

The [governing laws and self-healing disciplines](ARCHITECTURE.md#governing-laws)
come before everything else in this file. They are cited by number in the
source and in review, so a comment reading "governing law 3" or "discipline 1"
refers to that section.

A change that touches scheduling, admission, priority, retry or recovery is not
complete until it says which law it serves and which discipline it follows. In
practice that means answering:

- **Which class of work does this make wait?** If the answer is a viewer, and
  the cause is another class of work, the change is wrong regardless of what it
  improves.
- **What happens when it fails repeatedly?** A new retry needs backoff, a
  failure budget, a parked state visible in `GET /api/v1/status`, and an
  operator action. A new RPC wait needs a deadline.
- **What happens when it starts up against inconsistent state?** A deterministic
  resolution is resolved, logged once, re-journalled and counted. Only key
  mismatch or header corruption may refuse to start.
- **Does anything here trust bookkeeping over ground truth it could re-derive?**
  If a check can probe the content-addressed store instead, it must.
- **If this goes wrong on the node furthest away, does it come back without
  me?** Law 4. If the honest answer is no, it does not ship in that form,
  whatever it does for throughput.
- **Is every bound here larger than one unit of the work it bounds?**
  Discipline 5. A cache that cannot hold one entry, or a budget that cannot
  admit one item, fails superlinearly and silently rather than degrading.

Memory-owning changes must additionally follow the repository's
[ownership and lifecycle contract](docs/ownership.md). Long-lived state is not
complete without an explicit owner, bound, release paths, diagnostics and a
repeat-cycle lifecycle test.

Macha is intentionally small. Prefer a direct fix over another layer.

## Branches and releases

There are exactly two long-lived branches. `main` is the last stable release;
`develop` is where work happens. No other branch is kept, and a branch is never
named after a version.

A release is a tag on `main` named exactly `x.y.z` — a bare semantic version,
with no `v` prefix, no suffix and no other decoration. The version bump belongs
in the release commit itself. New tags are annotated; the older lightweight tags
are left alone rather than rewritten.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMACHA_WARNINGS_AS_ERRORS=ON
cmake --build build -j
./run-tests.sh build
```

`-DMACHA_SANITIZE=address,undefined` (or `=thread`, separately) builds the
whole tree under a sanitizer; see `tests/TESTING.md`. Changes to threading,
lock ordering or ownership should be run under both before submitting.

Changes to cluster, metadata, storage, crypto, transport or MachaDFS semantics need a regression or fault-injection test. Do not weaken quorum safety, object integrity, crash durability or foreground I/O behaviour to simplify an error path.

## Style

- C++20.
- Prefer clear standard/POSIX interfaces.
- Keep ownership and thread boundaries explicit.
- Avoid frameworks, DI containers and new dependencies without a concrete reason.
- Build with `MACHA_WARNINGS_AS_ERRORS=ON` before submitting changes.
- Use `.clang-format`.
- Version and document public, wire and on-disk format changes.

## Dependencies

The dependency-light core/test set is C++20, OpenSSL and libcurl. Building the server and runtime-adapter tests additionally requires yaml-cpp and the FFmpeg/libavformat, libavcodec, libavutil, libswscale and libswresample development libraries. FUSE3/macFUSE is the mount dependency. libtorrent-rasterbar >= 2.0 enables the optional integrated BitTorrent acquisition component; generic filesystem ingest remains available without it. The FFmpeg command-line tools are not invoked by Macha. Boost is not otherwise required by Macha.

## Security

Do not put real cluster keys, private media or private host/address information in public issues. See `SECURITY.md`.
