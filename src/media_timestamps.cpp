// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_timestamps.hpp"

#include <algorithm>
#include <limits>

namespace macha {
namespace {

bool can_add(int64_t value, int64_t delta) {
    if (delta > 0) return value <= std::numeric_limits<int64_t>::max() - delta;
    if (delta < 0) return value >= std::numeric_limits<int64_t>::min() - delta;
    return true;
}

int64_t checked_add(int64_t value, int64_t delta) {
    if (!can_add(value, delta))
        return delta > 0 ? std::numeric_limits<int64_t>::max()
                         : std::numeric_limits<int64_t>::min() + 1;
    return value + delta;
}

} // namespace

void normalize_media_timestamps(MediaTimestampRepairState& state,
                                MediaPacketTimestamps& packet) {
    if (packet.pts != kNoMediaTimestamp)
        packet.pts = checked_add(packet.pts, state.timeline_shift);
    if (packet.dts != kNoMediaTimestamp)
        packet.dts = checked_add(packet.dts, state.timeline_shift);

    const int64_t duration = packet.duration > 0
                                 ? packet.duration
                                 : std::max<int64_t>(1, state.last_duration);

    if (packet.dts == kNoMediaTimestamp) {
        ++state.missing_dts;
        if (state.last_dts != kNoMediaTimestamp) {
            packet.dts = checked_add(state.last_dts,
                                     std::max<int64_t>(1, state.last_duration));
        } else if (packet.pts != kNoMediaTimestamp) {
            packet.dts = packet.pts;
        } else {
            packet.dts = 0;
        }
    }

    if (state.last_dts != kNoMediaTimestamp && packet.dts <= state.last_dts) {
        ++state.nonmonotonic_dts;
        const int64_t target = state.last_dts == std::numeric_limits<int64_t>::max()
                                   ? state.last_dts
                                   : state.last_dts + 1;
        const int64_t correction = target > packet.dts ? target - packet.dts : 0;
        packet.dts = checked_add(packet.dts, correction);
        if (packet.pts != kNoMediaTimestamp)
            packet.pts = checked_add(packet.pts, correction);
        state.timeline_shift = checked_add(state.timeline_shift, correction);
    }

    if (packet.pts == kNoMediaTimestamp) {
        ++state.missing_pts;
        packet.pts = packet.dts;
    }
    if (packet.pts < packet.dts)
        ++state.pts_before_dts;

    if (packet.duration <= 0) packet.duration = duration;
    state.last_duration = duration;
    state.last_dts = packet.dts;
}

} // namespace macha
