// SPDX-License-Identifier: GPL-3.0-or-later
//
// A/V timeline regressions against the real (non-stub) transcode pipeline.
//
// Every other playback test injects a media engine, which is the right choice
// for planning, negotiation and HTTP behaviour but says nothing about what
// libav actually produces. Both audio defects of 0.23.8 and 0.23.9 lived
// entirely inside that gap: the first (resample-ratio compensation shifting
// pitch) was caught by a person listening, the second (a unit-fraction error
// that lost nearly all audio) by a person listening again, during development.
// Neither could have failed a test, because no test ever ran the real encoder.
// This file closes that gap -- it is Phase 0 of
// TODO/2026-09-03-playback-resilience-and-av-sync-plan.md, whose stated
// prerequisite is a deterministic case longer than 90 seconds carrying
// non-zero starts, audio priming and a seek.
//
// What it measures, from the fragments the pipeline really published: where
// each output stream starts, how much media each one produced, and whether
// those two answers stay together over the length of the case. What it cannot
// measure: pitch. A resample-ratio change of the kind 0.23.8 shipped alters
// how the audio *sounds* while keeping the timeline honest, so it would pass
// here. That remains a listening test, and this file does not pretend
// otherwise.

#include "test_backend_support.hpp"

#include "media_engine.hpp"

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

// Longer than ninety seconds, because drift is a rate. A ten-second case
// cannot tell a millisecond of rounding from a defect that would be seconds
// out by the end of an episode; the 0.23.8 investigation only became legible
// when measured over minutes.
constexpr double kSourceSeconds = 100.0;

// The source's audio starts after its video, which is ordinary in a real
// container and is exactly the offset that "applied exactly once, by one
// documented owner" is about. Applied twice it doubles; applied never it
// disappears; either way this is where it shows.
constexpr double kAudioStartSeconds = 0.05;

constexpr auto kSegmentDuration = std::chrono::milliseconds{4000};

// End-to-end tolerance for how far the two output streams may disagree about
// how much media they carry. Bounded correction legitimately adds or drops
// whole audio frames (1024 samples, ~21ms at 48kHz), and the encoders flush
// on different frame boundaries, so this cannot be zero. It is far below what
// any of the real defects produced: 0.23.9 lost nearly all audio, and an
// uncompensated free-running audio clock diverged by seconds over this length.
constexpr double kDurationGapToleranceSeconds = 0.25;

// How far a fragment's declared length may differ from the media it carries.
// One video frame either side of the boundary the encoder actually chose.
constexpr double kFragmentDurationToleranceSeconds = 0.10;

// How far the playlist's accumulated timeline may drift from the media across
// the whole generation. This is the number a player's seek map is built from.
constexpr double kTimelineDriftToleranceSeconds = 0.25;

// How far apart the two streams may begin. A whole-frame difference at the
// start is expected; a priming or seek offset applied to one stream and not
// the other is not.
constexpr double kStartGapToleranceSeconds = 0.15;

void require_av(int rc, const char* what) {
    if (rc < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(rc, message, sizeof(message));
        throw std::runtime_error(std::string(what) + ": " + message);
    }
}

// ---------------------------------------------------------------------------
// A deterministic source, built here rather than committed as a fixture: the
// case has to be long, and a hundred seconds of media is not something to put
// in a git repository when the same bytes can be generated in a second.
// ---------------------------------------------------------------------------

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

// Drain one encoder into the muxer. `flush` sends the null frame that makes
// the encoder emit whatever it is still holding, which for AAC is the priming
// tail -- omitting it is how an audio track ends early.
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

