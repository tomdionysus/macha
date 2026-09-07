// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_engine.hpp"

#include "media_vod.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace macha {
namespace {
// Keep in step with media_engine.cpp: a short first fragment so a seek's
// generation answers after 2 s of encoding, not 4.
constexpr double kStartupFragmentSeconds = 2.0;

std::vector<double> fixed_vod_durations(double duration_seconds, double seek_seconds,
                                        double segment_seconds) {
    const double remaining = std::max(0.0, duration_seconds - seek_seconds);
    if (!(remaining > 0.001) || !(segment_seconds > 0.001))
        throw std::runtime_error("media duration is unavailable for VOD planning");
    std::vector<double> durations;
    double left = remaining;
    const double first = std::min(segment_seconds, kStartupFragmentSeconds);
    if (left > first + 0.001) {
        durations.push_back(first);
        left -= first;
    }
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

std::string hls_codec_string(std::string_view codec, const MediaStreamInfo* stream, bool transcoded) {
    if (transcoded) {
        // The libx264 transcode: High profile, level chosen by the encoder
        // for the resolution; 4.1 covers everything up to 1080p30/720p60.
        if (codec == "h264") return "avc1.640029";
        if (codec == "aac") return "mp4a.40.2";
    }
    if (codec == "h264") {
        // avc1.PPCCLL: profile_idc, constraint flags, level_idc, in hex.
        std::string profile = "64", constraints = "00";
        if (stream) {
            if (stream->profile.find("Baseline") != std::string::npos) { profile = "42"; constraints = "C0"; }
            else if (stream->profile.find("Main") != std::string::npos) { profile = "4D"; constraints = "40"; }
            else if (stream->profile.find("High 10") != std::string::npos) profile = "6E";
            else if (stream->profile.find("High 4:2:2") != std::string::npos) profile = "7A";
        }
        const int level = std::clamp(stream && stream->level > 0 ? stream->level : 41, 0, 255);
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%02X", static_cast<unsigned>(level));
        return "avc1." + profile + constraints + buffer;
    }
    if (codec == "hevc") {
        // hvc1.P.C.Lxxx.B0: general_profile_idc, compatibility flags, tier
        // + level. Main = 1 (flags 6), Main 10 = 2 (flags 4).
        const bool main10 = stream && (stream->bit_depth > 8 ||
                                       stream->profile.find("10") != std::string::npos);
        const int level = stream && stream->level > 0 ? stream->level : 153;
        return std::string("hvc1.") + (main10 ? "2.4" : "1.6") + ".L" + std::to_string(level) + ".B0";
    }
    if (codec == "av1") return "av01.0.08M.08";
    if (codec == "aac") return "mp4a.40.2";
    if (codec == "ac3") return "ac-3";
    if (codec == "eac3") return "ec-3";
    if (codec == "mp3") return "mp4a.40.34";
    if (codec == "opus") return "opus";
    if (codec == "flac") return "flac";
    return std::string(codec);
}

std::string hls_variant_stream_inf(const PlaybackPlan& plan, const MediaStreamInfo* video,
                                   const MediaStreamInfo* audio, uint64_t source_bitrate) {
    std::string codecs;
    if (plan.video != MediaTransform::omit)
        codecs = hls_codec_string(plan.video_codec, video, plan.video == MediaTransform::transcode);
    if (plan.audio != MediaTransform::omit) {
        if (!codecs.empty()) codecs += ',';
        codecs += hls_codec_string(plan.audio_codec, audio, plan.audio == MediaTransform::transcode);
    }
    int width = video ? video->width : 0;
    int height = video ? video->height : 0;
    if (plan.video == MediaTransform::transcode && plan.target_height && video && video->height > 0 &&
        *plan.target_height < video->height) {
        height = *plan.target_height;
        width = std::max(2, static_cast<int>(std::llround(static_cast<double>(video->width) *
                                                          static_cast<double>(height) /
                                                          static_cast<double>(video->height))) & ~1);
    }
    uint64_t bandwidth = source_bitrate;
    if (plan.video == MediaTransform::transcode) {
        bandwidth = plan.target_video_bitrate ? *plan.target_video_bitrate + 192000
                                              : (height >= 1080 ? 8000000 : height >= 720 ? 5000000 : 2500000);
    }
    if (!bandwidth) bandwidth = 8000000;
    std::string out = "#EXT-X-STREAM-INF:BANDWIDTH=" + std::to_string(bandwidth) +
                      ",CODECS=\"" + codecs + "\"";
    if (width > 0 && height > 0)
        out += ",RESOLUTION=" + std::to_string(width) + "x" + std::to_string(height);
    return out;
}

const char* media_container_name(MediaContainer container) noexcept {
    return container == MediaContainer::mpegts ? "mpegts" : "fmp4";
}

std::string playback_mode_name(PlaybackMode mode) {
    switch (mode) {
    case PlaybackMode::direct: return "direct";
    case PlaybackMode::remux: return "remux";
    case PlaybackMode::transcode: return "transcode";
    }
    return "unknown";
}

std::string_view media_failure_name(MediaFailure failure) noexcept {
    switch (failure) {
    case MediaFailure::unreadable: return "source_unreadable";
    case MediaFailure::unsupported: return "source_unsupported";
    case MediaFailure::timed_out: return "source_read_timed_out";
    }
    return "source_unreadable";
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

namespace {
MediaEngineFactory g_media_engine_factory = nullptr;
}

void set_media_engine_factory(MediaEngineFactory factory) {
    g_media_engine_factory = factory;
}

std::unique_ptr<MediaEngine> make_libav_media_engine(const StreamingConfig& config) {
    return g_media_engine_factory ? g_media_engine_factory(config) : nullptr;
}

} // namespace macha
