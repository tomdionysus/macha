// SPDX-License-Identifier: GPL-3.0-or-later
#include "playback.hpp"
#include "diagnostics.hpp"

#include "crypto.hpp"
#include "segment_holds.hpp"
#include "json.hpp"
#include "log.hpp"
#include "supervised.hpp"
#include "macha_version.hpp"
#include "media_containers.hpp"
#include "media_information.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cmath>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <malloc.h>
#endif

namespace macha {
namespace {

bool release_free_process_heap_pages() noexcept {
#if defined(__linux__) && defined(__GLIBC__)
    return ::malloc_trim(0) != 0;
#else
    return false;
#endif
}

std::string hex_token(size_t bytes = 24) {
    static constexpr char alphabet[] = "0123456789abcdef";
    auto random = random_bytes(bytes);
    std::string out;
    out.reserve(random.size() * 2);
    for (auto byte : random) {
        out.push_back(alphabet[byte >> 4]);
        out.push_back(alphabet[byte & 0x0f]);
    }
    return out;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

// The container vocabulary lives in media_containers.hpp; what remains here
// is the two shapes playback asks it about -- a probe result and a stream.
std::string source_container(const MediaProbeResult& probe, std::string_view path) {
    return container_for_format(probe.format, path);
}

bool webvtt_subtitle_supported(const MediaStreamInfo& stream) {
    return stream.type == MediaStreamType::subtitle &&
           webvtt_subtitle_codec_supported(stream.codec);
}

struct ByteRange {
    uint64_t offset{};
    uint64_t length{};
    bool partial{};
};

// A well-formed, coherent instruction this node cannot carry out. The only
// current case is a stream copy into a container that cannot hold that codec.
// Distinct from invalid_argument because nothing is malformed: another node on
// another build may accept the identical request, and a transcode would
// succeed here. Reaches the client as 422 copy_not_supported with
// scope=node and alternative_may_succeed=true, so a recovering client asks a
// neighbour for the copy before giving the copy up.
class PlaybackCapabilityError final : public std::invalid_argument {
  public:
    using std::invalid_argument::invalid_argument;
};

// The account cap refuses differently from every other limit here, and the
// difference is not cosmetic. A client recovering from a refusal decides
// whether another node is worth trying; a node-wide or transcode limit is a
// property of this node, and an account cap is identical on every node in the
// cluster. Core walks on the former and must not on the latter -- charging
// every healthy node it passes turns one account at its limit into a cluster
// core believes is failing. So this carries its own code, and the limit and
// the current count, rather than sharing the generic resource_limit envelope.
class AccountSessionLimitError final : public std::runtime_error {
public:
    AccountSessionLimitError(size_t held, size_t limit)
        : std::runtime_error("account playback session limit reached: holding " +
                             std::to_string(held) + " of " + std::to_string(limit) +
                             " on this node"),
          held_(held), limit_(limit) {}
    size_t held() const noexcept { return held_; }
    size_t limit() const noexcept { return limit_; }

private:
    size_t held_;
    size_t limit_;
};

// The single parse of a stream URL, used by BOTH the router and the
// authentication exemption. They must never disagree: a path the exemption
// calls a stream request but the router sends somewhere else is an
// authentication bypass, and keeping two spellings of "is this a stream
// request" is exactly how that happens. The exemption used to be a bare
// starts_with on the old top-level prefix, which was safe only because the
// stream lived at a root of its own; nested under the session it is not.
struct StreamRoute {
    std::string_view session_id;
    std::string_view stream_path;
};

std::optional<StreamRoute> parse_stream_route(std::string_view path) {
    constexpr std::string_view prefix = "/api/v1/playback/sessions/";
    if (!path.starts_with(prefix)) return std::nullopt;
    const auto rest = path.substr(prefix.size());
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0) return std::nullopt;
    constexpr std::string_view segment = "stream/";
    const auto tail = rest.substr(slash + 1);
    if (!tail.starts_with(segment)) return std::nullopt;
    return StreamRoute{rest.substr(0, slash), tail.substr(segment.size())};
}

class ResourceLimitError final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class PlaybackStageError final : public std::runtime_error {
    std::string trace_;
    std::string stage_;
    // Set when the engine said why it could not read the source. The reason
    // travels out to the client unchanged; the server does not act on it.
    std::optional<MediaFailure> failure_;

  public:
    PlaybackStageError(std::string trace, std::string stage, std::string message,
                       std::optional<MediaFailure> failure = std::nullopt)
        : std::runtime_error(std::move(message)), trace_(std::move(trace)),
          stage_(std::move(stage)), failure_(failure) {}
    const std::string& trace() const noexcept { return trace_; }
    const std::string& stage() const noexcept { return stage_; }
    const std::optional<MediaFailure>& failure() const noexcept { return failure_; }
};

// Wraps a stage failure, carrying the engine's reason when there is one.
PlaybackStageError stage_error(std::string trace, std::string stage, const std::exception& error) {
    if (const auto* media = dynamic_cast<const MediaError*>(&error))
        return PlaybackStageError(std::move(trace), std::move(stage), media->what(),
                                  media->failure());
    return PlaybackStageError(std::move(trace), std::move(stage), error.what());
}

std::optional<ByteRange> parse_range(const HttpRequest& request, uint64_t size) {
    auto it = request.headers.find("range");
    if (it == request.headers.end()) return ByteRange{0, size, false};
    auto value = it->second;
    if (!value.starts_with("bytes=") || value.find(',') != std::string::npos || !size) return {};
    value.erase(0, 6);
    auto dash = value.find('-');
    if (dash == std::string::npos) return {};
    auto left = value.substr(0, dash);
    auto right = value.substr(dash + 1);
    uint64_t start = 0, end = size - 1;
    auto parse = [](const std::string& text, uint64_t& out) {
        if (text.empty()) return false;
        auto [finish, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
        return ec == std::errc{} && finish == text.data() + text.size();
    };
    if (left.empty()) {
        uint64_t suffix = 0;
        if (!parse(right, suffix) || !suffix) return {};
        if (suffix >= size) start = 0;
        else start = size - suffix;
    } else {
        if (!parse(left, start) || start >= size) return {};
        if (!right.empty() && (!parse(right, end) || end < start)) return {};
        end = std::min(end, size - 1);
    }
    return ByteRange{start, end - start + 1, true};
}

class LogicalBody final : public HttpBodySource {
    std::shared_ptr<ReadHandle> handle_;
    uint64_t base_{};
    uint64_t size_{};
  public:
    LogicalBody(std::shared_ptr<ReadHandle> handle, uint64_t base, uint64_t size)
        : handle_(std::move(handle)), base_(base), size_(size) {}
    uint64_t size() const override { return size_; }
    size_t read(uint64_t offset, std::span<uint8_t> destination) override {
        if (offset >= size_) return 0;
        auto wanted = static_cast<size_t>(std::min<uint64_t>(destination.size(), size_ - offset));
        return handle_->read(base_ + offset, destination.first(wanted));
    }
};


class LogicalMediaInput final : public MediaInput {
    std::shared_ptr<ReadHandle> handle_;
    uint64_t size_{};
  public:
    LogicalMediaInput(std::shared_ptr<ReadHandle> handle, uint64_t size)
        : handle_(std::move(handle)), size_(size) {}
    uint64_t size() const override { return size_; }
    size_t read(uint64_t offset, std::span<uint8_t> destination,
                Clock::time_point deadline, std::atomic_bool* cancelled) override {
        if (offset >= size_) return 0;
        auto wanted = static_cast<size_t>(std::min<uint64_t>(destination.size(), size_ - offset));
        return handle_->read(offset, destination.first(wanted), deadline, cancelled);
    }
};

class MemoryBody final : public HttpBodySource {
    std::shared_ptr<const Bytes> bytes_;
    uint64_t base_{};
    uint64_t size_{};
  public:
    MemoryBody(std::shared_ptr<const Bytes> bytes, uint64_t base, uint64_t size)
        : bytes_(std::move(bytes)), base_(base), size_(size) {}
    uint64_t size() const override { return size_; }
    size_t read(uint64_t offset, std::span<uint8_t> destination) override {
        if (offset >= size_) return 0;
        auto wanted = static_cast<size_t>(std::min<uint64_t>(destination.size(), size_ - offset));
        std::copy_n(bytes_->data() + base_ + offset, wanted, destination.data());
        return wanted;
    }
    // Already in memory for as long as this body lives: the server sends
    // straight from it.
    const uint8_t* resident() const noexcept override { return bytes_->data() + base_; }
};

HttpResponse ranged_response(const HttpRequest& request, uint64_t size, std::string mime,
                             std::function<std::shared_ptr<HttpBodySource>(uint64_t, uint64_t)> make_body) {
    auto range = parse_range(request, size);
    if (!range) {
        auto response = http_error(416, "bad_range", "requested byte range is not satisfiable");
        response.headers["Content-Range"] = "bytes */" + std::to_string(size);
        return response;
    }
    HttpResponse response;
    response.status = range->partial ? 206 : 200;
    response.content_type = std::move(mime);
    response.stream = make_body(range->offset, range->length);
    response.headers["Accept-Ranges"] = "bytes";
    if (range->partial) {
        response.headers["Content-Range"] = "bytes " + std::to_string(range->offset) + "-" +
                                            std::to_string(range->offset + range->length - 1) + "/" +
                                            std::to_string(size);
    }
    return response;
}

struct PlaybackPreferences {
    // Required. The server performs what it is asked for and never chooses:
    // "direct" (the source object over byte ranges), "remux" (copy the
    // streams into an HLS container) or "transcode" (re-encode them).
    std::string mode;
    // Per-stream overrides of that shorthand, "copy" or "transcode", so a
    // client can ask for any mixture (copy the video, re-encode the audio)
    // without the server inferring anything.
    std::optional<std::string> video;
    std::optional<std::string> audio;
    // HLS segment container: "fmp4" (default) or "mpegts".
    std::string container{"fmp4"};
    std::optional<int> max_height;
    std::optional<uint64_t> max_bitrate;
    std::optional<int> audio_stream;
    std::optional<int> subtitle_stream;
    std::string audio_language;
    std::string subtitle_language;

    bool operator==(const PlaybackPreferences&) const = default;
};

std::optional<int> optional_int(const Json* value) {
    if (!value || value->isNull()) return {};
    try { return static_cast<int>(value->asInt64()); } catch (...) {}
    try { return static_cast<int>(value->asUInt64()); } catch (...) {}
    return {};
}

std::optional<uint64_t> optional_u64(const Json* value) {
    if (!value || value->isNull()) return {};
    try { return value->asUInt64(); } catch (...) {}
    try {
        auto n = value->asInt64();
        if (n >= 0) return static_cast<uint64_t>(n);
    } catch (...) {}
    return {};
}

PlaybackPreferences parse_preferences(const Json* value, PlaybackPreferences current = {}) {
    if (!value || !value->isObject()) return current;
    if (auto mode = value->find("mode"); mode && mode->isString()) {
        current.mode = lower(mode->asString());
        // `mode` is the shorthand for the whole transform, so naming it
        // restates the transform: the per-stream and quality instructions
        // that belonged to the previous mode do not outlive it. An update
        // naming both sets both, since these are read after the mode.
        //
        // Without this an update naming only `mode` was refused for a
        // combination the server assembled itself out of the session's
        // history: a session created as a transcode with the video copied
        // answered `{"mode":"direct"}` with "direct copies every stream",
        // against a request the client never made. Because the chooser's
        // usual answer for this library is transcode-with-the-video-copied,
        // that was every session, and the Direct and Remux controls failed
        // for viewers on most of the library (2026-09-08). It is the same
        // rule the session already applied when asking what a different mode
        // would do; see without_mode_overrides.
        current.video.reset();
        current.audio.reset();
        current.max_height.reset();
        current.max_bitrate.reset();
    }
    const auto transform_instruction = [](const Json* v, const char* what) -> std::optional<std::string> {
        if (!v || v->isNull()) return std::nullopt;
        if (!v->isString()) throw std::invalid_argument(std::string(what) + " must be a string");
        auto value = lower(v->asString());
        if (value != "copy" && value != "transcode")
            throw std::invalid_argument(std::string(what) + " must be copy or transcode");
        return value;
    };
    if (auto v = value->find("video")) current.video = transform_instruction(v, "preferences.video");
    if (auto v = value->find("audio")) current.audio = transform_instruction(v, "preferences.audio");
    if (auto v = value->find("container"); v && v->isString()) {
        current.container = lower(v->asString());
        if (current.container != "fmp4" && current.container != "mpegts")
            throw std::invalid_argument("preferences.container must be fmp4 or mpegts");
    }
    if (auto v = value->find("max_height")) current.max_height = optional_int(v);
    if (auto v = value->find("max_bitrate")) current.max_bitrate = optional_u64(v);
    if (auto v = value->find("audio_stream")) current.audio_stream = optional_int(v);
    if (auto v = value->find("subtitle_stream")) current.subtitle_stream = optional_int(v);
    if (auto v = value->find("audio_language"); v && v->isString()) current.audio_language = lower(v->asString());
    if (auto v = value->find("subtitle_language"); v && v->isString()) current.subtitle_language = lower(v->asString());
    if (current.max_height && *current.max_height <= 0) throw std::invalid_argument("preferences.max_height must be positive");
    if (current.max_bitrate && *current.max_bitrate == 0) throw std::invalid_argument("preferences.max_bitrate must be positive");
    if (current.audio_stream && *current.audio_stream < 0) throw std::invalid_argument("preferences.audio_stream must be non-negative");
    if (current.subtitle_stream && *current.subtitle_stream < 0) throw std::invalid_argument("preferences.subtitle_stream must be non-negative");
    return current;
}

const MediaStreamInfo* first_stream(const MediaProbeResult& probe, MediaStreamType type) {
    const MediaStreamInfo* first = nullptr;
    for (const auto& stream : probe.streams) {
        if (stream.attached_picture) continue;
        if (stream.type != type) continue;
        if (!first) first = &stream;
        if (stream.default_stream) return &stream;
    }
    return first;
}

const MediaStreamInfo* select_stream(const MediaProbeResult& probe, MediaStreamType type,
                                     const std::optional<int>& explicit_index,
                                     std::string_view language) {
    if (explicit_index) {
        for (const auto& stream : probe.streams)
            if (stream.type == type && stream.index == *explicit_index) return &stream;
        return nullptr;
    }
    if (!language.empty()) {
        for (const auto& stream : probe.streams)
            if (stream.type == type && lower(stream.language) == language) return &stream;
    }
    return first_stream(probe, type);
}

// Execute the client's instruction against the media's facts. The client's
// advertised capabilities are deliberately not a parameter: the server
// reports what the media is and performs what it is asked for, it does not
// choose (operator, 2026-09-07).
PlaybackPlan plan_for(const MediaProbeResult& probe, const PlaybackPreferences& prefs) {
    auto video = first_stream(probe, MediaStreamType::video);
    auto audio = select_stream(probe, MediaStreamType::audio, prefs.audio_stream, prefs.audio_language);
    const MediaStreamInfo* subtitle = nullptr;
    if (prefs.subtitle_stream || !prefs.subtitle_language.empty())
        subtitle = select_stream(probe, MediaStreamType::subtitle, prefs.subtitle_stream, prefs.subtitle_language);
    if (prefs.audio_stream && !audio) throw std::invalid_argument("requested audio stream does not exist");
    if (prefs.subtitle_stream && !subtitle) throw std::invalid_argument("requested subtitle stream does not exist");
    if (subtitle && !webvtt_subtitle_supported(*subtitle))
        throw std::invalid_argument("requested subtitle stream cannot be converted to WebVTT");
    if (!video && !audio) throw std::runtime_error("media contains no playable audio or video stream");

    PlaybackPlan plan;
    plan.video_stream = video ? video->index : -1;
    plan.audio_stream = audio ? audio->index : -1;
    plan.subtitle_stream = subtitle ? subtitle->index : -1;

    if (prefs.mode != "direct" && prefs.mode != "remux" && prefs.mode != "transcode")
        throw std::invalid_argument(
            "preferences.mode is required and must be direct, remux or transcode: the server "
            "reports what the media is and performs what it is asked for, it does not choose");
    plan.video = video ? MediaTransform::copy : MediaTransform::omit;
    plan.audio = audio ? MediaTransform::copy : MediaTransform::omit;
    plan.video_codec = video ? lower(video->codec) : std::string{};
    plan.audio_codec = audio ? lower(audio->codec) : std::string{};

    // Direct is the source object itself over byte ranges: no container
    // change and no re-encode. Asking for one alongside it describes something
    // direct is not doing, so it is refused rather than quietly ignored.
    if (prefs.mode == "direct") {
        if ((prefs.video && *prefs.video != "copy") || (prefs.audio && *prefs.audio != "copy"))
            throw std::invalid_argument(
                "direct serves the source file untouched and copies every stream: ask for "
                "mode=transcode to re-encode one");
        if (prefs.max_height || prefs.max_bitrate)
            throw std::invalid_argument(
                "direct serves the source file untouched: a quality instruction is a re-encode "
                "and requires mode=transcode");
        plan.mode = PlaybackMode::direct;
        return plan;
    }

    // HLS. The segment container is the client's instruction too; fMP4
    // unless it asked for MPEG-TS.
    plan.container = prefs.container == "mpegts" ? MediaContainer::mpegts : MediaContainer::fmp4;

    const auto source_video_codec = video ? lower(video->codec) : std::string{};
    const auto source_audio_codec = audio ? lower(audio->codec) : std::string{};
    // `mode` is the shorthand: remux copies both streams, transcode
    // re-encodes both. `preferences.video` / `preferences.audio` override
    // either one, which is how a client asks for the common mixture (copy
    // the video, re-encode the audio) without the server inferring anything.
    const bool transcode_shorthand = prefs.mode == "transcode";
    bool video_copy = !video || !transcode_shorthand;
    bool audio_copy = !audio || !transcode_shorthand;
    if (video && prefs.video) video_copy = *prefs.video == "copy";
    if (audio && prefs.audio) audio_copy = *prefs.audio == "copy";

    // A quality instruction is a re-encode by definition.
    if (video && prefs.max_height && video->height > *prefs.max_height) {
        plan.target_height = *prefs.max_height;
        if (video_copy && prefs.video && *prefs.video == "copy")
            throw std::invalid_argument(
                "preferences.video=copy cannot be combined with preferences.max_height below the "
                "source height");
        video_copy = false;
    }
    if (video && prefs.max_bitrate) {
        plan.target_video_bitrate = *prefs.max_bitrate;
        if (video_copy && prefs.video && *prefs.video == "copy")
            throw std::invalid_argument(
                "preferences.video=copy cannot be combined with preferences.max_bitrate");
        video_copy = false;
    }

    // The mode has to describe what is actually being done. remux repackages
    // and copies every stream; transcode re-encodes at least one and may copy
    // the other. A mode that names something it is not doing is refused, not
    // silently reinterpreted: until 0.32.20 a remux with a re-encoded stream
    // came back reported as a transcode, and a transcode with both streams
    // copied came back reported as a remux (2026-09-07).
    const bool re_encoding = (video && !video_copy) || (audio && !audio_copy);
    if (prefs.mode == "remux" && re_encoding)
        throw std::invalid_argument(
            "remux repackages and copies every stream: to re-encode one, ask for mode=transcode "
            "with video=copy or audio=copy for the stream that is being copied");
    if (prefs.mode == "transcode" && !re_encoding)
        throw std::invalid_argument(
            "transcode re-encodes at least one stream: to copy both into a new container, ask "
            "for mode=remux");

    // What the segment container can physically carry. This is a fact about
    // the media and the muxer, not about the client.
    if (video && video_copy && plan.container == MediaContainer::fmp4 &&
        !fmp4_video_copy_supported(source_video_codec))
        throw PlaybackCapabilityError("fragmented MP4 cannot carry a copied " + source_video_codec +
                                    " video stream; ask for preferences.video=transcode");
    if (audio && audio_copy && plan.container == MediaContainer::fmp4 &&
        !fmp4_audio_copy_supported(source_audio_codec))
        throw PlaybackCapabilityError("fragmented MP4 cannot carry a copied " + source_audio_codec +
                                    " audio stream; ask for preferences.audio=transcode");
    if (video && video_copy && plan.container == MediaContainer::mpegts &&
        !mpegts_video_copy_supported(source_video_codec))
        throw PlaybackCapabilityError("MPEG-TS cannot carry a copied " + source_video_codec +
                                    " video stream; ask for preferences.video=transcode");
    if (audio && audio_copy && plan.container == MediaContainer::mpegts &&
        !mpegts_audio_copy_supported(source_audio_codec))
        throw PlaybackCapabilityError("MPEG-TS cannot carry a copied " + source_audio_codec +
                                    " audio stream; ask for preferences.audio=transcode");

    plan.video = video ? (video_copy ? MediaTransform::copy : MediaTransform::transcode) : MediaTransform::omit;
    plan.audio = audio ? (audio_copy ? MediaTransform::copy : MediaTransform::transcode) : MediaTransform::omit;
    plan.video_codec = video ? (video_copy ? source_video_codec : "h264") : std::string{};
    plan.audio_codec = audio ? (audio_copy ? source_audio_codec : "aac") : std::string{};
    plan.mode = (plan.video == MediaTransform::transcode || plan.audio == MediaTransform::transcode)
                    ? PlaybackMode::transcode
                    : PlaybackMode::remux;
    return plan;
}

Json stream_json(const MediaStreamInfo& stream) {
    Json::Object out{{"index", stream.index},
                     {"type", media_stream_type_name(stream.type)},
                     {"codec", stream.codec},
                     {"profile", stream.profile},
                     {"language", stream.language},
                     {"default", stream.default_stream},
                     {"forced", stream.forced},
                     {"attached_picture", stream.attached_picture}};
    if (stream.width) out["width"] = stream.width;
    if (stream.height) out["height"] = stream.height;
    if (stream.channels) out["channels"] = stream.channels;
    if (stream.sample_rate) out["sample_rate"] = stream.sample_rate;
    if (stream.bit_depth) out["bit_depth"] = stream.bit_depth;
    if (stream.level) out["level"] = stream.level;
    if (!stream.color_transfer.empty()) out["color_transfer"] = stream.color_transfer;
    if (stream.dolby_vision_profile) {
        out["dolby_vision_profile"] = stream.dolby_vision_profile;
        out["dolby_vision_compatibility"] = stream.dolby_vision_compatibility;
    }
    if (stream.bitrate) out["bitrate"] = stream.bitrate;
    return Json(std::move(out));
}

const MediaStreamInfo* stream_at(const MediaProbeResult& probe, int index) {
    if (index < 0) return nullptr;
    for (const auto& stream : probe.streams)
        if (stream.index == index) return &stream;
    return nullptr;
}

std::string transform_name(MediaTransform transform) {
    switch (transform) {
    case MediaTransform::copy: return "copy";
    case MediaTransform::transcode: return "transcode";
    case MediaTransform::omit: return "omit";
    }
    return "omit";
}

Json preferences_json(const PlaybackPreferences& preferences) {
    Json::Object out{{"mode", preferences.mode},
                     {"video", preferences.video ? Json(*preferences.video) : Json(nullptr)},
                     {"audio", preferences.audio ? Json(*preferences.audio) : Json(nullptr)},
                     {"container", preferences.container},
                     {"max_height", preferences.max_height ? Json(*preferences.max_height) : Json(nullptr)},
                     {"max_bitrate", preferences.max_bitrate ? Json(*preferences.max_bitrate) : Json(nullptr)},
                     {"audio_stream", preferences.audio_stream ? Json(*preferences.audio_stream) : Json(nullptr)},
                     {"subtitle_stream", preferences.subtitle_stream ? Json(*preferences.subtitle_stream) : Json(nullptr)},
                     {"audio_language", preferences.audio_language},
                     {"subtitle_language", preferences.subtitle_language}};
    return Json(std::move(out));
}

Json output_json(const MediaProbeResult& probe, const PlaybackPlan& plan,
                 std::string_view source_format, std::string_view source_path) {
    const bool direct = plan.mode == PlaybackMode::direct;
    Json::Object out{{"format", direct ? std::string(source_format)
                                       : std::string(media_container_name(plan.container))},
                     // What the client is actually being handed, stated the
                     // same way the facts endpoint states a source container.
                     // A client can ask for mpegts; without this it has no way
                     // to see that it got it, and tonight is the argument
                     // against reading a request back as evidence (2026-09-07).
                     {"container", direct ? source_container(probe, source_path)
                                          : std::string(media_container_name(plan.container))}};

    if (const auto* video = stream_at(probe, plan.video_stream); video && plan.video != MediaTransform::omit) {
        Json::Object value{{"source_stream", video->index},
                           {"transform", transform_name(plan.video)},
                           {"codec", plan.video_codec}};
        int height = video->height;
        int width = video->width;
        if (plan.video == MediaTransform::transcode && plan.target_height && video->height > *plan.target_height) {
            height = std::max(2, *plan.target_height & ~1);
            if (video->width > 0 && video->height > 0) {
                width = static_cast<int>(std::llround(static_cast<double>(video->width) *
                                                      static_cast<double>(height) /
                                                      static_cast<double>(video->height)));
                width = std::max(2, width & ~1);
            }
        }
        if (width) value["width"] = width;
        if (height) value["height"] = height;
        if (plan.video == MediaTransform::copy) {
            if (!video->profile.empty()) value["profile"] = video->profile;
            if (video->bitrate) value["bitrate"] = video->bitrate;
            if (video->bit_depth) value["bit_depth"] = video->bit_depth;
            if (video->level) value["level"] = video->level;
            if (!video->color_transfer.empty()) value["color_transfer"] = video->color_transfer;
        } else if (plan.video == MediaTransform::transcode) {
            // What the encoder actually emits, so a client can tell a
            // downconverted PQ source from a gate that did nothing: 8-bit
            // 4:2:0 H.264 High with an SDR transfer.
            value["profile"] = "High";
            value["bit_depth"] = 8;
            value["color_transfer"] = "bt709";
            if (plan.target_video_bitrate) value["bitrate"] = *plan.target_video_bitrate;
        }
        out["video"] = Json(std::move(value));
    }

    if (const auto* audio = stream_at(probe, plan.audio_stream); audio && plan.audio != MediaTransform::omit) {
        Json::Object value{{"source_stream", audio->index},
                           {"transform", transform_name(plan.audio)},
                           {"codec", plan.audio_codec}};
        if (plan.audio == MediaTransform::transcode) {
            // A codec change is not a downmix. The encoder keeps the source's
            // channel layout, so report it rather than a stereo assumption.
            const auto channels = audio->channels > 0 ? audio->channels : 2;
            value["channels"] = channels;
            value["sample_rate"] = audio->sample_rate > 0 ? audio->sample_rate : 48000;
            value["bitrate"] = static_cast<uint64_t>(std::clamp(channels, 1, 8)) * 64000;
        } else {
            if (audio->channels) value["channels"] = audio->channels;
            if (audio->sample_rate) value["sample_rate"] = audio->sample_rate;
            if (audio->bit_depth) value["bit_depth"] = audio->bit_depth;
            if (audio->bitrate) value["bitrate"] = audio->bitrate;
            if (!audio->profile.empty()) value["profile"] = audio->profile;
        }
        out["audio"] = Json(std::move(value));
    }
    return Json(std::move(out));
}

} // namespace

struct PlaybackManager::Impl {
    struct LogicalViewerSession {
        mutable std::mutex operation_mutex;
        std::string client_key;
        bool video_transcode_entitled{};
        bool audio_transcode_entitled{};
    };

