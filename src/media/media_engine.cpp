// SPDX-License-Identifier: GPL-3.0-or-later
#include "media/media_engine.hpp"
#include "media/media_containers.hpp"
#include "media/subtitle_text.hpp"

#include "contract/thread_safety.hpp"
#include "log.hpp"
#include "supervised.hpp"
#include "media/media_timestamps.hpp"
#include "media/media_vod.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/pixdesc.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace macha {
namespace {

struct AvFrameDeleter {
    void operator()(AVFrame* value) const noexcept { av_frame_free(&value); }
};
struct AvPacketDeleter {
    void operator()(AVPacket* value) const noexcept { av_packet_free(&value); }
};
struct AvOutputContextDeleter {
    void operator()(AVFormatContext* value) const noexcept {
        if (value) avformat_free_context(value);
    }
};
struct AvCodecContextDeleter {
    void operator()(AVCodecContext* value) const noexcept { avcodec_free_context(&value); }
};
using AvFrameOwner = std::unique_ptr<AVFrame, AvFrameDeleter>;
using AvPacketOwner = std::unique_ptr<AVPacket, AvPacketDeleter>;
using AvOutputContextOwner = std::unique_ptr<AVFormatContext, AvOutputContextDeleter>;
using AvCodecContextOwner = std::unique_ptr<AVCodecContext, AvCodecContextDeleter>;

class AvDictionaryOwner {
    AVDictionary* value_{};
  public:
    ~AvDictionaryOwner() { av_dict_free(&value_); }
    AVDictionary** put() noexcept { return &value_; }
    AvDictionaryOwner(const AvDictionaryOwner&) = delete;
    AvDictionaryOwner& operator=(const AvDictionaryOwner&) = delete;
    AvDictionaryOwner() = default;
};

class AvChannelLayoutOwner {
    AVChannelLayout value_{};
  public:
    ~AvChannelLayoutOwner() { av_channel_layout_uninit(&value_); }
    AVChannelLayout* get() noexcept { return &value_; }
    AvChannelLayoutOwner(const AvChannelLayoutOwner&) = delete;
    AvChannelLayoutOwner& operator=(const AvChannelLayoutOwner&) = delete;
    AvChannelLayoutOwner() = default;
};

AvFrameOwner make_av_frame() {
    AvFrameOwner value(av_frame_alloc());
    if (!value) throw std::bad_alloc();
    return value;
}

AvPacketOwner make_av_packet() {
    AvPacketOwner value(av_packet_alloc());
    if (!value) throw std::bad_alloc();
    return value;
}

class AvSubtitleOwner {
    AVSubtitle* value_{};
  public:
    explicit AvSubtitleOwner(AVSubtitle& value) : value_(&value) {}
    ~AvSubtitleOwner() { avsubtitle_free(value_); }
    AvSubtitleOwner(const AvSubtitleOwner&) = delete;
    AvSubtitleOwner& operator=(const AvSubtitleOwner&) = delete;
};

std::string av_error(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(code, text.data(), text.size());
    return text.data();
}

void av_require(int rc, std::string_view operation) {
    if (rc < 0) throw std::runtime_error(std::string(operation) + ": " + av_error(rc));
}

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t be64(const uint8_t* p) {
    return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4);
}

constexpr uint32_t codec_tag(char a, char b, char c, char d) {
    return static_cast<uint8_t>(a) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

std::string fourcc(const uint8_t* p) {
    return std::string(reinterpret_cast<const char*>(p), 4);
}

std::string format_vtt_time(int64_t milliseconds) {
    if (milliseconds < 0) milliseconds = 0;
    auto hours = milliseconds / 3'600'000;
    milliseconds %= 3'600'000;
    auto minutes = milliseconds / 60'000;
    milliseconds %= 60'000;
    auto seconds = milliseconds / 1000;
    auto millis = milliseconds % 1000;
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2) << minutes << ':'
        << std::setw(2) << seconds << '.' << std::setw(3) << millis;
    return out.str();
}



struct InputIoState {
    std::shared_ptr<MediaInput> input;
    std::string media_id;
    MediaReadPurpose purpose{MediaReadPurpose::playback};
    uint64_t offset{};
    std::atomic_bool* cancelled{};
    Clock::time_point deadline{};
    // libav flattens read failures into EIO; keep the first underlying failure so
    // the caller can tell unreachable extents from bytes that are not media.
    std::string read_error;
    MediaStartProgress* progress{};
};

bool input_aborted(const InputIoState& state) {
    return (state.cancelled && state.cancelled->load()) ||
           (state.deadline != Clock::time_point{} && Clock::now() >= state.deadline);
}

int input_read(void* opaque, uint8_t* buffer, int buffer_size) {
    auto& state = *static_cast<InputIoState*>(opaque);
    if (input_aborted(state)) return AVERROR_EXIT;
    size_t wanted = 0;
    try {
        if (state.offset >= state.input->size()) return AVERROR_EOF;
        wanted = static_cast<size_t>(std::min<uint64_t>(
            static_cast<uint64_t>(buffer_size), state.input->size() - state.offset));
        const auto read_offset = state.offset;
        const auto started = Clock::now();
        auto n = state.input->read(state.offset, {buffer, wanted}, state.deadline, state.cancelled);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        if (elapsed >= std::chrono::milliseconds(250) && Log::enabled(LogLevel::debug)) {
            const char* purpose = state.purpose == MediaReadPurpose::probe ? "probe" :
                                  state.purpose == MediaReadPurpose::subtitle ? "subtitle" : "playback";
            Log::debug("playback source read media=" + state.media_id +
                       " purpose=" + purpose +
                       " offset=" + std::to_string(read_offset) +
                       " wanted=" + std::to_string(wanted) +
                       " got=" + std::to_string(n) +
                       " elapsed_ms=" + std::to_string(elapsed.count()));
        }
        if (!n) return AVERROR_EOF;
        state.offset += n;
        if (state.progress) {
            state.progress->source_bytes_read.fetch_add(n, std::memory_order_relaxed);
            state.progress->moved();
        }
        return static_cast<int>(n);
    } catch (const std::exception& error) {
        if (state.cancelled && state.cancelled->load()) return AVERROR_EXIT;
        if (state.deadline != Clock::time_point{} && Clock::now() >= state.deadline)
            return AVERROR(ETIMEDOUT);
        Log::warn("media input read failed media=" + state.media_id +
                  " offset=" + std::to_string(state.offset) +
                  " wanted=" + std::to_string(wanted) +
                  " error=" + error.what());
        if (state.read_error.empty()) state.read_error = error.what();
        return AVERROR(EIO);
    } catch (...) {
        if (state.cancelled && state.cancelled->load()) return AVERROR_EXIT;
        if (state.deadline != Clock::time_point{} && Clock::now() >= state.deadline)
            return AVERROR(ETIMEDOUT);
        Log::warn("media input read failed media=" + state.media_id +
                  " offset=" + std::to_string(state.offset) +
                  " wanted=" + std::to_string(wanted) +
                  " error=unknown");
        if (state.read_error.empty()) state.read_error = "unknown read failure";
        return AVERROR(EIO);
    }
}

int64_t input_seek(void* opaque, int64_t offset, int whence) {
    auto& state = *static_cast<InputIoState*>(opaque);
    if (whence == AVSEEK_SIZE) return static_cast<int64_t>(state.input->size());
    whence &= ~AVSEEK_FORCE;
    int64_t base = 0;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = static_cast<int64_t>(state.offset); break;
    case SEEK_END: base = static_cast<int64_t>(state.input->size()); break;
    default: return AVERROR(EINVAL);
    }
    if ((offset < 0 && base < -offset) ||
        (offset > 0 && base > std::numeric_limits<int64_t>::max() - offset))
        return AVERROR(EINVAL);
    auto target = base + offset;
    if (target < 0 || static_cast<uint64_t>(target) > state.input->size()) return AVERROR(EINVAL);
    state.offset = static_cast<uint64_t>(target);
    return target;
}

int input_interrupt(void* opaque) {
    auto* state = static_cast<InputIoState*>(opaque);
    return state && input_aborted(*state) ? 1 : 0;
}

class InputContext {
    InputIoState state_;
    AVIOContext* io_{};
    AVFormatContext* format_{};

    void cleanup() noexcept {
        if (format_) avformat_close_input(&format_);
        if (io_) {
            av_freep(&io_->buffer);
            avio_context_free(&io_);
        }
    }

