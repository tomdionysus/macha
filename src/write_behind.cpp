// SPDX-License-Identifier: GPL-3.0-or-later
#include "write_behind.hpp"

#include <fcntl.h>

namespace macha {

bool WriteBehind::window_filled(uint64_t bytes) {
    pending_ += bytes;
    if (pending_ < window_)
        return false;
    pending_ = 0;
    return true;
}

void WriteBehind::wrote(int fd, uint64_t bytes) {
    if (!window_filled(bytes))
        return;
#ifdef __linux__
    // Wait for the writeback started last window, then start this one.
    const unsigned flags = started_ ? SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE
                                    : SYNC_FILE_RANGE_WRITE;
    (void)::sync_file_range(fd, 0, 0, flags);
#else
    (void)fd;
#endif
    started_ = true;
}

void start_writeback(int fd) {
#ifdef __linux__
    (void)::sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE);
#else
    (void)fd;
#endif
}

} // namespace macha