Synthesized synthesize_source(const std::filesystem::path& path) {
    // mpeg4 for the source video: it is present in every ordinary build and
    // encodes a hundred seconds of this frame size in about a second. The
    // source codec is not what is under test -- the transcode of it is.
    const auto* video_codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    const auto* audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!video_codec || !audio_codec) throw std::runtime_error("source encoders unavailable");

    AVFormatContext* out = nullptr;
    require_av(avformat_alloc_output_context2(&out, nullptr, "matroska", path.c_str()),
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
    // A keyframe every second, so a seek has somewhere to land that is not the
    // start of the file.
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

    // Content is deterministic but deliberately cheap: a moving luma ramp and
    // a fixed tone. Nothing here inspects pixels or samples -- what matters is
    // that the same bytes are produced on every run and on every machine, so a
    // drift measurement is comparable between them.
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

        // Keep audio just ahead of video so the interleaver never has to buffer
        // a whole stream, which is what an unbounded av_interleaved_write_frame
        // queue would otherwise do over a hundred seconds.
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

    // Flush both encoders. The AAC encoder is holding priming samples at this
    // point; a source that never drains them is not the source we meant to
    // build.
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

// ---------------------------------------------------------------------------
// The source as the engine sees it: bytes in memory, no filesystem, no DHT.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Measuring what was actually published. The fragments are read back through
// libav rather than trusted from the pipeline's own bookkeeping, because the
// bookkeeping is the thing under test: a timeline defect that also reports
// itself correctly is not one this harness would be able to see.
// ---------------------------------------------------------------------------

struct StreamTimeline {
    bool present{};
    int64_t packets{};
    double first_seconds{};
    double last_seconds{};
    double end_seconds{}; // last presentation time plus that packet's duration
    // Media actually carried, start to end. This is the number a drift defect
    // moves: an audio clock running fast or slow relative to video produces a
    // different span from the same source over the same wall time.
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
        // Fragments are read in order, but a stream's packets are not
        // necessarily monotonic in a B-frame container, so take the maximum
        // rather than the last one seen.
        target->last_seconds = std::max(target->last_seconds, seconds);
        target->end_seconds = std::max(target->end_seconds, seconds + duration);
        ++target->packets;
        av_packet_unref(packet);
    }
    return timeline;
}

// ---------------------------------------------------------------------------
// Driving the real pipeline and collecting everything it published.
// ---------------------------------------------------------------------------

struct TranscodeRun {
    Timeline timeline;
    size_t segments{};
    size_t planned_segments{};
    double planned_seconds{};
    double probed_seconds{};
    // Set when the pipeline finished having published fewer fragments than the
    // plan promised. This is reported rather than thrown, because a plan that
    // over-promises and a timeline that drifts are separate findings and the
    // first must not hide the second.
    std::string production_error;
    std::vector<double> fragment_media_seconds;
    std::vector<double> fragment_declared_seconds;
    std::string playlist;
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

    // Plan from what the engine itself reports about the source, not from what
    // the synthesizer intended: production plans from `session.probe`, and a
    // duration the planner disagrees with would make this harness measure a
    // disagreement it invented rather than one the pipeline has.
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

    // Consume in order, exactly as a client does. The store bounds production
    // ahead of the consumer, so a harness that never asks for a fragment gets
    // a pipeline that correctly stops producing -- and then times out.
    Bytes collected;
    const auto init = store->object("init.mp4");
    REQUIRE(init.has_value());
    collected.insert(collected.end(), init->begin(), init->end());
    std::vector<Bytes> fragments;

    TranscodeRun run;
    run.probed_seconds = probe.duration_seconds;
    run.planned_segments = vod.segment_durations.size();
    run.planned_seconds = 0.0;
    for (double d : vod.segment_durations) run.planned_seconds += d;