  public:
    InputContext(const MediaSource& source, MediaReadPurpose purpose, std::atomic_bool* cancelled,
                 uint64_t probe_bytes = 0, std::chrono::milliseconds analyze = {},
                 std::chrono::milliseconds wall_timeout = {},
                 MediaStartProgress* progress = nullptr) {
        try {
            if (!source.open)
                throw MediaError(MediaFailure::unreadable, "media source has no reader factory");
            try {
                state_.input = source.open(purpose);
            } catch (const MediaError&) {
                throw;
            } catch (const std::exception& error) {
                throw MediaError(MediaFailure::unreadable,
                                 std::string("open media source: ") + error.what());
            }
            if (!state_.input)
                throw MediaError(MediaFailure::unreadable,
                                 "media source reader could not be opened");
            state_.media_id = source.media_id;
            state_.purpose = purpose;
            state_.cancelled = cancelled;
            state_.progress = progress;
            if (wall_timeout.count() > 0) state_.deadline = Clock::now() + wall_timeout;

            constexpr int io_buffer_size = 256 * 1024;
            auto* io_buffer = static_cast<uint8_t*>(av_malloc(io_buffer_size));
            if (!io_buffer) throw std::bad_alloc();
            io_ = avio_alloc_context(io_buffer, io_buffer_size, 0, &state_, input_read, nullptr, input_seek);
            if (!io_) {
                av_free(io_buffer);
                throw std::bad_alloc();
            }
            io_->seekable = AVIO_SEEKABLE_NORMAL;

            format_ = avformat_alloc_context();
            if (!format_) throw std::bad_alloc();
            format_->pb = io_;
            format_->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_GENPTS;
            format_->interrupt_callback.callback = input_interrupt;
            format_->interrupt_callback.opaque = &state_;
            if (probe_bytes)
                format_->probesize = static_cast<int64_t>(std::min<uint64_t>(probe_bytes, source.size));
            if (analyze.count() > 0)
                format_->max_analyze_duration = static_cast<int64_t>(analyze.count()) * 1000;

            auto* candidate = format_;
            int rc = avformat_open_input(&candidate, nullptr, nullptr, nullptr);
            format_ = candidate;
            if (rc < 0) {
                if (timed_out())
                    throw MediaError(MediaFailure::timed_out,
                                     "media probe timed out while opening input");
                // The underlying read failure is the truth; libav's EIO only carried it.
                if (!state_.read_error.empty())
                    throw MediaError(MediaFailure::unreadable,
                                     "read media: " + state_.read_error);
                if (rc == AVERROR(ETIMEDOUT))
                    throw MediaError(MediaFailure::timed_out, "open media: " + av_error(rc));
                if (rc == AVERROR(EIO) || rc == AVERROR(EAGAIN) || rc == AVERROR_EXIT)
                    throw MediaError(MediaFailure::unreadable, "open media: " + av_error(rc));
                throw MediaError(MediaFailure::unsupported, "open media: " + av_error(rc));
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~InputContext() { cleanup(); }

    InputContext(const InputContext&) = delete;
    InputContext& operator=(const InputContext&) = delete;

    AVFormatContext* get() const { return format_; }
    // Empty unless a read underneath libav failed.
    const std::string& read_error() const noexcept { return state_.read_error; }
    bool timed_out() const {
        return state_.deadline != Clock::time_point{} && Clock::now() >= state_.deadline;
    }
    void clear_deadline() { state_.deadline = Clock::time_point{}; }
};

std::string stream_language(const AVStream* stream) {
    if (auto* tag = av_dict_get(stream->metadata, "language", nullptr, 0)) return tag->value ? tag->value : "";
    return {};
}

MediaStreamType stream_type(AVMediaType type) {
    switch (type) {
    case AVMEDIA_TYPE_VIDEO: return MediaStreamType::video;
    case AVMEDIA_TYPE_AUDIO: return MediaStreamType::audio;
    case AVMEDIA_TYPE_SUBTITLE: return MediaStreamType::subtitle;
    default: return MediaStreamType::other;
    }
}

int choose_height(const AVCodecParameters* input, const PlaybackPlan& plan) {
    if (!plan.target_height || input->height <= *plan.target_height) return input->height;
    return std::max(2, *plan.target_height & ~1);
}

int choose_width(const AVCodecParameters* input, int target_height) {
    if (!input->width || !input->height || target_height == input->height) return input->width;
    auto scaled = static_cast<int>(std::llround(static_cast<double>(input->width) * target_height / input->height));
    return std::max(2, scaled & ~1);
}

// A transcode generation answers only once its first fragment is encoded, so
// that fragment is short. The segmenter forces a keyframe at each planned cut,
// so the encoder's GOP does not constrain it. Later fragments keep the target.
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

void materialise_deferred_seek_index(AVFormatContext* format, int video_stream,
                                     double requested_seek_seconds) {
    if (!format || !format->iformat || !format->iformat->name || video_stream < 0 ||
        video_stream >= static_cast<int>(format->nb_streams))
        return;
    if (!media_vod::requires_seek_index_materialisation(format->iformat->name)) return;

    auto* stream = format->streams[video_stream];
    const int before = avformat_index_get_entries_count(stream);
    const int64_t input_start_us = format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time;
    const int64_t requested_us = input_start_us +
        av_rescale_q(static_cast<int64_t>(std::llround(requested_seek_seconds * 1000.0)),
                     AVRational{1, 1000}, AV_TIME_BASE_Q);
    const int64_t requested_ts = av_rescale_q(requested_us, AV_TIME_BASE_Q, stream->time_base);

    // FFmpeg's Matroska demuxer defers Cues parsing until a seek, so after probing
    // the index may hold only a few keyframes. Seeking here loads the Cues before
    // the planner decides whether stream copy is safe; this context is
    // planning-only, so moving its demux position is harmless.
    const int rc = avformat_seek_file(format, video_stream, std::numeric_limits<int64_t>::min(),
                                      requested_ts, std::numeric_limits<int64_t>::max(),
                                      AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        Log::debug("media VOD planner could not materialise deferred seek index format=" +
                   std::string(format->iformat->name) + " error=" + av_error(rc));
        return;
    }

    const int after = avformat_index_get_entries_count(stream);
    Log::debug("media VOD planner materialised deferred seek index format=" +
               std::string(format->iformat->name) + " entries_before=" +
               std::to_string(before) + " entries_after=" + std::to_string(after));
}

std::vector<double> video_keyframe_seconds(AVFormatContext* format, int video_stream) {
    std::vector<double> keyframes;
    if (video_stream < 0 || video_stream >= static_cast<int>(format->nb_streams)) return keyframes;
    auto* stream = format->streams[video_stream];
    const int entries = avformat_index_get_entries_count(stream);
    if (entries <= 0) return keyframes;

    const int64_t input_start_us = format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time;
    keyframes.reserve(static_cast<size_t>(entries));
    for (int i = 0; i < entries; ++i) {
        const auto* entry = avformat_index_get_entry(stream, i);
        if (!entry || !(entry->flags & AVINDEX_KEYFRAME) || entry->timestamp == AV_NOPTS_VALUE) continue;
        const int64_t absolute_us = av_rescale_q(entry->timestamp, stream->time_base, AV_TIME_BASE_Q);
        const double seconds =
            std::max(0.0, static_cast<double>(absolute_us - input_start_us) / AV_TIME_BASE);
        if (!keyframes.empty() && seconds <= keyframes.back() + 0.0005) continue;
        keyframes.push_back(seconds);
    }
    return keyframes;
}

std::string index_density_log(std::span<const double> keyframes, double duration_seconds) {
    const auto density = media_vod::index_density(keyframes, duration_seconds);
    return " keyframes=" + std::to_string(density.entries) +
           " longest_gap_s=" + std::to_string(density.longest_gap_seconds) +
           " median_gap_s=" + std::to_string(density.median_gap_seconds);
}

} // namespace


namespace {

class FragmentWriter {
    std::shared_ptr<MediaSegmentStore> store_;
    Bytes pending_;
    Bytes init_;
    Bytes prefix_;
    Bytes fragment_;
    std::deque<double> durations_;
    double fallback_duration_{};
    size_t published_{};
    // Planned boundaries that produced no fragment; their media is in the next one.
    size_t carried_boundaries_{};
    bool init_published_{};
    // MPEG-TS: no box parsing and no init segment; bytes accumulate into the
    // current fragment and cut() publishes it at the planned boundaries.
    bool raw_fragments_{};

    void append(Bytes& target, const uint8_t* data, size_t size) {
        target.insert(target.end(), data, data + size);
    }

    // Duration of the outgoing fragment: its own planned segment plus every
    // carried boundary. Understating it misplaces every later segment.
    double publish_duration() {
        double total = 0.0;
        for (size_t i = 0; i <= carried_boundaries_; ++i) {
            if (durations_.empty()) {
                total += fallback_duration_;
                continue;
            }
            total += durations_.front();
            durations_.pop_front();
        }
        carried_boundaries_ = 0;
        return total;
    }

    void handle_box(std::string_view type, const uint8_t* data, size_t size) {
        if (!init_published_ && (type == "ftyp" || type == "moov" || type == "free")) {
            append(init_, data, size);
            return;
        }
        if (type == "moof") {
            if (!init_published_) {
                if (init_.empty()) throw std::runtime_error("fragmented MP4 muxer produced media before init segment");
                if (!store_->publish_init(std::move(init_))) throw std::runtime_error("stream cancelled");
                init_published_ = true;
            }
            fragment_ = std::move(prefix_);
            prefix_.clear();
            append(fragment_, data, size);
            return;
        }
        if (!fragment_.empty()) {
            append(fragment_, data, size);
            if (type == "mdat") {
                if (!store_->publish_segment(std::move(fragment_), publish_duration()))
                    throw std::runtime_error("stream cancelled");
                ++published_;
                fragment_.clear();
            }
            return;
        }
        if (!init_published_) {
            append(init_, data, size);
        } else {
            append(prefix_, data, size);
        }
    }

    void parse() {
        size_t offset = 0;
        while (pending_.size() - offset >= 8) {
            const auto* p = pending_.data() + offset;
            uint64_t size = be32(p);
            size_t header = 8;
            if (size == 1) {
                if (pending_.size() - offset < 16) break;
                size = be64(p + 8);
                header = 16;
            } else if (size == 0) {
                break;
            }
            if (size < header || size > 2ULL * 1024 * 1024 * 1024)
                throw std::runtime_error("invalid fragmented MP4 box size");
            if (pending_.size() - offset < size) break;
            auto type = fourcc(p + 4);
            handle_box(type, p, static_cast<size_t>(size));
            offset += static_cast<size_t>(size);
        }
        if (offset) pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(offset));
    }

  public:
    FragmentWriter(std::shared_ptr<MediaSegmentStore> store, std::chrono::milliseconds target,
                   const std::vector<double>& planned_durations)
        : store_(std::move(store)), durations_(planned_durations.begin(), planned_durations.end()),
          fallback_duration_(std::max(0.001, target.count() / 1000.0)),
          raw_fragments_(store_->container() == MediaContainer::mpegts) {}

    bool raw_fragments() const noexcept { return raw_fragments_; }
    size_t published() const noexcept { return published_; }
    void carry_boundary() noexcept { ++carried_boundaries_; }

    int write(const uint8_t* data, int size) {
        if (raw_fragments_) {
            append(fragment_, data, static_cast<size_t>(size));
            return size;
        }
        pending_.insert(pending_.end(), data, data + size);
        parse();
        return size;
    }

    // MPEG-TS: everything written since the previous cut is one segment.
    void cut() {
        if (!raw_fragments_ || fragment_.empty()) return;
        if (!store_->publish_segment(std::move(fragment_), publish_duration()))
            throw std::runtime_error("stream cancelled");
        ++published_;
        fragment_.clear();
    }

    void finish() {
        if (raw_fragments_) {
            cut();
            return;
        }
        parse();
        if (!pending_.empty()) {
            if (!fragment_.empty()) {
                append(fragment_, pending_.data(), pending_.size());
                pending_.clear();
            } else if (!init_published_) {
                append(init_, pending_.data(), pending_.size());
                pending_.clear();
            }
        }
        if (!init_published_ && !init_.empty()) {
            if (!store_->publish_init(std::move(init_))) throw std::runtime_error("stream cancelled");
            init_published_ = true;
        }
        if (!fragment_.empty()) {
            if (!store_->publish_segment(std::move(fragment_), publish_duration()))
                throw std::runtime_error("stream cancelled");
            ++published_;
            fragment_.clear();
        }
    }
};

class FragmentCuts {
    std::vector<double> durations_;
    size_t next_force_{};
    size_t next_cut_{};
    double force_time_{};
    double cut_time_{};

    static void advance(const std::vector<double>& durations, size_t& next, double& time) {
        ++next;
        if (next + 1 < durations.size()) time += durations[next];
    }

  public:
    explicit FragmentCuts(std::vector<double> durations) : durations_(std::move(durations)) {
        if (durations_.size() > 1) force_time_ = cut_time_ = durations_.front();
    }

    bool force_transcode_keyframe(double seconds) {
        if (durations_.size() < 2 || next_force_ + 1 >= durations_.size()) return false;
        if (seconds + 0.002 < force_time_) return false;
        advance(durations_, next_force_, force_time_);
        return true;
    }

    bool before_keyframe(double seconds) {
        if (durations_.size() < 2 || next_cut_ + 1 >= durations_.size()) return false;
        if (seconds + 0.002 < cut_time_) return false;
        advance(durations_, next_cut_, cut_time_);
        return true;
    }
};

int output_write(void* opaque, const uint8_t* buffer, int size) {
    try {
        return static_cast<FragmentWriter*>(opaque)->write(buffer, size);
    } catch (...) {
        return AVERROR(EIO);
    }
}

// Close the current fragment before a keyframe: flush the muxer (fMP4
// moof/mdat, or buffered TS packets); for MPEG-TS, publish the bytes.
void cut_fragment(AVFormatContext* output) {
    av_require(av_write_frame(output, nullptr), "flush VOD fragment");
    avio_flush(output->pb);
    if (auto* writer = static_cast<FragmentWriter*>(output->pb->opaque))
        writer->cut();
}

class OutputIo {
    AVIOContext* io_{};
  public:
    explicit OutputIo(FragmentWriter& writer) {
        constexpr int buffer_size = 256 * 1024;
        auto* buffer = static_cast<uint8_t*>(av_malloc(buffer_size));
        if (!buffer) throw std::bad_alloc();
        io_ = avio_alloc_context(buffer, buffer_size, 1, &writer, nullptr, output_write, nullptr);
        if (!io_) {
            av_free(buffer);
            throw std::bad_alloc();
        }
        io_->seekable = 0;
    }
    ~OutputIo() {
        if (io_) {
            av_freep(&io_->buffer);
            avio_context_free(&io_);
        }
    }
    AVIOContext* get() const { return io_; }
};

struct StreamPipeline {
    int input_index{-1};
    int output_index{-1};
    MediaStreamType type{MediaStreamType::other};
    MediaTransform transform{MediaTransform::copy};
    AVStream* input_stream{};
    AVStream* output_stream{};
    AVCodecContext* decoder{};
    AVCodecContext* encoder{};
    SwsContext* sws{};
    SwrContext* swr{};
    AVAudioFifo* fifo{};
    int audio_input_rate{};
    int64_t audio_next_pts{};
    bool audio_pts_initialized{};

    // Stream-copy timestamps are repaired after rescaling into the muxer's output
    // timebase, since rescaling can collapse two source DTS values into one tick.
    MediaTimestampRepairState copy_timestamps;
    bool copy_repair_reported{};
    // Encoded streams need the same repair: rescaling can collapse adjacent DTS.
    MediaTimestampRepairState encoded_timestamps;
    bool encoded_repair_reported{};
    int64_t last_video_encoder_pts{AV_NOPTS_VALUE};
    // Set once this pipeline has muxed its first packet (copied or encoded); the
    // delayed moov is written once every pipeline has (see run_pipeline).
    bool output_started{};
    MediaStartProgress* progress{};

    ~StreamPipeline() {
        if (fifo) av_audio_fifo_free(fifo);
        if (swr) swr_free(&swr);
        if (sws) sws_freeContext(sws);
        if (encoder) avcodec_free_context(&encoder);
        if (decoder) avcodec_free_context(&decoder);
    }
};

void open_decoder(StreamPipeline& pipe, size_t thread_limit = 1) {
    const auto* codec = avcodec_find_decoder(pipe.input_stream->codecpar->codec_id);
    if (!codec) throw std::runtime_error("decoder unavailable for " + std::string(avcodec_get_name(pipe.input_stream->codecpar->codec_id)));
    pipe.decoder = avcodec_alloc_context3(codec);
    if (!pipe.decoder) throw std::bad_alloc();
    av_require(avcodec_parameters_to_context(pipe.decoder, pipe.input_stream->codecpar), "copy decoder parameters");
    pipe.decoder->pkt_timebase = pipe.input_stream->time_base;
    // Viewer work: never libav's automatic thread count; more than one thread by
    // default, bounded per pipeline by configuration.
    pipe.decoder->thread_count = static_cast<int>(thread_limit);
    av_require(avcodec_open2(pipe.decoder, codec, nullptr), "open decoder");
    if (pipe.decoder->thread_count > static_cast<int>(thread_limit))
        throw std::runtime_error("decoder exceeded configured thread limit");
}

const AVCodec* h264_encoder() {
    if (auto* codec = avcodec_find_encoder_by_name("libx264")) return codec;
    return avcodec_find_encoder(AV_CODEC_ID_H264);
}

void setup_video_transcode(StreamPipeline& pipe, AVFormatContext* input, AVFormatContext* output,
                           const PlaybackPlan& plan, std::chrono::milliseconds segment_duration,
                           size_t decoder_threads, size_t encoder_threads) {
    open_decoder(pipe, decoder_threads);
    const auto* codec = h264_encoder();
    if (!codec) throw std::runtime_error("H.264 encoder is unavailable in libavcodec");
    pipe.encoder = avcodec_alloc_context3(codec);
    if (!pipe.encoder) throw std::bad_alloc();
    auto* enc = pipe.encoder;
    const auto* par = pipe.input_stream->codecpar;
    enc->height = choose_height(par, plan);
    enc->width = choose_width(par, enc->height);
    enc->sample_aspect_ratio = pipe.input_stream->sample_aspect_ratio;
    if (enc->sample_aspect_ratio.num <= 0 || enc->sample_aspect_ratio.den <= 0)
        enc->sample_aspect_ratio = AVRational{1, 1};
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    auto frame_rate = av_guess_frame_rate(input, pipe.input_stream, nullptr);
    if (frame_rate.num <= 0 || frame_rate.den <= 0) frame_rate = AVRational{25, 1};
    enc->framerate = frame_rate;
    enc->time_base = av_inv_q(frame_rate);
    if (enc->time_base.num <= 0 || enc->time_base.den <= 0) enc->time_base = pipe.input_stream->time_base;
    enc->gop_size = std::max(1, static_cast<int>(std::llround(av_q2d(frame_rate) * segment_duration.count() / 1000.0)));
    enc->max_b_frames = 0;
    if (plan.target_video_bitrate) {
        enc->bit_rate = static_cast<int64_t>(*plan.target_video_bitrate);
        enc->rc_max_rate = enc->bit_rate;
        enc->rc_buffer_size = std::min<int64_t>(std::numeric_limits<int>::max(), enc->bit_rate * 2);
    }
    if (output->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    // Frame threading across the node's cores. No tune=zerolatency: it switches
    // x264 to sliced threading, a large throughput loss for latency hidden behind
    // a fragment anyway.
    const auto hardware_threads = std::max(1U, std::thread::hardware_concurrency());
    enc->thread_count = static_cast<int>(
        encoder_threads ? std::min<size_t>(encoder_threads, 64) : hardware_threads);
    enc->thread_type = FF_THREAD_FRAME;
    if (enc->priv_data) {
        // Interactive fragmented stream: short look-ahead so the first fragment is not
        // held back, and no B-frames so decode order is presentation order.
        if (std::string_view(codec->name) == "libx264") {
            av_require(av_opt_set(enc->priv_data, "preset", "veryfast", 0),
                       "set x264 realtime preset");
            av_require(av_opt_set(enc->priv_data, "x264-params",
                                  "rc-lookahead=8:sync-lookahead=0:sliced-threads=0", 0),
                       "set x264 interactive look-ahead");
        } else {
            (void)av_opt_set(enc->priv_data, "preset", "veryfast", 0);
        }
        if (!plan.target_video_bitrate) (void)av_opt_set(enc->priv_data, "crf", "20", 0);
    }
    av_require(avcodec_open2(enc, codec, nullptr), "open H.264 encoder");
    Log::debug("libav interactive video codec decoder=" +
               std::string(pipe.decoder->codec ? pipe.decoder->codec->name : "unknown") +
               " decoder_threads=" + std::to_string(pipe.decoder->thread_count) +
               " encoder=" + std::string(codec->name ? codec->name : "unknown") +
               " encoder_threads=" + std::to_string(enc->thread_count) +
               " frame_threads=1 zero_latency=0 width=" + std::to_string(enc->width) +
               " height=" + std::to_string(enc->height));
    pipe.output_stream->time_base = enc->time_base;
    pipe.output_stream->sample_aspect_ratio = enc->sample_aspect_ratio;
    av_require(avcodec_parameters_from_context(pipe.output_stream->codecpar, enc), "export video encoder parameters");
    pipe.output_stream->codecpar->codec_tag = 0;
    // The decoded pixel format may be unknown until the first frame; the scaler is
    // created from actual frame properties.
}

// channelConfiguration of an AAC AudioSpecificConfig (bits 9-12, after object
// type and sampling frequency index). Zero means a Program Config Element,
// which Chrome refuses. Negative when there is no extradata (no global header).
int aac_channel_configuration(const AVCodecContext* enc) noexcept {
    if (!enc->extradata || enc->extradata_size < 2) return -1;
    const auto bits = (static_cast<unsigned>(enc->extradata[0]) << 8) |
                      static_cast<unsigned>(enc->extradata[1]);
    return static_cast<int>((bits >> 3) & 0xf);
}

void setup_audio_transcode(StreamPipeline& pipe, AVFormatContext* output) {
    open_decoder(pipe);
    const auto* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) throw std::runtime_error("AAC encoder is unavailable in libavcodec");
    const int source_channels = pipe.decoder->ch_layout.nb_channels;
    const auto bitrate_for = [](int channels) {
        return static_cast<int64_t>(std::clamp(channels, 1, 8)) * 64000;
    };
    // Keep the source channel count: a codec change is not a downmix request.
    // Encode in AAC's standard layout for it, since a non-standard layout needs a
    // PCE that Chrome will not parse; see aac_standard_channel_layout.
    const auto open_encoder = [&](const char* layout) -> int {
        if (pipe.encoder) avcodec_free_context(&pipe.encoder);
        pipe.encoder = avcodec_alloc_context3(codec);
        if (!pipe.encoder) throw std::bad_alloc();
        auto* enc = pipe.encoder;
        enc->sample_rate = pipe.decoder->sample_rate > 0 ? pipe.decoder->sample_rate : 48000;
        enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
        if (const int rc = av_channel_layout_from_string(&enc->ch_layout, layout); rc < 0) return rc;
        enc->time_base = AVRational{1, enc->sample_rate};
        enc->bit_rate = bitrate_for(enc->ch_layout.nb_channels);
        if (output->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (const int rc = avcodec_open2(enc, codec, nullptr); rc < 0) return rc;
        // Refuse an encoder that described the layout in a PCE. Stereo is always
        // standard.
        if (aac_channel_configuration(enc) == 0) return AVERROR(EINVAL);
        return 0;
    };

    const auto* layout = aac_standard_channel_layout(source_channels > 0 ? source_channels : 2);
    if (open_encoder(layout) < 0) {
        Log::warn(std::string("libav AAC encoder would not encode a standard channel "
                              "configuration layout=") + layout + " channels=" +
                  std::to_string(source_channels) + "; encoding stereo");
        av_require(open_encoder("stereo"), "open AAC encoder");
    }
    auto* enc = pipe.encoder;
    pipe.output_stream->time_base = enc->time_base;
    av_require(avcodec_parameters_from_context(pipe.output_stream->codecpar, enc), "export audio encoder parameters");
    pipe.output_stream->codecpar->codec_tag = 0;
    // Input sample format/layout is authoritative only from the first decoded
    // frame, so the resampler is created lazily in process_audio_frame().
    pipe.fifo = av_audio_fifo_alloc(enc->sample_fmt, enc->ch_layout.nb_channels,
                                    std::max(1024, enc->frame_size * 2));
    if (!pipe.fifo) throw std::bad_alloc();
}

void prepare_copy_packet(StreamPipeline& pipe, AVPacket* packet, int64_t origin) {
    if (packet->pts != AV_NOPTS_VALUE) packet->pts -= origin;
    if (packet->dts != AV_NOPTS_VALUE) packet->dts -= origin;

    // The muxer may change the stream timebase in write_header; repair after this
    // rescale so DTS is strictly monotonic as av_interleaved_write_frame sees it.
    av_packet_rescale_ts(packet, pipe.input_stream->time_base,
                         pipe.output_stream->time_base);

    static_assert(AV_NOPTS_VALUE == kNoMediaTimestamp);
    MediaPacketTimestamps timestamps{packet->pts, packet->dts, packet->duration};
    normalize_media_timestamps(pipe.copy_timestamps, timestamps);
    packet->pts = timestamps.pts;
    packet->dts = timestamps.dts;
    packet->duration = timestamps.duration;
    packet->stream_index = pipe.output_stream->index;
    packet->pos = -1;
}

void write_mux_packet(AVFormatContext* output, AVPacket* packet) {
    av_require(av_interleaved_write_frame(output, packet), "mux packet");
    avio_flush(output->pb);
}


void encode_video_frame(StreamPipeline& pipe, AVFormatContext* output, AVFrame* decoded,
                        AVPacket* encoded, FragmentCuts& cuts) {
    auto source_pts = decoded->best_effort_timestamp != AV_NOPTS_VALUE
                          ? decoded->best_effort_timestamp
                          : decoded->pts;
    if (source_pts != AV_NOPTS_VALUE && source_pts < 0) return;
    auto frame = make_av_frame();
    frame->format = pipe.encoder->pix_fmt;
    frame->width = pipe.encoder->width;
    frame->height = pipe.encoder->height;
    int rc = av_frame_get_buffer(frame.get(), 32);
    if (rc >= 0) rc = av_frame_make_writable(frame.get());
    if (rc >= 0) {
        pipe.sws = sws_getCachedContext(
            pipe.sws, decoded->width, decoded->height, static_cast<AVPixelFormat>(decoded->format),
            pipe.encoder->width, pipe.encoder->height, pipe.encoder->pix_fmt, SWS_BILINEAR,
            nullptr, nullptr, nullptr);
        if (!pipe.sws) {
            throw std::runtime_error("cannot create video scaler for decoded frame");
        }
        sws_scale(pipe.sws, decoded->data, decoded->linesize, 0, decoded->height,
                  frame->data, frame->linesize);
        frame->pts = source_pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE
                     : av_rescale_q(source_pts, pipe.input_stream->time_base, pipe.encoder->time_base);
        // Malformed files and quantisation around seeks can yield duplicate or
        // backwards PTS in the encoder timebase, which libx264 rejects. Repair before
        // avcodec_send_frame() so the encoder sees a strict display-order timeline.
        static_assert(AV_NOPTS_VALUE == kNoMediaTimestamp);
        frame->pts = normalize_encoder_pts(pipe.last_video_encoder_pts, frame->pts);
        if (frame->pts != AV_NOPTS_VALUE &&
            cuts.force_transcode_keyframe(frame->pts * av_q2d(pipe.encoder->time_base)))
            frame->pict_type = AV_PICTURE_TYPE_I;
        rc = avcodec_send_frame(pipe.encoder, frame.get());
    }
    av_require(rc, "send video frame to encoder");
    while (true) {
        rc = avcodec_receive_packet(pipe.encoder, encoded);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        av_require(rc, "receive encoded video packet");
        auto seconds = encoded->pts == AV_NOPTS_VALUE ? 0.0 : encoded->pts * av_q2d(pipe.encoder->time_base);
        if ((encoded->flags & AV_PKT_FLAG_KEY) != 0 && cuts.before_keyframe(seconds)) {
            auto* writer = static_cast<FragmentWriter*>(output->pb->opaque);
            const auto before = writer ? writer->published() : 0;
            cut_fragment(output);
            // The flush that writes the delayed moov produces no moof, so this boundary's
            // media joins the next fragment; carry its duration or the playlist
            // misplaces every later segment.
            if (writer && writer->published() == before) writer->carry_boundary();
        }
        av_packet_rescale_ts(encoded, pipe.encoder->time_base, pipe.output_stream->time_base);
        static_assert(AV_NOPTS_VALUE == kNoMediaTimestamp);
        const auto repairs_before = pipe.encoded_timestamps.repair_count();
        MediaPacketTimestamps timestamps{encoded->pts, encoded->dts, encoded->duration};
        normalize_media_timestamps(pipe.encoded_timestamps, timestamps);
        encoded->pts = timestamps.pts;
        encoded->dts = timestamps.dts;
        encoded->duration = timestamps.duration;
        if (!pipe.encoded_repair_reported &&
            pipe.encoded_timestamps.repair_count() != repairs_before) {
            pipe.encoded_repair_reported = true;
            Log::warn("libav transcode repairing encoded video timestamps stream=" +
                      std::to_string(pipe.input_index));
        }
        encoded->stream_index = pipe.output_stream->index;
        encoded->pos = -1;
        const auto pts = encoded->pts;
        const auto dts = encoded->dts;
        const auto duration = encoded->duration;
        const auto flags = encoded->flags;
        const int mux_rc = av_interleaved_write_frame(output, encoded);
        if (mux_rc >= 0) pipe.output_started = true;
        if (mux_rc >= 0 && pipe.progress && pts != AV_NOPTS_VALUE) {
            const auto us = av_rescale_q(pts, pipe.output_stream->time_base, AV_TIME_BASE_Q);
            if (us > pipe.progress->output_media_us.load(std::memory_order_relaxed)) {
                pipe.progress->output_media_us.store(us, std::memory_order_relaxed);
                pipe.progress->moved();
            }
        }
        if (mux_rc < 0) {
            Log::warn("libav mux rejected encoded video packet stream=" +
                      std::to_string(pipe.input_index) +
                      " pts=" + std::to_string(pts) +
                      " dts=" + std::to_string(dts) +
                      " duration=" + std::to_string(duration) +
                      " key=" + std::string((flags & AV_PKT_FLAG_KEY) ? "true" : "false") +
                      " encoder_tb=" + std::to_string(pipe.encoder->time_base.num) + "/" +
                      std::to_string(pipe.encoder->time_base.den) +
                      " output_tb=" + std::to_string(pipe.output_stream->time_base.num) + "/" +
                      std::to_string(pipe.output_stream->time_base.den) +
                      " size=" + std::to_string(pipe.encoder->width) + "x" +
                      std::to_string(pipe.encoder->height) +
                      " sar=" + std::to_string(pipe.encoder->sample_aspect_ratio.num) + "/" +
                      std::to_string(pipe.encoder->sample_aspect_ratio.den) +
                      " error=" + av_error(mux_rc));
            av_require(mux_rc, "mux encoded video packet");
        }
        avio_flush(output->pb);
        av_packet_unref(encoded);
    }
}

void encode_audio_available(StreamPipeline& pipe, AVFormatContext* output, AVPacket* encoded,
                            bool flush_partial) {
    const auto frame_size = pipe.encoder->frame_size > 0 ? pipe.encoder->frame_size : 1024;
    while (av_audio_fifo_size(pipe.fifo) >= frame_size || (flush_partial && av_audio_fifo_size(pipe.fifo) > 0)) {
        auto available = av_audio_fifo_size(pipe.fifo);
        auto frame = make_av_frame();
        frame->nb_samples = frame_size;
        frame->format = pipe.encoder->sample_fmt;
        frame->sample_rate = pipe.encoder->sample_rate;
        int rc = av_channel_layout_copy(&frame->ch_layout, &pipe.encoder->ch_layout);
        if (rc >= 0) rc = av_frame_get_buffer(frame.get(), 0);
        if (rc >= 0) rc = av_frame_make_writable(frame.get());
        if (rc >= 0) {
            auto take = std::min(available, frame_size);
            rc = av_audio_fifo_read(pipe.fifo, reinterpret_cast<void**>(frame->extended_data), take);
            if (rc >= 0 && take < frame_size) {
                av_samples_set_silence(frame->extended_data, take, frame_size - take,
                                       pipe.encoder->ch_layout.nb_channels, pipe.encoder->sample_fmt);
                rc = frame_size;
            }
        }
        frame->pts = pipe.audio_next_pts;
        pipe.audio_next_pts += frame_size;
        if (rc >= 0) rc = avcodec_send_frame(pipe.encoder, frame.get());
        av_require(rc, "send audio frame to encoder");
        while (true) {
            rc = avcodec_receive_packet(pipe.encoder, encoded);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            av_require(rc, "receive encoded audio packet");
            av_packet_rescale_ts(encoded, pipe.encoder->time_base, pipe.output_stream->time_base);
            encoded->stream_index = pipe.output_stream->index;
            encoded->pos = -1;
            av_require(av_interleaved_write_frame(output, encoded), "mux encoded audio packet");
            pipe.output_started = true;
            avio_flush(output->pb);
            av_packet_unref(encoded);
        }
    }
}

void ensure_audio_resampler(StreamPipeline& pipe, const AVFrame* decoded) {
    if (pipe.swr) return;
    auto input_rate = decoded->sample_rate > 0 ? decoded->sample_rate : pipe.decoder->sample_rate;
    if (input_rate <= 0) throw std::runtime_error("decoded audio has no sample rate");
    auto input_format = static_cast<AVSampleFormat>(decoded->format);
    if (input_format == AV_SAMPLE_FMT_NONE)
        throw std::runtime_error("decoded audio has no sample format");

    AvChannelLayoutOwner input_layout;
    int rc = 0;
    if (decoded->ch_layout.nb_channels > 0)
        rc = av_channel_layout_copy(input_layout.get(), &decoded->ch_layout);
    else if (pipe.decoder->ch_layout.nb_channels > 0)
        rc = av_channel_layout_copy(input_layout.get(), &pipe.decoder->ch_layout);
    else
        av_channel_layout_default(input_layout.get(), 2);
    av_require(rc, "copy decoded audio channel layout");

    rc = swr_alloc_set_opts2(&pipe.swr, &pipe.encoder->ch_layout, pipe.encoder->sample_fmt,
                             pipe.encoder->sample_rate, input_layout.get(), input_format,
                             input_rate, 0, nullptr);
    av_require(rc, "configure audio resampler");
    // Without correction the audio clock is a free-running sample counter seeded
    // once, so any mismatch between resampled samples and elapsed source time
    // compounds. async=1 enables libswresample's fill/trim (silence or drop) to
    // track the PTS fed via swr_next_pts(). Never add max_soft_comp: a ratio
    // nudge shifts pitch, which is worse than a silent fill/trim.
    av_require(av_opt_set_double(pipe.swr, "async", 1, 0), "enable resampler fill/trim compensation");
    av_require(swr_init(pipe.swr), "open audio resampler");
    pipe.audio_input_rate = input_rate;
}

void process_audio_frame(StreamPipeline& pipe, AVFormatContext* output, AVFrame* decoded, AVPacket* encoded) {
    auto source_pts = decoded->best_effort_timestamp != AV_NOPTS_VALUE
                          ? decoded->best_effort_timestamp
                          : decoded->pts;
    if (source_pts != AV_NOPTS_VALUE && source_pts < 0) return;
    ensure_audio_resampler(pipe, decoded);
    auto input_rate = decoded->sample_rate > 0 ? decoded->sample_rate : pipe.decoder->sample_rate;
    if (!pipe.audio_pts_initialized) {
        if (source_pts != AV_NOPTS_VALUE)
            pipe.audio_next_pts = av_rescale_q(source_pts, pipe.input_stream->time_base,
                                               pipe.encoder->time_base);
        pipe.audio_pts_initialized = true;
    }
    // Feed the source PTS to swr_next_pts in 1/(in_rate*out_rate) units. Forming
    // that AVRational overflows its 32-bit denominator (48 kHz squared exceeds
    // INT32_MAX), so rescale to 1/in_rate and multiply by out_rate in int64, as
    // af_aresample.c does.
    if (source_pts != AV_NOPTS_VALUE) {
        const auto in_rate_samples =
            av_rescale_q(source_pts, pipe.input_stream->time_base, AVRational{1, input_rate});
        swr_next_pts(pipe.swr, in_rate_samples * pipe.encoder->sample_rate);
    }
    auto max_samples = static_cast<int>(av_rescale_rnd(
        swr_get_delay(pipe.swr, input_rate) + decoded->nb_samples,
        pipe.encoder->sample_rate, input_rate, AV_ROUND_UP));
    auto converted = make_av_frame();
    converted->nb_samples = std::max(1, max_samples);
    converted->format = pipe.encoder->sample_fmt;
    converted->sample_rate = pipe.encoder->sample_rate;
    int rc = av_channel_layout_copy(&converted->ch_layout, &pipe.encoder->ch_layout);
    if (rc >= 0) rc = av_frame_get_buffer(converted.get(), 0);
    if (rc >= 0) {
        auto** input_data = const_cast<const uint8_t**>(decoded->extended_data);
        rc = swr_convert(pipe.swr, converted->extended_data, converted->nb_samples,
                         input_data, decoded->nb_samples);
    }
    if (rc >= 0) converted->nb_samples = rc;
    if (rc >= 0 && converted->nb_samples) {
        av_require(av_audio_fifo_realloc(pipe.fifo, av_audio_fifo_size(pipe.fifo) + converted->nb_samples),
                   "grow audio FIFO");
        rc = av_audio_fifo_write(pipe.fifo, reinterpret_cast<void**>(converted->extended_data), converted->nb_samples);
        if (rc != converted->nb_samples) rc = AVERROR(EIO);
        else rc = 0;
    }
    av_require(rc, "resample audio frame");
    encode_audio_available(pipe, output, encoded, false);
}

void flush_audio_resampler(StreamPipeline& pipe) {
    if (!pipe.swr || !pipe.fifo || pipe.audio_input_rate <= 0) return;
    while (true) {
        auto delay = swr_get_delay(pipe.swr, pipe.audio_input_rate);
        if (delay <= 0) break;
        auto max_samples = static_cast<int>(av_rescale_rnd(
            delay, pipe.encoder->sample_rate, pipe.audio_input_rate, AV_ROUND_UP));
        if (max_samples <= 0) break;
        auto converted = make_av_frame();
        converted->nb_samples = max_samples;
        converted->format = pipe.encoder->sample_fmt;
        converted->sample_rate = pipe.encoder->sample_rate;
        int rc = av_channel_layout_copy(&converted->ch_layout, &pipe.encoder->ch_layout);
        if (rc >= 0) rc = av_frame_get_buffer(converted.get(), 0);
        if (rc >= 0) rc = swr_convert(pipe.swr, converted->extended_data, converted->nb_samples, nullptr, 0);
        if (rc > 0) {
            converted->nb_samples = rc;
            av_require(av_audio_fifo_realloc(pipe.fifo, av_audio_fifo_size(pipe.fifo) + rc),
                       "grow audio FIFO while draining resampler");
            auto written = av_audio_fifo_write(pipe.fifo, reinterpret_cast<void**>(converted->extended_data), rc);
            if (written != rc) rc = AVERROR(EIO);
            else rc = 0;
        }
        av_require(rc, "drain audio resampler");
        if (delay == swr_get_delay(pipe.swr, pipe.audio_input_rate)) break;
    }
}

void flush_decoder(StreamPipeline& pipe, AVFormatContext* output, AVPacket* encoded,
                   FragmentCuts& cuts) {
    if (!pipe.decoder) return;
    int rc = avcodec_send_packet(pipe.decoder, nullptr);
    if (rc < 0 && rc != AVERROR_EOF) av_require(rc, "flush decoder");
    auto decoded = make_av_frame();
    while (true) {
        rc = avcodec_receive_frame(pipe.decoder, decoded.get());
        if (rc == AVERROR_EOF || rc == AVERROR(EAGAIN)) break;
        av_require(rc, "receive flushed frame");
        if (pipe.type == MediaStreamType::video)
            encode_video_frame(pipe, output, decoded.get(), encoded, cuts);
        else
            process_audio_frame(pipe, output, decoded.get(), encoded);
        av_frame_unref(decoded.get());
    }
}

void flush_encoder(StreamPipeline& pipe, AVFormatContext* output, AVPacket* packet) {
    if (!pipe.encoder) return;
    if (pipe.type == MediaStreamType::audio) {
        flush_audio_resampler(pipe);
        encode_audio_available(pipe, output, packet, true);
    }
    int rc = avcodec_send_frame(pipe.encoder, nullptr);
    if (rc < 0 && rc != AVERROR_EOF) av_require(rc, "flush encoder");
    while (true) {
        rc = avcodec_receive_packet(pipe.encoder, packet);
        if (rc == AVERROR_EOF || rc == AVERROR(EAGAIN)) break;
        av_require(rc, "receive flushed packet");
        av_packet_rescale_ts(packet, pipe.encoder->time_base, pipe.output_stream->time_base);
        if (pipe.type == MediaStreamType::video) {
            MediaPacketTimestamps timestamps{packet->pts, packet->dts, packet->duration};
            normalize_media_timestamps(pipe.encoded_timestamps, timestamps);
            packet->pts = timestamps.pts;
            packet->dts = timestamps.dts;
            packet->duration = timestamps.duration;
        }
        packet->stream_index = pipe.output_stream->index;
        packet->pos = -1;
        av_require(av_interleaved_write_frame(output, packet), "mux flushed packet");
        avio_flush(output->pb);
        av_packet_unref(packet);
    }
}

void run_pipeline(const MediaSource& source, const HlsVodPlan& vod_plan,
                  std::chrono::milliseconds segment_duration,
                  std::shared_ptr<MediaSegmentStore> store, std::atomic_bool& cancelled,
                  uint64_t probe_bytes, std::chrono::milliseconds analyze_duration,
                  std::chrono::milliseconds startup_timeout, size_t video_decoder_threads,
                  size_t video_encoder_threads, MediaStartProgress& progress) {
    const auto& plan = vod_plan.playback;
    if (vod_plan.segment_durations.empty())
        throw std::runtime_error("VOD plan contains no media segments");

    // This context repeats some container inspection because it owns the
    // decoder/muxer state. Bound it so stopping a failed generation never joins a
    // worker blocked in stream discovery.
    InputContext input(source, MediaReadPurpose::playback, &cancelled,
                       probe_bytes, analyze_duration, startup_timeout, &progress);
    auto* in = input.get();
    const auto stream_info_started = Clock::now();
    auto stream_info_rc = avformat_find_stream_info(in, nullptr);
    const auto stream_info_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    Clock::now() - stream_info_started)
                                    .count();
    if (stream_info_rc < 0 && input.timed_out())
        throw MediaError(MediaFailure::timed_out,
                         "playback pipeline timed out while reading stream information");
    av_require(stream_info_rc, "read stream information");
    if (cancelled.load()) throw std::runtime_error("stream cancelled");
    // The deadline guards startup only; playback reads use the cancellation path.
    input.clear_deadline();

    // Output timestamps are relative to plan.seek (published as seek_ms). A remux
    // seek is the indexed keyframe at or before the request, so the backward
    // search lands on it; a transcode seeks back for pre-roll and discards decoded
    // frames before zero.
    const int64_t input_start_us = in->start_time == AV_NOPTS_VALUE ? 0 : in->start_time;
    const int64_t seek_target_us = input_start_us +
        av_rescale_q(plan.seek.count(), AVRational{1, 1000}, AV_TIME_BASE_Q);
    int64_t seek_ms = 0;
    if (plan.seek.count() > 0) {
        const auto seek_started = Clock::now();
        av_require(avformat_seek_file(in, -1, std::numeric_limits<int64_t>::min(), seek_target_us,
                                     std::numeric_limits<int64_t>::max(), AVSEEK_FLAG_BACKWARD),
                   "seek media");
        seek_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - seek_started)
                     .count();
    }
    // Separates stream-info rediscovery and the container seek (paid again here:
    // this pipeline opens its own InputContext) from decode time in startup.
    Log::debug("media playback pipeline seek timing media=" + source.media_id +
              " seek_target_ms=" + std::to_string(plan.seek.count()) +
              " stream_info_ms=" + std::to_string(stream_info_ms) +
              " container_seek_ms=" + std::to_string(seek_ms));

    const bool mpegts = plan.container == MediaContainer::mpegts;
    AVFormatContext* raw_out = nullptr;
    const auto output_rc =
        avformat_alloc_output_context2(&raw_out, nullptr, mpegts ? "mpegts" : "mp4", nullptr);
    AvOutputContextOwner output_owner(raw_out);
    av_require(output_rc, mpegts ? "create MPEG-TS muxer" : "create fragmented MP4 muxer");
    if (!output_owner) throw std::runtime_error("segment muxer is unavailable");
    auto* out = output_owner.get();

    std::vector<std::unique_ptr<StreamPipeline>> pipelines;
    auto add_stream = [&](int index, MediaStreamType type, MediaTransform transform) {
        if (index < 0) return;
        if (index >= static_cast<int>(in->nb_streams)) throw std::runtime_error("selected stream index is out of range");
        auto* input_stream = in->streams[index];
        if ((type == MediaStreamType::video && input_stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) ||
            (type == MediaStreamType::audio && input_stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO))
            throw std::runtime_error("selected stream has the wrong media type");
        auto pipe = std::make_unique<StreamPipeline>();
        pipe->input_index = index;
        pipe->type = type;
        pipe->transform = transform;
        pipe->input_stream = input_stream;
        pipe->output_stream = avformat_new_stream(out, nullptr);
        if (!pipe->output_stream) throw std::bad_alloc();
        pipe->output_index = pipe->output_stream->index;
        pipe->progress = &progress;
        if (transform == MediaTransform::copy) {
            av_require(avcodec_parameters_copy(pipe->output_stream->codecpar, input_stream->codecpar), "copy stream parameters");
            pipe->output_stream->codecpar->codec_tag = 0;
            if (type == MediaStreamType::video && input_stream->codecpar->codec_id == AV_CODEC_ID_HEVC)
                pipe->output_stream->codecpar->codec_tag = codec_tag('h', 'v', 'c', '1');
            pipe->output_stream->time_base = input_stream->time_base;
            pipe->output_stream->sample_aspect_ratio = input_stream->sample_aspect_ratio;
        } else if (type == MediaStreamType::video) {
            setup_video_transcode(*pipe, in, out, plan, segment_duration,
                                  video_decoder_threads, video_encoder_threads);
        } else {
            setup_audio_transcode(*pipe, out);
        }
        pipelines.push_back(std::move(pipe));
    };

    try {
        if (plan.video != MediaTransform::omit) add_stream(plan.video_stream, MediaStreamType::video, plan.video);
        if (plan.audio != MediaTransform::omit) add_stream(plan.audio_stream, MediaStreamType::audio, plan.audio);
        if (pipelines.empty()) throw std::runtime_error("playback plan contains no output streams");

        FragmentWriter writer(store, segment_duration, vod_plan.segment_durations);
        FragmentCuts cuts(vod_plan.segment_durations);
        OutputIo output_io(writer);
        out->pb = output_io.get();
        out->flags |= AVFMT_FLAG_CUSTOM_IO;
        out->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;

        AvDictionaryOwner options;
        if (!mpegts) {
            // delay_moov: the (E-)AC-3 sample entry's dac3/dec3 box can only be filled
            // from a parsed packet; the moov still precedes the first moof.
            av_require(av_dict_set(options.put(), "movflags",
                                   "frag_custom+empty_moov+default_base_moof+omit_tfhd_offset+negative_cts_offsets+delay_moov",
                                   0),
                       "set fragmented MP4 options");
        }
        const bool has_video = std::any_of(pipelines.begin(), pipelines.end(), [](const auto& p) {
            return p->type == MediaStreamType::video;
        });
        if (!has_video && !mpegts) {
            av_require(av_dict_set(
                           options.put(), "frag_duration",
                           std::to_string(static_cast<int64_t>(segment_duration.count()) * 1000)
                               .c_str(),
                           0),
                       "set audio fragment duration");
        }
        int rc = avformat_write_header(out, options.put());
        av_require(rc, mpegts ? "write MPEG-TS header" : "write fragmented MP4 header");
        avio_flush(out->pb);

        std::map<int, StreamPipeline*> by_input;
        for (auto& pipe : pipelines) by_input[pipe->input_index] = pipe.get();

        auto packet = make_av_packet();
        auto encoded = make_av_packet();
        auto decoded = make_av_frame();
        // The delayed moov must be flushed before the first planned boundary, not on
        // it; MPEG-TS has none. See the early flush below.
        bool moov_flushed = mpegts;
        const auto all_streams_started = [&pipelines] {
            return std::all_of(pipelines.begin(), pipelines.end(),
                               [](const auto& p) { return p->output_started; });
        };

        try {
            while (!cancelled.load() && (rc = av_read_frame(in, packet.get())) >= 0) {
                auto it = by_input.find(packet->stream_index);
                if (it == by_input.end()) {
                    av_packet_unref(packet.get());
                    continue;
                }
                auto& pipe = *it->second;
                const auto origin = av_rescale_q(seek_target_us, AV_TIME_BASE_Q,
                                                 pipe.input_stream->time_base);
                if (pipe.transform == MediaTransform::copy) {
                    const auto presentation = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
                    if (presentation != AV_NOPTS_VALUE && presentation < origin) {
                        av_packet_unref(packet.get());
                        continue;
                    }
                    const auto repairs_before = pipe.copy_timestamps.repair_count();
                    prepare_copy_packet(pipe, packet.get(), origin);
                    if (!pipe.copy_repair_reported && pipe.copy_timestamps.repair_count() != repairs_before) {
                        pipe.copy_repair_reported = true;
                        Log::warn("libav remux repairing packet timestamps media=" + source.media_id +
                                  " stream=" + std::to_string(pipe.input_index));
                    }
                    if (pipe.type == MediaStreamType::video) {
                        auto seconds = packet->pts == AV_NOPTS_VALUE ? 0.0 :
                                           packet->pts * av_q2d(pipe.output_stream->time_base);
                        if ((packet->flags & AV_PKT_FLAG_KEY) != 0 && cuts.before_keyframe(seconds)) {
                            const auto before = writer.published();
                            cut_fragment(out);
                            // Writing the delayed moov produces no moof; the buffered media joins the
                            // next fragment, so carry the boundary.
                            if (writer.published() == before) writer.carry_boundary();
                        }
                    }
                    write_mux_packet(out, packet.get());
                    pipe.output_started = true;
                    if (presentation != AV_NOPTS_VALUE) {
                        const auto us = av_rescale_q(presentation - origin, pipe.input_stream->time_base,
                                                     AV_TIME_BASE_Q);
                        if (us > progress.output_media_us.load(std::memory_order_relaxed)) {
                            progress.output_media_us.store(us, std::memory_order_relaxed);
                            progress.moved();
                        }
                    }
                } else {
                    if (packet->pts != AV_NOPTS_VALUE) packet->pts -= origin;
                    if (packet->dts != AV_NOPTS_VALUE) packet->dts -= origin;
                    av_require(avcodec_send_packet(pipe.decoder, packet.get()), "send packet to decoder");
                    while (true) {
                        rc = avcodec_receive_frame(pipe.decoder, decoded.get());
                        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
                        av_require(rc, "receive decoded frame");
                        if (pipe.type == MediaStreamType::video) {
                            // Timestamps are origin-relative, so frames before zero are pre-roll: the
                            // first gives the keyframe distance, later ones the decoder's progress.
                            const auto frame_pts = decoded->best_effort_timestamp != AV_NOPTS_VALUE
                                                       ? decoded->best_effort_timestamp
                                                       : decoded->pts;
                            if (frame_pts != AV_NOPTS_VALUE && frame_pts < 0) {
                                const auto behind = av_rescale_q(-frame_pts, pipe.input_stream->time_base,
                                                                 AV_TIME_BASE_Q);
                                if (progress.preroll_total_us.load(std::memory_order_relaxed) < 0)
                                    progress.preroll_total_us.store(behind, std::memory_order_relaxed);
                                progress.preroll_decoded_us.store(
                                    progress.preroll_total_us.load(std::memory_order_relaxed) - behind,
                                    std::memory_order_relaxed);
                                progress.moved();
                            }
                            encode_video_frame(pipe, out, decoded.get(), encoded.get(), cuts);
                        }
                        else
                            process_audio_frame(pipe, out, decoded.get(), encoded.get());
                        av_frame_unref(decoded.get());
                    }
                }
                if (!moov_flushed && all_streams_started()) {
                    // Every stream has muxed a packet, so the (E-)AC-3 sample entry can be filled
                    // and the moov written now, before any real boundary; spending a real
                    // boundary on it would double the first fragment.
                    moov_flushed = true;
                    const auto before = writer.published();
                    cut_fragment(out);
                    if (writer.published() != before)
                        Log::warn("libav early moov flush produced a fragment media=" +
                                  source.media_id);
                }
                av_packet_unref(packet.get());
            }
            if (rc < 0 && rc != AVERROR_EOF && !cancelled.load())
                av_require(rc, "read media packet");

            if (!cancelled.load()) {
                for (const auto& pipe : pipelines) {
                    if (pipe->transform != MediaTransform::copy || !pipe->copy_timestamps.repair_count()) continue;
                    const auto& ts = pipe->copy_timestamps;
                    Log::info("libav remux timestamp repairs media=" + source.media_id +
                              " stream=" + std::to_string(pipe->input_index) +
                              " missing_pts=" + std::to_string(ts.missing_pts) +
                              " missing_dts=" + std::to_string(ts.missing_dts) +
                              " nonmonotonic_dts=" + std::to_string(ts.nonmonotonic_dts) +
                              " pts_before_dts=" + std::to_string(ts.pts_before_dts));
                }
                for (auto& pipe : pipelines)
                    if (pipe->transform == MediaTransform::transcode)
                        flush_decoder(*pipe, out, encoded.get(), cuts);
                for (auto& pipe : pipelines)
                    if (pipe->transform == MediaTransform::transcode)
                        flush_encoder(*pipe, out, encoded.get());
                av_require(av_write_trailer(out), mpegts ? "write MPEG-TS trailer"
                                                         : "write fragmented MP4 trailer");
                avio_flush(out->pb);
                writer.finish();
            }
        } catch (...) {
            throw;
        }
    } catch (...) {
        throw;
    }
}

class LibavSession final : public MediaEngineSession {
    MediaSource source_;
    HlsVodPlan vod_plan_;
    std::chrono::milliseconds segment_duration_;
    uint64_t probe_bytes_{};
    std::chrono::milliseconds analyze_duration_{};
    std::chrono::milliseconds startup_timeout_{};
    size_t video_decoder_threads_{};
    size_t video_encoder_threads_{};
    std::shared_ptr<MediaSegmentStore> store_;
    std::jthread worker_;
    std::atomic_bool cancelled_{};
    std::atomic_bool running_{true};
    std::atomic_int exit_code_{-1};
    mutable Mutex diagnostics_mutex_;
    std::string diagnostics_ MACHA_GUARDED_BY(diagnostics_mutex_);
    MediaStartProgress progress_;

