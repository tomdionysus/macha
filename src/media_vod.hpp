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

// Finds the nearest keyframe at or after requested_seek_seconds, matching
// indexed_plan's "never show content earlier than requested" convention,
// but WITHOUT indexed_plan's whole-file segment-density requirement.
// Transcode re-encodes and lays down its own GOP structure regardless of
// source keyframes (unlike remux, which must cut every output segment on
// one), so it only needs this one nearby keyframe to avoid fully decoding
// (not just skipping) every source frame between the landing keyframe and
// an exact frame-accurate target -- costly on slow software decoders and
// unnecessary precision for a viewer rather than a nonlinear editor.
// Returns a negative value if no keyframe at or after the target exists
// (e.g. seeking past the last one).
double nearest_keyframe_at_or_after(std::span<const double> keyframe_seconds,
                                    double requested_seek_seconds);

} // namespace macha::media_vod
