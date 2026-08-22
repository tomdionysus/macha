# Contributing

Macha is intentionally small. Prefer a direct fix over another layer.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMACHA_WARNINGS_AS_ERRORS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Changes to cluster, metadata, storage, crypto, transport or filesystem semantics need a regression or fault-injection test. Do not weaken quorum safety, object integrity, crash durability or foreground I/O behaviour to simplify an error path.

## Style

- C++20.
- Prefer clear standard/POSIX interfaces.
- Keep ownership and thread boundaries explicit.
- Avoid frameworks, DI containers and new dependencies without a concrete reason.
- Build with `MACHA_WARNINGS_AS_ERRORS=ON` before submitting changes.
- Use `.clang-format`.
- Version and document public, wire and on-disk format changes.

## Dependencies

The core dependency set is C++20, OpenSSL, yaml-cpp, libcurl and the FFmpeg/libavformat, libavcodec, libavutil, libswscale and libswresample development libraries. FUSE3/macFUSE is the mount dependency. libtorrent-rasterbar >= 2.0 enables the optional integrated BitTorrent acquisition component; generic filesystem ingest remains available without it. The FFmpeg command-line tools are not invoked by Macha. Boost is not otherwise required by Macha.

## Security

Do not put real cluster keys, private media or private host/address information in public issues. See `SECURITY.md`.