    void run(std::stop_token stop) {
        try {
            if (stop.stop_requested()) cancelled_.store(true);
            run_pipeline(source_, vod_plan_, segment_duration_, store_, cancelled_,
                         probe_bytes_, analyze_duration_, startup_timeout_,
                         video_decoder_threads_, video_encoder_threads_, progress_);
            if (cancelled_.load()) {
                exit_code_.store(0);
            } else {
                store_->finish();
                auto state = store_->snapshot();
                if (!state.error.empty()) {
                    Lock lock(diagnostics_mutex_);
                    diagnostics_ = state.error;
                    exit_code_.store(1);
                } else {
                    exit_code_.store(0);
                }
            }
        } catch (const std::exception& e) {
            if (!cancelled_.load()) {
                {
                    Lock lock(diagnostics_mutex_);
                    diagnostics_ = e.what();
                }
                store_->fail(e.what());
                Log::warn("libav playback pipeline failed for " + source_.media_id + ": " + e.what());
                exit_code_.store(1);
            } else {
                exit_code_.store(0);
            }
        }
        running_.store(false);
    }

  public:
    LibavSession(MediaSource source, HlsVodPlan vod_plan, std::chrono::milliseconds segment_duration,
                 size_t max_ahead_segments, uint64_t memory_limit,
                 std::filesystem::path spill_directory, uint64_t probe_bytes,
                 std::chrono::milliseconds analyze_duration,
                 std::chrono::milliseconds startup_timeout, size_t video_decoder_threads,
                 size_t video_encoder_threads)
        : source_(std::move(source)), vod_plan_(std::move(vod_plan)), segment_duration_(segment_duration),
          probe_bytes_(probe_bytes), analyze_duration_(analyze_duration),
          startup_timeout_(startup_timeout), video_decoder_threads_(video_decoder_threads),
          video_encoder_threads_(video_encoder_threads),
          store_(std::make_shared<MediaSegmentStore>(max_ahead_segments, memory_limit,
                                                     std::move(spill_directory), segment_duration,
                                                     vod_plan_.segment_durations,
                                                     vod_plan_.playback.container)) {
        worker_ = std::jthread([this](std::stop_token stop) {
            run_supervised_once("media-engine-session", [this, stop] { run(stop); });
        });
    }

