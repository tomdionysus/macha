// SPDX-License-Identifier: GPL-3.0-or-later
//
// A/V timeline checks against the real libav transcode pipeline, over a
// deterministic source longer than 90 seconds with non-zero starts, audio
// priming and a seek. From the fragments actually published it measures where
// each stream starts, how much media each carries, and whether they stay
// together. It cannot measure pitch.

#include "test_backend_support.hpp"

#include "media/media_engine.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace macha;
using namespace macha::test_support;

namespace {

constexpr int kWidth = 160;
constexpr int kHeight = 90;
constexpr int kFrameRate = 25;
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;

// Long, because drift is a rate: a short case cannot tell rounding from a defect.
constexpr double kSourceSeconds = 100.0;

// Audio starts after video, as is ordinary; the offset must be applied exactly once.
constexpr double kAudioStartSeconds = 0.05;

constexpr auto kSegmentDuration = std::chrono::milliseconds{4000};

// How far the two streams may disagree about how much media they carry: bounded
// correction adds or drops whole audio frames (~21 ms at 48 kHz) and the
// encoders flush on different boundaries, so it cannot be zero.
constexpr double kDurationGapToleranceSeconds = 0.25;

// How far a fragment's declared length may differ from the media it carries.
constexpr double kFragmentDurationToleranceSeconds = 0.10;

// How far the playlist's accumulated timeline (a player's seek map) may drift
// from the media across the whole generation.
constexpr double kTimelineDriftToleranceSeconds = 0.25;

// How far apart the two streams may begin: a frame, not a one-sided offset.
constexpr double kStartGapToleranceSeconds = 0.15;

void require_av(int rc, const char* what) {
    if (rc < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(rc, message, sizeof(message));
        throw std::runtime_error(std::string(what) + ": " + message);
    }
}

// A deterministic source, generated rather than committed as a fixture.

struct Synthesized {
    Bytes bytes;
    double duration_seconds{};
    int video_stream{-1};
    int audio_stream{-1};
};

struct EncoderContext {
    AVCodecContext* ctx{};
    ~EncoderContext() { avcodec_free_context(&ctx); }
};

// Drains one encoder into the muxer; a null frame flushes what it still holds
// (for AAC, the priming tail).
void drain_encoder(AVFormatContext* out, AVCodecContext* enc, AVStream* stream, AVFrame* frame) {
    require_av(avcodec_send_frame(enc, frame), "send frame to source encoder");
    AVPacket* packet = av_packet_alloc();
    if (!packet) throw std::runtime_error("allocate source packet");
    while (true) {
        const int rc = avcodec_receive_packet(enc, packet);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) {
            av_packet_free(&packet);
            require_av(rc, "receive source packet");
        }
        packet->stream_index = stream->index;
        av_packet_rescale_ts(packet, enc->time_base, stream->time_base);
        const int written = av_interleaved_write_frame(out, packet);
        av_packet_unref(packet);
        if (written < 0) {
            av_packet_free(&packet);
            require_av(written, "write source packet");
        }
    }
    av_packet_free(&packet);
}

