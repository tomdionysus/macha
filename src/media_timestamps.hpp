// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <limits>

namespace macha {

// Matches AV_NOPTS_VALUE without making timestamp repair depend on libav
// headers. media_engine.cpp converts AVPacket timestamps directly because
// FFmpeg uses INT64_MIN for the same sentinel.
inline constexpr int64_t kNoMediaTimestamp = std::numeric_limits<int64_t>::min();

struct MediaPacketTimestamps {
    int64_t pts{kNoMediaTimestamp};
    int64_t dts{kNoMediaTimestamp};
    int64_t duration{};
};

struct MediaTimestampRepairState {
    int64_t last_dts{kNoMediaTimestamp};
    int64_t last_duration{1};
    int64_t timeline_shift{};
    uint64_t missing_pts{};
    uint64_t missing_dts{};
    uint64_t nonmonotonic_dts{};
    uint64_t pts_before_dts{};

    uint64_t repair_count() const {
        return missing_pts + missing_dts + nonmonotonic_dts;
    }
};

// Repair one stream-copy packet after its timestamps have been rescaled to the
// muxer's output timebase. MP4 requires defined, strictly increasing DTS.
// Some demuxers can produce missing/equal timestamps around a backward seek,
// and timestamp rescaling itself can collapse adjacent source ticks. A repair
// is carried forward as a timeline shift so later packets keep their spacing
// instead of being repeatedly squeezed forward one tick at a time. PTS before
// DTS is observed but preserved: signed composition offsets are valid in MP4
// when the muxer uses version-1 CTTS entries.
void normalize_media_timestamps(MediaTimestampRepairState& state,
                                MediaPacketTimestamps& packet);

} // namespace macha