    struct SourceLease {
        std::string media_id;
        std::string path;
        FsEntry entry;
    };

    struct Session {
        std::string id;
        std::string token;
        std::string item_id;
        PlaybackPreferences preferences;
        MediaSource source;
        FsEntry source_entry;
        MediaProbeResult probe;
        PlaybackPlan plan;
        std::optional<HlsVodPlan> vod_plan;
        uint64_t generation{};
        std::filesystem::path generation_dir;
        mutable std::mutex pipeline_mutex;
        std::shared_ptr<MediaEngineSession> engine_session;
        std::string stream_url;
        struct SubtitleCache {
            std::mutex mutex;
            std::map<std::pair<int, uint64_t>, std::string> segments;
            std::deque<std::pair<int, uint64_t>> order;
            size_t bytes{};
            static constexpr size_t max_entries = 256;
            static constexpr size_t max_bytes = 4ULL * 1024 * 1024;
        };
        std::string subtitle_url;
        std::shared_ptr<SubtitleCache> subtitle_cache{std::make_shared<SubtitleCache>()};
        Clock::time_point touched{Clock::now()};
        Clock::time_point stream_touched{Clock::now()};
        // Has this session ever served a stream object -- playlist, fragment,
        // subtitle or direct body? stream_touched cannot answer that: it is
        // set at construction and reset by every start_pipeline(), so it says
        // "not recently", never "not ever". Set once and never cleared: a
        // session that has been used stays used across a seek or a quality
        // change, and keeps the full session_idle.
        bool stream_served{false};
        size_t active_stream_requests{};
        std::shared_ptr<LogicalViewerSession> logical_session;
        // The account this session is held against: what the collection
        // listing filters on and what the per-account cap counts. Sessions
        // stopped belonging to a bearer, so the owner has to be recorded
        // rather than re-derived from whoever is asking.
        std::string account;
    };

    FileSystem& fs;
    CatalogueManager& catalogue;
    StreamingConfig config;
    // Node-global: a fairness and memory bound across every session, not a
    // property of any one of them.
    SegmentHoldArbiter segment_holds{config.max_session_holds, config.max_concurrent_holds};
    // What a held segment request parks across its deferral: the admitted
    // hold, released when the request is finally answered or its connection
    // goes away, whichever comes first.
    struct HeldRequest {
        SegmentHoldArbiter::Hold hold;
        explicit HeldRequest(SegmentHoldArbiter::Hold admitted) : hold(std::move(admitted)) {}
    };
    std::shared_ptr<MediaEngine> engine;
    std::jthread cleanup_thread;
    std::jthread profile_publish_thread;
    mutable std::mutex mutex;
    std::condition_variable_any cleanup_cv;
    uint64_t cleanup_revision{};
    std::map<std::string, std::shared_ptr<Session>, std::less<>> sessions;
    // Client keys are advisory local reconciliation handles, never cluster
    // ownership. Weak values ensure an expired/deleted logical session leaves
    // no permanent server-side playback state.
    std::map<std::string, std::weak_ptr<LogicalViewerSession>, std::less<>> logical_sessions;
    // In-flight creations per account, so concurrent creates cannot race past
    // the cap. Guarded by `mutex`, emptied as each create settles.
    std::map<std::string, size_t, std::less<>> pending_by_account;
    std::map<std::string, MediaProbeResult, std::less<>> probe_cache;
    size_t probe_cache_bytes{};
    static constexpr size_t max_probe_cache_entries = 512;
    static constexpr size_t max_probe_cache_bytes = 8ULL * 1024 * 1024;
    struct ProbeFlight {
        std::mutex mutex;
        std::condition_variable cv;
        bool complete{};
        std::optional<MediaProbeResult> result;
        std::exception_ptr error;
    };
    std::map<std::string, std::shared_ptr<ProbeFlight>, std::less<>> probe_flights;
    std::mutex profile_publish_mutex;
    std::condition_variable_any profile_publish_cv;
    std::map<std::string, MediaProbeResult, std::less<>> pending_profile_publications;
    size_t pending_profile_publication_bytes{};
    static constexpr size_t max_pending_profile_publications = 128;
    static constexpr size_t max_pending_profile_publication_bytes = 4ULL * 1024 * 1024;
    std::map<std::string, HlsVodPlan, std::less<>> vod_plan_cache;
    std::deque<std::string> vod_plan_cache_order;
    static constexpr size_t max_vod_plan_cache_entries = 64;
    // Session/pipeline admission happens before a newly-created pipeline is
    // visible in `sessions`.  Reserve those slots explicitly so concurrent
    // POST/PATCH requests cannot all pass the same resource-limit check.
    size_t pending_sessions{};
    size_t reserved_video_transcodes{};
    size_t reserved_audio_transcodes{};
    uint64_t idle_pipelines_reclaimed{};
    uint64_t unused_sessions_reclaimed{};
    bool heap_reclaim_pending{};
    uint64_t heap_reclaim_requests{};
    uint64_t heap_reclaim_runs{};
    uint64_t heap_reclaim_successes{};
    bool started{};
    std::function<size_t(const std::vector<std::string>&)> request_media_profiles;
    MediaInformationService* media_information{};

    static size_t probe_resident_weight(const MediaProbeResult& probe) {
        size_t bytes = sizeof(probe) + probe.format.capacity();
        for (const auto& stream : probe.streams) {
            bytes += sizeof(stream) + stream.codec.capacity() + stream.profile.capacity() +
                     stream.language.capacity();
        }
        return bytes;
    }

    // Precondition: profile_publish_mutex is held. This retry cache is an
    // optimization, not durable state; a broken metadata publisher must not
    // turn successful playback probes into an unbounded process-lifetime owner.
    void queue_profile_publication(std::string media_id, MediaProbeResult probe) {
        const auto weight = probe_resident_weight(probe) + media_id.size();
        if (weight > max_pending_profile_publication_bytes)
            return;
        if (auto found = pending_profile_publications.find(media_id);
            found != pending_profile_publications.end()) {
            pending_profile_publication_bytes -=
                probe_resident_weight(found->second) + found->first.size();
            found->second = std::move(probe);
            pending_profile_publication_bytes += weight;
            return;
        }
        while (!pending_profile_publications.empty() &&
               (pending_profile_publications.size() >= max_pending_profile_publications ||
                pending_profile_publication_bytes >
                    max_pending_profile_publication_bytes - weight)) {
            auto victim = pending_profile_publications.begin();
            pending_profile_publication_bytes -=
                probe_resident_weight(victim->second) + victim->first.size();
            pending_profile_publications.erase(victim);
        }
        pending_profile_publications.emplace(std::move(media_id), std::move(probe));
        pending_profile_publication_bytes += weight;
    }

    // Precondition: mutex is held. Immutable identity makes entries valid, but
    // validity is not ownership: this process cache has a hard count and byte
    // lifetime independent of catalogue size and is safely repopulated.
    void cache_probe(std::string key, MediaProbeResult probe) {
        const auto weight = probe_resident_weight(probe);
        if (weight > max_probe_cache_bytes)
            return;
        if (auto found = probe_cache.find(key); found != probe_cache.end()) {
            probe_cache_bytes -= probe_resident_weight(found->second);
            found->second = std::move(probe);
            probe_cache_bytes += weight;
            return;
        }
        while (!probe_cache.empty() &&
               (probe_cache.size() >= max_probe_cache_entries ||
                probe_cache_bytes > max_probe_cache_bytes - weight)) {
            auto victim = probe_cache.begin();
            probe_cache_bytes -= probe_resident_weight(victim->second);
            probe_cache.erase(victim);
        }
        probe_cache.emplace(std::move(key), std::move(probe));
        probe_cache_bytes += weight;
    }
    struct IdempotentCreation {
        std::mutex mutex;
        std::condition_variable cv;
        std::string fingerprint;
        bool complete{};
        std::string session_id;
        std::exception_ptr error;
    };
    std::map<std::string, std::shared_ptr<IdempotentCreation>, std::less<>> idempotent_creations;

