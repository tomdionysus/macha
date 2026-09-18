// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_vod.hpp"

#include <algorithm>
#include <cmath>

namespace macha::media_vod {
namespace {

constexpr double kTimestampEpsilon = 0.0005;
constexpr double kMinimumDuration = 0.001;
// A microsecond, in seconds. Keyframe timestamps arrive as doubles rescaled
// from the container's time base, so an exact millisecond boundary can land a
// hair either side of itself; this is the slack ceil() is given so that
// 62.000s rounds to 62000 ms rather than 62001 ms.
constexpr double kMillisecondSlack = 0.000001;
// A remux fragment starts at a source keyframe, so its length is whatever
// the encoder's GOP structure gives. Until 0.32.11 any fragment longer than
// 3x the target (12 s) rejected remux for the whole file. Scene-cut x264/x265
// encodes have such gaps routinely, so on the live cluster every HEVC title
// went to a full software transcode (5-13 s setup per request, ~real-time
// segments) although the client could play the stream as-is (2026-09-07).
// HLS carries variable fragment lengths; the bound now only rejects what the
// original check was written for: a partial index whose last entry is
// followed by minutes of unindexed media (`kMaximumTailSeconds`), and
// fragments so long that a seek would wait unreasonably
// (`kMaximumFragmentSeconds`).
constexpr double kMaximumFragmentSeconds = 90.0;
constexpr double kMaximumTailSeconds = 90.0;

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

// A keyframe timestamp, in whole milliseconds, rounded UP rather than to
// nearest. llround can round a fractional-millisecond keyframe timestamp down;
// reconstructing microseconds from that truncated value later (run_pipeline)
// then lands avformat_seek_file's AVSEEK_FLAG_BACKWARD search one keyframe
// *earlier* than intended -- a full GOP's worth of avoidable decode, and now
// also a generation that would begin before the baseline it published.
int64_t keyframe_ms(double seconds) {
    return static_cast<int64_t>(std::ceil(seconds * 1000.0 - kMillisecondSlack));
}

} // namespace

int64_t clamp_seek_ms(int64_t requested_seek_ms, double duration_seconds) {
    if (!(duration_seconds > 0.0)) return 0;
    const auto duration_ms = static_cast<int64_t>(std::floor(duration_seconds * 1000.0));
    return std::clamp<int64_t>(requested_seek_ms, 0, std::max<int64_t>(0, duration_ms - 1));
}

std::optional<IndexedPlan> indexed_plan(std::span<const double> keyframe_seconds,
                                        double duration_seconds,
                                        int64_t requested_seek_ms,
                                        double target_segment_seconds) {
    if (!(duration_seconds > kMinimumDuration) || !(target_segment_seconds > kMinimumDuration))
        return std::nullopt;

    IndexedPlan result;
    result.seek_requested_ms = clamp_seek_ms(requested_seek_ms, duration_seconds);

    // The baseline is the LAST indexed keyframe at or BEFORE the request, not
    // the first one after it. A stream copy has no decoder and an fMP4
    // fragment's first sample must be a sync sample, so this is the only split
    // the container permits that still contains the position asked for.
    // Starting after the request instead put the content between the two in no
    // generation at all, recoverable by no client: a skipped scene for a viewer
    // seek, and deleted content mid-playback on the reaped-session recovery
    // path, which rebuilds a generation at a position a viewer has reached.
    //
    // A candidate is compared after it has been rounded up to milliseconds, so
    // a keyframe that rounds past the request is not a candidate. That is what
    // keeps seek_offset_ms from going negative by a millisecond where a
    // keyframe sits a fraction of a millisecond after the requested position.
    double baseline_seconds = 0.0;
    bool have_baseline = false;
    for (const double seconds : keyframe_seconds) {
        if (!std::isfinite(seconds) || seconds < 0.0 ||
            seconds >= duration_seconds - kMinimumDuration)
            continue;
        const auto candidate_ms = keyframe_ms(seconds);
        if (candidate_ms > result.seek_requested_ms) continue;
        if (have_baseline && candidate_ms < result.seek_ms) continue;
        result.seek_ms = candidate_ms;
        baseline_seconds = seconds;
        have_baseline = true;
    }
    // No indexed keyframe at or before the request leaves the baseline at zero
    // and the whole request in the offset. A decodable stream's first sample is
    // necessarily a sync sample, so a copy can always begin at the beginning;
    // the index simply did not name it. The mode is preserved, the invariant is
    // preserved, and nothing is lost. In practice unreachable -- a file's first
    // frame is virtually always indexed -- so this exists to keep the invariant
    // free of an escape hatch rather than to serve a case seen in the field.
    result.actual_seek_seconds = baseline_seconds;
    result.seek_offset_ms = result.seek_requested_ms - result.seek_ms;

    std::vector<double> keyframes;
    keyframes.reserve(keyframe_seconds.size() + 1);
    keyframes.push_back(baseline_seconds);
    for (const double seconds : keyframe_seconds) {
        if (!std::isfinite(seconds) || seconds <= keyframes.back() + kTimestampEpsilon ||
            seconds >= duration_seconds - kMinimumDuration)
            continue;
        keyframes.push_back(seconds);
    }

    std::vector<double> starts{baseline_seconds};
    double wanted = baseline_seconds + target_segment_seconds;
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

    result.longest_segment_seconds =
        *std::max_element(result.segment_durations.begin(), result.segment_durations.end());
    const double maximum_segment =
        std::max(kMaximumFragmentSeconds, target_segment_seconds * 3.0);
    const double maximum_tail = std::max(kMaximumTailSeconds, target_segment_seconds * 3.0);
    if (result.segment_durations.back() > maximum_tail + kTimestampEpsilon)
        return std::nullopt;
    if (result.longest_segment_seconds > maximum_segment + kTimestampEpsilon)
        return std::nullopt;

    return result;
}

bool requires_seek_index_materialisation(std::string_view input_format_name) {
    return format_token(input_format_name, "matroska") || format_token(input_format_name, "webm");
}

IndexDensity index_density(std::span<const double> keyframe_seconds, double duration_seconds) {
    IndexDensity density;
    density.entries = keyframe_seconds.size();
    std::vector<double> gaps;
    gaps.reserve(keyframe_seconds.size() + 1);
    double previous = 0.0;
    for (const double seconds : keyframe_seconds) {
        if (!std::isfinite(seconds)) continue;
        gaps.push_back(std::max(0.0, seconds - previous));
        previous = seconds;
    }
    gaps.push_back(std::max(0.0, duration_seconds - previous));
    density.longest_gap_seconds = *std::max_element(gaps.begin(), gaps.end());
    std::sort(gaps.begin(), gaps.end());
    density.median_gap_seconds = gaps.size() % 2 == 1
                                     ? gaps[gaps.size() / 2]
                                     : (gaps[gaps.size() / 2 - 1] + gaps[gaps.size() / 2]) / 2.0;
    return density;
}

} // namespace macha::media_vod
