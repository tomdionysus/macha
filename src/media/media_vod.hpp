// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace macha::media_vod {

// Where a generation's media begins, and where in it the position the client
// asked for sits. The server does not move a requested position; where a mode
// cannot begin a stream exactly there, it says so with an offset instead of
// relocating the request and reporting the relocation as the answer.
//
// The invariant, exactly, in integer milliseconds:
//
//     seek_ms + seek_offset_ms == seek_requested_ms
//
// seek_offset_ms is never negative, so a generation always contains the
// position asked for and nothing between the request and the stream start can
// go missing.
struct IndexedPlan {
    // The baseline as a source timestamp, for segment-boundary arithmetic.
    double actual_seek_seconds{};
    int64_t seek_ms{};
    int64_t seek_offset_ms{};
    int64_t seek_requested_ms{};
    std::vector<double> segment_durations;
    double longest_segment_seconds{};
};

// The request the server honours: the client's position clamped to
// [0, duration - 1 ms]. Reported as seek_requested_ms so a client can tell an
// ordinary clamp near the end of a title from a violated invariant; without it
// the two are indistinguishable and they want opposite handling.
int64_t clamp_seek_ms(int64_t requested_seek_ms, double duration_seconds);

// Build an immutable VOD segment plan from known random-access video points,
// beginning at the last indexed keyframe at or before the request. Returns no
// plan when the supplied keyframes do not cover the requested presentation
// densely enough to support approximately target-sized segments.
std::optional<IndexedPlan> indexed_plan(std::span<const double> keyframe_seconds,
                                        double duration_seconds,
                                        int64_t requested_seek_ms,
                                        double target_segment_seconds);

// Matroska/WebM defers parsing Cues until the demuxer is asked to seek. The
// VOD planner must materialise that seek index before inspecting AVStream's
// index entries; otherwise probe-discovered entries can look like a complete
// but extremely sparse index.
bool requires_seek_index_materialisation(std::string_view input_format_name);

// What an index looks like where a plan succeeded: how many entries, the
// longest gap between consecutive entries (the tail counts as a gap) and the
// median gap. video_keyframe_seconds reads the demuxer's index, which for
// Matroska is the Cues, and Cues are not obliged to name every keyframe -- so
// these gaps are an upper bound on the true GOP, not the GOP. Logging them on
// success as well as on rejection is what tells a sparse index from a long
// GOP: offsets clustering well below the gaps mean the Cues are sparse.
struct IndexDensity {
    size_t entries{};
    double longest_gap_seconds{};
    double median_gap_seconds{};
};

IndexDensity index_density(std::span<const double> keyframe_seconds, double duration_seconds);

} // namespace macha::media_vod