    Impl(FileSystem& filesystem, CatalogueManager& cat, CatalogueApiConfig api,
         StreamingConfig streaming, std::shared_ptr<MediaEngine> media_engine,
         std::function<size_t(const std::vector<std::string>&)> request_profiles,
         MediaInformationService* information)
        : fs(filesystem), catalogue(cat), config(std::move(streaming)),
          engine(media_engine ? std::move(media_engine) :
                (config.enabled ? make_libav_media_engine(config) : nullptr)),
          request_media_profiles(std::move(request_profiles)), media_information(information) {
        (void)api;
    }

    // A broken generation, as distinct from one that is merely not ready yet.
    //
    // 503, which is also what every intermediary emits when a service is
    // genuinely down -- and that is deliberate. A dead node and a broken
    // generation warrant the same conclusion from a client: this node cannot
    // serve me, go elsewhere. Sharing the status with infrastructure is
    // therefore harmless here, and it is what lets the hold have a status
    // nothing else on the path can produce.
    static HttpResponse stream_failed(std::string_view detail) {
        return http_error(503, "stream_failed", detail);
    }

    // The refusal: not absent, just not made yet. Never 404 -- the playlist
    // promises this object exists, a 404 invites an intermediary to cache the
    // miss, and some players treat it as terminal.
    //
    // 500, which is the wrong status by the letter of the spec and the right
    // one in practice. A client cannot read our JSON body on a fragment error:
    // hls.js's XHR loader surfaces only `{code: xhr.status, text:
    // xhr.statusText}`, the body is absent from the error event, and a header
    // is reachable only through the raw XMLHttpRequest -- undocumented
    // coupling that breaks if the default loader changes. So the status is the
    // discriminator, and a discriminator readable only as a status has to be a
    // status nothing else on the path can emit. 503 fails that test: every
    // proxy, tunnel and load balancer emits it when a service is down, so a
    // client classifying "503 means hold, stay on this node" would read a
    // genuinely dead node as a healthy one and never fail over -- a silent,
    // permanent stall. Misreading an infrastructure 500 as a hold costs a
    // pointless retry instead. The two faults are not symmetric, and this
    // picks the recoverable one. Both stay 5xx, because a 4xx stops hls.js
    // retrying at all.
    //
    // Retry-After and no-store keep an intermediary from turning a transient
    // answer into a durable one. hls.js ignores Retry-After on the fragment
    // path -- it reads that header only in its content-steering loader -- so
    // it is sent because it is correct, not because anything depends on it.
    static HttpResponse segment_not_ready(std::string_view session, uint64_t index,
                                          std::string_view reason) {
        Log::debug("playback stream refused session=" + std::string(session) +
                   " index=" + std::to_string(index) + " reason=" + std::string(reason));
        auto response = http_error(500, "segment_not_ready",
                                   "media is not ready yet, retry shortly", reason);
        response.headers["Retry-After"] = "1";
        response.headers["Cache-Control"] = "no-store";
        return response;
    }

    std::string public_stream_prefix(const Session& session) const {
        // The stream is a subresource of the session it belongs to, and the
        // token sits immediately before the part it authorises: this session,
        // proven by this token, this generation, this object. The token stays
        // in the path because it is a capability, not a credential -- media
        // players fetch segments without application headers.
        return "/api/v1/playback/sessions/" + session.id + "/stream/" + session.token;
    }

    std::pair<std::string, std::string> deterministic_session_credentials(
        std::string_view request_key, std::string_view fingerprint) const {
        auto derive = [&](std::string_view domain) {
            std::string material(domain);
            material.push_back('\0');
            material.append(request_key);
            material.push_back('\0');
            material.append(fingerprint);
            return hmac_sha256(catalogue.cluster_keys().auth,
                               {reinterpret_cast<const uint8_t*>(material.data()), material.size()});
        };
        const auto id = derive("macha-playback-session-id-v1");
        const auto token = derive("macha-playback-session-token-v1");
        return {hex(std::span<const uint8_t>(id).first(16)), hex(token)};
    }

    std::string creation_fingerprint(
        std::string_view item_id, std::string_view media_id,
        const PlaybackPreferences& prefs,
        const std::optional<int64_t>& seek_ms,
        std::string_view session_id) const {
        std::ostringstream canonical;
        canonical << "v2|item=" << item_id << "|media=" << media_id
                  << "|video=" << prefs.video.value_or("-")
                  << "|audio=" << prefs.audio.value_or("-")
                  << "|container=" << prefs.container
                  << "|mode=" << prefs.mode
                  << "|ph=" << prefs.max_height.value_or(-1)
                  << "|pb=" << prefs.max_bitrate.value_or(0)
                  << "|pa=" << prefs.audio_stream.value_or(-1)
                  << "|ps=" << prefs.subtitle_stream.value_or(-1)
                  << "|pal=" << prefs.audio_language
                  << "|psl=" << prefs.subtitle_language
                  << "|seek=" << seek_ms.value_or(0)
                  << "|session=" << session_id;
        const auto text = canonical.str();
        return to_string(sha256({reinterpret_cast<const uint8_t*>(text.data()), text.size()}));
    }

    HttpResponse creation_response(const Session& session, std::string trace,
                                   std::string_view idempotency_status = {}) const {
        auto payload = session_json(session);
        payload["trace_id"] = trace;
        // What this account may hold here and what it holds now, so a client
        // can plan against the cap instead of discovering it by refusal at the
        // worst moment. Counted live, under the lock, at the instant of the
        // response: this is the most perishable number the API carries -- it
        // moves whenever anyone on the account starts or stops anything, from
        // a device neither end can see -- so it is deliberately absent from
        // /api/v1/playback/status, which clients cache. A count that can only
        // arrive fresh cannot be read stale.
        payload["account"] = account_state_json(session.account);
        auto response = http_json(201, payload.dump());
        response.headers["Location"] = "/api/v1/playback/sessions/" + session.id;
        response.headers["X-Macha-Playback-Trace"] = std::move(trace);
        // Unlike the retired Idempotency-Key/Macha-Viewer-Session echoes (pure
        // restatements of what the client already sent), this conveys new
        // information the client cannot otherwise infer: whether its request
        // created a session or joined/replayed an existing one.
        if (!idempotency_status.empty())
            response.headers["X-Macha-Idempotency"] = std::string(idempotency_status);
        return response;
    }

    void erase_idempotency_for_session_locked(std::string_view session_id) {
        for (auto it = idempotent_creations.begin(); it != idempotent_creations.end();) {
            std::lock_guard creation_lock(it->second->mutex);
            if (it->second->complete && it->second->session_id == session_id)
                it = idempotent_creations.erase(it);
            else
                ++it;
        }
    }

    // THE KEY. Everything account-scoped in this file goes through here: the
    // cap counts it and the collection listing filters on it.
    //
    // Per account, not per viewer, and that is the whole point: the threat is
    // a rogue client launching a media DoS, and a per-viewer bound is no
    // defence at all against a client that simply claims more viewers. The
    // cost of the choice is that it also bounds a household, which is why the
    // default is generous -- see max_sessions_per_account.
    //
    // An anonymous session has no user_id, so it is bounded as itself rather
    // than joining every other anonymous caller in one bucket.
    static std::string account_key(const SessionIdentity& identity) {
        if (!identity.user_id.empty())
            return identity.user_id;
        return "session:" + identity.id;
    }

    std::shared_ptr<LogicalViewerSession> logical_session_for(std::string client_key) {
        if (client_key.empty())
            return std::make_shared<LogicalViewerSession>();
        std::lock_guard lock(mutex);
        auto& weak = logical_sessions[client_key];
        auto logical = weak.lock();
        if (!logical) {
            logical = std::make_shared<LogicalViewerSession>();
            logical->client_key = std::move(client_key);
            weak = logical;
        }
        return logical;
    }

    std::shared_ptr<Session> session_for_logical_locked(
        const std::shared_ptr<LogicalViewerSession>& logical) const {
        for (const auto& [_, session] : sessions)
            if (session->logical_session == logical) return session;
        return {};
    }


    bool plan_supported(const PlaybackPlan& plan) const {
        if (!engine) return plan.video != MediaTransform::transcode && plan.audio != MediaTransform::transcode;
        const auto status = engine->status();
        if (plan.video == MediaTransform::transcode && !status.h264_encoder) return false;
        if (plan.audio == MediaTransform::transcode && !status.aac_encoder) return false;
        return true;
    }

    void require_plan_supported(const PlaybackPlan& plan) const {
        if (plan_supported(plan)) return;
        if (plan.video == MediaTransform::transcode && (!engine || !engine->status().h264_encoder))
            throw std::invalid_argument("server H.264 encoder is unavailable");
        throw std::invalid_argument("server AAC encoder is unavailable");
    }

    SourceLease create_source(std::string_view media_id) {
        auto found = fs.find_media(media_id);
        if (!found) throw std::out_of_range("media object not found in filesystem");
        return {std::string(media_id), found->first, found->second};
    }

    MediaSource media_source(const SourceLease& lease) {
        auto entry = lease.entry;
        auto path = lease.path;
        auto size = lease.entry.size;
        return MediaSource{
            lease.media_id, lease.path, size,
            [this, entry = std::move(entry), path = std::move(path), size](MediaReadPurpose purpose) mutable
                -> std::shared_ptr<MediaInput> {
                const bool track_playback = purpose == MediaReadPurpose::playback;
                return std::make_shared<LogicalMediaInput>(
                    fs.open_read(entry, path, track_playback, FrameType::foreground), size);
            }, {}};
    }

    std::string probe_key(const SourceLease& lease) const {
        if (lease.media_id.starts_with("path:") || (!lease.media_id.empty() && lease.media_id.front() == '/')) {
            return lease.media_id + "#" + std::to_string(lease.entry.version) + ":" +
                   std::to_string(lease.entry.size) + ":" + std::to_string(lease.entry.mtime_ns);
        }
        return lease.media_id;
    }

