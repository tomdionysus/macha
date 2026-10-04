// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/time_source.hpp"
#include "http/http.hpp"
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
// Segment container of a transformed (HLS) session. fMP4 by default; MPEG-TS
// for clients whose native HLS player cannot take fMP4 with muxed audio.
enum class MediaContainer : uint8_t { fmp4, mpegts };
const char* media_container_name(MediaContainer) noexcept;

// The AAC standard channel layout (libavutil name) for a channel count. Any
// other layout makes the encoder write a PCE with channelConfiguration 0,
// which Chrome's MP4 parser rejects.
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
    // 5.1) and colour transfer ("smpte2084" = PQ, "arib-std-b67" = HLG), for
    // HLS CODECS strings and HDR negotiation. Last, so positional
    // initialisers keep working.
    int level{};
    std::string color_transfer{};
    // Dolby Vision profile (5, 7, 8, ...) and base-layer compatibility id
    // (1 = HDR10, 2 = SDR, 4 = HLG), when present; a client needs both.
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

// Why the engine could not produce facts about a source. Reported, never
// acted on: only the client knows whether asking another node is worth it.
enum class MediaFailure : uint8_t {
    // The bytes could not be read from this node; the file may be fine.
    unreadable,
    // The bytes were read and are not media this build can demux.
    unsupported,
    timed_out,
};

std::string_view media_failure_name(MediaFailure) noexcept;
FailureAxes media_failure_axes(MediaFailure) noexcept;

class MediaError : public std::runtime_error {
    MediaFailure failure_;

  public:
    MediaError(MediaFailure failure, const std::string& message)
        : std::runtime_error(message), failure_(failure) {}
    MediaFailure failure() const noexcept { return failure_; }
};

// A seekable immutable media view. libav sees only this interface and never
// reaches back into Macha through HTTP.
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
    // Where this generation's media begins: the first sample the client gets.
    std::chrono::milliseconds seek{};
    // Seek contract, exact in integer ms: seek + seek_offset == seek_requested,
    // seek_offset >= 0. seek_requested is the clamped request; the server never
    // moves it, and reports any gap to where the stream can begin as the offset.
    std::chrono::milliseconds seek_offset{};
    std::chrono::milliseconds seek_requested{};

    // Record a request before planning: baseline equals request, offset zero.
    // Direct play, having no generation, stays that way.
    void request_seek(std::chrono::milliseconds position) {
        seek = position;
        seek_offset = {};
        seek_requested = position;
    }
};

struct HlsVodPlan {
    PlaybackPlan playback;
    std::vector<double> segment_durations;

    // Retained seek-planning state, so a seek-only PATCH can create a new
    // generation without reprobing the source or rebuilding its index.
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

// Fragments published by the muxer, bounded ahead of the consumer. Older
// fragments may spill to disk; the filesystem is never polled.
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
        // Media produced and encoder time spent producing it; a client
        // divides them for a rate from one response. producing_ms excludes
        // time parked on the max_ahead_segments gate, so the rate shows
        // whether the encoder can outrun realtime rather than reading ~1.0x.
        uint64_t produced_media_ms{};
        uint64_t producing_ms{};
        // Age in ms of the last publication (the pair above), measured here
        // so it does not depend on the client's clock.
        uint64_t produced_age_ms{};
        // Producer blocked on the max_ahead_segments gate: tells "ahead and
        // waiting" from "wedged" when produced_age_ms is large. Derived from
        // being at the gate, not tracked.
        bool producer_parked{};
    };

    // `time`: what production time and the age of the last publication are
    // measured by.
    MediaSegmentStore(size_t max_ahead_segments, uint64_t memory_limit,
                      std::filesystem::path spill_directory,
                      std::chrono::milliseconds target_duration,
                      std::vector<double> vod_segment_durations = {},
                      MediaContainer container = MediaContainer::fmp4,
                      const TimeSource& time = steady_time_source());
    MediaContainer container() const noexcept;
    ~MediaSegmentStore();

    MediaSegmentStore(const MediaSegmentStore&) = delete;
    MediaSegmentStore& operator=(const MediaSegmentStore&) = delete;

    bool wait_ready(std::chrono::milliseconds timeout);
    std::string playlist() const;
    std::optional<Bytes> object(std::string_view name) const;
    // Waits until the named init or segment object is published, the
    // generation ends, or the timeout expires (zero waits indefinitely). Any
    // other name is an immediate lookup.
    std::optional<Bytes> wait_object(std::string_view name, std::chrono::milliseconds timeout) const;
    // Non-blocking wait_object. Returns the object if present; `ended` if it
    // never will be; otherwise registers `wake` under the store lock (no lost
    // publication) to fire once on the next publication or ending. Spurious
    // wakes are normal: ask again.
    struct Awaited {
        std::optional<Bytes> object;
        bool ended{};
    };
    Awaited object_or_subscribe(std::string_view name, std::function<void()> wake) const;
    void note_requested(uint64_t index);
    Snapshot snapshot() const;
    void cancel();
    // Mark or clear supersession by a replacement generation. Unlike cancel()
    // it only wakes wait_object() callers on unproduced segments; production
    // continues, and clearing it restores long-polling if the replacement fails.
    void mark_superseded(bool superseded);
    // Reserve resident capacity as viewer ownership before exposing the
    // pipeline. False means admission must fail.
    bool attach_memory_ledger(RetainedMemoryLedger&);

    // Producer side; engine-neutral so libav can be replaced without changing
    // playback policy.
    bool publish_init(Bytes bytes);
    bool publish_segment(Bytes bytes, double duration_seconds);
    void finish();
    void fail(std::string message);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Start progress as facts, never estimates: the engine worker writes,
