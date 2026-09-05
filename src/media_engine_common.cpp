// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_engine.hpp"

#include "media_vod.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace macha {
namespace {
std::vector<double> fixed_vod_durations(double duration_seconds, double seek_seconds,
                                        double segment_seconds) {
    const double remaining = std::max(0.0, duration_seconds - seek_seconds);
    if (!(remaining > 0.001) || !(segment_seconds > 0.001))
        throw std::runtime_error("media duration is unavailable for VOD planning");
    std::vector<double> durations;
    double left = remaining;
    while (left > segment_seconds + 0.001) {
        durations.push_back(segment_seconds);
        left -= segment_seconds;
    }
    durations.push_back(std::max(0.001, left));
    return durations;
}
} // namespace

std::optional<HlsVodPlan> reseek_hls_vod(const HlsVodPlan& prepared,
                                         std::chrono::milliseconds requested_seek) {
    if (!prepared.reusable_seek || !(prepared.source_duration_seconds > 0.001) ||
        !(prepared.seek_segment_seconds > 0.001))
        return std::nullopt;

    HlsVodPlan result = prepared;
    const double requested_seconds = std::clamp(
        requested_seek.count() / 1000.0, 0.0,
        std::max(0.0, prepared.source_duration_seconds - 0.001));

    if (prepared.playback.mode == PlaybackMode::transcode) {
        // Transcode lays down its own GOP structure via fixed_vod_durations
        // regardless of source keyframes, so -- unlike the remux/copy branch
        // below -- it only needs one nearby keyframe to avoid fully decoding
        // (not just skipping) every source frame between the landing
        // keyframe and an exact frame-accurate target: costly on slow
        // software decoders and unnecessary precision for a viewer rather
        // than a nonlinear editor. indexed_plan's whole-file segment-density
        // check doesn't apply here and can spuriously reject an otherwise
        // perfectly usable seek point if any other part of a long file has a
        // sparser GOP (see nearest_keyframe_at_or_after).
        double actual_seek = requested_seconds;
        if (!prepared.video_random_access_points.empty()) {
            if (const double snapped = media_vod::nearest_keyframe_at_or_after(
                    prepared.video_random_access_points, requested_seconds);
                snapped >= 0.0)
                actual_seek = snapped;
        }
        // Round UP, not to nearest: actual_seek may be a real keyframe
        // timestamp, and llround can round a fractional-millisecond
        // keyframe timestamp down. Reconstructing microseconds from that
        // truncated value later (run_pipeline) then lands
        // avformat_seek_file's AVSEEK_FLAG_BACKWARD search one keyframe
        // *earlier* than intended -- a full GOP's worth of avoidable decode.
        result.playback.seek = std::chrono::milliseconds(
            static_cast<int64_t>(std::ceil(actual_seek * 1000.0)));
        result.segment_durations = fixed_vod_durations(prepared.source_duration_seconds,
                                                       actual_seek,
                                                       prepared.seek_segment_seconds);
    } else if (!prepared.video_random_access_points.empty()) {
        auto indexed = media_vod::indexed_plan(prepared.video_random_access_points,
                                               prepared.source_duration_seconds,
                                               requested_seconds,
                                               prepared.seek_segment_seconds);
        if (!indexed) return std::nullopt;
        // Same rounding hazard as above: indexed->actual_seek_seconds is a
        // real keyframe timestamp.
        result.playback.seek = std::chrono::milliseconds(
            static_cast<int64_t>(std::ceil(indexed->actual_seek_seconds * 1000.0)));
        result.segment_durations = std::move(indexed->segment_durations);
    } else {
        result.playback.seek = std::chrono::milliseconds(
            static_cast<int64_t>(std::llround(requested_seconds * 1000.0)));
        result.segment_durations = fixed_vod_durations(prepared.source_duration_seconds,
                                                       requested_seconds,
                                                       prepared.seek_segment_seconds);
    }
    return result;
}

std::string playback_mode_name(PlaybackMode mode) {
    switch (mode) {
    case PlaybackMode::direct: return "direct";
    case PlaybackMode::remux: return "remux";
    case PlaybackMode::transcode: return "transcode";
    }
    return "unknown";
}

std::string media_stream_type_name(MediaStreamType type) {
    switch (type) {
    case MediaStreamType::video: return "video";
    case MediaStreamType::audio: return "audio";
    case MediaStreamType::subtitle: return "subtitle";
    case MediaStreamType::other: return "other";
    }
    return "other";
}

} // namespace macha