    MediaProbeResult probe_source(const SourceLease& lease, std::string_view trace,
                                  Clock::time_point resolve_deadline) {
        auto key = probe_key(lease);
        const auto lookup_started = Clock::now();
        {
            std::lock_guard lock(mutex);
            if (auto it = probe_cache.find(key); it != probe_cache.end()) {
                Log::debug("playback[" + std::string(trace) + "] probe cache-hit media=" + lease.media_id);
                return it->second;
            }
        }
        if (lease.media_id.starts_with("macha:")) {
            if (auto stored = catalogue.media_profile(lease.media_id)) {
                const auto lookup_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - lookup_started).count();
                Log::info("playback[" + std::string(trace) +
                          "] immutable profile hit media=" + lease.media_id +
                          " lookup_ms=" + std::to_string(lookup_elapsed));
                std::lock_guard lock(mutex);
                cache_probe(key, *stored);
                return *stored;
            }
            const auto lookup_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - lookup_started).count();
            Log::info("playback[" + std::string(trace) +
                      "] immutable profile miss media=" + lease.media_id +
                      " lookup_ms=" + std::to_string(lookup_elapsed));
        }
        if (lease.media_id.starts_with("macha:") && media_information) {
            try {
                const auto fallback_started = Clock::now();
                auto resolved = media_information->resolve_playback(
                    lease.media_id, lease.path, lease.entry, resolve_deadline);
                const auto fallback_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - fallback_started).count();
                Log::info("playback[" + std::string(trace) +
                          "] foreground media inspection complete media=" + lease.media_id +
                          " fallback_ms=" + std::to_string(fallback_elapsed) +
                          " format=" + resolved.format + " streams=" +
                          std::to_string(resolved.streams.size()));
                std::lock_guard lock(mutex);
                cache_probe(key, resolved);
                return resolved;
            } catch (const std::exception& e) {
                throw stage_error(std::string(trace), "probe", e);
            }
        }
        if (lease.media_id.starts_with("macha:")) {
            try {
                auto resolved = catalogue.resolve_media_profile(
                    lease.media_id, resolve_deadline, [&] {
                        auto started_at = Clock::now();
                        if (started_at >= resolve_deadline)
                            throw PlaybackStageError(
                                std::string(trace), "probe",
                                "media inspection deadline exhausted before probing candidate");
                        auto remaining = std::max(
                            std::chrono::milliseconds(1),
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                resolve_deadline - started_at));
                        Log::debug("playback[" + std::string(trace) + "] probe start media=" +
                                   lease.media_id + " path=" + lease.path + " bytes=" +
                                   std::to_string(lease.entry.size) + " deadline_ms=" +
                                   std::to_string(remaining.count()));
                        MediaProbeResult result;
                        try {
                            result = engine->probe(media_source(lease), remaining);
                        } catch (const PlaybackStageError&) {
                            throw;
                        } catch (const std::exception& e) {
                            throw stage_error(std::string(trace), "probe", e);
                        }
                        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now() - started_at).count();
                        Log::info("playback[" + std::string(trace) + "] probe complete media=" +
                                  lease.media_id + " elapsed_ms=" + std::to_string(elapsed) +
                                  " format=" + result.format + " streams=" +
                                  std::to_string(result.streams.size()));
                        return result;
                    });
                {
                    std::lock_guard lock(mutex);
                    cache_probe(key, resolved.probe);
                }
                const auto lookup_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - lookup_started).count();
                if (!resolved.generated && !resolved.coalesced) {
                    Log::info("playback[" + std::string(trace) +
                              "] immutable profile hit media=" + lease.media_id +
                              " lookup_ms=" + std::to_string(lookup_elapsed));
                } else {
                    if (resolved.coalesced)
                        Log::info("playback[" + std::string(trace) +
                                  "] immutable profile coalesced media=" + lease.media_id +
                                  " wait_ms=" + std::to_string(lookup_elapsed));
                    {
                        std::lock_guard lock(profile_publish_mutex);
                        queue_profile_publication(lease.media_id, resolved.probe);
                    }
                    profile_publish_cv.notify_one();
                }
                return resolved.probe;
            } catch (const PlaybackStageError&) {
                throw;
            } catch (const std::exception& e) {
                throw stage_error(std::string(trace), "probe", e);
            }
        }

        std::shared_ptr<ProbeFlight> flight;
        bool owner = false;
        {
            std::lock_guard lock(mutex);
            if (auto it = probe_cache.find(key); it != probe_cache.end()) return it->second;
            auto [it, inserted] = probe_flights.try_emplace(key, std::make_shared<ProbeFlight>());
            flight = it->second;
            owner = inserted;
        }
        if (!owner) {
            const auto wait_started = Clock::now();
            std::unique_lock flight_lock(flight->mutex);
            if (!flight->cv.wait_until(flight_lock, resolve_deadline,
                                       [&] { return flight->complete; }))
                throw PlaybackStageError(std::string(trace), "probe",
                                         "timed out waiting for concurrent media inspection");
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - wait_started).count();
            Log::info("playback[" + std::string(trace) + "] probe coalesced media=" +
                      lease.media_id + " wait_ms=" + std::to_string(elapsed));
            if (flight->error) std::rethrow_exception(flight->error);
            return *flight->result;
        }

        auto complete_flight = [&](std::optional<MediaProbeResult> result,
                                   std::exception_ptr error = {}) {
            {
                std::lock_guard flight_lock(flight->mutex);
                flight->result = std::move(result);
                flight->error = error;
                flight->complete = true;
            }
            flight->cv.notify_all();
            std::lock_guard lock(mutex);
            auto it = probe_flights.find(key);
            if (it != probe_flights.end() && it->second == flight) probe_flights.erase(it);
        };
        auto started_at = Clock::now();
        if (started_at >= resolve_deadline) {
            auto error = std::make_exception_ptr(PlaybackStageError(
                std::string(trace), "probe", "media inspection deadline exhausted before probing candidate"));
            complete_flight({}, error);
            std::rethrow_exception(error);
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(resolve_deadline - started_at);
        remaining = std::max(std::chrono::milliseconds(1), remaining);
        Log::debug("playback[" + std::string(trace) + "] probe start media=" + lease.media_id +
                   " path=" + lease.path + " bytes=" + std::to_string(lease.entry.size) +
                   " deadline_ms=" + std::to_string(remaining.count()));
        MediaProbeResult probed;
        try {
            probed = engine->probe(media_source(lease), remaining);
        } catch (const PlaybackStageError&) {
            complete_flight({}, std::current_exception());
            throw;
        } catch (const std::exception& e) {
            auto error = std::make_exception_ptr(stage_error(std::string(trace), "probe", e));
            complete_flight({}, error);
            std::rethrow_exception(error);
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count();
        Log::info("playback[" + std::string(trace) + "] probe complete media=" + lease.media_id +
                  " elapsed_ms=" + std::to_string(elapsed) + " format=" + probed.format +
                  " streams=" + std::to_string(probed.streams.size()));
        {
            std::lock_guard lock(mutex);
            cache_probe(key, probed);
        }
        complete_flight(probed);
        return probed;
    }

    void publish_profiles(std::stop_token stop) {
        set_thread_name("macha-prof-pub");
        while (!stop.stop_requested()) {
            std::pair<std::string, MediaProbeResult> pending;
            {
                std::unique_lock lock(profile_publish_mutex);
                profile_publish_cv.wait(lock, stop, [&] {
                    return !pending_profile_publications.empty();
                });
                if (stop.stop_requested()) break;
                auto it = pending_profile_publications.begin();
                pending = *it;
                pending_profile_publication_bytes -=
                    probe_resident_weight(it->second) + it->first.size();
                pending_profile_publications.erase(it);
            }
            try {
                catalogue.put_media_profile(pending.first, pending.second);
                Log::debug("playback immutable profile published asynchronously media=" +
                           pending.first);
            } catch (const std::exception& e) {
                Log::warn("playback immutable profile asynchronous publication failed media=" +
                          pending.first + " error=" + e.what());
                std::unique_lock lock(profile_publish_mutex);
                queue_profile_publication(pending.first, pending.second);
                // A catalogue CAS conflict is transient. Preserve the completed
                // scan and retry from this event-driven worker after a bounded
                // backoff instead of forcing another foreground probe.
                profile_publish_cv.wait_for(lock, stop, std::chrono::milliseconds(250),
                                            [] { return false; });
            }
        }
        drain_profile_publications();
    }

    // A queued profile is a container inspection that has already been paid
    // for -- seconds of it on a large Matroska source, in the foreground, with
    // a viewer waiting. Until 0.46.1 a stop broke out of the loop above and
    // left whatever had not been published yet in memory, so every restart
    // discarded the profiles probed in the moments before it and the next play
    // of those titles paid for them again from scratch. Publish them on the
    // way out instead.
    //
    // One commit for the whole queue, not one per entry: shutdown must stay
    // prompt, and put_media_profiles() takes the catalogue mutation lock once.
    // Failure here is not retried -- shutdown is not the place to fight a
    // transient CAS conflict -- but it is reported, because a profile lost
    // this way is silent work the node will repeat.
    void drain_profile_publications() {
        std::map<std::string, MediaProbeResult, std::less<>> pending;
        {
            std::lock_guard lock(profile_publish_mutex);
            pending.swap(pending_profile_publications);
            pending_profile_publication_bytes = 0;
        }
        if (pending.empty()) return;
        const auto count = pending.size();
        try {
            catalogue.put_media_profiles(std::move(pending));
            Log::debug("playback immutable profiles published at shutdown count=" +
                       std::to_string(count));
        } catch (const std::exception& e) {
            Log::warn("playback immutable profiles lost at shutdown count=" +
                      std::to_string(count) + " error=" + e.what());
        }
    }

    size_t video_transcodes_locked(std::string_view excluding = {}) const {
        size_t count = 0;
        std::set<const LogicalViewerSession*> counted;
        for (const auto& [id, session] : sessions) {
            if (id == excluding) continue;
            if (session->logical_session &&
                session->logical_session->video_transcode_entitled &&
                counted.insert(session->logical_session.get()).second)
                ++count;
        }
        return count;
    }

    size_t audio_transcodes_locked(std::string_view excluding = {}) const {
        size_t count = 0;
        std::set<const LogicalViewerSession*> counted;
        for (const auto& [id, session] : sessions) {
            if (id == excluding) continue;
            if (session->logical_session &&
                session->logical_session->audio_transcode_entitled &&
                counted.insert(session->logical_session.get()).second)
                ++count;
        }
        return count;
    }

    size_t running_video_transcode_pipelines_locked() const {
        size_t count = 0;
        for (const auto& [_, session] : sessions) {
            if (session->plan.video != MediaTransform::transcode) continue;
            std::lock_guard pipeline_lock(session->pipeline_mutex);
            if (session->engine_session && session->engine_session->running()) ++count;
        }
        return count;
    }

    size_t running_audio_transcode_pipelines_locked() const {
        size_t count = 0;
        for (const auto& [_, session] : sessions) {
            if (session->plan.audio != MediaTransform::transcode) continue;
            std::lock_guard pipeline_lock(session->pipeline_mutex);
            if (session->engine_session && session->engine_session->running()) ++count;
        }
        return count;
    }

    struct ResourceReservation {
        bool video{};
        bool audio{};
    };

    void reserve_session_slot(std::string_view account) {
        std::lock_guard lock(mutex);
        if (sessions.size() + pending_sessions >= config.max_sessions)
            throw ResourceLimitError("playback session limit reached");
        if (config.max_sessions_per_account) {
            const auto held = sessions_held_by_locked(account);
            if (held >= config.max_sessions_per_account)
                throw AccountSessionLimitError(held, config.max_sessions_per_account);
        }
        ++pending_sessions;
        ++pending_by_account[std::string(account)];
    }

    void release_session_slot(std::string_view account) {
        std::lock_guard lock(mutex);
        if (pending_sessions) --pending_sessions;
        release_pending_account_locked(account);
    }

    // Live sessions plus this account's in-flight creations. Counting only
    // live ones would let a burst of concurrent creates walk straight past the
    // cap, which is exactly the shape a rogue client would use.
    size_t sessions_held_by_locked(std::string_view account) const {
        size_t held = 0;
        for (const auto& [_, session] : sessions)
            if (session->account == account) ++held;
        if (auto pending = pending_by_account.find(account); pending != pending_by_account.end())
            held += pending->second;
        return held;
    }

    void release_pending_account_locked(std::string_view account) {
        auto pending = pending_by_account.find(account);
        if (pending == pending_by_account.end())
            return;
        if (pending->second > 1)
            --pending->second;
        else
            pending_by_account.erase(pending);
    }

    ResourceReservation reserve_resources(Session& session, std::string_view excluding = {}) {
        std::lock_guard lock(mutex);
        if (!session.logical_session)
            throw std::logic_error("playback session has no logical viewer session");
        const bool video = session.plan.video == MediaTransform::transcode &&
                           !session.logical_session->video_transcode_entitled;
        const bool audio = session.plan.audio == MediaTransform::transcode &&
                           !session.logical_session->audio_transcode_entitled;
        if (video && video_transcodes_locked(excluding) + reserved_video_transcodes >=
                         config.max_video_transcodes)
            throw ResourceLimitError("video transcode limit reached");
        if (audio && audio_transcodes_locked(excluding) + reserved_audio_transcodes >=
                         config.max_audio_transcodes)
            throw ResourceLimitError("audio transcode limit reached");
        if (video) ++reserved_video_transcodes;
        if (audio) ++reserved_audio_transcodes;
        if (video) session.logical_session->video_transcode_entitled = true;
        if (audio) session.logical_session->audio_transcode_entitled = true;
        return {video, audio};
    }

    void commit_resources_locked(const ResourceReservation& reservation) {
        if (reservation.video && reserved_video_transcodes) --reserved_video_transcodes;
        if (reservation.audio && reserved_audio_transcodes) --reserved_audio_transcodes;
    }

    void rollback_resources(Session& session, const ResourceReservation& reservation) {
        std::lock_guard lock(mutex);
        commit_resources_locked(reservation);
        if (reservation.video) session.logical_session->video_transcode_entitled = false;
        if (reservation.audio) session.logical_session->audio_transcode_entitled = false;
    }

    std::shared_ptr<MediaEngineSession> active_engine(const Session& session) const {
        std::lock_guard lock(session.pipeline_mutex);
        return session.engine_session;
    }

    static bool transformed(const PlaybackPlan& plan) {
        return plan.video == MediaTransform::transcode ||
               plan.audio == MediaTransform::transcode;
    }

    void request_heap_reclaim() {
        std::lock_guard lock(mutex);
        heap_reclaim_pending = true;
        ++heap_reclaim_requests;
        signal_cleanup_locked();
    }

    void stop_pipeline(Session& session) {
        std::shared_ptr<MediaEngineSession> active;
        {
            std::lock_guard lock(session.pipeline_mutex);
            active.swap(session.engine_session);
        }
        if (active) {
            active->stop();
            // Destroy codec and segment owners before waking the asynchronous
            // reclaimer. stop_pipeline() is also used by viewer-facing seek
            // and reconfiguration paths, so it must never trim synchronously.
            active.reset();
            if (transformed(session.plan))
                request_heap_reclaim();
        }
    }

    void wait_for_initial_fragment(Session& session, std::string_view trace) {
        auto active = active_engine(session);
        if (!active) throw std::runtime_error("media pipeline did not start");
        auto store = active->segments();
        auto started_at = Clock::now();
        if (store->wait_ready(config.startup_timeout)) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count();
            auto state = store->snapshot();
            Log::info("playback[" + std::string(trace) + "] first fragment ready elapsed_ms=" +
                      std::to_string(elapsed) + " segments=" + std::to_string(state.segment_count));
            return;
        }
        auto state = store->snapshot();
        if (!state.error.empty()) throw std::runtime_error("libav pipeline failed before first fragment: " + state.error);
        auto diagnostic = active->diagnostics();
        if (!active->running())
            throw std::runtime_error("libav pipeline ended before first fragment" +
                                     (diagnostic.empty() ? std::string{} : ": " + diagnostic));
        throw std::runtime_error("timed out waiting for first fragmented-MP4 segment");
    }

    std::string vod_plan_key(const Session& session) const {
        const auto& plan = session.plan;
        std::ostringstream key;
        key << session.source.media_id << '|'
            << static_cast<int>(plan.mode) << '|'
            << plan.video_stream << '|' << plan.audio_stream << '|' << plan.subtitle_stream << '|'
            << static_cast<int>(plan.video) << '|' << static_cast<int>(plan.audio) << '|'
            << plan.video_codec << '|' << plan.audio_codec << '|'
            << (plan.target_height ? *plan.target_height : -1) << '|'
            << (plan.target_video_bitrate ? *plan.target_video_bitrate : 0) << '|'
            << static_cast<int>(plan.container) << '|'
            // The seek position is deliberately not part of the key: a cached
            // plan is re-seeked on a hit (reseek_hls_vod), so a representation
            // change at a new position no longer re-opens and re-indexes the
            // container (2026-09-07: three container opens per PATCH).
            << config.segment_duration.count() << '|'
            << (session.preferences.mode != "remux" &&
                false);
        return key.str();
    }

    void prepare_transformed_vod(Session& session, std::string_view trace) {
        session.vod_plan.reset();
        if (session.plan.mode == PlaybackMode::direct) return;

        const auto cache_key = vod_plan_key(session);
        {
            std::optional<HlsVodPlan> cached;
            {
                std::lock_guard lock(mutex);
                if (auto it = vod_plan_cache.find(cache_key); it != vod_plan_cache.end())
                    cached = it->second;
            }
            if (cached) {
                const auto requested_seek = session.plan.seek;
                // Against the cached plan's honoured REQUEST, not its baseline.
                // The two differ now that a remux generation begins at the
                // keyframe before the request, and reusing a cached plan whose
                // baseline happens to equal this request would publish that
                // plan's offset for a position it was not measured from.
                auto reseeked = requested_seek == cached->playback.seek_requested
                                    ? std::optional<HlsVodPlan>(*cached)
                                    : reseek_hls_vod(*cached, requested_seek);
                if (reseeked) {
                    session.plan = reseeked->playback;
                    session.vod_plan = std::move(*reseeked);
                    Log::debug("playback[" + std::string(trace) +
                               "] VOD plan cache-hit media=" + session.source.media_id +
                               " requested_ms=" + std::to_string(requested_seek.count()) +
                               " seek_ms=" + std::to_string(session.plan.seek.count()) +
                               " seek_offset_ms=" +
                               std::to_string(session.plan.seek_offset.count()) +
                               " segments=" +
                               std::to_string(session.vod_plan->segment_durations.size()));
                    return;
                }
            }
        }

        auto started = Clock::now();
        auto prepared = engine->prepare_hls_vod(session.source, session.plan,
                                                session.probe.duration_seconds, config.segment_duration,
                                                session.preferences.mode != "remux" &&
                                                    false,
                                                config.probe_timeout);
        session.plan = prepared.playback;
        Log::debug("playback[" + std::string(trace) + "] VOD plan ready mode=" +
                   playback_mode_name(session.plan.mode) +
                   " seek_ms=" + std::to_string(session.plan.seek.count()) +
                   " segments=" + std::to_string(prepared.segment_durations.size()) +
                   " elapsed_ms=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                       Clock::now() - started).count()));
        {
            std::lock_guard lock(mutex);
            if (!vod_plan_cache.contains(cache_key)) {
                while (vod_plan_cache_order.size() >= max_vod_plan_cache_entries) {
                    vod_plan_cache.erase(vod_plan_cache_order.front());
                    vod_plan_cache_order.pop_front();
                }
                vod_plan_cache_order.push_back(cache_key);
            }
            vod_plan_cache[cache_key] = prepared;
        }
        session.vod_plan = std::move(prepared);
    }

    void start_pipeline(Session& session, std::string_view trace) {
        stop_pipeline(session);
        session.stream_touched = Clock::now();
        ++session.generation;
        session.generation_dir = *config.temp_path / session.id / std::to_string(session.generation);
        std::error_code ec;
        std::filesystem::remove_all(session.generation_dir, ec);
        session.subtitle_cache = std::make_shared<Session::SubtitleCache>();
        session.subtitle_url.clear();
        auto prefix = public_stream_prefix(session);
        if (session.plan.mode == PlaybackMode::direct) {
            session.stream_url = prefix + "/direct";
            Log::info("playback[" + std::string(trace) + "] pipeline direct media=" + session.source.media_id);
        } else {
            auto started_at = Clock::now();
            Log::info("playback[" + std::string(trace) + "] pipeline start media=" + session.source.media_id +
                      " mode=" + playback_mode_name(session.plan.mode));
            if (!session.vod_plan) throw std::runtime_error("transformed session has no VOD plan");
            auto launched = engine->start_hls(session.source, *session.vod_plan, config.segment_duration,
                                              config.max_ahead_segments, config.segment_memory_bytes,
                                              session.generation_dir);
            auto segment_store = launched->segments();
            if (!segment_store || !segment_store->attach_memory_ledger(fs.node().retained_memory())) {
                launched->stop();
                throw std::runtime_error("viewer fragment memory admission unavailable");
            }
            {
                std::lock_guard lock(session.pipeline_mutex);
                session.engine_session = std::shared_ptr<MediaEngineSession>(std::move(launched));
            }
            try {
                wait_for_initial_fragment(session, trace);
            } catch (const PlaybackStageError&) {
                stop_pipeline(session);
                std::error_code cleanup_ec;
                std::filesystem::remove_all(session.generation_dir, cleanup_ec);
                throw;
            } catch (const std::exception& e) {
                stop_pipeline(session);
                std::error_code cleanup_ec;
                std::filesystem::remove_all(session.generation_dir, cleanup_ec);
                throw stage_error(std::string(trace), "pipeline_start", e);
            }
            session.stream_url = prefix + "/" + std::to_string(session.generation) + "/master.m3u8";
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count();
            Log::info("playback[" + std::string(trace) + "] pipeline startup complete elapsed_ms=" +
                      std::to_string(elapsed));
        }
        if (session.plan.subtitle_stream >= 0)
            session.subtitle_url = prefix + "/" + std::to_string(session.generation) +
                                   "/subtitle-" + std::to_string(session.plan.subtitle_stream) +
                                   "/manifest.json";
    }

    std::shared_ptr<Session> resolve_session(std::string item_id, std::vector<std::string> media_ids,
                                             PlaybackPreferences preferences,
                                             std::string_view trace,
                                             std::string existing_id = {}, std::string existing_token = {}) {
        if (media_ids.empty()) throw std::runtime_error("no media representations are available");
        struct Candidate {
            SourceLease lease;
            MediaProbeResult probe;
            PlaybackPlan plan;
            int rank{};
        };
        std::optional<Candidate> best;
        std::exception_ptr last_exception;
        const auto resolve_deadline = Clock::now() + config.probe_timeout;
        for (const auto& media_id : media_ids) {
            try {
                auto lease = create_source(media_id);
                const auto profile_started = Clock::now();
                auto probe = probe_source(lease, trace, resolve_deadline);
                const auto profile_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - profile_started).count();
                const auto selection_started = Clock::now();
                auto plan = plan_for(probe, preferences);
                require_plan_supported(plan);
                const auto selection_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - selection_started).count();
                int rank = plan.mode == PlaybackMode::direct ? 0 : (plan.mode == PlaybackMode::remux ? 1 : 2);
                Log::info("playback[" + std::string(trace) + "] admission candidate media=" +
                          media_id + " mode=" + playback_mode_name(plan.mode) +
                          " rank=" + std::to_string(rank) +
                          " metadata_ms=" + std::to_string(profile_ms) +
                          " selection_ms=" + std::to_string(selection_ms));
                if (!best || rank < best->rank) best = Candidate{std::move(lease), std::move(probe), plan, rank};
                if (rank == 0) break;
            } catch (const std::exception& e) {
                Log::debug("playback[" + std::string(trace) + "] candidate rejected media=" + media_id +
                           " error=" + e.what());
                last_exception = std::current_exception();
                if (Clock::now() >= resolve_deadline) break;
            }
        }
        if (!best) {
            if (last_exception) std::rethrow_exception(last_exception);
            throw std::runtime_error("no playable media representation");
        }

        auto session = std::make_shared<Session>();
        session->id = existing_id.empty() ? hex_token(16) : std::move(existing_id);
        session->token = existing_token.empty() ? hex_token() : std::move(existing_token);
        session->item_id = std::move(item_id);
        session->preferences = std::move(preferences);
        session->source_entry = best->lease.entry;
        session->source = media_source(best->lease);
        session->probe = std::move(best->probe);
        session->plan = best->plan;
        session->touched = Clock::now();
        return session;
    }

    std::shared_ptr<Session> reuse_seek_session(const Session& old,
                                                std::chrono::milliseconds requested_seek,
                                                std::string_view trace) {
        // Every exit from here says why. Across a whole day on es-1 there were
        // zero `seek fast-path` lines and nothing recorded which precondition
        // was failing, so the slow path could not be told from a path that was
        // never attempted.
        const auto declined = [&](const std::string& reason) {
            Log::info("playback[" + std::string(trace) + "] seek fast-path declined media=" +
                      old.source.media_id + " requested_ms=" +
                      std::to_string(requested_seek.count()) + " reason=" + reason);
            return std::shared_ptr<Session>{};
        };
        if (old.plan.mode == PlaybackMode::direct) return declined("direct-mode");
        if (!old.vod_plan) return declined("no-prepared-vod-plan");
        std::string reseek_reason;
        auto reseeked = reseek_hls_vod(*old.vod_plan, requested_seek, &reseek_reason);
        if (!reseeked) return declined(reseek_reason);

        auto session = std::make_shared<Session>();
        session->id = old.id;
        session->token = old.token;
        session->item_id = old.item_id;
        session->preferences = old.preferences;
        session->source = old.source;
        session->source_entry = old.source_entry;
        session->probe = old.probe;
        session->plan = reseeked->playback;
        session->vod_plan = std::move(*reseeked);
        session->generation = old.generation;
        session->touched = Clock::now();
        session->logical_session = old.logical_session;
        session->account = old.account;
        Log::info("playback[" + std::string(trace) + "] seek fast-path media=" +
                  session->source.media_id + " requested_ms=" +
                  std::to_string(requested_seek.count()) + " seek_ms=" +
                  std::to_string(session->plan.seek.count()) + " seek_offset_ms=" +
                  std::to_string(session->plan.seek_offset.count()) + " segments=" +
                  std::to_string(session->vod_plan->segment_durations.size()));
        return session;
    }

    std::shared_ptr<Session> reuse_subtitle_session(const Session& old,
                                                     PlaybackPreferences preferences,
                                                     std::string_view trace) {
        const MediaStreamInfo* subtitle = nullptr;
        if (preferences.subtitle_stream || !preferences.subtitle_language.empty())
            subtitle = select_stream(old.probe, MediaStreamType::subtitle,
                                     preferences.subtitle_stream, preferences.subtitle_language);
        if (preferences.subtitle_stream && !subtitle)
            throw std::invalid_argument("requested subtitle stream does not exist");
        if (subtitle && !webvtt_subtitle_supported(*subtitle))
            throw std::invalid_argument("requested subtitle stream cannot be converted to WebVTT");

        // Subtitle extraction is an independent WebVTT resource. Resolve only
        // the subtitle selection here: re-running A/V negotiation could choose
        // a different theoretical mode from the already-prepared live VOD
        // generation. The active representation is deliberately copied intact.
        auto session = std::make_shared<Session>();
        session->id = old.id;
        session->token = old.token;
        session->item_id = old.item_id;
        session->preferences = std::move(preferences);
        session->source = old.source;
        session->source_entry = old.source_entry;
        session->probe = old.probe;
        session->plan = old.plan;
        session->plan.subtitle_stream = subtitle ? subtitle->index : -1;
        session->vod_plan = old.vod_plan;
        session->generation = old.generation;
        session->generation_dir = old.generation_dir;
        session->engine_session = active_engine(old);
        session->stream_url = old.stream_url;
        session->subtitle_cache = old.subtitle_cache;
        session->logical_session = old.logical_session;
        // Ownership travels with the session, not with the object. A
        // replacement that drops it becomes unreachable to the account that
        // made it -- 404 on its own GET, absent from its own listing -- and,
        // worse, stops counting against the per-account cap, which turns a
        // subtitle change into a way to launder sessions past the limit.
        session->account = old.account;
        if (session->plan.subtitle_stream >= 0) {
            session->subtitle_url = public_stream_prefix(*session) + "/" +
                                    std::to_string(session->generation) + "/subtitle-" +
                                    std::to_string(session->plan.subtitle_stream) + "/manifest.json";
        }
        session->touched = Clock::now();
        Log::info("playback[" + std::string(trace) + "] subtitle update media=" +
                  session->source.media_id + " stream=" +
                  std::to_string(session->plan.subtitle_stream) + " generation=" +
                  std::to_string(session->generation));
        return session;
    }

    Json session_json(const Session& session) const {
        Json::Array streams;
        for (const auto& stream : session.probe.streams) streams.push_back(stream_json(stream));
        Json::Object selected{{"video_stream", session.plan.video_stream},
                              {"audio_stream", session.plan.audio_stream},
                              {"subtitle_stream", session.plan.subtitle_stream}};
        // What else this media could be asked for. Per-stream transforms and
        // quality belong to the mode that was asked for, so they are dropped
        // when asking about a different one: carrying max_height into a remux
        // probe asks an illegal question and answers "remux is unavailable".
        const auto without_mode_overrides = [](PlaybackPreferences preferences) {
            preferences.video.reset();
            preferences.audio.reset();
            preferences.max_height.reset();
            preferences.max_bitrate.reset();
            return preferences;
        };
        Json::Array modes{Json("direct")};
        for (const auto* candidate : {"remux", "transcode"}) {
            auto preferences = without_mode_overrides(session.preferences);
            preferences.mode = candidate;
            try {
                const auto plan = plan_for(session.probe, preferences);
                if (plan_supported(plan)) modes.emplace_back(candidate);
            } catch (...) {}
        }
        Json::Array quality_heights;
        if (const auto* video = stream_at(session.probe, session.plan.video_stream); video && video->height > 0) {
            static constexpr std::array<int, 6> candidates{2160, 1440, 1080, 720, 480, 360};
            for (const auto height : candidates) {
                if (height >= video->height) continue;
                // A quality change is a re-encode, so the question is only
                // ever "could this be transcoded to that height".
                auto preferences = without_mode_overrides(session.preferences);
                preferences.mode = "transcode";
                preferences.max_height = height;
                try {
                    const auto plan = plan_for(session.probe, preferences);
                    if (plan_supported(plan)) quality_heights.emplace_back(height);
                } catch (...) {}
            }
        }
        Json::Array audio_streams, subtitle_streams;
        for (const auto& stream : session.probe.streams) {
            if (stream.type == MediaStreamType::audio) {
                auto preferences = session.preferences;
                preferences.audio_stream = stream.index;
                preferences.audio_language.clear();
                try {
                    const auto plan = plan_for(session.probe, preferences);
                    if (plan_supported(plan)) audio_streams.emplace_back(stream_json(stream));
                } catch (...) {}
            }
            if (stream.type == MediaStreamType::subtitle) {
                auto preferences = session.preferences;
                preferences.subtitle_stream = stream.index;
                preferences.subtitle_language.clear();
                try {
                    const auto plan = plan_for(session.probe, preferences);
                    if (plan_supported(plan)) subtitle_streams.emplace_back(stream_json(stream));
                } catch (...) {}
            }
        }
        Json::Array media_ids;
        if (!session.item_id.empty()) {
            try {
                for (const auto& media_id : item_media(session.item_id)) {
                    // The active source remains valid for the lifetime of this
                    // session lease. Alternate source controls should only expose
                    // catalogue bindings that still resolve in the live namespace.
                    if (media_id == session.source.media_id || fs.find_media(media_id))
                        media_ids.emplace_back(media_id);
                }
            } catch (...) {
                // Catalogue reconciliation may remove the item while an active
                // session is still serving its immutable source lease. Keep that
                // session usable rather than making serialization fail.
            }
        }
        if (std::none_of(media_ids.begin(), media_ids.end(), [&](const Json& id) {
                return id.isString() && id.asString() == session.source.media_id;
            }))
            media_ids.emplace_back(session.source.media_id);
        const bool can_change_quality = !quality_heights.empty() || session.preferences.max_height.has_value() ||
                                        session.preferences.max_bitrate.has_value();
        const bool can_switch_media = media_ids.size() > 1;
        Json::Object options{{"modes", Json(std::move(modes))},
                             {"quality_heights", Json(std::move(quality_heights))},
                             {"media_ids", Json(std::move(media_ids))},
                             {"audio_streams", Json(std::move(audio_streams))},
                             {"subtitle_streams", Json(std::move(subtitle_streams))},
                             {"can_seek", true},
                             {"can_change_quality", can_change_quality},
                             {"can_switch_media", can_switch_media}};
        const auto mime_type = session.plan.mode == PlaybackMode::direct
                                   ? direct_mime(session.source.logical_path)
                                   : "application/vnd.apple.mpegurl";
        Json::Object source{{"path", session.source.logical_path},
                            {"format", session.probe.format},
                            {"size", session.source.size},
                            {"bitrate", session.probe.bitrate},
                            {"streams", Json(std::move(streams))}};
        // How far past the fragment it last asked for a client may arrive and
        // still find media already produced: the producer runs to
        // highest_requested + max_ahead_segments and then parks, and
        // segment_hold_window is deliberately the same distance, so a request
        // inside this window is one production is authorised to reach and a
        // request outside it is one nothing is working toward.
        //
        // Reported as milliseconds rather than as the two knobs it is derived
        // from. A count and a duration are two numbers the client has to
        // multiply and then keep in step with ours -- a client that hardcoded
        // 8 and 4000 silently under-runs on a node configured with 4 -- and
        // the derived figure stays meaningful if this bound ever stops being
        // counted in segments. Null for direct play, which has no pipeline and
        // therefore no frontier.
        const auto look_ahead_ms =
            static_cast<uint64_t>(config.max_ahead_segments) *
            static_cast<uint64_t>(std::max<int64_t>(0, config.segment_duration.count()));
        Json::Object stream{{"url", session.stream_url},
                            {"mime_type", mime_type},
                            {"look_ahead_ms", session.plan.mode == PlaybackMode::direct
                                                  ? Json(nullptr) : Json(look_ahead_ms)},
                            {"subtitle_url", session.subtitle_url.empty() ? Json(nullptr) : Json(session.subtitle_url)}};
        // How fast this generation is actually producing media, as a pair a
        // client divides itself. Raw on purpose: a rate computed here is a
        // rate with our smoothing and our window baked in, and a client
        // deciding whether to hand over needs to pick those itself. One
        // response answers it -- no polling, nothing on a viewer's path.
        //
        // producing_ms is encoder time with the parked interval removed, so
        // the ratio is what this node COULD sustain, not what this viewer
        // happened to ask for. Wall clock would read about 1.0x for anyone
        // watching at normal speed and would say "cannot outrun realtime"
        // about a node that comfortably can -- the answer that turns a
        // workable handover into a stall.
        //
        // produced_ms is also the production frontier in media time, which is
        // the other half of a handover decision: how long a join at position
        // P must wait is (P - produced_ms) / (rate - 1).
        //
        // Absent for direct play, which has no pipeline, and zeroed until the
        // first fragment lands. A client must treat producing_ms == 0 as "no
        // reading yet" rather than as an infinite rate.
        if (session.plan.mode != PlaybackMode::direct) {
            if (auto active = active_engine(session)) {
                const auto state = active->segments()->snapshot();
                stream["production"] =
                    Json::Object{{"produced_ms", state.produced_media_ms},
                                 {"producing_ms", state.producing_ms},
                                 {"produced_age_ms", state.produced_age_ms},
                                 {"producer_parked", state.producer_parked}};
            }
        }
        Json::Object out{{"session_id", session.id},
                         {"generation", session.generation},
                         {"media_id", session.source.media_id},
                         {"mode", playback_mode_name(session.plan.mode)},
                         {"duration_ms", static_cast<uint64_t>(std::max(0.0, session.probe.duration_seconds) * 1000.0)},
                         // The baseline, the offset into it, and the request
                         // that was honoured. Exactly, in integer
                         // milliseconds, with no tolerance:
                         //
                         //     seek_ms + seek_offset_ms == seek_requested_ms
                         //
                         // seek_ms means what it has always meant -- where the
                         // generation's media begins -- so a client that reads
                         // only it behaves as before. seek_offset_ms is how far
                         // into the generation the requested position sits, and
                         // is zero exactly when the mode could begin there:
                         // always for transcode and direct, and for remux when
                         // the request was already a keyframe. seek_requested_ms
                         // is what lets a client tell a violated invariant from
                         // an ordinary clamp near the end of a title; those want
                         // opposite handling, and without it they are the same
                         // number.
                         {"seek_ms", static_cast<uint64_t>(std::max<int64_t>(0, session.plan.seek.count()))},
                         {"seek_offset_ms", static_cast<uint64_t>(std::max<int64_t>(0, session.plan.seek_offset.count()))},
                         {"seek_requested_ms", static_cast<uint64_t>(std::max<int64_t>(0, session.plan.seek_requested.count()))},
                         {"preferences", preferences_json(session.preferences)},
                         {"selection", Json(std::move(selected))},
                         {"source", Json(std::move(source))},
                         {"output", output_json(session.probe, session.plan, session.probe.format,
                                                session.source.logical_path)},
                         {"stream", Json(std::move(stream))},
                         {"options", Json(std::move(options))}};
        if (!session.item_id.empty()) out["item_id"] = session.item_id;
        return Json(std::move(out));
    }

    std::vector<uint64_t> subtitle_segment_durations_ms(const Session& session) const {
        std::vector<uint64_t> durations;
        if (session.plan.mode != PlaybackMode::direct && session.vod_plan) {
            durations.reserve(session.vod_plan->segment_durations.size());
            for (double seconds : session.vod_plan->segment_durations)
                durations.push_back(std::max<uint64_t>(1, static_cast<uint64_t>(std::llround(seconds * 1000.0))));
            return durations;
        }

        const auto total_ms = static_cast<uint64_t>(std::max(0.0, session.probe.duration_seconds) * 1000.0);
        const auto segment_ms = static_cast<uint64_t>(std::max<int64_t>(1, config.segment_duration.count()));
        if (total_ms == 0) return durations;
        for (uint64_t start = 0; start < total_ms; start += segment_ms)
            durations.push_back(std::min(segment_ms, total_ms - start));
        return durations;
    }

    Json subtitle_manifest(const Session& session, int stream_index) const {
        Json::Array durations;
        for (auto duration : subtitle_segment_durations_ms(session))
            durations.emplace_back(duration);
        return Json(Json::Object{
            {"format", "macha-webvtt-segments"},
            {"version", 1},
            {"stream_index", stream_index},
            {"segment_durations_ms", Json(std::move(durations))}
        });
    }

    static std::optional<uint64_t> subtitle_segment_index(std::string_view name) {
        constexpr std::string_view prefix = "segment-";
        constexpr std::string_view suffix = ".vtt";
        if (!name.starts_with(prefix) || !name.ends_with(suffix)) return {};
        auto number = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        uint64_t index = 0;
        auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), index);
        if (ec != std::errc{} || end != number.data() + number.size()) return {};
        return index;
    }

    static std::optional<uint64_t> segment_index(std::string_view name) {
        constexpr std::string_view prefix = "segment-";
        if (!name.starts_with(prefix)) return {};
        std::string_view number = name.substr(prefix.size());
        if (number.ends_with(".m4s")) number.remove_suffix(4);
        else if (number.ends_with(".ts")) number.remove_suffix(3);
        else return {};
        uint64_t index = 0;
        auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), index);
        if (ec != std::errc{} || end != number.data() + number.size()) return {};
        return index;
    }

    HttpResponse bytes_response(const HttpRequest& request, Bytes bytes, std::string mime) {
        auto shared = std::make_shared<const Bytes>(std::move(bytes));
        return ranged_response(request, shared->size(), std::move(mime),
                               [shared](uint64_t offset, uint64_t length) {
                                   return std::make_shared<MemoryBody>(shared, offset, length);
                               });
    }

    HttpResponse session_stream_response(const HttpRequest& request, std::string_view session_id,
                                         std::string_view stream_path) {
        auto rest = stream_path;
        auto slash1 = rest.find('/');
        if (slash1 == std::string_view::npos) return http_error(404, "not_found", "stream not found");
        auto id = std::string(session_id);
        auto token = std::string(rest.substr(0, slash1));
        rest.remove_prefix(slash1 + 1);
        std::shared_ptr<Session> session;
        {
            std::lock_guard lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end() || it->second->token != token)
                return http_error(404, "not_found", "stream not found");
            session = it->second;
        }
        if (request.method != "GET" && request.method != "HEAD") return http_error(405, "method", "GET or HEAD required");
        if (rest == "direct") {
            {
                std::lock_guard lock(mutex);
                auto it = sessions.find(id);
                if (it == sessions.end() || it->second != session)
                    return http_error(404, "not_found", "stream not found");
                session->touched = Clock::now();
                // Direct Play is a stream object like any other: a single
                // ranged body can outlive several idle windows without another
                // request, so this must count as having been used.
                session->stream_served = true;
                signal_cleanup_locked();
            }
            return ranged_response(request, session->source_entry.size, direct_mime(session->source.logical_path),
                                   [this, path = session->source.logical_path, entry = session->source_entry](uint64_t offset, uint64_t length) {
                                       return std::make_shared<LogicalBody>(fs.open_read(entry, path, false, FrameType::foreground), offset, length);
                                   });
        }
        auto slash3 = rest.find('/');
        if (slash3 == std::string_view::npos) return http_error(404, "not_found", "stream object not found");
        uint64_t generation = 0;
        auto generation_text = rest.substr(0, slash3);
        auto [end, ec] = std::from_chars(generation_text.data(), generation_text.data() + generation_text.size(), generation);
        if (ec != std::errc{} || end != generation_text.data() + generation_text.size() || generation != session->generation)
            return http_error(404, "not_found", "stream generation not found");
        auto name = std::string(rest.substr(slash3 + 1));
        if (name.empty() || name == "." || name == ".." || name.find("..") != std::string::npos)
            return http_error(400, "bad_path", "invalid stream object");
        {
            std::lock_guard lock(mutex);
            auto it = sessions.find(id);
            // NOTE: this should be 410 generation_superseded, not 404 -- the
            // generation existed here and was replaced, which is a different
            // fact from a segment index that never existed, and a client must
            // not retry it. Held back deliberately (operator, 2026-09-20):
            // macha-client-core maps an unrecognised fragment status to
            // `unknown` and treats `unknown` as endpoint evidence, so a node
            // emitting 410 before core ships tolerance would charge a node
            // that is producing perfectly and build a standby that cannot
            // help. Core ships tolerance first, then this becomes a 410.
            // See TODO/ACTIVE.md.
            if (it == sessions.end() || it->second != session ||
                session->generation != generation)
                return http_error(404, "not_found", "stream generation not found");
            const auto now = Clock::now();
            session->touched = now;
            session->stream_touched = now;
            session->stream_served = true;
            ++session->active_stream_requests;
            signal_cleanup_locked();
        }
        [[maybe_unused]] auto stream_request = std::shared_ptr<void>(nullptr, [this, session](void*) {
            std::lock_guard lock(mutex);
            if (session->active_stream_requests) --session->active_stream_requests;
            signal_cleanup_locked();
        });
        if (name.starts_with("subtitle-")) {
            auto slash = name.find('/');
            if (slash == std::string::npos)
                return http_error(404, "not_found", "subtitle object not found");
            auto stream_text = std::string_view(name).substr(9, slash - 9);
            int stream_index = -1;
            auto [stream_end, stream_ec] = std::from_chars(stream_text.data(),
                                                           stream_text.data() + stream_text.size(),
                                                           stream_index);
            if (stream_ec != std::errc{} || stream_end != stream_text.data() + stream_text.size() ||
                stream_index != session->plan.subtitle_stream)
                return http_error(404, "not_found", "subtitle track not selected");

            const auto object_name = std::string_view(name).substr(slash + 1);
            const auto durations = subtitle_segment_durations_ms(*session);
            if (object_name == "manifest.json") {
                auto text = subtitle_manifest(*session, stream_index).dump();
                Bytes bytes(text.begin(), text.end());
                auto response = bytes_response(request, std::move(bytes), "application/json; charset=utf-8");
                response.headers["Cache-Control"] = "private, max-age=31536000, immutable";
                return response;
            }

            auto index = subtitle_segment_index(object_name);
            if (!index || *index >= durations.size())
                return http_error(404, "not_found", "subtitle segment not found");

            std::string data;
            {
                std::lock_guard subtitle_lock(session->subtitle_cache->mutex);
                auto key = std::make_pair(stream_index, *index);
                auto cached = session->subtitle_cache->segments.find(key);
                if (cached != session->subtitle_cache->segments.end()) {
                    data = cached->second;
                } else {
                    try {
                        uint64_t local_start_ms = 0;
                        for (uint64_t i = 0; i < *index; ++i) local_start_ms += durations[i];
                        const auto origin_ms = session->plan.mode == PlaybackMode::direct
                                                   ? int64_t{0} : session->plan.seek.count();
                        const auto source_start_ms = origin_ms + static_cast<int64_t>(local_start_ms);
                        const auto source_end_ms = source_start_ms + static_cast<int64_t>(durations[*index]);
                        data = engine->extract_webvtt_segment(
                            session->source, stream_index,
                            std::chrono::milliseconds(source_start_ms),
                            std::chrono::milliseconds(source_end_ms),
                            std::chrono::milliseconds(origin_ms));
                        auto& cache = *session->subtitle_cache;
                        if (data.size() <= cache.max_bytes) {
                            while (!cache.order.empty() &&
                                   (cache.segments.size() >= cache.max_entries ||
                                    cache.bytes > cache.max_bytes - data.size())) {
                                auto victim = cache.order.front();
                                cache.order.pop_front();
                                auto found = cache.segments.find(victim);
                                if (found == cache.segments.end()) continue;
                                cache.bytes -= found->second.size();
                                cache.segments.erase(found);
                            }
                            cache.segments.emplace(key, data);
                            cache.order.push_back(key);
                            cache.bytes += data.size();
                        }
                        Log::debug("subtitle segment generated session=" + session->id +
                                   " stream=" + std::to_string(stream_index) +
                                   " index=" + std::to_string(*index) +
                                   " bytes=" + std::to_string(data.size()));
                    } catch (const std::exception& e) {
                        Log::warn("subtitle extraction failed session=" + session->id +
                                  " stream=" + std::to_string(stream_index) +
                                  " index=" + std::to_string(*index) + " error=" + e.what());
                        return http_error(503, "subtitle_unavailable", e.what());
                    }
                }
            }
            Bytes bytes(data.begin(), data.end());
            auto response = bytes_response(request, std::move(bytes), "text/vtt; charset=utf-8");
            response.headers["Cache-Control"] = "private, max-age=31536000, immutable";
            return response;
        }

        if (name.find('/') != std::string::npos)
            return http_error(400, "bad_path", "invalid stream object");
        auto active = active_engine(*session);
        if (!active) return http_error(404, "not_found", "transformed stream is not active");
        auto store = active->segments();
        if (name == "master.m3u8") {
            // A real master playlist. Until 0.32.12 this URL answered with
            // the media playlist itself, so no CODECS attribute ever reached
            // the player and hls.js had to infer the source-buffer codecs
            // from the init segment -- on an old MSE that dropped the muxed
            // audio silently (Samsung Tizen 3, 2026-09-07).
            const MediaStreamInfo* video = nullptr;
            const MediaStreamInfo* audio = nullptr;
            for (const auto& stream : session->probe.streams) {
                if (stream.index == session->plan.video_stream) video = &stream;
                if (stream.index == session->plan.audio_stream) audio = &stream;
            }
            const auto variant = hls_variant_stream_inf(session->plan, video, audio,
                                                        session->probe.bitrate);
            std::string master = "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-INDEPENDENT-SEGMENTS\n" +
                                 variant + "\nmedia.m3u8\n";
            Bytes bytes(master.begin(), master.end());
            auto response = bytes_response(request, std::move(bytes), "application/vnd.apple.mpegurl");
            response.headers["Cache-Control"] = "no-store";
            return response;
        }
        if (name == "media.m3u8") {
            // No readiness gate. The playlist is complete from the moment the
            // plan exists, so there is nothing to wait for -- the request that
            // used to be held here is now the init and segment requests that
            // follow it, which is the better channel for the wait: a fragment
            // failure is fragLoadError rather than the levelLoadError a client
            // weighs as node health.
            auto playlist = store->playlist();
            auto state = store->snapshot();
            Log::debug("playback stream playlist session=" + session->id +
                       " generation=" + std::to_string(session->generation) +
                       " segments=" + std::to_string(state.segment_count) +
                       " highest_requested=" + std::to_string(state.highest_requested) +
                       " finished=" + std::string(state.finished ? "true" : "false"));
            if (playlist.empty()) {
                if (!state.error.empty()) return stream_failed(state.error);
                return http_error(404, "not_ready", "playlist not ready");
            }
            Bytes bytes(playlist.begin(), playlist.end());
            auto response = bytes_response(request, std::move(bytes), "application/vnd.apple.mpegurl");
            response.headers["Cache-Control"] = "no-store";
            return response;
        }
        // A complete VOD playlist promises every fragment before any of them
        // exists, so a request for one that has not been produced yet is
        // ordinary rather than erroneous. It is held -- but only as an
        // explicitly admitted resource, and only for media something is
        // actually working toward.
        const auto index = segment_index(name);
        const bool holdable_init =
            name == "init.mp4" && store->container() != MediaContainer::mpegts;

        std::optional<Bytes> object = store->object(name);
        if (!object && (index || holdable_init)) {
            auto state = store->snapshot();
            if (!state.error.empty()) return stream_failed(state.error);
            // Never promised: the playlist stops at the plan, and an index past
            // it is a genuine miss rather than something to wait for.
            if (index && state.planned_segments && *index >= state.planned_segments)
                return http_error(404, "not_found", "stream object not found");
            // A request the server parked earlier, woken because something
            // was published or because its time ran out. It still owns the
            // hold it was admitted with; nothing is re-admitted.
            auto held = request.resumed
                            ? std::static_pointer_cast<HeldRequest>(request.resumed_state)
                            : std::shared_ptr<HeldRequest>{};
            if (!held) {
                // Window. Beyond it nothing is working toward this fragment,
                // so waiting for it would be waiting on work that has not
                // been authorised to start.
                if (index && *index >= state.segment_count + config.segment_hold_window)
                    return segment_not_ready(session->id, *index, "beyond_hold_window");
                auto why = SegmentHoldArbiter::Refusal::budget_exhausted;
                auto hold = segment_holds.try_acquire(session->id, &why);
                if (!hold)
                    return segment_not_ready(session->id, index.value_or(0),
                                             why == SegmentHoldArbiter::Refusal::session_limit
                                                 ? "session_hold_limit"
                                                 : "hold_budget_exhausted");
                held = std::make_shared<HeldRequest>(std::move(*hold));
                // Only now, admitted: noting an index we had declined to
                // serve would drag the producer's authorised window forward
                // on behalf of a request we refused.
                if (index) active->note_segment_requested(*index);
            }
            // The wait itself costs no thread. The request asks the store
            // for the object and, in the same locked step, subscribes to
            // the next publication if it is not there; then it hands the
            // server a deferral and returns. The server re-runs it when the
            // store fires or the deadline passes. The hold rides along in
            // the deferral's state and is released with it.
            const auto deadline =
                request.resumed ? request.resume_deadline
                : config.segment_timeout.count() > 0
                    ? Clock::now() + config.segment_timeout
                    : Clock::time_point::max();
            auto waker = std::make_shared<HttpWaker>();
            auto awaited = store->object_or_subscribe(name, [waker] { waker->fire(); });
            if (awaited.object) {
                object = std::move(awaited.object);
                held.reset();
                state = store->snapshot();
                Log::debug("playback stream segment session=" + session->id +
                           " generation=" + std::to_string(session->generation) +
                           " index=" + std::to_string(index.value_or(0)) +
                           " bytes=" + std::to_string(object->size()) +
                           " segments_ready=" + std::to_string(state.segment_count));
            } else if (!awaited.ended && Clock::now() < deadline) {
                HttpResponse deferred;
                deferred.defer = HttpDeferral{std::move(waker), deadline, std::move(held)};
                return deferred;
            } else {
                held.reset();
            }
        }
        if (!object) {
            auto state = store->snapshot();
            if (!state.error.empty()) return stream_failed(state.error);
            // The playlist promised this object, so its absence is "not yet",
            // never "not there". A 404 invites an intermediary to cache the
            // miss and some players treat it as terminal.
            if (index || holdable_init)
                return segment_not_ready(session->id, index.value_or(0), "hold_timed_out");
            return http_error(404, state.finished ? "not_found" : "not_ready",
                              state.finished ? "stream object not found" : "stream object not ready");
        }
        return bytes_response(request, std::move(*object), segment_mime(name));
    }

    std::vector<std::string> item_media(std::string_view item_id) const {
        auto item = catalogue.get(item_id);
        if (!item) throw std::out_of_range("catalogue item not found");
        return item->media_ids;
    }

    HttpResponse create(const HttpRequest& request) {
        if (!config.enabled) return http_error(503, "streaming_disabled", "streaming is disabled");
        // Unreachable in production -- this route is not in capability_request's
        // exempt list, so HttpServer has already rejected an unauthenticated
        // request with 401 -- but cheap to check and matches this project's
        // fail-loudly style elsewhere.
        if (!request.session) return http_error(401, "unauthorized", "a valid session bearer token is required");
        auto trace = hex_token(4);
        auto request_started = Clock::now();
        Log::info("playback[" + trace + "] session create start");
        Json root = Json::parse(std::string_view(reinterpret_cast<const char*>(request.body.data()), request.body.size()));
        if (!root.isObject()) return http_error(400, "bad_request", "JSON object required");
        std::string item_id, media_id;
        if (auto v = root.find("item_id"); v && v->isString()) item_id = v->asString();
        if (auto v = root.find("media_id"); v && v->isString()) media_id = v->asString();
        if (item_id.empty() && media_id.empty()) return http_error(400, "bad_request", "item_id or media_id is required");
        auto prefs = parse_preferences(root.find("preferences"));
        std::optional<int64_t> seek_ms;
        if (auto seek = root.find("seek_ms")) {
            try {
                auto value = seek->asInt64();
                if (value >= 0) seek_ms = value;
            } catch (...) {}
            if (!seek_ms) return http_error(400, "bad_seek", "seek_ms must be non-negative");
        }
        auto media = media_id.empty() ? item_media(item_id) : std::vector<std::string>{media_id};
        const auto account = account_key(*request.session);
        std::string idempotency_key;
        if (auto it = request.query.find("idempotency_key"); it != request.query.end())
            idempotency_key = it->second;
        if (idempotency_key.size() > 256 ||
            std::any_of(idempotency_key.begin(), idempotency_key.end(), [](unsigned char c) {
                return c < 0x21 || c > 0x7e;
            }))
            return http_error(400, "bad_idempotency_key",
                              "idempotency_key must be 1..256 visible ASCII characters");

        std::string fingerprint;
        // Declared out here because the error path below erases by it.
        std::string idempotency_scope;
        std::shared_ptr<IdempotentCreation> idempotent;
        bool idempotent_owner = false;
        if (!idempotency_key.empty()) {
            fingerprint = creation_fingerprint(item_id, media_id, prefs, seek_ms,
                                               request.session->id);
            // Scoped to the account. The key is client-chosen and often
            // predictable ("retry-1"), and the map was global: any
            // authenticated account could occupy another's key and turn that
            // account's legitimate retry into a 409 idempotency_conflict.
            // Not a takeover -- the fingerprint carries the auth session id,
            // so a stolen key conflicts rather than replaying someone else's
            // session -- but a targeted denial of the retry path, which is
            // the path a client is on when something has already gone wrong.
            idempotency_scope = account + '\0' + idempotency_key;
            {
                std::lock_guard lock(mutex);
                auto it = idempotent_creations.find(idempotency_scope);
                if (it != idempotent_creations.end() &&
                    it->second->fingerprint != fingerprint)
                    return http_error(409, "idempotency_conflict",
                                      "idempotency_key was already used for a different playback request");
                if (it == idempotent_creations.end()) {
                    idempotent = std::make_shared<IdempotentCreation>();
                    idempotent->fingerprint = fingerprint;
                    idempotent_creations.emplace(idempotency_scope, idempotent);
                    idempotent_owner = true;
                } else {
                    idempotent = it->second;
                }
            }
            if (!idempotent_owner) {
                std::unique_lock lock(idempotent->mutex);
                if (!idempotent->complete) {
                    const auto deadline = Clock::now() + config.probe_timeout + config.startup_timeout;
                    if (!idempotent->cv.wait_until(lock, deadline,
                                                   [&] { return idempotent->complete; }))
                        return http_error(503, "idempotency_in_progress",
                                          "matching session creation is still in progress");
                }
                if (idempotent->error) std::rethrow_exception(idempotent->error);
                auto existing_id = idempotent->session_id;
                lock.unlock();
                if (existing_id.empty())
                    return http_error(409, "idempotency_expired",
                                      "the prior playback session has expired");
                std::shared_ptr<Session> existing;
                {
                    std::lock_guard sessions_lock(mutex);
                    auto active = sessions.find(existing_id);
                    if (active == sessions.end())
                        return http_error(409, "idempotency_expired",
                                          "the prior playback session has expired");
                    existing = active->second;
                    existing->touched = Clock::now();
                    signal_cleanup_locked();
                }
                return creation_response(*existing, trace, "replayed");
            }
        }
        // A POST to a collection creates a member, every time. There is no
        // "previous" to supersede: one bearer may hold many sessions now, and
        // what bounds that is the per-account cap rather than a hidden slot of
        // one. Each session is its own logical viewer, so two viewers sharing
        // a login do not share a transcode entitlement -- the cap and the
        // entitlement share the account key instead.
        auto logical_session = std::make_shared<LogicalViewerSession>();
        std::unique_lock logical_operation(logical_session->operation_mutex);
        reserve_session_slot(account);
        ResourceReservation resource_reservation;
        std::shared_ptr<Session> session;
        try {
            std::string deterministic_id, deterministic_token;
            if (idempotent) {
                auto credentials = deterministic_session_credentials(idempotency_key, fingerprint);
                deterministic_id = std::move(credentials.first);
                deterministic_token = std::move(credentials.second);
            }
            session = resolve_session(item_id, std::move(media), std::move(prefs), trace,
                                      std::move(deterministic_id), std::move(deterministic_token));
            session->logical_session = logical_session;
            session->account = account;
            if (seek_ms) session->plan.request_seek(std::chrono::milliseconds(*seek_ms));
            prepare_transformed_vod(*session, trace);
            resource_reservation = reserve_resources(*session);
            start_pipeline(*session, trace);
            {
                std::lock_guard lock(mutex);
                sessions[session->id] = session;
                signal_cleanup_locked();
                if (pending_sessions) --pending_sessions;
                release_pending_account_locked(account);
                commit_resources_locked(resource_reservation);
                resource_reservation = {};
            }
        } catch (...) {
            auto error = std::current_exception();
            if (session) stop_pipeline(*session);
            if (session && (resource_reservation.video || resource_reservation.audio))
                rollback_resources(*session, resource_reservation);
            release_session_slot(account);
            if (idempotent) {
                {
                    std::lock_guard lock(idempotent->mutex);
                    idempotent->error = error;
                    idempotent->complete = true;
                }
                idempotent->cv.notify_all();
                std::lock_guard lock(mutex);
                auto it = idempotent_creations.find(idempotency_scope);
                if (it != idempotent_creations.end() && it->second == idempotent)
                    idempotent_creations.erase(it);
            }
            std::rethrow_exception(error);
        }
        if (idempotent) {
            {
                std::lock_guard lock(idempotent->mutex);
                idempotent->session_id = session->id;
                idempotent->complete = true;
            }
            idempotent->cv.notify_all();
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - request_started).count();
        // What was negotiated and from what: the transforms and codecs the
        // viewer will get, and the capabilities it advertised. A TV that
        // claimed hevc and got a silent transcode could not be diagnosed
        // without either (2026-09-07).
        Log::info("playback[" + trace + "] session create complete id=" + session->id +
                  " mode=" + playback_mode_name(session->plan.mode) +
                  " video=" + transform_name(session->plan.video) + "/" + session->plan.video_codec +
                  " audio=" + transform_name(session->plan.audio) + "/" + session->plan.audio_codec +
                  (session->plan.target_height
                       ? " target_height=" + std::to_string(*session->plan.target_height)
                       : std::string{}) +
                  " elapsed_ms=" + std::to_string(elapsed));
        return creation_response(*session, trace, idempotent ? "created" : "");
    }

    // A playback session is now addressable by an id that outlives the request
    // that made it, several may exist per account, and a listing hands the ids
    // out. So the control routes have to check who is asking: without this,
    // any authenticated account that learns an id can read, re-seek or DELETE
    // another viewer's session mid-film, and the per-account cap means nothing
    // because a stranger can free your slots. Under one-session-per-bearer the
    // gap was masked -- an id was only ever known to the client that made it.
    //
    // A mismatch answers 404 rather than 403: whether an id exists on this
    // node is not something one account gets to learn about another.
    bool caller_owns(const Session& session, const HttpRequest& request) const {
        return request.session && session.account == account_key(*request.session);
    }

    Json account_state_json(std::string_view account) const {
        Json::Object out;
        {
            std::lock_guard lock(mutex);
            out["sessions"] = static_cast<uint64_t>(sessions_held_by_locked(account));
        }
        out["max_sessions"] = static_cast<uint64_t>(config.max_sessions_per_account);
        return Json(std::move(out));
    }

    HttpResponse list_sessions(const HttpRequest& request) {
        if (!request.session)
            return http_error(401, "unauthorized", "a valid session bearer token is required");
        // Node-local, by decision rather than omission. A playback session
        // owns a generation directory, a transcode slot and a live pipeline on
        // *this* node, so an id only means anything here; a client wanting the
        // account's sessions cluster-wide asks each node it knows, and thereby
        // learns which node each one came from. That provenance is what a
        // client needs to probe, regenerate or release an adopted session, and
        // per-node listing hands it over for free.
        const auto account = account_key(*request.session);
        Json::Array out;
        {
            std::lock_guard lock(mutex);
            for (const auto& [_, session] : sessions) {
                if (session->account != account) continue;
                out.push_back(session_json(*session));
            }
        }
        Json::Object body;
        const auto held = out.size();
        body["items"] = std::move(out);
        // The cap is the node's and a client does not get a vote, but it does
        // get to know. Stating it here means a client can plan against it --
        // decline to prepare a standby it knows will be refused, and say "at
        // the session limit" rather than showing a failover that merely
        // failed -- instead of discovering it by refusal at the worst moment.
        Json::Object account_info;
        account_info["sessions"] = static_cast<uint64_t>(held);
        account_info["max_sessions"] = static_cast<uint64_t>(config.max_sessions_per_account);
        body["account"] = std::move(account_info);
        return http_json(200, Json(std::move(body)).dump());
    }

    HttpResponse get_session(std::string_view id, const HttpRequest& request) {
        std::shared_ptr<Session> session;
        {
            std::lock_guard lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end()) return http_error(404, "not_found", "playback session not found");
            if (!caller_owns(*it->second, request))
                return http_error(404, "not_found", "playback session not found");
            session = it->second;
            session->touched = Clock::now();
            signal_cleanup_locked();
        }
        auto result = session_json(*session);
        if (auto active = active_engine(*session)) {
            result["engine_running"] = active->running();
            if (auto code = active->exit_code()) result["engine_exit_code"] = *code;
            auto state = active->segments()->snapshot();
            result["segments_ready"] = state.segment_count;
            if (!state.error.empty()) result["engine_error"] = state.error;
        }
        return http_json(200, result.dump());
    }

    HttpResponse update_session(std::string_view id, const HttpRequest& request) {
        std::shared_ptr<Session> old;
        {
            std::lock_guard lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end()) return http_error(404, "not_found", "playback session not found");
            if (!caller_owns(*it->second, request))
                return http_error(404, "not_found", "playback session not found");
            old = it->second;
        }
        if (!old->logical_session)
            throw std::logic_error("playback session has no logical viewer session");
        std::unique_lock logical_operation(old->logical_session->operation_mutex);
        {
            std::lock_guard lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end())
                return http_error(404, "not_found", "playback session not found");
            old = it->second;
        }
        auto trace = hex_token(4);
        Json root = Json::parse(std::string_view(reinterpret_cast<const char*>(request.body.data()), request.body.size()));
        if (!root.isObject()) return http_error(400, "bad_request", "JSON object required");
        auto prefs = parse_preferences(root.find("preferences"), old->preferences);
        std::optional<int64_t> seek_ms;
        if (auto seek = root.find("seek_ms")) {
            try {
                auto value = seek->asInt64();
                if (value >= 0) seek_ms = value;
            } catch (...) {}
            if (!seek_ms) return http_error(400, "bad_seek", "seek_ms must be non-negative");
        }
        std::string media_override;
        if (auto media = root.find("media_id"); media && media->isString()) media_override = media->asString();

        const auto* preference_patch = root.find("preferences");
        bool subtitle_only = false;
        if (!seek_ms && media_override.empty() && root.asObject().size() == 1 &&
            preference_patch && preference_patch->isObject() && !preference_patch->asObject().empty()) {
            subtitle_only = std::all_of(preference_patch->asObject().begin(),
                                        preference_patch->asObject().end(),
                                        [](const auto& entry) {
                                            return entry.first == "subtitle_stream" ||
                                                   entry.first == "subtitle_language";
                                        });
        }
        if (subtitle_only) {
            auto replacement = reuse_subtitle_session(*old, std::move(prefs), trace);
            {
                std::lock_guard lock(mutex);
                auto it = sessions.find(std::string(id));
                if (it == sessions.end() || it->second != old)
                    throw std::runtime_error("playback session changed during subtitle update");
                it->second = replacement;
            }
            return http_json(200, session_json(*replacement).dump());
        }

        // A seek-only update does not alter representation, tracks, quality or
        // codec negotiation. Reuse the prepared VOD random-access plan instead
        // of resolving, probing and materialising the source index again.
        // Clients commonly resend their current preferences with a seek.  A
        // semantically unchanged preference object must not turn a seek into a
        // fresh probe/VOD-planning pass; that re-opens container metadata and
        // remote extents precisely when the viewer is waiting for the seek.
        const bool seek_only = seek_ms.has_value() && media_override.empty() &&
                               prefs == old->preferences;
        std::shared_ptr<Session> replacement;
        if (seek_only)
            replacement = reuse_seek_session(*old, std::chrono::milliseconds(*seek_ms), trace);
        else if (seek_ms)
            // A seek that is not seek-only pays a full probe/VOD-planning pass
            // while the viewer waits, so say which part of the request made it
            // one rather than leaving the slow path unexplained.
            Log::info("playback[" + trace + "] seek fast-path skipped media=" +
                      old->source.media_id + " requested_ms=" + std::to_string(*seek_ms) +
                      " reason=" + (media_override.empty() ? "preferences-changed"
                                                           : "media-override"));

        if (!replacement) {
            auto media = media_override.empty()
                             ? (old->item_id.empty() ? std::vector<std::string>{old->source.media_id}
                                                     : item_media(old->item_id))
                             : std::vector<std::string>{media_override};
            replacement = resolve_session(old->item_id, std::move(media), prefs,
                                          trace, old->id, old->token);
            replacement->logical_session = old->logical_session;
            // Same rule as the subtitle and seek replacements: ownership
            // travels with the session. A mode change must not hand the
            // caller back a session it no longer owns.
            replacement->account = old->account;
            replacement->generation = old->generation;
            if (seek_ms) replacement->plan.request_seek(std::chrono::milliseconds(*seek_ms));
            prepare_transformed_vod(*replacement, trace);
        }
        ResourceReservation resource_reservation;
        // start_pipeline() below can block for several seconds (up to
        // streaming.startup_timeout_ms) waiting for the replacement's first
        // fragment, and stop_pipeline(*old) -- which fully cancels old's
        // segment store and wakes anything blocked in its wait_object() --
        // does not run until after that completes. Without this, an
        // in-flight request for a not-yet-produced segment on the
        // superseded generation (e.g. client read-ahead) stays parked for
        // that whole window instead of getting a prompt 404. Mark old as
        // superseded up front so such requests wake immediately; this is
        // reversible (unlike stop_pipeline's real cancel()), so on failure
        // below we clear it and old keeps serving normally as the still-
        // active session.
        auto old_active = active_engine(*old);
        if (old_active) old_active->segments()->mark_superseded(true);
        try {
            resource_reservation = reserve_resources(*replacement, old->id);
            start_pipeline(*replacement, trace);
            // Keep the replacement reservation until the old physical pipeline
            // is stopped.  Otherwise a third concurrent request could consume
            // the apparent free slot during this handover window.
            stop_pipeline(*old);
            {
                std::lock_guard lock(mutex);
                auto it = sessions.find(std::string(id));
                if (it == sessions.end() || it->second != old)
                    throw std::runtime_error("playback session changed during update");
                it->second = replacement;
                signal_cleanup_locked();
                commit_resources_locked(resource_reservation);
                resource_reservation = {};
            }
        } catch (...) {
            if (old_active) old_active->segments()->mark_superseded(false);
            stop_pipeline(*replacement);
            if (resource_reservation.video || resource_reservation.audio)
                rollback_resources(*replacement, resource_reservation);
            throw;
        }
        std::error_code ec;
        if (!old->generation_dir.empty() && old->generation_dir != replacement->generation_dir)
            std::filesystem::remove_all(old->generation_dir, ec);
        return http_json(200, session_json(*replacement).dump());
    }

    HttpResponse erase_session(std::string_view id, const HttpRequest& request) {
        std::shared_ptr<Session> session;
        {
            std::lock_guard lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end()) return http_error(404, "not_found", "playback session not found");
            if (!caller_owns(*it->second, request))
                return http_error(404, "not_found", "playback session not found");
            session = it->second;
        }
        std::unique_lock logical_operation(session->logical_session->operation_mutex);
        {
            std::lock_guard lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end()) return http_error(404, "not_found", "playback session not found");
            session = it->second;
            sessions.erase(it);
            erase_idempotency_for_session_locked(id);
            if (!session->logical_session->client_key.empty())
                logical_sessions.erase(session->logical_session->client_key);
            signal_cleanup_locked();
        }
        stop_pipeline(*session);
        std::error_code ec;
        std::filesystem::remove_all(*config.temp_path / session->id, ec);
        return {204, "application/json; charset=utf-8", {}, {}, {}};
    }

    HttpResponse status() const {
        MediaEngineStatus state;
        if (engine) state = engine->status();
        size_t session_count = 0, video_transcodes = 0, audio_transcodes = 0;
        size_t running_video_transcode_pipelines = 0;
        size_t running_audio_transcode_pipelines = 0;
        size_t cached_probes = 0, cached_probe_bytes = 0;
        size_t cached_subtitle_segments = 0, cached_subtitle_bytes = 0;
        uint64_t segment_store_resident_bytes = 0, segment_store_spill_bytes = 0;
        uint64_t segment_store_descriptor_bytes = 0, segment_store_segments = 0;
        uint64_t segment_store_planned_segments = 0;
        uint64_t reclaimed = 0;
        bool heap_pending = false;
        uint64_t heap_requests = 0, heap_runs = 0, heap_successes = 0;
        std::chrono::milliseconds pipeline_idle{};
        std::chrono::milliseconds session_unused_idle{};
        uint64_t unused_reclaimed = 0;
        std::vector<std::shared_ptr<Session>> active_sessions;
        {
            std::lock_guard lock(mutex);
            session_count = sessions.size();
            video_transcodes = video_transcodes_locked();
            audio_transcodes = audio_transcodes_locked();
            running_video_transcode_pipelines = running_video_transcode_pipelines_locked();
            running_audio_transcode_pipelines = running_audio_transcode_pipelines_locked();
            reclaimed = idle_pipelines_reclaimed;
            unused_reclaimed = unused_sessions_reclaimed;
            heap_pending = heap_reclaim_pending;
            heap_requests = heap_reclaim_requests;
            heap_runs = heap_reclaim_runs;
            heap_successes = heap_reclaim_successes;
            pipeline_idle = config.pipeline_idle;
            session_unused_idle = config.session_unused_idle;
            cached_probes = probe_cache.size();
            cached_probe_bytes = probe_cache_bytes;
            active_sessions.reserve(sessions.size());
            for (const auto& [_, session] : sessions)
                active_sessions.push_back(session);
        }
        // Per-session subtitle-cache and segment-store reads happen with the
        // global session mutex already released: each only needs that one
        // session's own lock, and serializing them behind the global mutex
        // would let contention on any single session's subtitle cache (e.g. a
        // slow WebVTT extraction, see public_stream_response) stall every
        // other playback operation -- create/patch/delete/cleanup all take
        // the same global mutex -- for as long as that one lock is held.
        // The per-session lock itself is best-effort (try_lock): a session
        // mid-extraction just contributes stale/zero counts for this status
        // call rather than making status() block on it too.
        for (const auto& session : active_sessions) {
            if (std::unique_lock subtitle_lock(session->subtitle_cache->mutex, std::try_to_lock);
                subtitle_lock.owns_lock()) {
                cached_subtitle_segments += session->subtitle_cache->segments.size();
                cached_subtitle_bytes += session->subtitle_cache->bytes;
            }
            auto active = active_engine(*session);
            if (!active)
                continue;
            const auto segment_state = active->segments()->snapshot();
            segment_store_resident_bytes += segment_state.resident_bytes;
            segment_store_spill_bytes += segment_state.spill_bytes;
            segment_store_descriptor_bytes += segment_state.descriptor_bytes;
            segment_store_segments += segment_state.segment_count;
            segment_store_planned_segments += segment_state.planned_segments;
        }
        Json::Object out{{"server_version", std::string(kServerVersion)},
                         {"enabled", config.enabled},
                         {"sessions", static_cast<uint64_t>(session_count)},
                         {"max_sessions", static_cast<uint64_t>(config.max_sessions)},
                         {"max_sessions_per_account",
                          static_cast<uint64_t>(config.max_sessions_per_account)},
                         {"video_transcodes", static_cast<uint64_t>(video_transcodes)},
                         {"max_video_transcodes", static_cast<uint64_t>(config.max_video_transcodes)},
                         {"audio_transcodes", static_cast<uint64_t>(audio_transcodes)},
                         {"max_audio_transcodes", static_cast<uint64_t>(config.max_audio_transcodes)},
                         {"running_video_transcode_pipelines",
                          static_cast<uint64_t>(running_video_transcode_pipelines)},
                         {"running_audio_transcode_pipelines",
                          static_cast<uint64_t>(running_audio_transcode_pipelines)},
                         {"video_decoder_threads",
                          static_cast<uint64_t>(config.video_decoder_threads)},
                         {"pipeline_idle_ms", static_cast<uint64_t>(pipeline_idle.count())},
                         {"idle_pipelines_reclaimed", reclaimed},
                         {"session_unused_idle_ms",
                          static_cast<uint64_t>(session_unused_idle.count())},
                         {"unused_sessions_reclaimed", unused_reclaimed},
                         {"heap_reclaim_pending", heap_pending},
                         {"heap_reclaim_requests", heap_requests},
                         {"heap_reclaim_runs", heap_runs},
                         {"heap_reclaim_successes", heap_successes},
                         {"probe_cache_entries", static_cast<uint64_t>(cached_probes)},
                         {"probe_cache_bytes", static_cast<uint64_t>(cached_probe_bytes)},
                         {"probe_cache_limit_entries",
                          static_cast<uint64_t>(max_probe_cache_entries)},
                         {"probe_cache_limit_bytes",
                          static_cast<uint64_t>(max_probe_cache_bytes)},
                         {"subtitle_cache_entries",
                          static_cast<uint64_t>(cached_subtitle_segments)},
                         {"subtitle_cache_bytes", static_cast<uint64_t>(cached_subtitle_bytes)},
                         {"segment_store_resident_bytes", segment_store_resident_bytes},
                         {"segment_store_spill_bytes", segment_store_spill_bytes},
                         {"segment_store_descriptor_bytes", segment_store_descriptor_bytes},
                         {"segment_store_segments", segment_store_segments},
                         {"segment_store_planned_segments", segment_store_planned_segments},
                         {"media_engine_available", state.available},
                         {"media_engine", state.backend},
                         {"media_engine_version", state.version},
                         {"h264_encoder", state.h264_encoder},
                         {"aac_encoder", state.aac_encoder},
                         // Kept for one release so older diagnostics UIs do not
                         // mistake the field's disappearance for a parse failure.
                         {"ffmpeg_available", false},
                         {"ffprobe_available", false},
                         {"ffmpeg_version", ""}};
        return http_json(200, Json(std::move(out)).dump());
    }

    // GET /api/v1/playback/media?media_id=...  (also accepts item_id)
    // The server's half of the contract: what the media is, and what can be
    // done with it, before any instruction is given. No session, no
    // pipeline, no capability matching -- a client reads these facts and
    // then tells the server what to do (operator, 2026-09-07).
    HttpResponse media_facts(const HttpRequest& request) {
        if (!config.enabled) return http_error(503, "streaming_disabled", "streaming is disabled");
        if (!request.session)
            return http_error(401, "unauthorized", "a valid session bearer token is required");
        const auto find_query = [&](std::string_view key) {
            auto it = request.query.find(key);
            return it == request.query.end() ? std::string{} : it->second;
        };
        const auto media_id = find_query("media_id");
        const auto item_id = find_query("item_id");
        std::vector<std::string> media_ids;
        if (!media_id.empty()) {
            media_ids.push_back(media_id);
        } else if (!item_id.empty()) {
            media_ids = item_media(item_id);
            if (media_ids.empty())
                return http_error(404, "not_found", "no media representations for that item");
        } else {
            return http_error(400, "bad_request", "media_id or item_id is required");
        }

        const auto deadline = Clock::now() + config.probe_timeout;
        Json::Array reported;
        // A media this node could not read is a fact too, and a different one
        // from a media that does not exist. Report both, per media, and let
        // the client decide whether another node is worth asking.
        Json::Array unavailable;
        std::string last_error;
        std::string last_reason;
        for (const auto& id : media_ids) {
            try {
                auto lease = create_source(id);
                auto probe = probe_source(lease, "facts", deadline);
                Json::Array streams;
                for (const auto& stream : probe.streams) streams.emplace_back(stream_json(stream));
                // What this media supports, as a fact about the media and the
                // muxers, not about any client: direct is always the bytes;
                // a copy into a container depends on what that container can
                // carry; a transcode depends on the encoders being present.
                const auto* video = first_stream(probe, MediaStreamType::video);
                const auto* audio = first_stream(probe, MediaStreamType::audio);
                const auto engine_state = engine ? engine->status() : MediaEngineStatus{};
                Json::Object copy_fmp4{
                    {"video", !video || fmp4_video_copy_supported(lower(video->codec))},
                    {"audio", !audio || fmp4_audio_copy_supported(lower(audio->codec))}};
                Json::Object copy_mpegts{
                    {"video", !video || mpegts_video_copy_supported(lower(video->codec))},
                    {"audio", !audio || mpegts_audio_copy_supported(lower(audio->codec))}};
                Json::Object operations{
                    {"direct", true},
                    {"copy_into_fmp4", Json(std::move(copy_fmp4))},
                    {"copy_into_mpegts", Json(std::move(copy_mpegts))},
                    {"transcode_video", engine_state.h264_encoder},
                    {"transcode_audio", engine_state.aac_encoder}};
                Json::Object entry{
                    {"media_id", lease.media_id},
                    {"path", lease.path},
                    {"size", lease.entry.size},
                    {"container", source_container(probe, lease.path)},
                    {"format", probe.format},
                    {"duration_ms",
                     static_cast<uint64_t>(std::max(0.0, probe.duration_seconds) * 1000.0)},
                    {"bitrate", probe.bitrate},
                    {"streams", Json(std::move(streams))},
                    {"operations", Json(std::move(operations))}};
                reported.emplace_back(std::move(entry));
            } catch (const MediaError& e) {
                last_error = e.what();
                last_reason = media_failure_name(e.failure());
                unavailable.emplace_back(Json::Object{{"media_id", id},
                                                      {"reason", last_reason},
                                                      {"message", std::string(e.what())}});
            } catch (const std::exception& e) {
                last_error = e.what();
                unavailable.emplace_back(Json::Object{{"media_id", id},
                                                      {"reason", std::string("not_found")},
                                                      {"message", std::string(e.what())}});
            }
        }
        if (reported.empty()) {
            if (!last_reason.empty())
                return http_error(422, "facts_unavailable", last_error, last_reason);
            return http_error(404, "not_found",
                              last_error.empty() ? "media is not available" : last_error);
        }
        Json::Object out{{"media", Json(std::move(reported))}};
        if (!unavailable.empty()) out["unavailable"] = Json(std::move(unavailable));
        if (!item_id.empty()) out["item_id"] = item_id;
        return http_json(200, Json(std::move(out)).dump());
    }

    HttpResponse handle_api(const HttpRequest& request) {
        if (request.path == "/api/v1/playback/status" && request.method == "GET") return status();
        if (request.path == "/api/v1/playback/sessions") {
            // A POST to a collection creates a member. Every time: a session
            // is a resource, not a property of the bearer that asked for it.
            if (request.method == "POST") return create(request);
            if (request.method == "GET") return list_sessions(request);
            return http_error(405, "method", "GET or POST required");
        }
        constexpr std::string_view sessions_prefix = "/api/v1/playback/sessions/";
        if (request.path.starts_with(sessions_prefix)) {
            auto rest = std::string_view(request.path).substr(sessions_prefix.size());
            const auto slash = rest.find('/');
            const auto id = rest.substr(0, slash);
            if (id.empty()) return http_error(404, "not_found", "endpoint not found");
            if (slash == std::string_view::npos) {
                if (request.method == "GET") return get_session(id, request);
                if (request.method == "PATCH") return update_session(id, request);
                if (request.method == "DELETE") return erase_session(id, request);
                return http_error(405, "method", "GET, PATCH or DELETE required");
            }
            auto stream = parse_stream_route(request.path);
            if (!stream)
                return http_error(404, "not_found", "endpoint not found");
            return session_stream_response(request, stream->session_id, stream->stream_path);
        }
        if (request.path == "/api/v1/playback/media" && request.method == "GET")
            return media_facts(request);
        return http_error(404, "not_found", "endpoint not found");
    }

    void signal_cleanup_locked() {
        ++cleanup_revision;
        cleanup_cv.notify_all();
    }

    void cleanup(std::stop_token stop) {
        set_thread_name("macha-play-gc");
        while (!stop.stop_requested()) {
            std::vector<std::shared_ptr<Session>> expired;
            std::vector<std::pair<std::shared_ptr<Session>,
                                  std::shared_ptr<MediaEngineSession>>> idle_pipelines;
            std::optional<Clock::time_point> next_expiry;
            std::chrono::milliseconds idle_timeout{};
            std::chrono::milliseconds unused_idle_timeout{};
            bool reclaim_heap = false;
            {
                std::unique_lock lock(mutex);
                const auto now = Clock::now();
                idle_timeout = config.pipeline_idle;
                unused_idle_timeout = config.session_unused_idle;
                for (auto it = sessions.begin(); it != sessions.end();) {
                    // A session that has never served a stream object expires on
                    // the shorter clock. The transcode entitlement is held by the
                    // session rather than by the pipeline, so reclaiming the
                    // engine at pipeline_idle only makes an abandoned session
                    // cheap -- it goes on holding the slot until the session
                    // itself is erased, and with max_video_transcodes at 1 that
                    // closes the node to transcoding for the whole session_idle.
                    // Both clocks run from `touched`, so a client that is still
                    // talking to us -- polling the session, PATCHing a plan --
                    // is never evicted by this; only one that created a session
                    // and never came back for the media is.
                    // Clamped, not merely validated: an unused session must
                    // never outlive a used one, whatever the two knobs say.
                    // config_base rejects that ordering in a config file, but
                    // reconfigure() takes a StreamingConfig from callers that
                    // never went through it.
                    const auto idle_budget =
                        it->second->stream_served
                            ? config.session_idle
                            : std::min(config.session_unused_idle, config.session_idle);
                    const auto expires = it->second->touched + idle_budget;
                    if (now >= expires) {
                        if (!it->second->stream_served)
                            ++unused_sessions_reclaimed;
                        expired.push_back(it->second);
                        erase_idempotency_for_session_locked(it->first);
                        if (!it->second->logical_session->client_key.empty())
                            logical_sessions.erase(it->second->logical_session->client_key);
                        it = sessions.erase(it);
                    } else {
                        if (!next_expiry || expires < *next_expiry) next_expiry = expires;
                        const auto pipeline_expires =
                            it->second->stream_touched + idle_timeout;
                        {
                            std::lock_guard pipeline_lock(it->second->pipeline_mutex);
                            if (!it->second->active_stream_requests &&
                                it->second->engine_session &&
                                it->second->engine_session->running()) {
                                if (now >= pipeline_expires) {
                                    idle_pipelines.emplace_back(
                                        it->second, std::move(it->second->engine_session));
                                    ++idle_pipelines_reclaimed;
                                } else if (!next_expiry || pipeline_expires < *next_expiry) {
                                    next_expiry = pipeline_expires;
                                }
                            }
                        }
                        ++it;
                    }
                }

                if (heap_reclaim_pending) {
                    bool transformed_pipeline_active = false;
                    for (const auto& [_, session] : sessions) {
                        if (!transformed(session->plan))
                            continue;
                        std::lock_guard pipeline_lock(session->pipeline_mutex);
                        if (session->engine_session) {
                            transformed_pipeline_active = true;
                            break;
                        }
                    }
                    if (!transformed_pipeline_active) {
                        heap_reclaim_pending = false;
                        reclaim_heap = true;
                    }
                }

                if (expired.empty() && idle_pipelines.empty() && !reclaim_heap) {
                    const auto observed_revision = cleanup_revision;
                    const auto changed = [&] { return cleanup_revision != observed_revision; };
                    if (next_expiry)
                        cleanup_cv.wait_until(lock, stop, *next_expiry, changed);
                    else
                        cleanup_cv.wait(lock, stop, changed);
                    continue;
                }
            }
            for (auto& session : expired) {
                if (!session->stream_served)
                    Log::info("playback session reclaimed without ever being streamed session=" +
                              session->id + " idle_ms=" +
                              std::to_string(unused_idle_timeout.count()));
                stop_pipeline(*session);
                std::error_code ec;
                std::filesystem::remove_all(*config.temp_path / session->id, ec);
            }
            for (auto& [session, pipeline] : idle_pipelines) {
                pipeline->stop();
                pipeline.reset();
                if (transformed(session->plan))
                    request_heap_reclaim();
                std::error_code ec;
                std::filesystem::remove_all(session->generation_dir, ec);
                Log::info("playback pipeline reclaimed after stream inactivity session=" +
                          session->id + " idle_ms=" +
                          std::to_string(idle_timeout.count()));
            }
            if (reclaim_heap) {
                const bool released = release_free_process_heap_pages();
                {
                    std::lock_guard lock(mutex);
                    ++heap_reclaim_runs;
                    if (released)
                        ++heap_reclaim_successes;
                }
                Log::debug("playback post-transcode heap reclaim released=" +
                           std::to_string(released ? 1 : 0));
            }
        }
    }
};