// playback reads while waiting for the first fragment. `seq` moves with any
// counter.
struct MediaStartProgress {
    std::atomic_uint64_t seq{};
    std::atomic_uint64_t source_bytes_read{};
    // Pre-roll: frames decoded from the keyframe before the origin and
    // discarded. Total is -1 until the first pre-roll frame.
    std::atomic_int64_t preroll_total_us{-1};
    std::atomic_int64_t preroll_decoded_us{};
    // Media time past the origin that has reached the muxer.
    std::atomic_int64_t output_media_us{};
    void moved() noexcept { seq.fetch_add(1, std::memory_order_relaxed); }
};

class MediaEngineSession {
  public:
    virtual ~MediaEngineSession() = default;
    // Null when the engine does not report start progress.
    virtual const MediaStartProgress* start_progress() const { return nullptr; }
    virtual bool running() const = 0;
    virtual std::optional<int> exit_code() const = 0;
    virtual std::string diagnostics() const = 0;
    virtual std::shared_ptr<MediaSegmentStore> segments() const = 0;
    virtual void note_segment_requested(uint64_t index) = 0;
    virtual void stop() = 0;
};

// Byte positions of keyframes (video) and samples (audio) from the
// container's own index, so a client can map held byte ranges to times. MP4
// offsets are exact; Matroska offsets are the enclosing Cluster. Entries are
// (time_ms, byte offset) sorted by offset; audio at most one per second. The
// file ends at (duration_ms, size_bytes).
struct MediaKeyframeIndex {
    std::string container;
    bool exact_offsets{};
    uint64_t size_bytes{};
    int64_t duration_ms{};
    struct Stream {
        int index{-1};
        MediaStreamType type{MediaStreamType::other};
        std::string codec;
        std::vector<std::pair<int64_t, uint64_t>> entries;
    };
    std::vector<Stream> streams;
};

class MediaEngine {
  public:
    virtual ~MediaEngine() = default;
    // Empty unless the container is MP4 or Matroska. Throws MediaError when
    // the source cannot be read.
    virtual std::optional<MediaKeyframeIndex> keyframe_index(const MediaSource&,
                                                             std::chrono::milliseconds timeout = {}) {
        (void)timeout;
        return std::nullopt;
    }
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

// macha_core is FFmpeg-free; the FFmpeg engine (media_engine.cpp, linked only
// into executables with FFmpeg) registers its factory at static-init time.
// With nothing registered, make_libav_media_engine() returns nullptr.
using MediaEngineFactory = std::unique_ptr<MediaEngine> (*)(const StreamingConfig&);
void set_media_engine_factory(MediaEngineFactory);
std::unique_ptr<MediaEngine> make_libav_media_engine(const StreamingConfig&);

// Derive a new transformed VOD generation from a prepared plan, or none when
// the plan lacks the random-access data for a seek-only fast path.
// `declined_reason`, when supplied, names the failed precondition.
std::optional<HlsVodPlan> reseek_hls_vod(const HlsVodPlan&,
                                         std::chrono::milliseconds requested_seek,
                                         std::string* declined_reason = nullptr);

std::string playback_mode_name(PlaybackMode);
// RFC 6381 codec string for one stream ("avc1.640029", "mp4a.40.2", ...);
// `transcoded` describes the libx264/AAC output instead of the source.
std::string hls_codec_string(std::string_view codec, const MediaStreamInfo* stream, bool transcoded);
// Master-playlist EXT-X-STREAM-INF (BANDWIDTH, CODECS, RESOLUTION), so the
// player builds source buffers from what the segments really carry.
std::string hls_variant_stream_inf(const PlaybackPlan& plan, const MediaStreamInfo* video,
                                   const MediaStreamInfo* audio, uint64_t source_bitrate);
std::string media_stream_type_name(MediaStreamType);

} // namespace macha