Synthesized synthesize_source(const std::filesystem::path& path, const char* muxer = "matroska") {
    // mpeg4: in every ordinary build, and fast; the source codec is not under test.
    const auto* video_codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    const auto* audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!video_codec || !audio_codec) throw std::runtime_error("source encoders unavailable");

    AVFormatContext* out = nullptr;
    require_av(avformat_alloc_output_context2(&out, nullptr, muxer, path.c_str()),
               "allocate source container");
    const std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> out_guard(
        out, [](AVFormatContext* c) {
            if (c && c->pb) avio_closep(&c->pb);
            avformat_free_context(c);
        });

    EncoderContext video;
    video.ctx = avcodec_alloc_context3(video_codec);
    if (!video.ctx) throw std::runtime_error("allocate source video encoder");
    video.ctx->width = kWidth;
    video.ctx->height = kHeight;
    video.ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    video.ctx->time_base = AVRational{1, kFrameRate};
    video.ctx->framerate = AVRational{kFrameRate, 1};
    // A keyframe every second, so a seek can land past the start.
    video.ctx->gop_size = kFrameRate;
    video.ctx->bit_rate = 200000;
    if (out->oformat->flags & AVFMT_GLOBALHEADER)
        video.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    require_av(avcodec_open2(video.ctx, video_codec, nullptr), "open source video encoder");

    EncoderContext audio;
    audio.ctx = avcodec_alloc_context3(audio_codec);
    if (!audio.ctx) throw std::runtime_error("allocate source audio encoder");
    audio.ctx->sample_rate = kSampleRate;
    audio.ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    audio.ctx->time_base = AVRational{1, kSampleRate};
    audio.ctx->bit_rate = 128000;
    require_av(av_channel_layout_from_string(&audio.ctx->ch_layout, "stereo"),
               "set source audio layout");
    if (out->oformat->flags & AVFMT_GLOBALHEADER)
        audio.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    require_av(avcodec_open2(audio.ctx, audio_codec, nullptr), "open source audio encoder");

    AVStream* video_stream = avformat_new_stream(out, nullptr);
    AVStream* audio_stream = avformat_new_stream(out, nullptr);
    if (!video_stream || !audio_stream) throw std::runtime_error("allocate source streams");
    require_av(avcodec_parameters_from_context(video_stream->codecpar, video.ctx),
               "copy source video parameters");
    require_av(avcodec_parameters_from_context(audio_stream->codecpar, audio.ctx),
               "copy source audio parameters");
    video_stream->time_base = video.ctx->time_base;
    audio_stream->time_base = audio.ctx->time_base;

    require_av(avio_open(&out->pb, path.c_str(), AVIO_FLAG_WRITE), "open source file");
    require_av(avformat_write_header(out, nullptr), "write source header");

    AVFrame* video_frame = av_frame_alloc();
    AVFrame* audio_frame = av_frame_alloc();
    if (!video_frame || !audio_frame) throw std::runtime_error("allocate source frames");
    const std::unique_ptr<AVFrame, void (*)(AVFrame*)> video_frame_guard(
        video_frame, [](AVFrame* f) { av_frame_free(&f); });
    const std::unique_ptr<AVFrame, void (*)(AVFrame*)> audio_frame_guard(
        audio_frame, [](AVFrame* f) { av_frame_free(&f); });

    video_frame->format = video.ctx->pix_fmt;
    video_frame->width = kWidth;
    video_frame->height = kHeight;
    require_av(av_frame_get_buffer(video_frame, 0), "allocate source video buffer");

    const int audio_frame_size = audio.ctx->frame_size > 0 ? audio.ctx->frame_size : 1024;
    audio_frame->format = audio.ctx->sample_fmt;
    audio_frame->nb_samples = audio_frame_size;
    require_av(av_channel_layout_copy(&audio_frame->ch_layout, &audio.ctx->ch_layout),
               "copy source frame layout");
    require_av(av_frame_get_buffer(audio_frame, 0), "allocate source audio buffer");

    const int64_t total_video_frames = static_cast<int64_t>(kSourceSeconds * kFrameRate);
    const int64_t total_audio_samples = static_cast<int64_t>(kSourceSeconds * kSampleRate);
    const int64_t audio_start_sample = static_cast<int64_t>(kAudioStartSeconds * kSampleRate);

    // Cheap deterministic content: a moving luma ramp and a fixed tone, the
    // same bytes on every run and machine.
    int64_t audio_sample = 0;
    for (int64_t frame_index = 0; frame_index < total_video_frames; ++frame_index) {
        require_av(av_frame_make_writable(video_frame), "make source video frame writable");
        const auto luma = static_cast<uint8_t>(frame_index & 0xff);
        for (int y = 0; y < kHeight; ++y)
            std::memset(video_frame->data[0] + y * video_frame->linesize[0],
                        static_cast<int>((luma + y) & 0xff), static_cast<size_t>(kWidth));
        for (int y = 0; y < kHeight / 2; ++y) {
            std::memset(video_frame->data[1] + y * video_frame->linesize[1], 128,
                        static_cast<size_t>(kWidth / 2));
            std::memset(video_frame->data[2] + y * video_frame->linesize[2], 128,
                        static_cast<size_t>(kWidth / 2));
        }
        video_frame->pts = frame_index;
        drain_encoder(out, video.ctx, video_stream, video_frame);

        // Keep audio just ahead of video so the interleaver never buffers a whole stream.
        const int64_t audio_target =
            av_rescale(frame_index + 1, kSampleRate, kFrameRate);
        while (audio_sample < audio_target && audio_sample < total_audio_samples) {
            require_av(av_frame_make_writable(audio_frame), "make source audio frame writable");
            for (int channel = 0; channel < kChannels; ++channel) {
                auto* samples = reinterpret_cast<float*>(audio_frame->data[channel]);
                for (int i = 0; i < audio_frame_size; ++i) {
                    const double t = static_cast<double>(audio_sample + i) / kSampleRate;
                    samples[i] = static_cast<float>(0.25 * std::sin(2.0 * M_PI * 440.0 * t));
                }
            }
            audio_frame->pts = audio_start_sample + audio_sample;
            drain_encoder(out, audio.ctx, audio_stream, audio_frame);
            audio_sample += audio_frame_size;
        }
    }

    // Flush both encoders; AAC still holds priming samples.
    drain_encoder(out, video.ctx, video_stream, nullptr);
    drain_encoder(out, audio.ctx, audio_stream, nullptr);
    require_av(av_write_trailer(out), "write source trailer");
    avio_closep(&out->pb);

    Synthesized result;
    result.duration_seconds = kSourceSeconds;
    result.video_stream = video_stream->index;
    result.audio_stream = audio_stream->index;

    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("reopen synthesized source");
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size <= 0) throw std::runtime_error("synthesized source is empty");
    result.bytes.resize(static_cast<size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(result.bytes.data()), size);
    if (!in) throw std::runtime_error("read synthesized source");
    return result;
}