    // A plan entry does not have to become a fragment of its own -- a boundary
    // whose flush produced no moof is carried into the next one -- so consume
    // until the store stops producing rather than demanding one fragment per
    // planned entry.
    for (size_t i = 0; i < vod.segment_durations.size(); ++i) {
        session->note_segment_requested(i);
        std::ostringstream name;
        name << "segment-" << std::setfill('0') << std::setw(6) << i << ".m4s";
        const auto fragment = store->wait_object(name.str(), std::chrono::milliseconds{60000});
        if (!fragment) break;
        collected.insert(collected.end(), fragment->begin(), fragment->end());
        fragments.push_back(*fragment);
        ++run.segments;
        // The store withholds a playlist once an error is set, so keep the
        // last one it was willing to serve; a generation that ends badly is
        // then still describable.
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

    // Per-fragment media spans, measured from the fragments rather than taken
    // from the playlist: the playlist is the claim under test. Phase 0 asks
    // for fragment media durations precisely so a boundary that produced no
    // fragment cannot hide behind an aggregate that still adds up.
    //
    // A fragment's span is the distance to where the next one starts, which is
    // what a player accumulates and what EXTINF has to equal. Taking it from
    // the fragment's own last packet instead would add that packet's duration
    // to every fragment and drift by a frame per boundary -- an artefact of
    // the measurement, not of the pipeline.
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

// The playlist is a promise about where each fragment sits on the viewer's
// timeline, and a player builds its seek map by accumulating EXTINF. Checking
// each declared duration against the media that fragment really carries is
// therefore the difference between "the file plays" and "the file plays and
// seeking lands where the viewer asked".
void check_playlist_describes_the_media(const TranscodeRun& run) {
    // One fragment per planned entry. This is what lets a complete playlist be
    // written before anything is published: if the plan and the output can
    // disagree on how many fragments there are, a playlist built from the plan
    // is wrong from its first line. Transcode used to produce one fewer than
    // planned, because the delayed moov was flushed at the first real boundary
    // and consumed it, merging fragments 0 and 1.
    CHECK(run.segments == run.planned_segments);
    // And the first fragment is the short startup fragment it was planned as,
    // not a merged double-length one -- the merge cost time to first frame as
    // well as correctness.
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
        // Per fragment: a video frame either side, no more. Before the fix
        // this file found, fragment 0 carried six seconds and declared two.
        CHECK(std::fabs(declared - measured) <= kFragmentDurationToleranceSeconds);
    }
    // And cumulatively, because a per-fragment tolerance repeated twenty-five
    // times is not a bound on where the last fragment lands.
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
    // The regression the plan asks for: a hundred seconds of real transcode,
    // measured from the published fragments. An audio clock that free-runs
    // against a re-anchored video clock diverges by seconds over this length;
    // 0.23.9's unit error produced almost no audio at all. Both are span
    // failures, and both are invisible in a ten-second case.
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

    // A generation that produced all of its media finishes clean. It did not
    // before: the store compared fragment count against plan entries, called a
    // correct run an error, and then withheld the playlist entirely.
    CHECK(run.production_error.empty());
    check_playlist_describes_the_media(run);

    REQUIRE(run.timeline.video.present);
    REQUIRE(run.timeline.audio.present);

    // Both streams begin together. A priming or start-time offset applied to
    // one and not the other lands here.
    const double start_gap =
        std::fabs(run.timeline.audio.first_seconds - run.timeline.video.first_seconds);
    CHECK(start_gap <= kStartGapToleranceSeconds);

    // Both streams carry the same amount of media. This is the drift gate.
    const double span_gap =
        std::fabs(run.timeline.audio.span_seconds() - run.timeline.video.span_seconds());
    CHECK(span_gap <= kDurationGapToleranceSeconds);

    // And that amount is the source's, not some fraction of it. 0.23.9 would
    // have failed here even without the comparison above, because the audio
    // span collapsed rather than drifting.
    CHECK(run.timeline.video.span_seconds() > kSourceSeconds * 0.95);
    CHECK(run.timeline.audio.span_seconds() > kSourceSeconds * 0.95);
}

MACHA_HEAVY_TEST("transcode_timeline", test_transcoded_seek_starts_both_streams_at_the_origin) {
    // A seek generation publishes a timeline relative to zero -- run_pipeline
    // states that contract directly ("Keep the public playback generation
    // relative to zero, including after a seek"). The failure this guards
    // against is one stream honouring the seek offset and the other not,
    // which is silent in the playlist and audible immediately.
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

    // The generation covers what remains after the seek, so a seek that was
    // silently ignored (a full-length generation) fails here.
    const double remaining = kSourceSeconds - static_cast<double>(kSeek.count()) / 1000.0;
    CHECK(run.timeline.video.span_seconds() < remaining + 5.0);
}
