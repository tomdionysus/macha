// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "types.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace macha {

enum class MediaStreamType { video, audio, subtitle, other };
enum class MediaTransform { copy, transcode, omit };
enum class PlaybackMode { direct, remux, transcode };
enum class MediaReadPurpose { probe, playback, subtitle };

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

// A seekable immutable media view. Implementations may be backed by the DHT,
// a local file, tests, or any later storage engine. libav only sees this
// interface; it never reaches back into Macha through HTTP.
class MediaInput {
  public:
    virtual ~MediaInput() = default;
    virtual uint64_t size() const = 0;
    virtual size_t read(uint64_t offset, std::span<uint8_t> destination,
                        Clock::time_point deadline = {},
                        std::atomic_bool* cancelled = nullptr) = 0;
};

struct MediaSource {
    std::string media_id;
    std::string logical_path;
    uint64_t size{};
    std::function<std::shared_ptr<MediaInput>(MediaReadPurpose)> open;
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

struct HlsVodPlan {
    PlaybackPlan playback;
    std::vector<double> segment_durations;

    // Reusable seek-planning state. A transformed session retains this after
    // startup so a seek-only PATCH can create a new generation without
    // reopening/probing the source or rebuilding its Matroska Cues/index.
    double source_duration_seconds{};
    double seek_segment_seconds{};
    std::vector<double> video_random_access_points;
    bool reusable_seek{};
};

struct MediaEngineStatus {
    bool available{};
    std::string backend;
    std::string version;
    bool h264_encoder{};
    bool aac_encoder{};
};

// Published fragments are produced directly by the libav muxer. The store is
// bounded ahead of the consumer; older fragments may spill to temp_path but
// are never discovered by polling the filesystem.
class MediaSegmentStore {
  public:
    struct Snapshot {
        bool init_ready{};
        bool finished{};
        std::string error;
        uint64_t segment_count{};
        uint64_t highest_requested{};
    };

    MediaSegmentStore(size_t max_ahead_segments, uint64_t memory_limit,
                      std::filesystem::path spill_directory,
                      std::chrono::milliseconds target_duration,
                      std::vector<double> vod_segment_durations = {});
    ~MediaSegmentStore();

    MediaSegmentStore(const MediaSegmentStore&) = delete;
    MediaSegmentStore& operator=(const MediaSegmentStore&) = delete;

    bool wait_ready(std::chrono::milliseconds timeout);
    std::string playlist() const;
    std::optional<Bytes> object(std::string_view name) const;
    std::optional<Bytes> wait_object(std::string_view name, std::chrono::milliseconds timeout) const;
    void note_requested(uint64_t index);
    Snapshot snapshot() const;
    void cancel();

    // Producer-side publication API. MediaEngine implementations publish an
    // initialization fragment and media fragments here; consumers only use
    // wait_ready/playlist/object/note_requested. Keeping this engine-neutral
    // is what allows libav to be replaced without changing playback policy.
    bool publish_init(Bytes bytes);
    bool publish_segment(Bytes bytes, double duration_seconds);
    void finish();
    void fail(std::string message);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class MediaEngineSession {
  public:
    virtual ~MediaEngineSession() = default;
    virtual bool running() const = 0;
    virtual std::optional<int> exit_code() const = 0;
    virtual std::string diagnostics() const = 0;
    virtual std::shared_ptr<MediaSegmentStore> segments() const = 0;
    virtual void note_segment_requested(uint64_t index) = 0;
    virtual void stop() = 0;
};

class MediaEngine {
  public:
    virtual ~MediaEngine() = default;
    virtual MediaEngineStatus status() const = 0;
    virtual MediaProbeResult probe(const MediaSource&,
                                   std::chrono::milliseconds timeout = {}) = 0;
    virtual HlsVodPlan prepare_hls_vod(
        const MediaSource&, const PlaybackPlan&, double source_duration_seconds,
        std::chrono::milliseconds segment_duration, bool allow_video_transcode_fallback,
        std::chrono::milliseconds timeout = {}) = 0;
    virtual std::unique_ptr<MediaEngineSession> start_hls(
        const MediaSource&, const HlsVodPlan&, std::chrono::milliseconds segment_duration,
        size_t max_ahead_segments, uint64_t segment_memory_bytes,
        const std::filesystem::path& spill_directory) = 0;
    virtual std::string extract_webvtt(const MediaSource&, int subtitle_stream,
                                       std::chrono::milliseconds seek = {}) = 0;
};

std::unique_ptr<MediaEngine> make_libav_media_engine(const StreamingConfig&);

// Derive a new transformed VOD generation from an already prepared plan.
// Returns no plan when the original preparation did not retain sufficient
// random-access information for a seek-only fast path.
std::optional<HlsVodPlan> reseek_hls_vod(const HlsVodPlan&,
                                         std::chrono::milliseconds requested_seek);

std::string playback_mode_name(PlaybackMode);
std::string media_stream_type_name(MediaStreamType);

} // namespace macha