// The source as bytes in memory: no filesystem, no DHT.

class MemoryInput final : public MediaInput {
    const Bytes& bytes_;

  public:
    explicit MemoryInput(const Bytes& bytes) : bytes_(bytes) {}
    uint64_t size() const override { return bytes_.size(); }
    size_t read(uint64_t offset, std::span<uint8_t> destination, Clock::time_point,
                std::atomic_bool*) override {
        if (offset >= bytes_.size()) return 0;
        const auto available = static_cast<size_t>(bytes_.size() - offset);
        const auto n = std::min(destination.size(), available);
        std::copy_n(bytes_.begin() + static_cast<ptrdiff_t>(offset), n, destination.begin());
        return n;
    }
};

// Published fragments are read back through libav, not taken from the
// pipeline's own bookkeeping, which is what is under test.

struct StreamTimeline {
    bool present{};
    int64_t packets{};
    double first_seconds{};
    double last_seconds{};
    double end_seconds{}; // last presentation time plus that packet's duration
    // Media actually carried, start to end: the number drift moves.
    double span_seconds() const { return end_seconds - first_seconds; }
};

struct Timeline {
    StreamTimeline video;
    StreamTimeline audio;
    std::string describe() const {
        auto one = [](const char* label, const StreamTimeline& s) {
            if (!s.present) return std::string(label) + "=absent ";
            return std::string(label) + "={packets=" + std::to_string(s.packets) +
                   " first=" + std::to_string(s.first_seconds) +
                   " end=" + std::to_string(s.end_seconds) +
                   " span=" + std::to_string(s.span_seconds()) + "} ";
        };
        return one("video", video) + one("audio", audio);
    }
};

struct ReadCursor {
    const Bytes* bytes{};
    size_t offset{};
};

int read_packet(void* opaque, uint8_t* buffer, int size) {
    auto* cursor = static_cast<ReadCursor*>(opaque);
    if (cursor->offset >= cursor->bytes->size()) return AVERROR_EOF;
    const auto available = cursor->bytes->size() - cursor->offset;
    const auto n = std::min(static_cast<size_t>(size), available);
    std::memcpy(buffer, cursor->bytes->data() + cursor->offset, n);
    cursor->offset += n;
    return static_cast<int>(n);
}

int64_t seek_packet(void* opaque, int64_t offset, int whence) {
    auto* cursor = static_cast<ReadCursor*>(opaque);
    const auto size = static_cast<int64_t>(cursor->bytes->size());
    if (whence == AVSEEK_SIZE) return size;
    int64_t target = offset;
    if (whence == SEEK_CUR) target = static_cast<int64_t>(cursor->offset) + offset;
    else if (whence == SEEK_END) target = size + offset;
    if (target < 0 || target > size) return AVERROR(EINVAL);
    cursor->offset = static_cast<size_t>(target);
    return target;
}

