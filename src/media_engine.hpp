// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "retained_memory.hpp"
#include "types.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace macha {

enum class MediaStreamType { video, audio, subtitle, other };
enum class MediaTransform { copy, transcode, omit };
// Segment container of a transformed (HLS) session. Fragmented MP4 is the
// default; MPEG-TS is offered to clients that cannot take fMP4 (a 2017 TV's
// native HLS player rendered fMP4 video and dropped the muxed AAC, 2026-09-07).
enum class MediaContainer : uint8_t { fmp4, mpegts };
const char* media_container_name(MediaContainer) noexcept;

// The AAC standard channel configuration for a channel count, named as a
// libavutil channel layout. An encoder handed any other layout for that many
// channels writes a Program Config Element and sets channelConfiguration 0 in
// the AudioSpecificConfig, which Chrome's MP4 parser rejects outright.
const char* aac_standard_channel_layout(int channels) noexcept;
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
    uint64_t bitrate{};
    bool attached_picture{};
    // Codec level as the container reports it (H.264: 41 = 4.1; HEVC: 153 =
    // 5.1), and the colour transfer name ("smpte2084" = PQ/HDR10 and Dolby
    // Vision profile 8, "arib-std-b67" = HLG), for HLS CODECS strings and
    // the HDR negotiation gate. Last, so positional initialisers keep working.
    int level{};
    std::string color_transfer{};
    // Dolby Vision configuration record, when the stream carries one:
    // profile (5, 7, 8, ...) and the base-layer compatibility id (1 =
    // HDR10-compatible, 2 = SDR, 4 = HLG). A client needs both to know
    // whether its decoder can take the stream (2026-09-07).
    int dolby_vision_profile{};
    int dolby_vision_compatibility{};
    auto operator<=>(const MediaStreamInfo&) const = default;
};

struct MediaProbeResult {
    std::string format;
    double duration_seconds{};
    uint64_t bitrate{};
    std::vector<MediaStreamInfo> streams;
    auto operator<=>(const MediaProbeResult&) const = default;
};

// Why the engine could not produce facts about a source. The server reports
// this and does not act on it: "this node could not read the bytes" and "the
// bytes are not media we can parse" are different situations for a client,
// and only the client knows whether asking another node is worth doing.
enum class MediaFailure : uint8_t {
    // The source's bytes could not be read here. The file may be perfectly
    // good and simply unreachable from this node, as happens when a partition
    // cuts it off from the extents.
    unreadable,
    // The bytes were read and are not media this build can demux.
    unsupported,
    // Reading did not finish inside the caller's deadline.
    timed_out,
};

std::string_view media_failure_name(MediaFailure) noexcept;

class MediaError : public std::runtime_error {
    MediaFailure failure_;

  public:
    MediaError(MediaFailure failure, const std::string& message)
        : std::runtime_error(message), failure_(failure) {}
    MediaFailure failure() const noexcept { return failure_; }
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
    std::shared_ptr<std::atomic_bool> cancelled;
};

