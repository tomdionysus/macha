// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>

namespace macha {

inline constexpr uint32_t fuse_journal_max_record = 16U * 1024U * 1024U;

// Journal framing, separate from the record state machine so recovery and
// tests share one torn-tail/checksum implementation.
struct FuseJournalScanResult {
    size_t last_good{};
    size_t discarded_tail{};
    // Set when a complete frame before EOF fails its checksum (corruption, not
    // a torn append). The scan stops there, last_good is that frame's offset,
    // and the caller decides; discipline 3 quarantines the tail and starts.
    std::optional<size_t> corrupt_frame_offset;
};

using FuseJournalRecordConsumer =
    std::function<void(std::span<const uint8_t> payload, size_t frame_offset)>;

Bytes fuse_journal_frame(std::span<const uint8_t> payload);
FuseJournalScanResult scan_fuse_journal_frames(std::span<const uint8_t> bytes,
                                               size_t first_frame_offset,
                                               const FuseJournalRecordConsumer& consume);

} // namespace macha
