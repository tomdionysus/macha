// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace macha {

// Keeps a bulk writer's unwritten data small. On a shared ext4 filesystem a
// small fsync (a metadata commit) commits the journal, which can wait for
// every other file's dirty data; a writer that leaves gigabytes dirty makes
// control wait. A writer told of each write starts writeback every window
// and, before starting the next, waits for the last to reach the disk, so it
// holds at most about two windows unwritten. Not durability: nothing here
// is an fsync. Linux only; elsewhere it does nothing.
class WriteBehind {
  public:
    static constexpr uint64_t default_window = 8ull * 1024 * 1024;
    explicit WriteBehind(uint64_t window = default_window) : window_(window) {}

    // `bytes` were just written to `fd`.
    void wrote(int fd, uint64_t bytes);

    // Whether the bytes written so far fill a window, which then starts again:
    // the decision wrote() acts on.
    bool window_filled(uint64_t bytes);

  private:
    uint64_t window_;
    uint64_t pending_{};
    bool started_{};
};

// A whole file was just written to `fd`: starts its writeback without
// waiting for it.
void start_writeback(int fd);

} // namespace macha