struct PlaybackPlan {
    PlaybackMode mode{PlaybackMode::direct};
    MediaContainer container{MediaContainer::fmp4};
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
    size_t video_decoder_threads{};
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
        uint64_t resident_bytes{};
        uint64_t spill_bytes{};
        uint64_t descriptor_bytes{};
        uint64_t planned_segments{};
    };

    MediaSegmentStore(size_t max_ahead_segments, uint64_t memory_limit,
                      std::filesystem::path spill_directory,
                      std::chrono::milliseconds target_duration,
                      std::vector<double> vod_segment_durations = {},
                      MediaContainer container = MediaContainer::fmp4);
    MediaContainer container() const noexcept;
    ~MediaSegmentStore();

    MediaSegmentStore(const MediaSegmentStore&) = delete;
    MediaSegmentStore& operator=(const MediaSegmentStore&) = delete;

    bool wait_ready(std::chrono::milliseconds timeout);
    std::string playlist() const;
    std::optional<Bytes> object(std::string_view name) const;
    // Holds the request until the named object is published, the generation
    // ends, or the timeout expires; a zero timeout waits indefinitely. Covers
    // both fMP4 init publication and segment indices, so a caller does not
    // have to know which kind of object it is asking for. Any other name is
    // an immediate lookup.
    std::optional<Bytes> wait_object(std::string_view name, std::chrono::milliseconds timeout) const;
    // The non-blocking form of wait_object, for a caller that will not park
    // a thread on the answer. Either the object, if it is present now; or
    // `ended`, when nothing will ever make it present (cancelled,
    // superseded, failed, finished without it, or never planned); or
    // neither, in which case `wake` has been registered under the store's
    // own lock -- so a publication cannot slip between the check and the
    // subscription -- and fires once on the next publication or ending.
    // A spurious wake (another object was published) is ordinary: ask again.
    struct Awaited {
        std::optional<Bytes> object;
        bool ended{};
    };
    Awaited object_or_subscribe(std::string_view name, std::function<void()> wake) const;
    void note_requested(uint64_t index);
    Snapshot snapshot() const;
    void cancel();
    // Mark (or clear) this store as superseded by a replacement generation.
    // Unlike cancel(), this only wakes wait_object() callers blocked on a
    // not-yet-produced segment -- it does not stop production or set
    // finished/error, and is reversible: if the replacement attempt that
    // called this fails before taking over, clearing it restores normal
    // long-poll behaviour for a store that remains the active generation.
    void mark_superseded(bool superseded);
    // Reserve this store's bounded resident capacity as viewer ownership before
    // exposing the pipeline. False means admission must fail cleanly.
    bool attach_memory_ledger(RetainedMemoryLedger&);

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
    virtual std::string extract_webvtt_segment(
        const MediaSource&, int subtitle_stream, std::chrono::milliseconds range_start,
        std::chrono::milliseconds range_end, std::chrono::milliseconds timeline_origin = {}) = 0;
};

// The real (FFmpeg-backed) implementation lives in media_engine.cpp, which is
// only linked into executables that carry an FFmpeg dependency (macha,
// macha-tests-runtime); macha_core itself stays FFmpeg-free so the fast unit
// test suite does not need FFmpeg development files installed. Previously
// this was a link seam left for the final executable to resolve, which relied
// on macha_core being a static library; now that macha_core is shared (see
// CMakeLists.txt), it must resolve its own symbols, so the real
// implementation instead registers itself into macha_core at static-init
// time via set_media_engine_factory(). With nothing registered,
// make_libav_media_engine() returns nullptr -- the same behaviour the old
// media_engine_stub.cpp default provided.
using MediaEngineFactory = std::unique_ptr<MediaEngine> (*)(const StreamingConfig&);
void set_media_engine_factory(MediaEngineFactory);
std::unique_ptr<MediaEngine> make_libav_media_engine(const StreamingConfig&);

// Derive a new transformed VOD generation from an already prepared plan.
// Returns no plan when the original preparation did not retain sufficient
// random-access information for a seek-only fast path.
std::optional<HlsVodPlan> reseek_hls_vod(const HlsVodPlan&,
                                         std::chrono::milliseconds requested_seek);

std::string playback_mode_name(PlaybackMode);
// RFC 6381 codec string for one stream of a plan ("avc1.640029",
// "hvc1.2.4.L153.B0", "mp4a.40.2", "ec-3"); `transcoded` describes the
// libx264/AAC output instead of the source stream.
std::string hls_codec_string(std::string_view codec, const MediaStreamInfo* stream, bool transcoded);
// The EXT-X-STREAM-INF line of the master playlist for a plan: BANDWIDTH,
// CODECS and RESOLUTION, so the player creates its source buffers from what
// the segments really carry instead of inferring it from the init segment.
std::string hls_variant_stream_inf(const PlaybackPlan& plan, const MediaStreamInfo* video,
                                   const MediaStreamInfo* audio, uint64_t source_bitrate);
std::string media_stream_type_name(MediaStreamType);

} // namespace macha