Timeline measure(const Bytes& fragmented_mp4) {
    ReadCursor cursor{&fragmented_mp4, 0};
    constexpr int kBufferSize = 32768;
    auto* buffer = static_cast<uint8_t*>(av_malloc(kBufferSize));
    if (!buffer) throw std::runtime_error("allocate demuxer buffer");
    AVIOContext* io = avio_alloc_context(buffer, kBufferSize, 0, &cursor, &read_packet, nullptr,
                                         &seek_packet);
    if (!io) {
        av_free(buffer);
        throw std::runtime_error("allocate demuxer io");
    }
    AVFormatContext* in = avformat_alloc_context();
    if (!in) {
        av_freep(&io->buffer);
        avio_context_free(&io);
        throw std::runtime_error("allocate demuxer");
    }
    in->pb = io;
    const std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> guard(
        in, [](AVFormatContext* c) {
            if (c && c->pb) {
                av_freep(&c->pb->buffer);
                avio_context_free(&c->pb);
            }
            avformat_close_input(&c);
        });

    require_av(avformat_open_input(&in, nullptr, nullptr, nullptr), "open published fragments");
    require_av(avformat_find_stream_info(in, nullptr), "read published stream information");

    Timeline timeline;
    AVPacket* packet = av_packet_alloc();
    if (!packet) throw std::runtime_error("allocate measurement packet");
    const std::unique_ptr<AVPacket, void (*)(AVPacket*)> packet_guard(
        packet, [](AVPacket* p) { av_packet_free(&p); });

    while (av_read_frame(in, packet) >= 0) {
        const AVStream* stream = in->streams[packet->stream_index];
        StreamTimeline* target = nullptr;
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) target = &timeline.video;
        else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) target = &timeline.audio;
        if (!target) {
            av_packet_unref(packet);
            continue;
        }
        const auto pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        if (pts == AV_NOPTS_VALUE) {
            av_packet_unref(packet);
            continue;
        }
        const double seconds = static_cast<double>(pts) * av_q2d(stream->time_base);
        const double duration = packet->duration > 0
                                    ? static_cast<double>(packet->duration) *
                                          av_q2d(stream->time_base)
                                    : 0.0;
        if (!target->present) {
            target->present = true;
            target->first_seconds = seconds;
            target->last_seconds = seconds;
            target->end_seconds = seconds + duration;
        }
        // Packets are not monotonic with B-frames: take the maximum, not the last.
        target->last_seconds = std::max(target->last_seconds, seconds);
        target->end_seconds = std::max(target->end_seconds, seconds + duration);
        ++target->packets;
        av_packet_unref(packet);
    }
    return timeline;
}

struct TranscodeRun {
    Timeline timeline;
    size_t segments{};
    size_t planned_segments{};
    double planned_seconds{};
    double probed_seconds{};
    // Set when fewer fragments were published than planned; reported, not
    // thrown, so it cannot hide a timeline defect.
    std::string production_error;
    std::vector<double> fragment_media_seconds;
    std::vector<double> fragment_declared_seconds;
    std::string playlist;
    // The engine's start progress when the first fragment was ready.
    uint64_t start_bytes_read{};
    int64_t start_preroll_total_us{-1};
    int64_t start_preroll_decoded_us{};
    int64_t start_output_media_us{};
};

