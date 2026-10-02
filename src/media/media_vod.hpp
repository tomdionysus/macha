// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace macha::media_vod {

// Where a generation's media begins and where the request sits in it. Exact,
// in integer ms: seek_ms + seek_offset_ms == seek_requested_ms, with
// seek_offset_ms >= 0. The server never moves the request, so nothing between
// it and the stream start goes missing.
struct IndexedPlan {
    // The baseline as a source timestamp, for segment-boundary arithmetic.
    double actual_seek_seconds{};
    int64_t seek_ms{};
    int64_t seek_offset_ms{};
    int64_t seek_requested_ms{};
    std::vector<double> segment_durations;
    double longest_segment_seconds{};
};

// The honoured request: the client's position clamped to [0, duration - 1 ms].
// Reported as seek_requested_ms so a client can tell a clamp from a violated
// invariant.
int64_t clamp_seek_ms(int64_t requested_seek_ms, double duration_seconds);

// An immutable VOD segment plan starting at the last indexed keyframe at or
// before the request, or none when the keyframes are too sparse for roughly
// target-sized segments.
std::optional<IndexedPlan> indexed_plan(std::span<const double> keyframe_seconds,
                                        double duration_seconds,
                                        int64_t requested_seek_ms,
                                        double target_segment_seconds);

// Matroska/WebM defers Cues parsing until a seek; materialise the index before
// inspecting AVStream's entries, or it can look complete but very sparse.
bool requires_seek_index_materialisation(std::string_view input_format_name);

// Index shape where a plan succeeded: entry count, longest and median gap
// (tail included). Matroska Cues need not name every keyframe, so gaps bound
// the GOP from above; offsets well below them mean sparse Cues, not long GOPs.
struct IndexDensity {
    size_t entries{};
    double longest_gap_seconds{};
    double median_gap_seconds{};
};

IndexDensity index_density(std::span<const double> keyframe_seconds, double duration_seconds);

} // namespace macha::media_vod