    ~LibavSession() override { stop(); }
    bool running() const override { return running_.load(); }
    std::optional<int> exit_code() const override {
        auto code = exit_code_.load();
        return code < 0 ? std::optional<int>{} : std::optional<int>{code};
    }
    std::string diagnostics() const override {
        Lock lock(diagnostics_mutex_);
        return diagnostics_;
    }
    std::shared_ptr<MediaSegmentStore> segments() const override { return store_; }
    const MediaStartProgress* start_progress() const override { return &progress_; }
    void note_segment_requested(uint64_t index) override { store_->note_requested(index); }
    void stop() override {
        cancelled_.store(true);
        store_->cancel();
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
        running_.store(false);
    }
};

class LibavMediaEngine final : public MediaEngine {
    StreamingConfig config_;
    MediaEngineStatus status_;

  public:
    explicit LibavMediaEngine(StreamingConfig config) : config_(std::move(config)) {
        status_.available = true;
        status_.backend = "libav";
        status_.version = av_version_info();
        status_.h264_encoder = h264_encoder() != nullptr;
        status_.aac_encoder = avcodec_find_encoder(AV_CODEC_ID_AAC) != nullptr;
        status_.video_decoder_threads = config_.video_decoder_threads;
    }

    MediaEngineStatus status() const override { return status_; }