PlaybackManager::PlaybackManager(FileSystem& fs, CatalogueManager& catalogue, CatalogueApiConfig api,
                                 StreamingConfig streaming, std::shared_ptr<MediaEngine> engine,
                                 std::function<size_t(const std::vector<std::string>&)> request_profiles,
                                 MediaInformationService* media_information)
    : impl_(std::make_unique<Impl>(fs, catalogue, std::move(api), std::move(streaming),
                                  std::move(engine), std::move(request_profiles),
                                  media_information)) {}

PlaybackManager::~PlaybackManager() { stop(); }

void PlaybackManager::start() {
    if (impl_->started || !impl_->config.enabled) return;
    std::filesystem::create_directories(*impl_->config.temp_path);
    if (!impl_->engine) throw std::runtime_error("streaming media engine is unavailable");
    auto status = impl_->engine->status();
    if (!status.available) throw std::runtime_error("streaming media engine is unavailable");
    impl_->cleanup_thread = std::jthread([this](std::stop_token stop) {
        run_supervised("playback-cleanup", [this, stop] { impl_->cleanup(stop); });
    });
    impl_->profile_publish_thread = std::jthread([this](std::stop_token stop) {
        run_supervised("playback-profile-publish", [this, stop] { impl_->publish_profiles(stop); });
    });
    impl_->started = true;
    Log::info("streaming enabled engine=" + status.backend + " version=" + status.version +
              " h264_encoder=" + std::string(status.h264_encoder ? "yes" : "no") +
              " aac_encoder=" + std::string(status.aac_encoder ? "yes" : "no"));
}

