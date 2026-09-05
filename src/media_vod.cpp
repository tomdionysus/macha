// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_vod.hpp"

#include <algorithm>
#include <cmath>

namespace macha::media_vod {
namespace {

constexpr double kTimestampEpsilon = 0.0005;
constexpr double kMinimumDuration = 0.001;
// A remux plan is only useful if every advertised fragment is reasonably
// close to the configured target. This is deliberately generous enough for
// ordinary GOP variation while rejecting a partial index whose final entry is
// followed by minutes of unindexed media.
constexpr double kMaximumSegmentFactor = 3.0;

bool format_token(std::string_view names, std::string_view wanted) {
    size_t begin = 0;
    while (begin <= names.size()) {
        const auto end = names.find(',', begin);
        const auto token = names.substr(begin, end == std::string_view::npos ? names.size() - begin
                                                                             : end - begin);
        if (token == wanted) return true;
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return false;
}

} // namespace

std::optional<IndexedPlan> indexed_plan(std::span<const double> keyframe_seconds,
                                        double duration_seconds,
                                        double requested_seek_seconds,
                                        double target_segment_seconds) {
    if (!(duration_seconds > kMinimumDuration) || !(target_segment_seconds > kMinimumDuration))
        return std::nullopt;

    requested_seek_seconds = std::clamp(requested_seek_seconds, 0.0,
                                        std::max(0.0, duration_seconds - kMinimumDuration));

    std::vector<double> keyframes;
    keyframes.reserve(keyframe_seconds.size());
    for (const double seconds : keyframe_seconds) {
        if (!std::isfinite(seconds) || seconds + kTimestampEpsilon < requested_seek_seconds ||
            seconds >= duration_seconds - kMinimumDuration)
            continue;
        if (!keyframes.empty() && seconds <= keyframes.back() + kTimestampEpsilon) continue;
        keyframes.push_back(std::max(0.0, seconds));
    }
    if (keyframes.empty()) return std::nullopt;

    IndexedPlan result;
    result.actual_seek_seconds = keyframes.front();

    std::vector<double> starts{result.actual_seek_seconds};
    double wanted = result.actual_seek_seconds + target_segment_seconds;
    for (size_t i = 1; i < keyframes.size(); ++i) {
        const double seconds = keyframes[i];
        if (seconds + kTimestampEpsilon < wanted) continue;
        starts.push_back(seconds);
        wanted = seconds + target_segment_seconds;
    }

    result.segment_durations.reserve(starts.size());
    for (size_t i = 0; i + 1 < starts.size(); ++i)
        result.segment_durations.push_back(std::max(kMinimumDuration, starts[i + 1] - starts[i]));
    result.segment_durations.push_back(
        std::max(kMinimumDuration, duration_seconds - starts.back()));

    const double maximum_segment = target_segment_seconds * kMaximumSegmentFactor;
    if (std::any_of(result.segment_durations.begin(), result.segment_durations.end(),
                    [&](double duration) { return duration > maximum_segment + kTimestampEpsilon; }))
        return std::nullopt;

    return result;
}

bool requires_seek_index_materialisation(std::string_view input_format_name) {
    return format_token(input_format_name, "matroska") || format_token(input_format_name, "webm");
}

double nearest_keyframe_at_or_after(std::span<const double> keyframe_seconds,
                                    double requested_seek_seconds) {
    double best = -1.0;
    for (const double seconds : keyframe_seconds) {
        if (seconds + kTimestampEpsilon < requested_seek_seconds) continue;
        if (best < 0.0 || seconds < best) best = seconds;
    }
    return best;
}

} // namespace macha::media_vod
