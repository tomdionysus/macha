// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace macha {

enum class MediaStreamType { video, audio, subtitle, other };
enum class MediaTransform { copy, transcode, omit };
enum class PlaybackMode { direct, remux, transcode };

struct MediaStreamInfo {
    int index{-1};
    MediaStreamType type{MediaStreamType::other};
    std::string codec;
    std::string profile;
    std::string language;
    int width{};
    int height{};
    int channels{};
    int sample_rate{};
    int bit_depth{};
    bool default_stream{};
    bool forced{};
};

struct MediaProbeResult {
    std::string format;
    double duration_seconds{};
    uint64_t bitrate{};
    std::vector<MediaStreamInfo> streams;
};

struct MediaSource {
    std::string media_id;
    std::string logical_path;
    std::string url;
    uint64_t size{};
};

struct PlaybackPlan {
    PlaybackMode mode{PlaybackMode::direct};
    int video_stream{-1};
    int audio_stream{-1};
    int subtitle_stream{-1};
    MediaTransform video{MediaTransform::copy};
    MediaTransform audio{MediaTransform::copy};
    std::string video_codec{"h264"};
    std::string audio_codec{"aac"};
    std::optional<int> target_height;
    std::optional<uint64_t> target_video_bitrate;
    std::chrono::milliseconds seek{};
};

struct MediaEngineStatus {
    bool ffmpeg_available{};
    bool ffprobe_available{};
    std::string ffmpeg_version;
};

class MediaEngineSession {
  public:
    virtual ~MediaEngineSession() = default;
    virtual bool running() const = 0;
    virtual std::optional<int> exit_code() const = 0;
    virtual std::string diagnostics() const = 0;
    virtual void set_paused(bool paused) = 0;
    virtual void stop() = 0;
};

class MediaEngine {
  public:
    virtual ~MediaEngine() = default;
    virtual MediaEngineStatus status() const = 0;
    virtual MediaProbeResult probe(const MediaSource&) = 0;
    virtual std::unique_ptr<MediaEngineSession> start_hls(
        const MediaSource&, const PlaybackPlan&, const std::filesystem::path& output_directory,
        std::chrono::milliseconds segment_duration) = 0;
    virtual void extract_webvtt(const MediaSource&, int subtitle_stream,
                                const std::filesystem::path& output_file,
                                std::chrono::milliseconds seek = {}) = 0;
};

std::unique_ptr<MediaEngine> make_ffmpeg_process_engine(const StreamingConfig&);
std::string playback_mode_name(PlaybackMode);
std::string media_stream_type_name(MediaStreamType);

} // namespace macha