void PlaybackManager::stop() {
    if (!impl_ || !impl_->started) return;
    request_stop();
    if (impl_->cleanup_thread.joinable()) {
        impl_->cleanup_thread.join();
    }
    if (impl_->profile_publish_thread.joinable()) {
        impl_->profile_publish_thread.join();
    }
    std::vector<std::shared_ptr<Impl::Session>> sessions;
    {
        std::lock_guard lock(impl_->mutex);
        for (auto& [_, session] : impl_->sessions) sessions.push_back(session);
        impl_->sessions.clear();
        impl_->logical_sessions.clear();
        impl_->idempotent_creations.clear();
    }
    for (auto& session : sessions) impl_->stop_pipeline(*session);
    impl_->started = false;
}

void PlaybackManager::request_stop() {
    if (!impl_ || !impl_->started) return;
    if (impl_->cleanup_thread.joinable()) {
        impl_->cleanup_thread.request_stop();
        impl_->cleanup_cv.notify_all();
    }
    if (impl_->profile_publish_thread.joinable()) {
        impl_->profile_publish_thread.request_stop();
        impl_->profile_publish_cv.notify_all();
    }
}

void PlaybackManager::reconfigure(StreamingConfig config) {
    std::lock_guard lock(impl_->mutex);
    // Backend, probe, buffering and temp-path changes require a service restart.
    // Policy limits and playback timing apply to subsequent sessions immediately.
    impl_->config.max_sessions = config.max_sessions;
    impl_->config.max_sessions_per_account = config.max_sessions_per_account;
    impl_->config.max_video_transcodes = config.max_video_transcodes;
    impl_->config.max_audio_transcodes = config.max_audio_transcodes;
    impl_->config.session_idle = config.session_idle;
    impl_->config.session_unused_idle = config.session_unused_idle;
    impl_->config.pipeline_idle = config.pipeline_idle;
    impl_->config.startup_timeout = config.startup_timeout;
    impl_->config.segment_duration = config.segment_duration;
    impl_->config.max_ahead_segments = config.max_ahead_segments;
    impl_->config.segment_hold_window = config.segment_hold_window;
    impl_->config.max_session_holds = config.max_session_holds;
    impl_->config.max_concurrent_holds = config.max_concurrent_holds;
    impl_->config.segment_timeout = config.segment_timeout;
    impl_->segment_holds.reconfigure(config.max_session_holds, config.max_concurrent_holds);
    impl_->signal_cleanup_locked();
}