TranscodeRun transcode(MediaEngine& engine, const Synthesized& synthesized,
                       const std::filesystem::path& spill, std::chrono::milliseconds seek) {
    MediaSource source;
    source.media_id = "macha:transcode-timeline";
    source.logical_path = "/Movies/timeline.mkv";
    source.size = synthesized.bytes.size();
    source.open = [&synthesized](MediaReadPurpose) {
        return std::make_shared<MemoryInput>(synthesized.bytes);
    };

    // Plan from the engine's own probe, as production does, not from what the
    // synthesizer intended.
    const auto probe = engine.probe(source, std::chrono::milliseconds{30000});

    PlaybackPlan plan;
    plan.mode = PlaybackMode::transcode;
    plan.container = MediaContainer::fmp4;
    plan.video_stream = synthesized.video_stream;
    plan.audio_stream = synthesized.audio_stream;
    plan.subtitle_stream = -1;
    plan.video = MediaTransform::transcode;
    plan.audio = MediaTransform::transcode;
    plan.video_codec = "h264";
    plan.audio_codec = "aac";
    plan.seek = seek;

    auto vod = engine.prepare_hls_vod(source, plan, probe.duration_seconds, kSegmentDuration,
                                      false, std::chrono::milliseconds{60000});
    REQUIRE(!vod.segment_durations.empty());

    auto session = engine.start_hls(source, vod, kSegmentDuration, 8, 64ULL * 1024 * 1024, spill);
    REQUIRE(session != nullptr);
    auto store = session->segments();
    REQUIRE(store != nullptr);
    REQUIRE(store->wait_ready(std::chrono::milliseconds{60000}));
    const auto* progress = session->start_progress();
    REQUIRE(progress != nullptr);

    // Consume in order as a client does: the store bounds production ahead of
    // the consumer, so an idle consumer stalls the pipeline.
    Bytes collected;
    const auto init = store->object("init.mp4");
    REQUIRE(init.has_value());
    collected.insert(collected.end(), init->begin(), init->end());
    std::vector<Bytes> fragments;

    TranscodeRun run;
    run.start_bytes_read = progress->source_bytes_read.load();
    run.start_preroll_total_us = progress->preroll_total_us.load();
    run.start_preroll_decoded_us = progress->preroll_decoded_us.load();
    run.start_output_media_us = progress->output_media_us.load();
    run.probed_seconds = probe.duration_seconds;
    run.planned_segments = vod.segment_durations.size();
    run.planned_seconds = 0.0;
    for (double d : vod.segment_durations) run.planned_seconds += d;

    // Consume until the store stops producing rather than demanding one fragment
    // per planned entry.
    for (size_t i = 0; i < vod.segment_durations.size(); ++i) {
        session->note_segment_requested(i);
        std::ostringstream name;
        name << "segment-" << std::setfill('0') << std::setw(6) << i << ".m4s";
        const auto fragment = store->wait_object(name.str(), std::chrono::milliseconds{60000});
        if (!fragment) break;
        collected.insert(collected.end(), fragment->begin(), fragment->end());
        fragments.push_back(*fragment);
        ++run.segments;
        // The store withholds the playlist once an error is set; keep the last one served.
        if (auto text = store->playlist(); !text.empty()) run.playlist = std::move(text);
    }
    if (auto text = store->playlist(); !text.empty()) run.playlist = std::move(text);
    run.production_error = store->snapshot().error;

    for (size_t at = run.playlist.find("#EXTINF:"); at != std::string::npos;
         at = run.playlist.find("#EXTINF:", at + 1)) {
        run.fragment_declared_seconds.push_back(
            std::strtod(run.playlist.c_str() + at + 8, nullptr));
    }

    session->stop();
    run.timeline = measure(collected);

    // Per-fragment spans measured from the fragments, since the playlist is
    // under test. A span is the distance to the next fragment's start, which is
    // what a player accumulates and what EXTINF must equal.
    std::vector<double> starts;
    for (const auto& fragment : fragments) {
        Bytes one(init->begin(), init->end());
        one.insert(one.end(), fragment.begin(), fragment.end());
        starts.push_back(measure(one).video.first_seconds);
    }
    for (size_t i = 0; i < starts.size(); ++i) {
        const double next = i + 1 < starts.size() ? starts[i + 1] : run.timeline.video.end_seconds;
        run.fragment_media_seconds.push_back(next - starts[i]);
    }
    return run;
}

bool transcode_available(MediaEngine& engine) {
    const auto status = engine.status();
    return status.available && status.h264_encoder && status.aac_encoder;
}

// Each declared EXTINF must match the media its fragment carries, since a
// player builds its seek map by accumulating them.
void check_playlist_describes_the_media(const TranscodeRun& run) {
    // One fragment per planned entry, so a playlist built from the plan is right.
    CHECK(run.segments == run.planned_segments);
    // The first fragment is the short startup fragment it was planned as.
    if (!run.fragment_media_seconds.empty())
        CHECK(run.fragment_media_seconds.front() < 2.0 * kSegmentDuration.count() / 1000.0);
    REQUIRE(run.fragment_declared_seconds.size() == run.fragment_media_seconds.size());
    double declared_total = 0.0;
    double measured_total = 0.0;
    for (size_t i = 0; i < run.fragment_media_seconds.size(); ++i) {
        const double declared = run.fragment_declared_seconds[i];
        const double measured = run.fragment_media_seconds[i];
        declared_total += declared;
        measured_total += measured;
        CHECK(std::fabs(declared - measured) <= kFragmentDurationToleranceSeconds);
    }
    // Cumulatively too: per-fragment tolerance does not bound the last fragment.
    CHECK(std::fabs(declared_total - measured_total) <= kTimelineDriftToleranceSeconds);
}

