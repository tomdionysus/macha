// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>

namespace macha {

inline constexpr uint32_t fuse_journal_max_record = 16U * 1024U * 1024U;

// Durable FUSE journal framing is intentionally separate from the higher-level
// record state machine. Keeping framing here gives both the runtime recovery
// path and tests one implementation of torn-tail/checksum semantics.
struct FuseJournalScanResult {
    size_t last_good{};
    size_t discarded_tail{};
    // Set when a complete frame before EOF failed its checksum: durable
    // middle-of-journal corruption rather than a torn append. The scan stops
    // there (last_good is that frame's offset) and the caller decides what
    // to do with the tail; discipline 3 quarantines it and starts anyway.
    std::optional<size_t> corrupt_frame_offset;
};

using FuseJournalRecordConsumer =
    std::function<void(std::span<const uint8_t> payload, size_t frame_offset)>;

Bytes fuse_journal_frame(std::span<const uint8_t> payload);
FuseJournalScanResult scan_fuse_journal_frames(std::span<const uint8_t> bytes,
                                               size_t first_frame_offset,
                                               const FuseJournalRecordConsumer& consume);

} // namespace macha