HttpResponse PlaybackManager::handle(const HttpRequest& request) {
    auto began = Clock::now();
    try {
        return impl_->handle_api(request);
    } catch (const JsonError& e) {
        return http_error(400, "bad_json", e.what());
    } catch (const PlaybackCapabilityError& e) {
        // Well-formed, coherent, and this node cannot do it: 422 rather than
        // 400, because nothing about the request is malformed. Another node on
        // another build may accept the same instruction, so the client should
        // ask one before giving up the copy -- and a different instruction
        // (a transcode) would succeed here.
        FailureAxes axes;
        axes.scope = FailureScope::node;
        axes.node_healthy = true;
        axes.alternative_may_succeed = true;
        return http_error(422, "copy_not_supported", e.what(), {}, axes);
    } catch (const std::invalid_argument& e) {
        // The request itself is wrong and every node would refuse it the same
        // way, so walking the cluster collects N copies of the caller's own
        // bug. Nothing else here could succeed either.
        FailureAxes axes;
        axes.scope = FailureScope::request;
        axes.node_healthy = true;
        axes.alternative_may_succeed = false;
        return http_error(400, "bad_playback_request", e.what(), {}, axes);
    } catch (const std::out_of_range& e) {
        return http_error(404, "not_found", e.what());
    } catch (const AccountSessionLimitError& e) {
        // Distinct from resource_limit on purpose. A node-wide or transcode
        // limit is this node's property and a client is right to try another;
        // an account cap is identical on every node in the cluster, so a
        // client that walks collects N identical refusals and charges N
        // healthy nodes on the way through. scope=request is what says "do not
        // walk" to a client that reads the axes rather than our error codes:
        // every node would refuse this the same way. It is the closest honest
        // value -- the request is not malformed, but the remedy is the
        // caller's, not another node's. A fourth `account` scope has been
        // proposed; it cannot ship before clients tolerate an unknown scope,
        // or the tolerance creates the condemnation it exists to prevent.
        Json::Object error{{"code", std::string("account_session_limit")},
                           {"message", std::string(e.what())},
                           {"scope", std::string("request")},
                           {"node_healthy", true},
                           {"alternative_may_succeed", false},
                           {"sessions", static_cast<uint64_t>(e.held())},
                           {"max_sessions", static_cast<uint64_t>(e.limit())}};
        Json::Object root;
        root["error"] = std::move(error);
        return http_json(429, Json(std::move(root)).dump());
    } catch (const ResourceLimitError& e) {
        return http_error(429, "resource_limit", e.what());
    } catch (const PlaybackStageError& e) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count();
        Log::warn("playback[" + e.trace() + "] request failed stage=" + e.stage() +
                  " method=" + request.method + " path=" + request.path +
                  " elapsed_ms=" + std::to_string(elapsed) + " error=" + e.what());
        // One envelope for every error this API returns: error.code is a
        // snake_case token a client may branch on, error.message is for a
        // human. Everything else about the failure hangs off the same object.
        // This path used to emit `error` as a bare string with message, trace
        // and stage as siblings, which is why clients grew parsers for
        // several shapes.
        Json::Object error{{"code", "playback_" + e.stage() + "_failed"},
                           {"message", std::string(e.what())},
                           {"trace", e.trace()},
                           {"stage", e.stage()}};
        // When the engine said why, say why. A source this node could not read
        // is a different situation for the client than one it could not parse,
        // and only the client can decide what to do about either.
        int status = 503;
        // A per-title fault leaves the node fit for every other title, so the
        // default here says so rather than letting a client charge the node's
        // health for one bad file. Scope is left unstated when the engine did
        // not say why: a guess is worse than a gap the client knows to handle.
        FailureAxes axes;
        axes.node_healthy = true;
        if (e.failure()) {
            error["reason"] = std::string(media_failure_name(*e.failure()));
            axes = media_failure_axes(*e.failure());
            if (*e.failure() == MediaFailure::unsupported) status = 422;
        }
        if (axes.scope) error["scope"] = std::string(failure_scope_name(*axes.scope));
        if (axes.node_healthy) error["node_healthy"] = *axes.node_healthy;
        if (axes.alternative_may_succeed)
            error["alternative_may_succeed"] = *axes.alternative_may_succeed;
        Json::Object body;
        body["error"] = std::move(error);
        auto response = http_json(status, Json(std::move(body)).dump());
        response.headers["X-Macha-Playback-Trace"] = e.trace();
        response.headers["X-Macha-Playback-Stage"] = e.stage();
        return response;
    } catch (const std::exception& e) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count();
        Log::warn("playback request failed method=" + request.method + " path=" + request.path +
                  " elapsed_ms=" + std::to_string(elapsed) + " error=" + e.what());
        return http_error(503, "playback_unavailable", e.what());
    }
}

bool PlaybackManager::capability_request(const HttpRequest& request) const {
    // The stream subresource authorises itself with the session token in its
    // path, so it is exempt from the bearer every other route requires. Same
    // parse the router uses, deliberately: see parse_stream_route.
    return parse_stream_route(request.path).has_value();
}

} // namespace macha