void report(const char* label, const TranscodeRun& run) {
    std::cout << label << ": " << run.timeline.describe() << "probed=" << run.probed_seconds
              << "s fragments=" << run.segments << " for a " << run.planned_segments
              << "-entry plan of " << run.planned_seconds << "s\n";
    if (!run.production_error.empty()) std::cout << "  production error: " << run.production_error << "\n";
    for (size_t i = 0; i < run.fragment_media_seconds.size(); ++i)
        std::cout << "  fragment " << i << " media=" << run.fragment_media_seconds[i]
                  << "s declared=" << (i < run.fragment_declared_seconds.size()
                                           ? run.fragment_declared_seconds[i] : -1.0) << "s\n";
}

} // namespace

MACHA_HEAVY_TEST("transcode_timeline", test_transcoded_audio_and_video_carry_the_same_timeline) {
    // A hundred seconds of real transcode, measured from the published
    // fragments: audio drift or lost audio shows as a span difference.
    StreamingConfig streaming;
    auto engine = make_libav_media_engine(streaming);
    REQUIRE(engine != nullptr);
    if (!transcode_available(*engine)) {
        std::cout << "skipped: this build has no H.264/AAC encoder\n";
        return;
    }

    TempDir temp;
    const auto source_path = temp.path() / "source.mkv";
    const auto synthesized = synthesize_source(source_path);
    REQUIRE(synthesized.bytes.size() > 0);

    const auto run = transcode(*engine, synthesized, temp.path() / "spill",
                               std::chrono::milliseconds{0});
    report("transcode timeline", run);

    // A generation that produced all of its media finishes clean.
    CHECK(run.production_error.empty());
    check_playlist_describes_the_media(run);

    REQUIRE(run.timeline.video.present);
    REQUIRE(run.timeline.audio.present);

    // Both streams begin together: no one-sided priming or start offset.
    const double start_gap =
        std::fabs(run.timeline.audio.first_seconds - run.timeline.video.first_seconds);
    CHECK(start_gap <= kStartGapToleranceSeconds);

    // Both streams carry the same amount of media: the drift gate.
    const double span_gap =
        std::fabs(run.timeline.audio.span_seconds() - run.timeline.video.span_seconds());
    CHECK(span_gap <= kDurationGapToleranceSeconds);

    // And that amount is the source's, not a fraction of it.
    CHECK(run.timeline.video.span_seconds() > kSourceSeconds * 0.95);
    CHECK(run.timeline.audio.span_seconds() > kSourceSeconds * 0.95);
}

MACHA_HEAVY_TEST("transcode_timeline", test_transcoded_seek_starts_both_streams_at_the_origin) {
    // A seek generation's timeline is relative to zero for both streams; one
    // stream honouring the seek offset and not the other is silent in the playlist.
    StreamingConfig streaming;
    auto engine = make_libav_media_engine(streaming);
    REQUIRE(engine != nullptr);
    if (!transcode_available(*engine)) {
        std::cout << "skipped: this build has no H.264/AAC encoder\n";
        return;
    }

    TempDir temp;
    const auto source_path = temp.path() / "source.mkv";
    const auto synthesized = synthesize_source(source_path);

    constexpr auto kSeek = std::chrono::milliseconds{60000};
    const auto run = transcode(*engine, synthesized, temp.path() / "spill", kSeek);
    report("seek timeline", run);
    CHECK(run.production_error.empty());
    check_playlist_describes_the_media(run);

    REQUIRE(run.timeline.video.present);
    REQUIRE(run.timeline.audio.present);

    // Relative to zero, not to the seek target.
    CHECK(run.timeline.video.first_seconds < 1.0);
    CHECK(run.timeline.audio.first_seconds < 1.0);

    const double start_gap =
        std::fabs(run.timeline.audio.first_seconds - run.timeline.video.first_seconds);
    CHECK(start_gap <= kStartGapToleranceSeconds);

    const double span_gap =
        std::fabs(run.timeline.audio.span_seconds() - run.timeline.video.span_seconds());
    CHECK(span_gap <= kDurationGapToleranceSeconds);

    // The generation covers only what remains after the seek.
    const double remaining = kSourceSeconds - static_cast<double>(kSeek.count()) / 1000.0;
    CHECK(run.timeline.video.span_seconds() < remaining + 5.0);
}