    MediaProbeResult probe(const MediaSource& source, std::chrono::milliseconds timeout) override {
        auto started = Clock::now();
        if (timeout.count() <= 0) timeout = config_.probe_timeout;
        Log::debug("playback probe begin media=" + source.media_id + " size=" + std::to_string(source.size));
        InputContext input(source, MediaReadPurpose::probe, source.cancelled.get(), config_.probe_bytes,
                           config_.probe_analyze_duration, timeout);
        auto* format = input.get();
        auto probe_rc = avformat_find_stream_info(format, nullptr);
        if (probe_rc < 0) {
            if (input.timed_out())
                throw MediaError(MediaFailure::timed_out,
                                 "media probe timed out after " +
                                     std::to_string(timeout.count()) + " ms");
            if (!input.read_error().empty())
                throw MediaError(MediaFailure::unreadable,
                                 "read media: " + input.read_error());
            throw MediaError(MediaFailure::unsupported,
                             "read stream information: " + av_error(probe_rc));
        }

        MediaProbeResult result;
        if (format->iformat && format->iformat->name) result.format = format->iformat->name;
        if (format->duration != AV_NOPTS_VALUE && format->duration > 0)
            result.duration_seconds = static_cast<double>(format->duration) / AV_TIME_BASE;
        if (format->bit_rate > 0) result.bitrate = static_cast<uint64_t>(format->bit_rate);
        for (unsigned i = 0; i < format->nb_streams; ++i) {
            auto* stream = format->streams[i];
            auto* par = stream->codecpar;
            MediaStreamInfo info;
            info.index = static_cast<int>(i);
            info.attached_picture = (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
            info.type = info.attached_picture ? MediaStreamType::other : stream_type(par->codec_type);
            info.codec = avcodec_get_name(par->codec_id);
            if (const char* profile = avcodec_profile_name(par->codec_id, par->profile)) info.profile = profile;
            info.language = stream_language(stream);
            info.width = par->width;
            info.height = par->height;
            info.channels = par->ch_layout.nb_channels;
            info.sample_rate = par->sample_rate;
            info.bit_depth = par->bits_per_raw_sample;
            if (info.bit_depth <= 0 && par->codec_type == AVMEDIA_TYPE_VIDEO && par->format >= 0) {
                // Matroska omits bits_per_raw_sample for HEVC; derive depth from the pixel
                // format (yuv420p10le -> 10).
                if (const auto* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(par->format));
                    desc && desc->nb_components > 0)
                    info.bit_depth = desc->comp[0].depth;
            }
            info.level = par->level > 0 ? par->level : 0;
            if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
                for (int side = 0; side < par->nb_coded_side_data; ++side) {
                    const auto& entry = par->coded_side_data[side];
                    if (entry.type != AV_PKT_DATA_DOVI_CONF ||
                        entry.size < static_cast<size_t>(sizeof(AVDOVIDecoderConfigurationRecord)))
                        continue;
                    const auto* dovi =
                        reinterpret_cast<const AVDOVIDecoderConfigurationRecord*>(entry.data);
                    info.dolby_vision_profile = dovi->dv_profile;
                    info.dolby_vision_compatibility = dovi->dv_bl_signal_compatibility_id;
                }
            }
            if (par->codec_type == AVMEDIA_TYPE_VIDEO && par->color_trc != AVCOL_TRC_UNSPECIFIED)
                if (const char* transfer = av_color_transfer_name(par->color_trc))
                    info.color_transfer = transfer;
            if (par->bit_rate > 0) info.bitrate = static_cast<uint64_t>(par->bit_rate);
            info.default_stream = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
            info.forced = (stream->disposition & AV_DISPOSITION_FORCED) != 0;
            result.streams.push_back(std::move(info));
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        Log::debug("playback probe end media=" + source.media_id + " elapsed_ms=" + std::to_string(elapsed) +
                   " streams=" + std::to_string(result.streams.size()));
        return result;
    }

