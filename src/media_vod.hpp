// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace macha::media_vod {

struct IndexedPlan {
    double actual_seek_seconds{};
    std::vector<double> segment_durations;
};

// Build an immutable VOD segment plan from known random-access video points.
// Returns no plan when the supplied keyframes do not cover the requested
// presentation densely enough to support approximately target-sized segments.
std::optional<IndexedPlan> indexed_plan(std::span<const double> keyframe_seconds,
                                        double duration_seconds,
                                        double requested_seek_seconds,
                                        double target_segment_seconds);

// Matroska/WebM defers parsing Cues until the demuxer is asked to seek. The
// VOD planner must materialise that seek index before inspecting AVStream's
// index entries; otherwise probe-discovered entries can look like a complete
// but extremely sparse index.
bool requires_seek_index_materialisation(std::string_view input_format_name);

} // namespace macha::media_vod