MACHA_HEAVY_TEST("transcode_timeline", test_a_transcode_start_reports_its_preroll) {
    // A seek decodes from the preceding keyframe and discards those frames; the
    // engine reports that pre-roll. Keyframes are 1 s apart; the seek is 0.5 s past one.
    StreamingConfig streaming;
    auto engine = make_libav_media_engine(streaming);
    REQUIRE(engine != nullptr);
    if (!transcode_available(*engine)) {
        std::cout << "skipped: this build has no H.264/AAC encoder\n";
        return;
    }

    TempDir temp;
    const auto source_path = temp.path() / "source.mkv";
    const auto synthesized = synthesize_source(source_path);

    const auto run = transcode(*engine, synthesized, temp.path() / "spill",
                               std::chrono::milliseconds{60500});
    std::cout << "start progress: bytes=" << run.start_bytes_read
              << " preroll_total_us=" << run.start_preroll_total_us
              << " preroll_decoded_us=" << run.start_preroll_decoded_us
              << " output_media_us=" << run.start_output_media_us << "\n";
    constexpr int64_t frame_us = 1'000'000 / kFrameRate;
    CHECK(run.start_bytes_read > 0);
    CHECK(run.start_preroll_total_us >= 500'000 - frame_us);
    CHECK(run.start_preroll_total_us <= 500'000 + frame_us);
    // Pre-roll ends one frame before the origin, and all of it was decoded
    // before the first fragment could exist.
    CHECK(run.start_preroll_decoded_us >= run.start_preroll_total_us - frame_us);
    CHECK(run.start_output_media_us > 0);
}

MACHA_HEAVY_TEST("transcode_timeline", test_a_keyframe_index_places_every_keyframe_by_byte) {
    // Clients map byte ranges to times with this index: keyframes in byte
    // order, inside the file, anchored at the file's end.
    StreamingConfig streaming;
    auto engine = make_libav_media_engine(streaming);
    REQUIRE(engine != nullptr);
    TempDir temp;
    for (const auto* muxer : {"matroska", "mp4"}) {
        const bool mp4 = std::string_view(muxer) == "mp4";
        const auto path = temp.path() / (mp4 ? "source.mp4" : "source.mkv");
        const auto synthesized = synthesize_source(path, muxer);
        MediaSource source;
        source.media_id = std::string("macha:keyframes-") + muxer;
        source.logical_path = path.filename().string();
        source.size = synthesized.bytes.size();
        source.open = [&synthesized](MediaReadPurpose) {
            return std::make_shared<MemoryInput>(synthesized.bytes);
        };
        const auto index = engine->keyframe_index(source, std::chrono::milliseconds{30000});
        REQUIRE(index.has_value());
        std::cout << muxer << ": container=" << index->container << " streams=" << index->streams.size();
        for (const auto& stream : index->streams)
            std::cout << " [" << stream.index << " " << stream.codec << " " << stream.entries.size() << "]";
        std::cout << " duration_ms=" << index->duration_ms << "\n";
        CHECK(index->container == (mp4 ? "mp4" : "matroska"));
        CHECK(index->exact_offsets == mp4);
        CHECK(index->size_bytes == synthesized.bytes.size());
        CHECK(std::llabs(index->duration_ms - static_cast<int64_t>(kSourceSeconds * 1000)) < 1000);
        const auto video = std::find_if(index->streams.begin(), index->streams.end(),
                                        [](const auto& s) { return s.type == MediaStreamType::video; });
        REQUIRE(video != index->streams.end());
        // One keyframe a second, give or take the ends.
        CHECK(video->entries.size() >= static_cast<size_t>(kSourceSeconds) - 2);
        CHECK(video->entries.size() <= static_cast<size_t>(kSourceSeconds) + 2);
        for (const auto& stream : index->streams) {
            for (size_t i = 0; i < stream.entries.size(); ++i) {
                CHECK(stream.entries[i].second < synthesized.bytes.size());
                CHECK(stream.entries[i].first <= index->duration_ms);
                if (i) CHECK(stream.entries[i].second >= stream.entries[i - 1].second);
            }
        }
        if (mp4) {
            // Every AAC frame is a sync sample; the index keeps one a second.
            const auto audio = std::find_if(index->streams.begin(), index->streams.end(),
                                            [](const auto& s) { return s.type == MediaStreamType::audio; });
            REQUIRE(audio != index->streams.end());
            CHECK(audio->entries.size() >= static_cast<size_t>(kSourceSeconds) - 2);
            CHECK(audio->entries.size() <= static_cast<size_t>(kSourceSeconds) + 2);
        }
    }
}