    std::optional<MediaKeyframeIndex> keyframe_index(const MediaSource& source,
                                                     std::chrono::milliseconds timeout) override {
        if (timeout.count() <= 0) timeout = config_.probe_timeout;
        InputContext input(source, MediaReadPurpose::probe, source.cancelled.get(), config_.probe_bytes,
                           config_.probe_analyze_duration, timeout);
        auto* format = input.get();
        const auto rc = avformat_find_stream_info(format, nullptr);
        if (rc < 0) {
            if (input.timed_out())
                throw MediaError(MediaFailure::timed_out, "keyframe index timed out reading stream information");
            if (!input.read_error().empty())
                throw MediaError(MediaFailure::unreadable, "read media: " + input.read_error());
            throw MediaError(MediaFailure::unsupported, "read stream information: " + av_error(rc));
        }
        const std::string format_name = format->iformat && format->iformat->name ? format->iformat->name : "";
        MediaKeyframeIndex out;
        out.container = container_for_format(format_name, source.logical_path);
        if (out.container == "mp4")
            out.exact_offsets = true;
        else if (out.container != "matroska" && out.container != "webm")
            return std::nullopt;
        const int video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (!out.exact_offsets && video >= 0) {
            materialise_deferred_seek_index(format, video, 0.0);
            if (input.timed_out())
                throw MediaError(MediaFailure::timed_out, "keyframe index timed out loading the Cues");
        }
        out.size_bytes = source.size;
        if (format->duration != AV_NOPTS_VALUE && format->duration > 0)
            out.duration_ms = format->duration / 1000;
        const int64_t start_us = format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time;
        for (unsigned i = 0; i < format->nb_streams; ++i) {
            auto* stream = format->streams[i];
            const auto type = stream->codecpar->codec_type;
            const bool picture = (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
            if (picture || (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO)) continue;
            MediaKeyframeIndex::Stream indexed;
            indexed.index = static_cast<int>(i);
            indexed.type = stream_type(type);
            indexed.codec = avcodec_get_name(stream->codecpar->codec_id);
            std::optional<int64_t> last_kept_ms;
            const int entries = avformat_index_get_entries_count(stream);
            for (int e = 0; e < entries; ++e) {
                const auto* entry = avformat_index_get_entry(stream, e);
                if (!entry || entry->timestamp == AV_NOPTS_VALUE || entry->pos < 0) continue;
                if (type == AVMEDIA_TYPE_VIDEO && !(entry->flags & AVINDEX_KEYFRAME)) continue;
                const int64_t us = av_rescale_q(entry->timestamp, stream->time_base, AV_TIME_BASE_Q);
                const int64_t ms = std::max<int64_t>(0, (us - start_us) / 1000);
                // Every audio sample is a sync point; one per second suffices.
                if (type == AVMEDIA_TYPE_AUDIO && last_kept_ms && ms < *last_kept_ms + 1000) continue;
                indexed.entries.emplace_back(ms, static_cast<uint64_t>(entry->pos));
                last_kept_ms = ms;
            }
            std::stable_sort(indexed.entries.begin(), indexed.entries.end(),
                             [](const auto& a, const auto& b) { return a.second < b.second; });
            out.streams.push_back(std::move(indexed));
        }
        return out;
    }

    HlsVodPlan prepare_hls_vod(const MediaSource& source, const PlaybackPlan& requested,
                               double source_duration_seconds,
                               std::chrono::milliseconds segment_duration,
                               bool allow_video_transcode_fallback,
                               std::chrono::milliseconds timeout) override {
        if (timeout.count() <= 0) timeout = config_.probe_timeout;
        if (!(source_duration_seconds > 0.001))
            throw std::runtime_error("source duration is unavailable for VOD planning");

        HlsVodPlan result;
        result.playback = requested;
        result.source_duration_seconds = source_duration_seconds;
        const double target = std::max(0.001, segment_duration.count() / 1000.0);
        result.seek_segment_seconds = target;
        // The honoured request in integer ms. Every baseline and offset derives from
        // it, so the seek invariant is exact.
        const int64_t requested_ms =
            media_vod::clamp_seek_ms(requested.seek.count(), source_duration_seconds);
        const double requested_seek = requested_ms / 1000.0;
        result.playback.seek_requested = std::chrono::milliseconds(requested_ms);

        InputContext input(source, MediaReadPurpose::probe, source.cancelled.get(), config_.probe_bytes,
                           config_.probe_analyze_duration, timeout);
        auto* format = input.get();
        auto rc = avformat_find_stream_info(format, nullptr);
        if (rc < 0 && input.timed_out())
            throw MediaError(MediaFailure::timed_out,
                             "VOD planning timed out while reading stream information");
        av_require(rc, "read stream information for VOD planning");

        if (result.playback.video != MediaTransform::omit &&
            result.playback.video_stream >= 0 &&
            result.playback.video_stream < static_cast<int>(format->nb_streams)) {
            auto* stream = format->streams[result.playback.video_stream];
            if (result.playback.video == MediaTransform::copy) {
                materialise_deferred_seek_index(format, result.playback.video_stream, requested_seek);
                if (input.timed_out())
                    throw MediaError(MediaFailure::timed_out,
                                     "VOD planning timed out while loading video seek index");
                const auto index_keyframes =
                    video_keyframe_seconds(format, result.playback.video_stream);
                std::optional<media_vod::IndexedPlan> indexed;
                if (!index_keyframes.empty())
                    indexed = media_vod::indexed_plan(index_keyframes, source_duration_seconds,
                                                      requested_ms, target);
                if (indexed) {
                    result.video_random_access_points = index_keyframes;
                    result.segment_durations = std::move(indexed->segment_durations);
                    result.playback.seek = std::chrono::milliseconds(indexed->seek_ms);
                    result.playback.seek_offset = std::chrono::milliseconds(indexed->seek_offset_ms);
                    // Index shape behind a working plan. Gaps between indexed entries bound the
                    // true GOP from above; offsets well below them mean sparse Cues, not a long GOP.
                    Log::debug("media VOD planner accepted remux keyframe index media=" +
                               source.media_id + " entries=" +
                               std::to_string(avformat_index_get_entries_count(stream)) +
                               index_density_log(index_keyframes, source_duration_seconds) +
                               " seek_ms=" + std::to_string(indexed->seek_ms) +
                               " seek_offset_ms=" + std::to_string(indexed->seek_offset_ms) +
                               " seek_requested_ms=" + std::to_string(indexed->seek_requested_ms));
                } else {
                    // Name the reason: entry count and keyframe gaps (tail included), to tell a
                    // partial index from a long-GOP encode.
                    Log::debug("media VOD planner rejected unusable remux keyframe index media=" +
                               source.media_id + " entries=" +
                               std::to_string(avformat_index_get_entries_count(stream)) +
                               index_density_log(index_keyframes, source_duration_seconds));
                    if (!allow_video_transcode_fallback || !status_.h264_encoder)
                        throw std::runtime_error(
                            "remux VOD requires a usable video keyframe index; H.264 fallback is not permitted or unavailable");
                    result.playback.video = MediaTransform::transcode;
                    result.playback.video_codec = "h264";
                    result.playback.mode = PlaybackMode::transcode;
                    result.video_random_access_points.clear();
                    Log::info("media VOD planner falling back from remux to video transcode media=" +
                              source.media_id + " reason=unusable-keyframe-index");
                }
            }

            if (result.playback.video == MediaTransform::transcode) {
                auto frame_rate = av_guess_frame_rate(format, stream, nullptr);
                double segment = target;
                if (frame_rate.num > 0 && frame_rate.den > 0) {
                    const double fps = av_q2d(frame_rate);
                    const int gop = std::max(1, static_cast<int>(std::llround(fps * target)));
                    segment = gop / fps;
                }
                result.seek_segment_seconds = segment;
                // The encoder can start on any frame: the seek is exactly the request, offset
                // zero. run_pipeline pays the pre-roll from the preceding keyframe; a client
                // wanting a cheap aligned seek asks for a keyframe position.
                // video_random_access_points is still collected for reseek_hls_vod's fast
                // path and the density diagnostic below.
                materialise_deferred_seek_index(format, result.playback.video_stream, requested_seek);
                if (input.timed_out())
                    throw MediaError(MediaFailure::timed_out,
                                     "VOD planning timed out while loading video seek index");
                result.video_random_access_points =
                    video_keyframe_seconds(format, result.playback.video_stream);
                result.segment_durations =
                    fixed_vod_durations(source_duration_seconds, requested_seek, segment);
                result.playback.seek = std::chrono::milliseconds(requested_ms);
                result.playback.seek_offset = {};
                Log::debug("media VOD planner transcode keyframe index media=" + source.media_id +
                           index_density_log(result.video_random_access_points,
                                             source_duration_seconds) +
                           " seek_ms=" + std::to_string(requested_ms) + " seek_offset_ms=0");
            }
        } else {
            result.segment_durations =
                fixed_vod_durations(source_duration_seconds, requested_seek, target);
            result.playback.seek = std::chrono::milliseconds(requested_ms);
            result.playback.seek_offset = {};
        }

        if (result.segment_durations.empty())
            throw std::runtime_error("VOD planning produced no media segments");
        result.reusable_seek = true;
        Log::debug("media VOD plan media=" + source.media_id +
                   " mode=" + playback_mode_name(result.playback.mode) +
                   " seek_ms=" + std::to_string(result.playback.seek.count()) +
                   " seek_offset_ms=" + std::to_string(result.playback.seek_offset.count()) +
                   " seek_requested_ms=" + std::to_string(result.playback.seek_requested.count()) +
                   " segments=" + std::to_string(result.segment_durations.size()));
        return result;
    }

    std::unique_ptr<MediaEngineSession> start_hls(
        const MediaSource& source, const HlsVodPlan& vod_plan, std::chrono::milliseconds segment_duration,
        size_t max_ahead_segments, uint64_t segment_memory_bytes,
        const std::filesystem::path& spill_directory) override {
        if (!status_.available) throw std::runtime_error("libav media engine is unavailable");
        const auto& plan = vod_plan.playback;
        if (plan.video == MediaTransform::transcode && !status_.h264_encoder)
            throw std::runtime_error("H.264 encoder is unavailable");
        if (plan.audio == MediaTransform::transcode && !status_.aac_encoder)
            throw std::runtime_error("AAC encoder is unavailable");
        Log::debug("media engine starting libav VOD pipeline media=" + source.media_id +
                   " mode=" + playback_mode_name(plan.mode) +
                   " segments=" + std::to_string(vod_plan.segment_durations.size()));
        return std::make_unique<LibavSession>(source, vod_plan, segment_duration, max_ahead_segments,
                                              segment_memory_bytes, spill_directory,
                                              config_.probe_bytes, config_.probe_analyze_duration,
                                              config_.startup_timeout,
                                              config_.video_decoder_threads,
                                              config_.video_encoder_threads);
    }

    std::string extract_webvtt_segment(const MediaSource& source, int subtitle_stream,
                                       std::chrono::milliseconds range_start,
                                       std::chrono::milliseconds range_end,
                                       std::chrono::milliseconds timeline_origin) override {
        if (range_end <= range_start)
            throw std::invalid_argument("subtitle segment range must be non-empty");

        std::atomic_bool cancelled{};
        InputContext input(source, MediaReadPurpose::subtitle, &cancelled, config_.probe_bytes,
                           config_.probe_analyze_duration, config_.probe_timeout);
        auto* format = input.get();

        // The session already probed this source and chose a subtitle stream; use the
        // container headers and fall back to find_stream_info only when needed.
        auto stream_ready = [&] {
            return subtitle_stream >= 0 &&
                   subtitle_stream < static_cast<int>(format->nb_streams) &&
                   format->streams[subtitle_stream]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE &&
                   format->streams[subtitle_stream]->codecpar->codec_id != AV_CODEC_ID_NONE;
        };
        if (!stream_ready()) {
            auto probe_rc = avformat_find_stream_info(format, nullptr);
            if (probe_rc < 0 && input.timed_out())
                throw MediaError(MediaFailure::timed_out, "subtitle probe timed out after " +
                                         std::to_string(config_.probe_timeout.count()) + " ms");
            av_require(probe_rc, "read subtitle stream information");
        }
        if (!stream_ready())
            throw std::invalid_argument("selected subtitle stream does not exist");

        auto* stream = format->streams[subtitle_stream];
        const auto* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) throw std::runtime_error("subtitle decoder is unavailable");
        AvCodecContextOwner decoder(avcodec_alloc_context3(codec));
        if (!decoder) throw std::bad_alloc();
        try {
            av_require(avcodec_parameters_to_context(decoder.get(), stream->codecpar),
                       "copy subtitle decoder parameters");
            decoder->pkt_timebase = stream->time_base;
            av_require(avcodec_open2(decoder.get(), codec, nullptr), "open subtitle decoder");

            const int64_t input_start_us = format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time;
            if (range_start.count() > 0) {
                const auto target_us = input_start_us +
                    av_rescale_q(range_start.count(), AVRational{1, 1000}, AV_TIME_BASE_Q);
                const auto target = av_rescale_q(target_us, AV_TIME_BASE_Q, stream->time_base);
                av_require(avformat_seek_file(format, subtitle_stream,
                                             std::numeric_limits<int64_t>::min(), target,
                                             std::numeric_limits<int64_t>::max(),
                                             AVSEEK_FLAG_BACKWARD),
                           "seek subtitle stream");
                avcodec_flush_buffers(decoder.get());
            }

            std::ostringstream out;
            out << "WEBVTT\n\n";
            auto packet = make_av_packet();
            uint64_t cue = 0;
            int rc = 0;
            bool past_range = false;
            while (!past_range && (rc = av_read_frame(format, packet.get())) >= 0) {
                // A/V packets bound the timeline cursor across long gaps between cues, so an
                // empty subtitle segment does not scan to the next cue; the grace allows for
                // interleave.
                const auto packet_time = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
                if (packet_time != AV_NOPTS_VALUE && packet->stream_index >= 0 &&
                    packet->stream_index < static_cast<int>(format->nb_streams)) {
                    const auto* packet_stream = format->streams[packet->stream_index];
                    const auto packet_us = av_rescale_q(packet_time, packet_stream->time_base, AV_TIME_BASE_Q);
                    const auto packet_ms = av_rescale_q(packet_us - input_start_us, AV_TIME_BASE_Q,
                                                        AVRational{1, 1000});
                    if (packet_ms >= range_end.count() + 1500) {
                        av_packet_unref(packet.get());
                        past_range = true;
                        break;
                    }
                }

                if (packet->stream_index != subtitle_stream) {
                    av_packet_unref(packet.get());
                    continue;
                }

                const auto packet_pts = packet->pts;
                AVSubtitle subtitle{};
                AvSubtitleOwner subtitle_owner(subtitle);
                int got = 0;
                rc = avcodec_decode_subtitle2(decoder.get(), &subtitle, &got, packet.get());
                av_packet_unref(packet.get());
                av_require(rc, "decode subtitle");
                if (!got) continue;

                int64_t base_ms = 0;
                if (subtitle.pts != AV_NOPTS_VALUE) {
                    base_ms = av_rescale_q(subtitle.pts - input_start_us, AV_TIME_BASE_Q,
                                           AVRational{1, 1000});
                } else if (packet_pts != AV_NOPTS_VALUE) {
                    const auto packet_us = av_rescale_q(packet_pts, stream->time_base, AV_TIME_BASE_Q);
                    base_ms = av_rescale_q(packet_us - input_start_us, AV_TIME_BASE_Q,
                                           AVRational{1, 1000});
                }
                auto begin_source = base_ms + subtitle.start_display_time;
                auto end_source = base_ms + subtitle.end_display_time;
                if (end_source <= begin_source) end_source = begin_source + 2000;

                // A cue belongs to the segment it starts in. The web client keeps the
                // preceding segment mounted, so cues may span a boundary without duplication.
                if (begin_source >= range_end.count()) {
                    past_range = true;
                    break;
                }
                if (begin_source >= range_start.count()) {
                    std::string text;
                    for (unsigned i = 0; i < subtitle.num_rects; ++i) {
                        auto* rect = subtitle.rects[i];
                        std::string part;
                        if (rect->text) part = rect->text;
                        else if (rect->ass) part = plain_ass_subtitle_text(rect->ass);
                        if (!part.empty()) {
                            if (!text.empty()) text.push_back('\n');
                            text += part;
                        }
                    }
                    if (!text.empty()) {
                        const auto begin = begin_source - timeline_origin.count();
                        const auto end = end_source - timeline_origin.count();
                        if (end > 0) {
                            out << ++cue << '\n' << format_vtt_time(begin) << " --> "
                                << format_vtt_time(end) << '\n' << text << "\n\n";
                        }
                    }
                }
            }
            if (rc < 0 && rc != AVERROR_EOF) av_require(rc, "read subtitle packets");
            return out.str();
        } catch (...) {
            throw;
        }
    }

};

std::unique_ptr<MediaEngine> make_libav_media_engine_impl(const StreamingConfig& config) {
    return std::make_unique<LibavMediaEngine>(config);
}

// Registers this engine into macha_core's factory slot before main().
struct MediaEngineRegistration {
    MediaEngineRegistration() { set_media_engine_factory(make_libav_media_engine_impl); }
};
const MediaEngineRegistration media_engine_registration;

} // namespace

} // namespace macha
