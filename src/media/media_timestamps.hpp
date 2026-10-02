// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <limits>

namespace macha {

// Equals AV_NOPTS_VALUE (INT64_MIN) without depending on libav headers.
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

// Repair one stream-copy packet after rescaling to the muxer timebase: MP4
// needs defined, strictly increasing DTS, which backward seeks and rescaling
// can break. A repair carries forward as a timeline shift so later packets
// keep their spacing. PTS before DTS is kept: version-1 CTTS allows signed
// composition offsets.
void normalize_media_timestamps(MediaTimestampRepairState& state,
                                MediaPacketTimestamps& packet);

// Encoders need strictly increasing PTS, which rescaled best-effort timestamps
// can violate around seeks in imperfect files. Keeps AV_NOPTS_VALUE; nudges
// only anomalous values forward by the minimum.
int64_t normalize_encoder_pts(int64_t& last_pts, int64_t pts);

} // namespace macha
