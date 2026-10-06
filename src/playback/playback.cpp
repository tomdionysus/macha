// SPDX-License-Identifier: GPL-3.0-or-later
#include "playback/playback.hpp"
#include "api/paging.hpp"
#include "contract/thread_safety.hpp"
#include "diagnostics.hpp"

#include "crypto.hpp"
#include "playback/segment_holds.hpp"
#include "json.hpp"
#include "log.hpp"
#include "observation.hpp"
#include "supervised.hpp"
#include "macha_version.hpp"
#include "media/media_containers.hpp"
#include "catalogue/media_information.hpp"

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
#include <thread>
#include <unistd.h>
#include <utility>

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

std::string source_container(const MediaProbeResult& probe, std::string_view path) {
    return container_for_format(probe.format, path);
}

// Content-Type for serving the source unchanged.
std::string source_mime(const MediaProbeResult& probe, std::string_view path) {
    const bool picture = std::any_of(probe.streams.begin(), probe.streams.end(), [](const auto& stream) {
        return stream.type == MediaStreamType::video && !stream.attached_picture;
    });
    return direct_mime(source_container(probe, path), picture);
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

// A well-formed instruction this node cannot carry out (a stream copy into a
// container that cannot hold the codec). Another node may accept it, so it
// reaches the client as 422 copy_not_supported, scope=node,
// alternative_may_succeed=true.
class PlaybackCapabilityError final : public std::invalid_argument {
  public:
    using std::invalid_argument::invalid_argument;
};

// The instruction leaves a choice open or names something the media lacks; the
// server never fills the gap. One candidate is used; otherwise the request is
// refused as 400 with `choice` naming what is open and `choices` listing the
// candidates. Every node would answer the same.
class PlaybackChoiceError final : public std::invalid_argument {
  public:
    PlaybackChoiceError(std::string code, std::string choice, const std::string& message,
                        Json::Array choices)
        : std::invalid_argument(message), code_(std::move(code)), choice_(std::move(choice)),
          choices_(std::move(choices)) {}
    const std::string& code() const noexcept { return code_; }
    const std::string& choice() const noexcept { return choice_; }
    const Json::Array& choices() const noexcept { return choices_; }

  private:
    std::string code_;
    std::string choice_;
    Json::Array choices_;
};

// An account cap is identical on every node, so unlike node limits a client
// must not walk the cluster on it. It carries its own code, the limit and the
// current count rather than the generic resource_limit envelope.
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

class AccountTranscodeLimitError final : public std::runtime_error {
public:
    AccountTranscodeLimitError(size_t held, size_t limit)
        : std::runtime_error("account transcode limit reached: holding " + std::to_string(held) +
                             " of " + std::to_string(limit) + " on this node"),
          held_(held), limit_(limit) {}
    size_t held() const noexcept { return held_; }
    size_t limit() const noexcept { return limit_; }

private:
    size_t held_;
    size_t limit_;
};

// The single parse of a stream URL, used by both the router and the
// authentication exemption. They must never disagree: a path the exemption
// treats as a stream but the router sends elsewhere is an authentication bypass.
struct StreamRoute {
    std::string_view session_id;
    std::string_view stream_path;
};

// A requested generation that is not the current one. Below current it was
// replaced (seek, track or quality change, media switch): 410, the client takes
// the new stream.url from the session. Above current it never existed: 404.
// Generations only increment, so the comparison is the whole test.
// Axes: scope=request (no other node has this session, so do not walk),
// node healthy, and a different request (the new stream.url) succeeds here.
HttpResponse generation_gone(uint64_t requested, uint64_t current) {
    if (requested < current)
        return http_error(410, "generation_superseded", "stream generation superseded",
                          "the generation was replaced; take stream.url from the session",
                          FailureAxes{FailureScope::request, true, true});
    return http_error(404, "not_found", "stream generation not found");
}

// The stream token is a capability: compare in constant time. A length
// mismatch is not secret.
bool stream_token_matches(const std::string& expected, const std::string& provided) {
    return constant_time_equal(
        {reinterpret_cast<const uint8_t*>(expected.data()), expected.size()},
        {reinterpret_cast<const uint8_t*>(provided.data()), provided.size()});
}

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
    // The engine's reason it could not read the source; passed to the client
    // unchanged, never acted on.
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
    // Resident for the body's lifetime, so the server sends straight from it.
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
    // Required; the server never chooses: "direct" (source over byte ranges),
    // "remux" (copy into HLS) or "transcode" (re-encode).
    std::string mode;
    // Per-stream overrides of `mode`: "copy" or "transcode".
    std::optional<std::string> video;
    std::optional<std::string> audio;
    // HLS segment container, "fmp4" or "mpegts". Required for remux and
    // transcode: a default would be a choice.
    std::string container;
    std::optional<int> max_height;
    std::optional<uint64_t> max_bitrate;
    std::optional<int> video_stream;
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
        // Naming `mode` restates the whole transform: the previous mode's
        // per-stream and quality instructions do not outlive it, so the server
        // never refuses a combination it assembled from session history. An
        // update naming both sets both (these are read after the mode). Same
        // rule as without_mode_overrides.
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
    if (auto v = value->find("video_stream")) current.video_stream = optional_int(v);
    if (auto v = value->find("audio_stream")) current.audio_stream = optional_int(v);
    if (auto v = value->find("subtitle_stream")) current.subtitle_stream = optional_int(v);
    if (auto v = value->find("audio_language"); v && v->isString()) current.audio_language = lower(v->asString());
    if (auto v = value->find("subtitle_language"); v && v->isString()) current.subtitle_language = lower(v->asString());
    if (current.max_height && *current.max_height <= 0) throw std::invalid_argument("preferences.max_height must be positive");
    if (current.max_bitrate && *current.max_bitrate == 0) throw std::invalid_argument("preferences.max_bitrate must be positive");
    if (current.video_stream && *current.video_stream < 0) throw std::invalid_argument("preferences.video_stream must be non-negative");
    if (current.audio_stream && *current.audio_stream < 0) throw std::invalid_argument("preferences.audio_stream must be non-negative");
    if (current.subtitle_stream && *current.subtitle_stream < 0) throw std::invalid_argument("preferences.subtitle_stream must be non-negative");
    return current;
}

// Streams of a type a plan may use; an attached picture is artwork, not a stream.
std::vector<const MediaStreamInfo*> streams_of(const MediaProbeResult& probe, MediaStreamType type) {
    std::vector<const MediaStreamInfo*> out;
    for (const auto& stream : probe.streams)
        if (stream.type == type && !stream.attached_picture) out.push_back(&stream);
    return out;
}

[[noreturn]] void refuse_stream_choice(std::string code, MediaStreamType type, const std::string& message,
                                       const std::vector<const MediaStreamInfo*>& candidates) {
    Json::Array choices;
    for (const auto* stream : candidates) choices.emplace_back(stream->index);
    throw PlaybackChoiceError(std::move(code), std::string(media_stream_type_name(type)) + "_stream",
                              message, std::move(choices));
}

// The stream the instruction names: an index, a language matching exactly one
// stream, or the only stream of the type. Otherwise refused with the
// candidates; the server never chooses or substitutes another language.
// `none_is_an_answer`: no instruction means "none" (subtitles, and direct
// play, where the player picks its own tracks).
const MediaStreamInfo* chosen_stream(const MediaProbeResult& probe, MediaStreamType type,
                                     const std::optional<int>& index, std::string_view language,
                                     bool none_is_an_answer) {
    const auto candidates = streams_of(probe, type);
    const std::string kind = media_stream_type_name(type);
    if (index) {
        for (const auto* stream : candidates)
            if (stream->index == *index) return stream;
        refuse_stream_choice("choice_not_available", type,
                             "preferences." + kind + "_stream " + std::to_string(*index) + " is not a " +
                                 kind + " stream of this media",
                             candidates);
    }
    if (!language.empty()) {
        std::vector<const MediaStreamInfo*> matches;
        for (const auto* stream : candidates)
            if (lower(stream->language) == language) matches.push_back(stream);
        if (matches.size() == 1) return matches.front();
        if (matches.empty())
            refuse_stream_choice("choice_not_available", type,
                                 "this media has no " + kind + " stream in language " + std::string(language),
                                 candidates);
        refuse_stream_choice("choice_required", type,
                             std::to_string(matches.size()) + " " + kind + " streams are in language " +
                                 std::string(language) + ": name one with preferences." + kind + "_stream",
                             matches);
    }
    if (candidates.size() == 1) return candidates.front();
    if (candidates.empty() || none_is_an_answer) return nullptr;
    refuse_stream_choice("choice_required", type,
                         "this media has " + std::to_string(candidates.size()) + " " + kind +
                             " streams: name one with preferences." + kind + "_stream",
                         candidates);
}

// Executes the client's instruction against the media's facts. Client
// capabilities are deliberately not a parameter: the server does not choose.
PlaybackPlan plan_for(const MediaProbeResult& probe, const PlaybackPreferences& prefs) {
    if (prefs.mode != "direct" && prefs.mode != "remux" && prefs.mode != "transcode")
        throw std::invalid_argument(
            "preferences.mode is required and must be direct, remux or transcode: the server "
            "reports what the media is and performs what it is asked for, it does not choose");
    if (streams_of(probe, MediaStreamType::video).empty() && streams_of(probe, MediaStreamType::audio).empty())
        throw std::runtime_error("media contains no playable audio or video stream");
    // Direct: the player picks its own tracks unless the client names one.
    const bool untouched = prefs.mode == "direct";
    auto video = chosen_stream(probe, MediaStreamType::video, prefs.video_stream, {}, untouched);
    auto audio = chosen_stream(probe, MediaStreamType::audio, prefs.audio_stream, prefs.audio_language,
                               untouched);
    const MediaStreamInfo* subtitle = nullptr;
    if (prefs.subtitle_stream || !prefs.subtitle_language.empty())
        subtitle = chosen_stream(probe, MediaStreamType::subtitle, prefs.subtitle_stream,
                                 prefs.subtitle_language, true);
    if (subtitle && !webvtt_subtitle_supported(*subtitle))
        throw std::invalid_argument("requested subtitle stream cannot be converted to WebVTT");

    PlaybackPlan plan;
    plan.video_stream = video ? video->index : -1;
    plan.audio_stream = audio ? audio->index : -1;
    plan.subtitle_stream = subtitle ? subtitle->index : -1;
    plan.video = video ? MediaTransform::copy : MediaTransform::omit;
    plan.audio = audio ? MediaTransform::copy : MediaTransform::omit;
    plan.video_codec = video ? lower(video->codec) : std::string{};
    plan.audio_codec = audio ? lower(audio->codec) : std::string{};

    // Direct is the source over byte ranges; a re-encode or quality
    // instruction alongside it is refused, not ignored.
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

    // HLS: the segment container has no default.
    if (prefs.container.empty())
        throw PlaybackChoiceError("choice_required", "container",
                                  "preferences.container is required for remux and transcode: fmp4 or mpegts",
                                  Json::Array{Json("fmp4"), Json("mpegts")});
    plan.container = prefs.container == "mpegts" ? MediaContainer::mpegts : MediaContainer::fmp4;

    const auto source_video_codec = video ? lower(video->codec) : std::string{};
    const auto source_audio_codec = audio ? lower(audio->codec) : std::string{};
    // remux copies both streams, transcode re-encodes both;
    // preferences.video / preferences.audio override either.
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

    // The mode must describe what is done: remux copies every stream,
    // transcode re-encodes at least one. A mismatch is refused, not
    // reinterpreted.
    const bool re_encoding = (video && !video_copy) || (audio && !audio_copy);
    if (prefs.mode == "remux" && re_encoding)
        throw std::invalid_argument(
            "remux repackages and copies every stream: to re-encode one, ask for mode=transcode "
            "with video=copy or audio=copy for the stream that is being copied");
    if (prefs.mode == "transcode" && !re_encoding)
        throw std::invalid_argument(
            "transcode re-encodes at least one stream: to copy both into a new container, ask "
            "for mode=remux");

    // What the segment container can carry: a fact of media and muxer.
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
                     {"video_stream", preferences.video_stream ? Json(*preferences.video_stream) : Json(nullptr)},
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
                     // What the client is handed, named as the facts
                     // endpoint names a source container.
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
            // What the encoder emits: 8-bit 4:2:0 H.264 High, SDR transfer.
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
            // The encoder keeps the source channel layout; no downmix.
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
        // Held across a whole create, update or teardown of the viewer's
        // session: probing, VOD planning, pipeline start and stop. Guards no
        // state; it keeps those operations serial.
        mutable IoMutex operation_mutex;
        std::string client_key; // set before the session is shared
        // Guarded by Impl::mutex (not expressible to the analysis).
        bool video_transcode_entitled{};
        bool audio_transcode_entitled{};
    };

    struct StartState;
    struct PendingReplacement;

    struct SourceLease {
        std::string media_id;
        std::string path;
        FsEntry entry;
    };

    struct Session {
        explicit Session(Clock::time_point created) : touched(created), stream_touched(created) {}
        std::string id;
        std::string token;
        PlaybackPreferences preferences;
        MediaSource source;
        FsEntry source_entry;
        MediaProbeResult probe;
        PlaybackPlan plan;
        std::optional<HlsVodPlan> vod_plan;
        uint64_t generation{};
        std::filesystem::path generation_dir;
        mutable Mutex pipeline_mutex;
        std::shared_ptr<MediaEngineSession> engine_session MACHA_GUARDED_BY(pipeline_mutex);
        std::string stream_url;
        struct SubtitleCache {
            // Held across WebVTT extraction from the source (media reads that
            // may fetch from peers) and a log line.
            IoMutex mutex;
            std::map<std::pair<int, uint64_t>, std::string> segments MACHA_GUARDED_BY(mutex);
            std::deque<std::pair<int, uint64_t>> order MACHA_GUARDED_BY(mutex);
            size_t bytes MACHA_GUARDED_BY(mutex){};
            static constexpr size_t max_entries = 256;
            static constexpr size_t max_bytes = 4ULL * 1024 * 1024;
        };
        std::string subtitle_url;
        std::shared_ptr<SubtitleCache> subtitle_cache{std::make_shared<SubtitleCache>()};
        // Both on the manager's TimeSource: the idle clocks run from them.
        Clock::time_point touched;
        Clock::time_point stream_touched;
        // Whether any stream object (playlist, fragment, subtitle, direct body)
        // was ever served. stream_touched is reset by start_pipeline(), so it
        // cannot say "never". Never cleared: a used session keeps the full
        // session_idle across seeks and quality changes.
        bool stream_served{false};
        size_t active_stream_requests{};
        std::shared_ptr<LogicalViewerSession> logical_session;
        // The owning account: the collection listing filters on it and the
        // per-account cap counts it. Recorded, not derived from the requester.
        std::string account;
        // `start=async` progress: on the placeholder while pending, then on
        // the session that replaces it.
        std::shared_ptr<StartState> start;
        // A replacement generation an async PATCH is starting while this one
        // keeps serving. Guarded by the Impl mutex.
        std::shared_ptr<PendingReplacement> pending;
    };

    // Adds the cache's usage unless an extraction holds it, so status() never
    // waits on one.
    static void subtitle_cache_usage(Session::SubtitleCache& cache, size_t& segments,
                                     size_t& bytes) {
        if (!cache.mutex.try_lock()) return;
        Lock lock(cache.mutex, std::adopt_lock);
        segments += cache.segments.size();
        bytes += cache.bytes;
    }

    // An async start. The session map holds the admitted placeholder (counted
    // against every cap, owned, deletable) while a worker builds the real
    // session and swaps it in once its first fragment exists. GET and
    // long-polls read this, never the half-built session.
    struct StartState {
        Mutex mutex;
        std::string stage MACHA_GUARDED_BY(mutex){"planning"};
        const Clock::time_point started{Clock::now()};
        Clock::time_point last_change MACHA_GUARDED_BY(mutex){Clock::now()};
        uint64_t seq MACHA_GUARDED_BY(mutex){};
        std::optional<uint64_t> source_bytes_read MACHA_GUARDED_BY(mutex);
        std::optional<int64_t> preroll_decoded_ms MACHA_GUARDED_BY(mutex);
        std::optional<int64_t> preroll_total_ms MACHA_GUARDED_BY(mutex);
        std::optional<int64_t> output_media_ms MACHA_GUARDED_BY(mutex);
        std::optional<int64_t> first_fragment_ms MACHA_GUARDED_BY(mutex);
        std::optional<Json> error MACHA_GUARDED_BY(mutex);
        std::vector<std::shared_ptr<HttpWaker>> waiters MACHA_GUARDED_BY(mutex);
        std::atomic_bool cancelled{};
        std::shared_ptr<Session> candidate; // set before the start is shared
        bool finished() const MACHA_REQUIRES(mutex) { return stage == "ready" || stage == "failed"; }
        std::vector<std::shared_ptr<HttpWaker>> changed_locked() MACHA_REQUIRES(mutex) {
            ++seq;
            last_change = Clock::now();
            return std::exchange(waiters, {});
        }
    };
    struct ResourceReservation;
    // An async PATCH's replacement: its start, the resources reserved for it
    // (committed at the swap, rolled back by its worker otherwise), and when
    // its worker is done.
    struct PendingReplacement {
        // These three are set before the replacement is shared.
        std::shared_ptr<StartState> start;
        std::shared_ptr<ResourceReservation> reservation;
        bool needs_plan{};
        // Guarded by Impl::mutex (not expressible to the analysis).
        Clock::time_point failed_until{};
        Mutex done_mutex;
        std::condition_variable done_cv;
        bool done MACHA_GUARDED_BY(done_mutex){};
    };
    struct FailedStart {
        std::string account;
        Json payload;
        Clock::time_point expires;
    };

    FileSystem& fs;
    // Session idle clocks and failed-start retention; elapsed times that are
    // only reported stay on the real clock.
    const TimeSource& time;
    TranscodeRateBook& transcode_rates;
    RetainedMemoryLedger& retained_memory;
    CatalogueManager& catalogue;
    // reconfigure() changes the live limits and timings under mutex.
    StreamingConfig config MACHA_GUARDED_BY(mutex);
    // Node-global fairness and memory bound across all sessions.
    SegmentHoldArbiter segment_holds{config.max_session_holds, config.max_concurrent_holds};
    // Parked across a held segment request's deferral; the hold is released
    // when the request is answered or its connection goes away.
    struct HeldRequest {
        SegmentHoldArbiter::Hold hold;
        explicit HeldRequest(SegmentHoldArbiter::Hold admitted) : hold(std::move(admitted)) {}
    };
    std::shared_ptr<MediaEngine> engine;
    std::jthread cleanup_thread;
    std::jthread profile_publish_thread;
    // Held across log lines (probe cache hits, entitlement releases).
    mutable IoMutex mutex;
    std::condition_variable_any cleanup_cv;
    uint64_t cleanup_revision MACHA_GUARDED_BY(mutex){};
    std::map<std::string, std::shared_ptr<Session>, std::less<>> sessions MACHA_GUARDED_BY(mutex);
    // Client keys are advisory local handles, never cluster ownership. Weak
    // values leave no state behind an expired or deleted logical session.
    std::map<std::string, std::weak_ptr<LogicalViewerSession>, std::less<>> logical_sessions
        MACHA_GUARDED_BY(mutex);
    // In-flight creations per account, so concurrent creates cannot race past
    // the cap. Emptied as each create settles.
    std::map<std::string, size_t, std::less<>> pending_by_account MACHA_GUARDED_BY(mutex);
    std::map<std::string, MediaProbeResult, std::less<>> probe_cache MACHA_GUARDED_BY(mutex);
    size_t probe_cache_bytes MACHA_GUARDED_BY(mutex){};
    static constexpr size_t max_probe_cache_entries = 512;
    static constexpr size_t max_probe_cache_bytes = 8ULL * 1024 * 1024;
    struct ProbeFlight {
        Mutex mutex;
        std::condition_variable cv;
        bool complete MACHA_GUARDED_BY(mutex){};
        std::optional<MediaProbeResult> result MACHA_GUARDED_BY(mutex);
        std::exception_ptr error MACHA_GUARDED_BY(mutex);
    };
    std::map<std::string, std::shared_ptr<ProbeFlight>, std::less<>> probe_flights
        MACHA_GUARDED_BY(mutex);
    Mutex profile_publish_mutex;
    std::condition_variable_any profile_publish_cv;
    std::map<std::string, MediaProbeResult, std::less<>> pending_profile_publications
        MACHA_GUARDED_BY(profile_publish_mutex);
    size_t pending_profile_publication_bytes MACHA_GUARDED_BY(profile_publish_mutex){};
    static constexpr size_t max_pending_profile_publications = 128;
    static constexpr size_t max_pending_profile_publication_bytes = 4ULL * 1024 * 1024;
    std::map<std::string, HlsVodPlan, std::less<>> vod_plan_cache MACHA_GUARDED_BY(mutex);
    std::deque<std::string> vod_plan_cache_order MACHA_GUARDED_BY(mutex);
    static constexpr size_t max_vod_plan_cache_entries = 64;
    // Admission precedes visibility in `sessions`, so slots are reserved
    // explicitly: concurrent POST/PATCH cannot all pass the same limit check.
    size_t pending_sessions MACHA_GUARDED_BY(mutex){};
    size_t reserved_video_transcodes MACHA_GUARDED_BY(mutex){};
    size_t reserved_audio_transcodes MACHA_GUARDED_BY(mutex){};
    // Transcode entitlements reserved per account and not yet committed.
    std::map<std::string, size_t, std::less<>> reserved_account_transcodes MACHA_GUARDED_BY(mutex);
    // Transcoding pipelines running now: the concurrency a rate observation
    // was taken at.
    std::atomic<uint32_t> running_transcodes{};
    uint64_t idle_pipelines_reclaimed MACHA_GUARDED_BY(mutex){};
    uint64_t unused_sessions_reclaimed MACHA_GUARDED_BY(mutex){};
    bool heap_reclaim_pending MACHA_GUARDED_BY(mutex){};
    uint64_t heap_reclaim_requests MACHA_GUARDED_BY(mutex){};
    uint64_t heap_reclaim_runs MACHA_GUARDED_BY(mutex){};
    uint64_t heap_reclaim_successes MACHA_GUARDED_BY(mutex){};
    bool started{}; // the owner's, through start() and stop()
    std::function<size_t(const std::vector<std::string>&)> request_media_profiles;
    MediaInformationService* media_information{};
    // Set at construction, before any request.
    PlaybackManager::MediaFacts extra_media_facts;

    static size_t probe_resident_weight(const MediaProbeResult& probe) {
        size_t bytes = sizeof(probe) + probe.format.capacity();
        for (const auto& stream : probe.streams) {
            bytes += sizeof(stream) + stream.codec.capacity() + stream.profile.capacity() +
                     stream.language.capacity();
        }
        return bytes;
    }

    // A bounded retry cache, not durable state: a broken publisher must not
    // make it grow without limit.
    void queue_profile_publication(std::string media_id, MediaProbeResult probe)
        MACHA_REQUIRES(profile_publish_mutex) {
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

    // Bounded by count and bytes, independent of catalogue size; safely
    // repopulated.
    void cache_probe(std::string key, MediaProbeResult probe) MACHA_REQUIRES(mutex) {
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
        Mutex mutex;
        std::condition_variable cv;
        std::string fingerprint; // set before the creation is shared
        bool complete MACHA_GUARDED_BY(mutex){};
        std::string session_id MACHA_GUARDED_BY(mutex);
        std::exception_ptr error MACHA_GUARDED_BY(mutex);
    };
    std::map<std::string, std::shared_ptr<IdempotentCreation>, std::less<>> idempotent_creations
        MACHA_GUARDED_BY(mutex);
    // Failed async starts, readable until they expire.
    std::map<std::string, FailedStart, std::less<>> failed_starts MACHA_GUARDED_BY(mutex);
    // Start workers still running, so stop() can wait for them.
    size_t start_workers MACHA_GUARDED_BY(mutex){};
    std::condition_variable_any start_workers_cv;

    Impl(FileSystem& filesystem, TranscodeRateBook& rates, RetainedMemoryLedger& memory,
         CatalogueManager& cat, CatalogueApiConfig api, StreamingConfig streaming,
         std::shared_ptr<MediaEngine> media_engine,
         std::function<size_t(const std::vector<std::string>&)> request_profiles,
         MediaInformationService* information, const TimeSource& time_source)
        : fs(filesystem), time(time_source), transcode_rates(rates), retained_memory(memory), catalogue(cat), config(std::move(streaming)),
          engine(media_engine ? std::move(media_engine) :
                (config.enabled ? make_libav_media_engine(config) : nullptr)),
          request_media_profiles(std::move(request_profiles)), media_information(information) {
        (void)api;
    }

    // A copy of the configuration; reconfigure() may change it at any time.
    StreamingConfig current_config() const MACHA_EXCLUDES(mutex) {
        Lock lock(mutex);
        return config;
    }

    // A broken generation, not merely an unready one. 503 deliberately shares
    // infrastructure's "service down" status: either way the client should go
    // elsewhere, which leaves 500 free for the hold.
    static HttpResponse stream_failed(std::string_view detail) {
        return http_error(503, "stream_failed", detail);
    }

    // Not made yet, not absent. Never 404: the playlist promises the object, an
    // intermediary may cache the miss, and some players treat it as terminal.
    // 500 because hls.js exposes only the status on a fragment error, so the
    // status is the discriminator and must be one infrastructure does not emit
    // for a dead node (503 would make a dead node look like a hold and stall
    // forever; a proxy's 500 misread as a hold costs only a retry). 5xx, since a
    // 4xx stops hls.js retrying. Retry-After and no-store stop an intermediary
    // making the answer durable.
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
        // The token is a capability in the path, before the part it
        // authorises: media players fetch segments without application headers.
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
        std::string_view media_id,
        const PlaybackPreferences& prefs,
        const std::optional<int64_t>& seek_ms,
        std::string_view session_id) const {
        std::ostringstream canonical;
        canonical << "v3|media=" << media_id
                  << "|video=" << prefs.video.value_or("-")
                  << "|audio=" << prefs.audio.value_or("-")
                  << "|vs=" << prefs.video_stream.value_or(-1)
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

    // The live pending replacement, dropping one whose failure has been
    // readable long enough.
    std::shared_ptr<PendingReplacement> pending_locked(Session& session) MACHA_REQUIRES(mutex) {
        if (session.pending && session.pending->failed_until != Clock::time_point{} &&
            time.now() >= session.pending->failed_until)
            session.pending.reset();
        return session.pending;
    }

    // Stops a pending replacement and waits for its worker to roll its
    // reservation back, so the next admission sees the slot free.
    void cancel_pending(Session& session) {
        std::shared_ptr<PendingReplacement> taken;
        {
            Lock lock(mutex);
            taken = std::exchange(session.pending, {});
        }
        if (!taken) return;
        taken->start->cancelled.store(true);
        stop_pipeline(*taken->start->candidate);
        Lock done(taken->done_mutex);
        taken->done_cv.wait(done.native(), [&]() MACHA_REQUIRES(taken->done_mutex) { return taken->done; });
    }

    static void wake(const std::vector<std::shared_ptr<HttpWaker>>& waiters) {
        for (const auto& waiter : waiters) waiter->fire();
    }

    static Json start_json_locked(const StartState& start) MACHA_REQUIRES(start.mutex) {
        const auto now = Clock::now();
        Json::Object out{{"stage", start.stage},
                         {"progress_seq", start.seq},
                         {"progress_age_ms", static_cast<uint64_t>(std::max<int64_t>(
                              0, std::chrono::duration_cast<std::chrono::milliseconds>(now - start.last_change).count()))},
                         {"elapsed_ms", static_cast<uint64_t>(std::max<int64_t>(
                              0, std::chrono::duration_cast<std::chrono::milliseconds>(now - start.started).count()))}};
        if (start.source_bytes_read) out["source_bytes_read"] = *start.source_bytes_read;
        if (start.preroll_total_ms) {
            out["preroll_decoded_ms"] = static_cast<int64_t>(start.preroll_decoded_ms.value_or(0));
            out["preroll_total_ms"] = static_cast<int64_t>(*start.preroll_total_ms);
        }
        if (start.output_media_ms) out["output_media_ms"] = static_cast<int64_t>(*start.output_media_ms);
        if (start.first_fragment_ms) out["first_fragment_ms"] = static_cast<int64_t>(*start.first_fragment_ms);
        if (start.error) out["error"] = *start.error;
        return Json(std::move(out));
    }

    // A session whose async start is not ready: admitted facts, start progress
    // and the close URL; no stream URL until ready.
    Json pending_json(const Session& placeholder, StartState& start) const {
        auto payload = session_json(placeholder);
        payload["stream"]["url"] = Json(nullptr);
        payload["stream"]["close_url"] = public_stream_prefix(placeholder) + "/close";
        Lock lock(start.mutex);
        payload["start"] = start_json_locked(start);
        return payload;
    }

    static bool start_pending(const Session& session) {
        if (!session.start) return false;
        Lock lock(session.start->mutex);
        return !session.start->finished();
    }

    void prune_failed_starts_locked() MACHA_REQUIRES(mutex) {
        const auto now = time.now();
        std::erase_if(failed_starts, [&](const auto& entry) { return entry.second.expires <= now; });
    }

    std::shared_ptr<Session> copy_for_start(const Session& admitted) const {
        auto copy = std::make_shared<Session>(time.now());
        copy->id = admitted.id;
        copy->token = admitted.token;
        copy->preferences = admitted.preferences;
        copy->source = admitted.source;
        copy->source_entry = admitted.source_entry;
        copy->probe = admitted.probe;
        copy->plan = admitted.plan;
        copy->touched = admitted.touched;
        copy->logical_session = admitted.logical_session;
        copy->account = admitted.account;
        return copy;
    }

    static Json start_error_json(std::exception_ptr error, const std::string& stalled) {
        try {
            std::rethrow_exception(error);
        } catch (const PlaybackStageError& e) {
            Json::Object out{{"code", "playback_" + e.stage() + "_failed"},
                             {"message", std::string(e.what())},
                             {"trace", e.trace()},
                             {"stage", e.stage()},
                             {"start_stage", stalled}};
            if (e.failure()) {
                out["reason"] = std::string(media_failure_name(*e.failure()));
                const auto axes = media_failure_axes(*e.failure());
                if (axes.scope) out["scope"] = std::string(failure_scope_name(*axes.scope));
                if (axes.node_healthy) out["node_healthy"] = *axes.node_healthy;
                if (axes.alternative_may_succeed)
                    out["alternative_may_succeed"] = *axes.alternative_may_succeed;
            }
            return Json(std::move(out));
        } catch (const std::exception& e) {
            return Json(Json::Object{{"code", std::string("playback_unavailable")},
                                     {"message", std::string(e.what())},
                                     {"start_stage", stalled}});
        }
    }

    // Waits for the first fragment, publishing engine progress; fails only
    // when nothing has moved for startup_no_progress.
    void wait_for_first_fragment_async(Session& session, StartState& start, std::string_view trace)
        MACHA_EXCLUDES(mutex) {
        auto active = active_engine(session);
        if (!active) throw std::runtime_error("media pipeline did not start");
        auto store = active->segments();
        const auto* progress = active->start_progress();
        std::optional<uint64_t> seen;
        while (true) {
            if (start.cancelled.load()) throw std::runtime_error("start cancelled");
            const bool ready = store->wait_ready(std::chrono::milliseconds(250));
            const auto no_progress = current_config().startup_no_progress;
            std::vector<std::shared_ptr<HttpWaker>> waiters;
            bool stalled = false;
            {
                Lock lock(start.mutex);
                if (progress) {
                    const auto seq = progress->seq.load(std::memory_order_relaxed);
                    if (seq != seen) {
                        seen = seq;
                        start.source_bytes_read = progress->source_bytes_read.load(std::memory_order_relaxed);
                        const auto total = progress->preroll_total_us.load(std::memory_order_relaxed);
                        const auto output = progress->output_media_us.load(std::memory_order_relaxed);
                        if (total >= 0) {
                            start.preroll_total_ms = total / 1000;
                            start.preroll_decoded_ms =
                                progress->preroll_decoded_us.load(std::memory_order_relaxed) / 1000;
                        }
                        start.output_media_ms = output / 1000;
                        start.stage = total >= 0 && output == 0 ? "preroll" : "encoding";
                        waiters = start.changed_locked();
                    }
                }
                stalled = !ready && Clock::now() - start.last_change > no_progress;
            }
            wake(waiters);
            if (ready) return;
            const auto state = store->snapshot();
            if (!state.error.empty())
                throw stage_error(std::string(trace), "pipeline_start",
                                  std::runtime_error("libav pipeline failed before first fragment: " + state.error));
            if (!active->running())
                throw stage_error(std::string(trace), "pipeline_start",
                                  std::runtime_error("libav pipeline ended before first fragment"));
            if (stalled)
                throw stage_error(std::string(trace), "pipeline_start",
                                  std::runtime_error("no start progress for " +
                                                     std::to_string(no_progress.count()) + " ms"));
        }
    }

    // Start worker: plan, start, wait for the first fragment, then swap the
    // built session in for the placeholder, or record the failure and release
    // everything.
    void run_start(std::shared_ptr<Session> placeholder, std::shared_ptr<StartState> start,
                   std::string trace) {
        auto candidate = start->candidate;
        std::string stalled = "planning";
        try {
            prepare_transformed_vod(*candidate, trace);
            if (start->cancelled.load()) throw std::runtime_error("start cancelled");
            std::vector<std::shared_ptr<HttpWaker>> waiters;
            {
                Lock lock(start->mutex);
                if (candidate->vod_plan && !candidate->vod_plan->segment_durations.empty())
                    start->first_fragment_ms =
                        std::llround(candidate->vod_plan->segment_durations.front() * 1000.0);
                start->stage = "encoding";
                stalled = "encoding";
                waiters = start->changed_locked();
            }
            wake(waiters);
            start_pipeline(*candidate, trace, false);
            wait_for_first_fragment_async(*candidate, *start, trace);
            {
                Lock lock(mutex);
                auto it = sessions.find(candidate->id);
                if (start->cancelled.load() || it == sessions.end() || it->second != placeholder)
                    throw std::runtime_error("start cancelled");
                it->second = candidate;
                signal_cleanup_locked();
            }
            {
                Lock lock(start->mutex);
                start->stage = "ready";
                waiters = start->changed_locked();
            }
            wake(waiters);
            observations().record("playback.start_ready_us", elapsed_us(start->started));
            Log::info("playback[" + trace + "] async start ready id=" + candidate->id + " elapsed_ms=" +
                      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         Clock::now() - start->started).count()));
        } catch (...) {
            const auto error = std::current_exception();
            stop_pipeline(*candidate);
            std::error_code ec;
            if (!candidate->generation_dir.empty()) std::filesystem::remove_all(candidate->generation_dir, ec);
            if (start->cancelled.load()) {
                std::vector<std::shared_ptr<HttpWaker>> waiters;
                {
                    Lock lock(start->mutex);
                    waiters = std::exchange(start->waiters, {});
                }
                wake(waiters);
            } else {
                std::vector<std::shared_ptr<HttpWaker>> waiters;
                Json payload;
                {
                    Lock lock(start->mutex);
                    start->error = start_error_json(error, stalled);
                    start->stage = "failed";
                    waiters = start->changed_locked();
                }
                payload = pending_json(*placeholder, *start);
                {
                    Lock lock(mutex);
                    auto it = sessions.find(placeholder->id);
                    if (it != sessions.end() && it->second == placeholder) {
                        sessions.erase(it);
                        erase_idempotency_for_session_locked(placeholder->id);
                        if (!placeholder->logical_session->client_key.empty())
                            logical_sessions.erase(placeholder->logical_session->client_key);
                        prune_failed_starts_locked();
                        failed_starts[placeholder->id] =
                            FailedStart{placeholder->account, std::move(payload),
                                        time.now() + config.start_failed_retention};
                        signal_cleanup_locked();
                    }
                }
                wake(waiters);
                std::string message = "unknown";
                try { std::rethrow_exception(error); } catch (const std::exception& e) { message = e.what(); } catch (...) {}
                Log::warn("playback[" + trace + "] async start failed id=" + placeholder->id +
                          " stage=" + stalled + " error=" + message);
            }
        }
        Lock lock(mutex);
        --start_workers;
        start_workers_cv.notify_all();
    }

    // Async PATCH worker: builds the replacement beside the playing generation
    // and swaps it in once its first fragment exists.
    void run_replacement(std::shared_ptr<Session> old, std::shared_ptr<PendingReplacement> pending,
                         std::string trace) {
        auto start = pending->start;
        auto replacement = start->candidate;
        std::string stalled = "planning";
        bool swapped = false;
        try {
            if (pending->needs_plan) prepare_transformed_vod(*replacement, trace);
            if (start->cancelled.load()) throw std::runtime_error("start cancelled");
            std::vector<std::shared_ptr<HttpWaker>> waiters;
            {
                Lock lock(start->mutex);
                if (replacement->vod_plan && !replacement->vod_plan->segment_durations.empty())
                    start->first_fragment_ms =
                        std::llround(replacement->vod_plan->segment_durations.front() * 1000.0);
                start->stage = "encoding";
                stalled = "encoding";
                waiters = start->changed_locked();
            }
            wake(waiters);
            start_pipeline(*replacement, trace, false);
            wait_for_first_fragment_async(*replacement, *start, trace);
            auto old_active = active_engine(*old);
            {
                Lock lock(mutex);
                auto it = sessions.find(old->id);
                if (start->cancelled.load() || it == sessions.end() || it->second != old ||
                    old->pending != pending)
                    throw std::runtime_error("start cancelled");
                it->second = replacement;
                old->pending.reset();
                commit_resources_locked(*pending->reservation);
                *pending->reservation = {};
                release_unused_transcode_entitlements_locked(*replacement);
                signal_cleanup_locked();
            }
            swapped = true;
            if (old_active) old_active->segments()->mark_superseded(true);
            stop_pipeline(*old);
            std::error_code ec;
            if (!old->generation_dir.empty() && old->generation_dir != replacement->generation_dir)
                std::filesystem::remove_all(old->generation_dir, ec);
            {
                Lock lock(start->mutex);
                start->stage = "ready";
                waiters = start->changed_locked();
            }
            wake(waiters);
            observations().record("playback.update_ready_us", elapsed_us(start->started));
            Log::info("playback[" + trace + "] async update ready id=" + replacement->id +
                      " generation=" + std::to_string(replacement->generation));
        } catch (...) {
            const auto error = std::current_exception();
            if (!swapped) {
                stop_pipeline(*replacement);
                std::error_code ec;
                if (!replacement->generation_dir.empty() && replacement->generation_dir != old->generation_dir)
                    std::filesystem::remove_all(replacement->generation_dir, ec);
                if (pending->reservation->video || pending->reservation->audio) {
                    rollback_resources(*replacement, *pending->reservation);
                    *pending->reservation = {};
                }
            }
            std::vector<std::shared_ptr<HttpWaker>> waiters;
            if (start->cancelled.load()) {
                Lock lock(start->mutex);
                waiters = std::exchange(start->waiters, {});
            } else {
                {
                    Lock lock(start->mutex);
                    start->error = start_error_json(error, stalled);
                    start->stage = "failed";
                    waiters = start->changed_locked();
                }
                {
                    Lock lock(mutex);
                    if (old->pending == pending)
                        pending->failed_until = time.now() + config.start_failed_retention;
                }
                std::string message = "unknown";
                try { std::rethrow_exception(error); } catch (const std::exception& e) { message = e.what(); } catch (...) {}
                Log::warn("playback[" + trace + "] async update failed id=" + old->id + " stage=" + stalled +
                          " error=" + message + "; the current generation keeps serving");
            }
            wake(waiters);
        }
        {
            Lock done(pending->done_mutex);
            pending->done = true;
        }
        pending->done_cv.notify_all();
        Lock lock(mutex);
        --start_workers;
        start_workers_cv.notify_all();
    }

    HttpResponse creation_response(const Session& session, std::string trace,
                                   std::string_view idempotency_status = {}) const {
        const bool pending = start_pending(session);
        auto payload = pending ? pending_json(session, *session.start) : session_json(session);
        payload["trace_id"] = std::move(trace);
        // The account's cap and current holding, counted live under the lock.
        // Deliberately absent from /api/v1/playback/status, which clients
        // cache: this number changes whenever any device on the account acts.
        payload["account"] = account_state_json(session.account);
        // Whether a keyed request created this session or replayed one.
        if (!idempotency_status.empty())
            payload["idempotency"] = std::string(idempotency_status);
        if (pending) payload["status"] = std::string("playback_starting");
        auto response = http_json(pending ? 202 : 201, payload.dump());
        response.headers["Location"] = "/api/v1/playback/sessions/" + session.id;
        return response;
    }

    void erase_idempotency_for_session_locked(std::string_view session_id) MACHA_REQUIRES(mutex) {
        for (auto it = idempotent_creations.begin(); it != idempotent_creations.end();) {
            Lock creation_lock(it->second->mutex);
            if (it->second->complete && it->second->session_id == session_id)
                it = idempotent_creations.erase(it);
            else
                ++it;
        }
    }

    // The key for everything account-scoped: the cap and the collection
    // listing. Per account, not per viewer, since a rogue client can claim any
    // number of viewers (hence the generous max_sessions_per_account). An
    // anonymous session is bounded as itself, not pooled with other anonymous
    // callers.
    static std::string account_key(const SessionIdentity& identity) {
        if (!identity.user_id.empty())
            return identity.user_id;
        return "session:" + identity.id;
    }

    std::shared_ptr<LogicalViewerSession> logical_session_for(std::string client_key) {
        if (client_key.empty())
            return std::make_shared<LogicalViewerSession>();
        Lock lock(mutex);
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
        const std::shared_ptr<LogicalViewerSession>& logical) const MACHA_REQUIRES(mutex) {
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
            Lock lock(mutex);
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
                Lock lock(mutex);
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
                Lock lock(mutex);
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
                    Lock lock(mutex);
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
                        Lock lock(profile_publish_mutex);
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
            Lock lock(mutex);
            if (auto it = probe_cache.find(key); it != probe_cache.end()) return it->second;
            auto [it, inserted] = probe_flights.try_emplace(key, std::make_shared<ProbeFlight>());
            flight = it->second;
            owner = inserted;
        }
        if (!owner) {
            const auto wait_started = Clock::now();
            Lock flight_lock(flight->mutex);
            if (!flight->cv.wait_until(flight_lock.native(), resolve_deadline,
                                       [&]() MACHA_REQUIRES(flight->mutex) { return flight->complete; }))
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
                Lock flight_lock(flight->mutex);
                flight->result = std::move(result);
                flight->error = error;
                flight->complete = true;
            }
            flight->cv.notify_all();
            Lock lock(mutex);
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
            Lock lock(mutex);
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
                Lock lock(profile_publish_mutex);
                profile_publish_cv.wait(lock.native(), stop, [&]() MACHA_REQUIRES(profile_publish_mutex) {
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
                Lock lock(profile_publish_mutex);
                queue_profile_publication(pending.first, pending.second);
                // A CAS conflict is transient: keep the completed probe and
                // retry after a bounded backoff rather than re-probing.
                profile_publish_cv.wait_for(lock.native(), stop, std::chrono::milliseconds(250),
                                            [] { return false; });
            }
        }
        drain_profile_publications();
    }

    // Publishes queued profiles at shutdown so already-paid probes are not
    // repeated after restart. One commit for the whole queue keeps shutdown
    // prompt; failure is logged, not retried.
    void drain_profile_publications() {
        std::map<std::string, MediaProbeResult, std::less<>> pending;
        {
            Lock lock(profile_publish_mutex);
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

    size_t video_transcodes_locked(std::string_view excluding = {}) const MACHA_REQUIRES(mutex) {
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

    size_t audio_transcodes_locked(std::string_view excluding = {}) const MACHA_REQUIRES(mutex) {
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

    size_t running_video_transcode_pipelines_locked() const MACHA_REQUIRES(mutex) {
        size_t count = 0;
        for (const auto& [_, session] : sessions) {
            if (session->plan.video != MediaTransform::transcode) continue;
            Lock pipeline_lock(session->pipeline_mutex);
            if (session->engine_session && session->engine_session->running()) ++count;
        }
        return count;
    }

    size_t running_audio_transcode_pipelines_locked() const MACHA_REQUIRES(mutex) {
        size_t count = 0;
        for (const auto& [_, session] : sessions) {
            if (session->plan.audio != MediaTransform::transcode) continue;
            Lock pipeline_lock(session->pipeline_mutex);
            if (session->engine_session && session->engine_session->running()) ++count;
        }
        return count;
    }

    struct ResourceReservation {
        bool video{};
        bool audio{};
        // The account's first entitlement for this logical viewer; counts
        // against max_transcodes_per_account until committed or rolled back.
        std::string account;
        bool account_slot{};
    };

    // Logical viewers of `account` holding a transcode entitlement, plus the
    // account's entitlements reserved and not yet committed.
    size_t account_transcodes_locked(std::string_view account,
                                     std::string_view excluding = {}) const MACHA_REQUIRES(mutex) {
        size_t count = 0;
        std::set<const LogicalViewerSession*> counted;
        for (const auto& [id, session] : sessions) {
            if (id == excluding || session->account != account || !session->logical_session)
                continue;
            if ((session->logical_session->video_transcode_entitled ||
                 session->logical_session->audio_transcode_entitled) &&
                counted.insert(session->logical_session.get()).second)
                ++count;
        }
        if (auto reserved = reserved_account_transcodes.find(account);
            reserved != reserved_account_transcodes.end())
            count += reserved->second;
        return count;
    }

    void reserve_session_slot(std::string_view account) {
        Lock lock(mutex);
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
        Lock lock(mutex);
        if (pending_sessions) --pending_sessions;
        release_pending_account_locked(account);
    }

    // Live sessions plus in-flight creations, so a burst of concurrent creates
    // cannot pass the cap.
    size_t sessions_held_by_locked(std::string_view account) const MACHA_REQUIRES(mutex) {
        size_t held = 0;
        for (const auto& [_, session] : sessions)
            if (session->account == account) ++held;
        if (auto pending = pending_by_account.find(account); pending != pending_by_account.end())
            held += pending->second;
        return held;
    }

    void release_pending_account_locked(std::string_view account) MACHA_REQUIRES(mutex) {
        auto pending = pending_by_account.find(account);
        if (pending == pending_by_account.end())
            return;
        if (pending->second > 1)
            --pending->second;
        else
            pending_by_account.erase(pending);
    }

    ResourceReservation reserve_resources(Session& session, std::string_view excluding = {}) {
        Lock lock(mutex);
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
        // Only a logical viewer holding no entitlement yet takes an account slot.
        const bool account_slot =
            (video || audio) && !session.logical_session->video_transcode_entitled &&
            !session.logical_session->audio_transcode_entitled;
        if (account_slot && config.max_transcodes_per_account) {
            const auto held = account_transcodes_locked(session.account, excluding);
            if (held >= config.max_transcodes_per_account)
                throw AccountTranscodeLimitError(held, config.max_transcodes_per_account);
        }
        if (video) ++reserved_video_transcodes;
        if (audio) ++reserved_audio_transcodes;
        if (account_slot) ++reserved_account_transcodes[session.account];
        if (video) session.logical_session->video_transcode_entitled = true;
        if (audio) session.logical_session->audio_transcode_entitled = true;
        return {video, audio, account_slot ? session.account : std::string{}, account_slot};
    }

    // True when no other session shares this logical viewer. Presence, not a
    // running engine, is checked: that avoids taking a sibling's
    // pipeline_mutex under `mutex`. An idle sibling keeps the entitlement.
    bool sole_session_for_logical_locked(const Session& keep) const MACHA_REQUIRES(mutex) {
        for (const auto& [id, other] : sessions) {
            if (other.get() == &keep) continue;
            if (other->logical_session == keep.logical_session) return false;
        }
        return true;
    }

    bool holds_transcode_entitlement_locked(const Session& session) const MACHA_REQUIRES(mutex) {
        return session.logical_session &&
               (session.logical_session->video_transcode_entitled ||
                session.logical_session->audio_transcode_entitled);
    }

    // Released after transcode_entitlement_idle without stream activity, so an
    // abandoned session cannot hold a node's transcode slots for session_idle.
    // Longer than the pipeline idle: a paused viewer keeps the slot a while.
    // Per logical viewer only. Reacquired on resume, where it may be refused.
    void release_transcode_entitlements_locked(Session& session) MACHA_REQUIRES(mutex) {
        if (!session.logical_session) return;
        const bool held = session.logical_session->video_transcode_entitled ||
                          session.logical_session->audio_transcode_entitled;
        if (!held) return;
        session.logical_session->video_transcode_entitled = false;
        session.logical_session->audio_transcode_entitled = false;
        Log::info("playback transcode entitlement released after stream inactivity session=" +
                  session.id);
    }

    // A session PATCHed out of transcode releases the entitlement at once,
    // unless a sibling on the same logical viewer holds it.
    void release_unused_transcode_entitlements_locked(Session& session) MACHA_REQUIRES(mutex) {
        if (!session.logical_session || !sole_session_for_logical_locked(session)) return;
        auto& logical = *session.logical_session;
        const bool video = logical.video_transcode_entitled &&
                           session.plan.video != MediaTransform::transcode;
        const bool audio = logical.audio_transcode_entitled &&
                           session.plan.audio != MediaTransform::transcode;
        if (video) logical.video_transcode_entitled = false;
        if (audio) logical.audio_transcode_entitled = false;
        if (video || audio)
            Log::info("playback transcode entitlement released: session no longer transcodes session=" +
                      session.id + (video ? " video" : "") + (audio ? " audio" : ""));
    }

    void commit_resources_locked(const ResourceReservation& reservation) MACHA_REQUIRES(mutex) {
        if (reservation.video && reserved_video_transcodes) --reserved_video_transcodes;
        if (reservation.audio && reserved_audio_transcodes) --reserved_audio_transcodes;
        if (reservation.account_slot) {
            auto reserved = reserved_account_transcodes.find(reservation.account);
            if (reserved != reserved_account_transcodes.end() && reserved->second &&
                --reserved->second == 0)
                reserved_account_transcodes.erase(reserved);
        }
    }

    void rollback_resources(Session& session, const ResourceReservation& reservation) {
        Lock lock(mutex);
        commit_resources_locked(reservation);
        if (reservation.video) session.logical_session->video_transcode_entitled = false;
        if (reservation.audio) session.logical_session->audio_transcode_entitled = false;
    }

    std::shared_ptr<MediaEngineSession> active_engine(const Session& session) const {
        Lock lock(session.pipeline_mutex);
        return session.engine_session;
    }

    static bool transformed(const PlaybackPlan& plan) {
        return plan.video == MediaTransform::transcode ||
               plan.audio == MediaTransform::transcode;
    }

    void request_heap_reclaim() {
        Lock lock(mutex);
        heap_reclaim_pending = true;
        ++heap_reclaim_requests;
        signal_cleanup_locked();
    }

    static bool transcoding(const PlaybackPlan& plan) {
        return plan.video == MediaTransform::transcode || plan.audio == MediaTransform::transcode;
    }

    // As a generation ends, records the rate this node sustained for its kind
    // of source, if it produced at least a minute of media.
    void record_transcode_rate(const Session& session, MediaEngineSession& active) {
        if (!transcoding(session.plan)) return;
        const auto state = active.segments()->snapshot();
        if (state.produced_media_ms < 60000 || state.producing_ms == 0) return;
        const double rate = static_cast<double>(state.produced_media_ms) / static_cast<double>(state.producing_ms);
        const auto concurrent = running_transcodes.load();
        auto& book = transcode_rates;
        if (session.plan.video == MediaTransform::transcode) {
            if (const auto* video = stream_at(session.probe, session.plan.video_stream))
                book.record("video", video->codec, static_cast<uint32_t>(std::max(0, video->bit_depth)),
                            TranscodeRateBook::height_class(video->height), rate, concurrent);
        } else if (const auto* audio = stream_at(session.probe, session.plan.audio_stream)) {
            book.record("audio", audio->codec, 0, 0, rate, concurrent);
        }
    }

    void stop_pipeline(Session& session) {
        std::shared_ptr<MediaEngineSession> active;
        {
            Lock lock(session.pipeline_mutex);
            active.swap(session.engine_session);
        }
        if (active) {
            record_transcode_rate(session, *active);
            if (transcoding(session.plan) && running_transcodes.load() > 0) --running_transcodes;
            active->stop();
            // Destroy codec and segment owners before waking the asynchronous
            // reclaimer; seek and reconfiguration call this, so never trim here.
            active.reset();
            if (transformed(session.plan))
                request_heap_reclaim();
        }
    }

    void wait_for_initial_fragment(Session& session, std::string_view trace) MACHA_EXCLUDES(mutex) {
        auto active = active_engine(session);
        if (!active) throw std::runtime_error("media pipeline did not start");
        auto store = active->segments();
        auto started_at = Clock::now();
        const auto deadline = started_at + current_config().startup_timeout;
        const auto* progress = active->start_progress();
        // Where a slow start's time went: source bytes, pre-roll, output media.
        const auto progress_text = [&] {
            if (!progress) return std::string{};
            const auto total = progress->preroll_total_us.load(std::memory_order_relaxed);
            return " progress_seq=" + std::to_string(progress->seq.load(std::memory_order_relaxed)) +
                   " source_bytes_read=" +
                   std::to_string(progress->source_bytes_read.load(std::memory_order_relaxed)) +
                   (total < 0 ? std::string{}
                              : " preroll_decoded_ms=" +
                                    std::to_string(progress->preroll_decoded_us.load(std::memory_order_relaxed) / 1000) +
                                    " preroll_total_ms=" + std::to_string(total / 1000)) +
                   " output_media_ms=" +
                   std::to_string(progress->output_media_us.load(std::memory_order_relaxed) / 1000);
        };
        bool ready = false;
        while (true) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (remaining.count() <= 0) break;
            if (store->wait_ready(std::min(remaining, std::chrono::milliseconds(1000)))) {
                ready = true;
                break;
            }
            if (!store->snapshot().error.empty() || !active->running()) break;
            if (progress)
                Log::debug("playback[" + std::string(trace) + "] waiting for first fragment elapsed_ms=" +
                           std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              Clock::now() - started_at).count()) +
                           progress_text());
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count();
        if (ready) {
            observations().record("playback.first_fragment_us", elapsed_us(started_at));
            auto state = store->snapshot();
            Log::info("playback[" + std::string(trace) + "] first fragment ready elapsed_ms=" +
                      std::to_string(elapsed) + " segments=" + std::to_string(state.segment_count) +
                      progress_text());
            return;
        }
        Log::info("playback[" + std::string(trace) + "] no first fragment elapsed_ms=" +
                  std::to_string(elapsed) + progress_text());
        auto state = store->snapshot();
        if (!state.error.empty()) throw std::runtime_error("libav pipeline failed before first fragment: " + state.error);
        auto diagnostic = active->diagnostics();
        if (!active->running())
            throw std::runtime_error("libav pipeline ended before first fragment" +
                                     (diagnostic.empty() ? std::string{} : ": " + diagnostic));
        throw std::runtime_error("timed out waiting for first fragmented-MP4 segment");
    }

    std::string vod_plan_key(const Session& session) const MACHA_EXCLUDES(mutex) {
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
            // The seek position is not part of the key: a hit is re-seeked
            // (reseek_hls_vod) rather than re-indexing the container.
            << current_config().segment_duration.count() << '|'
            << (session.preferences.mode != "remux" &&
                false);
        return key.str();
    }

    void prepare_transformed_vod(Session& session, std::string_view trace) MACHA_EXCLUDES(mutex) {
        session.vod_plan.reset();
        if (session.plan.mode == PlaybackMode::direct) return;

        const auto cache_key = vod_plan_key(session);
        {
            std::optional<HlsVodPlan> cached;
            {
                Lock lock(mutex);
                if (auto it = vod_plan_cache.find(cache_key); it != vod_plan_cache.end())
                    cached = it->second;
            }
            if (cached) {
                const auto requested_seek = session.plan.seek;
                // Compare with the cached plan's requested seek, not its
                // baseline: a remux generation starts at the keyframe before the
                // request, so a matching baseline would publish a wrong offset.
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

        const auto settings = current_config();
        auto started = Clock::now();
        auto prepared = engine->prepare_hls_vod(session.source, session.plan,
                                                session.probe.duration_seconds, settings.segment_duration,
                                                session.preferences.mode != "remux" &&
                                                    false,
                                                settings.probe_timeout);
        session.plan = prepared.playback;
        Log::debug("playback[" + std::string(trace) + "] VOD plan ready mode=" +
                   playback_mode_name(session.plan.mode) +
                   " seek_ms=" + std::to_string(session.plan.seek.count()) +
                   " segments=" + std::to_string(prepared.segment_durations.size()) +
                   " elapsed_ms=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                       Clock::now() - started).count()));
        {
            Lock lock(mutex);
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

    void start_pipeline(Session& session, std::string_view trace, bool wait_for_first_fragment = true)
        MACHA_EXCLUDES(mutex) {
        stop_pipeline(session);
        const auto settings = current_config();
        session.stream_touched = time.now();
        ++session.generation;
        session.generation_dir = *settings.temp_path / session.id / std::to_string(session.generation);
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
            auto launched = engine->start_hls(session.source, *session.vod_plan, settings.segment_duration,
                                              settings.max_ahead_segments, settings.segment_memory_bytes,
                                              session.generation_dir);
            auto segment_store = launched->segments();
            if (!segment_store || !segment_store->attach_memory_ledger(retained_memory)) {
                launched->stop();
                throw std::runtime_error("viewer fragment memory admission unavailable");
            }
            {
                Lock lock(session.pipeline_mutex);
                session.engine_session = std::shared_ptr<MediaEngineSession>(std::move(launched));
            }
            if (transcoding(session.plan)) ++running_transcodes;
            if (wait_for_first_fragment) {
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
            }
            session.stream_url = prefix + "/" + std::to_string(session.generation) + "/master.m3u8";
            if (wait_for_first_fragment) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count();
                Log::info("playback[" + std::string(trace) + "] pipeline startup complete elapsed_ms=" +
                          std::to_string(elapsed));
            }
        }
        if (session.plan.subtitle_stream >= 0)
            session.subtitle_url = prefix + "/" + std::to_string(session.generation) +
                                   "/subtitle-" + std::to_string(session.plan.subtitle_stream) +
                                   "/manifest.json";
    }

    // Playback is by media_id only: the client chooses the file, mode and
    // codecs from GET /api/v1/playback/media?item_id= and names them here.
    std::shared_ptr<Session> resolve_session(const std::string& media_id, PlaybackPreferences preferences,
                                             std::string_view trace,
                                             std::string existing_id = {}, std::string existing_token = {}) {
        auto lease = create_source(media_id);
        const auto profile_started = Clock::now();
        auto probe = probe_source(lease, trace, Clock::now() + current_config().probe_timeout);
        const auto profile_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - profile_started).count();
        auto plan = plan_for(probe, preferences);
        require_plan_supported(plan);
        Log::info("playback[" + std::string(trace) + "] admission media=" + media_id +
                  " mode=" + playback_mode_name(plan.mode) + " metadata_ms=" + std::to_string(profile_ms));

        auto session = std::make_shared<Session>(time.now());
        session->id = existing_id.empty() ? hex_token(16) : std::move(existing_id);
        session->token = existing_token.empty() ? hex_token() : std::move(existing_token);
        session->preferences = std::move(preferences);
        session->source_entry = lease.entry;
        session->source = media_source(lease);
        session->probe = std::move(probe);
        session->plan = plan;
        session->touched = time.now();
        return session;
    }

    std::shared_ptr<Session> reuse_seek_session(const Session& old,
                                                std::chrono::milliseconds requested_seek,
                                                std::string_view trace) {
        // Every decline is logged with its reason.
        const auto declined = [&](const std::string& reason) {
            observations().add("playback.seek_fastpath.declined");
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

        auto session = std::make_shared<Session>(time.now());
        session->id = old.id;
        session->token = old.token;
        session->preferences = old.preferences;
        session->source = old.source;
        session->source_entry = old.source_entry;
        session->probe = old.probe;
        session->plan = reseeked->playback;
        session->vod_plan = std::move(*reseeked);
        session->generation = old.generation;
        session->touched = time.now();
        session->logical_session = old.logical_session;
        session->account = old.account;
        observations().add("playback.seek_fastpath.taken");
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
            subtitle = chosen_stream(old.probe, MediaStreamType::subtitle, preferences.subtitle_stream,
                                     preferences.subtitle_language, true);
        if (subtitle && !webvtt_subtitle_supported(*subtitle))
            throw std::invalid_argument("requested subtitle stream cannot be converted to WebVTT");

        // Subtitles are an independent WebVTT resource: only the subtitle
        // selection changes; the active A/V generation is copied intact.
        auto session = std::make_shared<Session>(time.now());
        session->id = old.id;
        session->token = old.token;
        session->preferences = std::move(preferences);
        session->source = old.source;
        session->source_entry = old.source_entry;
        session->probe = old.probe;
        session->plan = old.plan;
        session->plan.subtitle_stream = subtitle ? subtitle->index : -1;
        session->vod_plan = old.vod_plan;
        session->generation = old.generation;
        session->generation_dir = old.generation_dir;
        {
            auto active = active_engine(old);
            Lock lock(session->pipeline_mutex);
            session->engine_session = std::move(active);
        }
        session->stream_url = old.stream_url;
        session->subtitle_cache = old.subtitle_cache;
        session->logical_session = old.logical_session;
        // Ownership must carry over, or the replacement is invisible to its
        // account and escapes the per-account cap.
        session->account = old.account;
        if (session->plan.subtitle_stream >= 0) {
            session->subtitle_url = public_stream_prefix(*session) + "/" +
                                    std::to_string(session->generation) + "/subtitle-" +
                                    std::to_string(session->plan.subtitle_stream) + "/manifest.json";
        }
        session->touched = time.now();
        Log::info("playback[" + std::string(trace) + "] subtitle update media=" +
                  session->source.media_id + " stream=" +
                  std::to_string(session->plan.subtitle_stream) + " generation=" +
                  std::to_string(session->generation));
        return session;
    }

    // Whether some instruction built from `preferences`, trying every candidate
    // for each open choice, would be carried out. For reporting only.
    bool some_plan_supported(const MediaProbeResult& probe, PlaybackPreferences preferences) const {
        const auto each = [&](MediaStreamType type, const std::optional<int>& named) {
            std::vector<std::optional<int>> out;
            if (named) return std::vector<std::optional<int>>{named};
            for (const auto* stream : streams_of(probe, type)) out.emplace_back(stream->index);
            if (out.empty()) out.emplace_back(std::nullopt);
            return out;
        };
        std::vector<std::string> containers{preferences.container};
        if (preferences.container.empty() && preferences.mode != "direct") containers = {"fmp4", "mpegts"};
        const auto videos = each(MediaStreamType::video, preferences.video_stream);
        const auto audios = preferences.audio_language.empty()
                                ? each(MediaStreamType::audio, preferences.audio_stream)
                                : std::vector<std::optional<int>>{preferences.audio_stream};
        for (const auto& container : containers)
            for (const auto& video : videos)
                for (const auto& audio : audios) {
                    auto candidate = preferences;
                    candidate.container = container;
                    candidate.video_stream = video;
                    if (preferences.audio_language.empty()) candidate.audio_stream = audio;
                    try {
                        if (plan_supported(plan_for(probe, candidate))) return true;
                    } catch (...) {}
                }
        return false;
    }

    Json session_json(const Session& session) const MACHA_EXCLUDES(mutex) {
        Json::Array streams;
        for (const auto& stream : session.probe.streams) streams.push_back(stream_json(stream));
        Json::Object selected{{"video_stream", session.plan.video_stream},
                              {"audio_stream", session.plan.audio_stream},
                              {"subtitle_stream", session.plan.subtitle_stream}};
        // What else this media could be asked for. Per-stream transforms and
        // quality belong to the current mode and are dropped when asking about
        // another (max_height would make every remux probe illegal).
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
                if (some_plan_supported(session.probe, preferences)) modes.emplace_back(candidate);
            } catch (...) {}
        }
        Json::Array quality_heights;
        if (const auto* video = stream_at(session.probe, session.plan.video_stream); video && video->height > 0) {
            static constexpr std::array<int, 6> candidates{2160, 1440, 1080, 720, 480, 360};
            for (const auto height : candidates) {
                if (height >= video->height) continue;
                // A quality change is a re-encode.
                auto preferences = without_mode_overrides(session.preferences);
                preferences.mode = "transcode";
                preferences.max_height = height;
                try {
                    if (some_plan_supported(session.probe, preferences)) quality_heights.emplace_back(height);
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
                    if (some_plan_supported(session.probe, preferences)) audio_streams.emplace_back(stream_json(stream));
                } catch (...) {}
            }
            if (stream.type == MediaStreamType::subtitle) {
                auto preferences = session.preferences;
                preferences.subtitle_stream = stream.index;
                preferences.subtitle_language.clear();
                try {
                    if (some_plan_supported(session.probe, preferences)) subtitle_streams.emplace_back(stream_json(stream));
                } catch (...) {}
            }
        }
        const bool can_change_quality = !quality_heights.empty() || session.preferences.max_height.has_value() ||
                                        session.preferences.max_bitrate.has_value();
        Json::Object options{{"modes", Json(std::move(modes))},
                             {"quality_heights", Json(std::move(quality_heights))},
                             {"audio_streams", Json(std::move(audio_streams))},
                             {"subtitle_streams", Json(std::move(subtitle_streams))},
                             {"can_seek", true},
                             {"can_change_quality", can_change_quality}};
        const auto mime_type = session.plan.mode == PlaybackMode::direct
                                   ? source_mime(session.probe, session.source.logical_path)
                                   : "application/vnd.apple.mpegurl";
        Json::Object source{{"path", session.source.logical_path},
                            {"format", session.probe.format},
                            {"size", session.source.size},
                            {"bitrate", session.probe.bitrate},
                            {"streams", Json(std::move(streams))}};
        // How far past its last requested fragment a client may ask and find
        // production authorised: the producer parks at highest_requested +
        // max_ahead_segments, matching segment_hold_window. Reported in ms so
        // clients need not know the knobs. Null for direct play.
        const auto settings = current_config();
        const auto look_ahead_ms =
            static_cast<uint64_t>(settings.max_ahead_segments) *
            static_cast<uint64_t>(std::max<int64_t>(0, settings.segment_duration.count()));
        Json::Object stream{{"url", session.stream_url},
                            {"mime_type", mime_type},
                            {"look_ahead_ms", session.plan.mode == PlaybackMode::direct
                                                  ? Json(nullptr) : Json(look_ahead_ms)},
                            {"subtitle_url", session.subtitle_url.empty() ? Json(nullptr) : Json(session.subtitle_url)}};
        // Production rate as a raw pair the client divides, choosing its own
        // smoothing. producing_ms excludes parked time, so produced/producing
        // is what the node could sustain, not the viewer's pace. produced_ms is
        // also the frontier in media time: a join at P waits
        // (P - produced_ms) / (rate - 1). Absent for direct play; zero until
        // the first fragment, so producing_ms == 0 means "no reading yet".
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
                         // Exactly, in integer ms:
                         //     seek_ms + seek_offset_ms == seek_requested_ms
                         // seek_ms is where the generation's media begins;
                         // seek_offset_ms is the requested position within it,
                         // zero unless remux began at an earlier keyframe.
                         // seek_requested_ms tells a violated invariant from a
                         // clamp near the end of a title.
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
        return Json(std::move(out));
    }

    std::vector<uint64_t> subtitle_segment_durations_ms(const Session& session) const
        MACHA_EXCLUDES(mutex) {
        std::vector<uint64_t> durations;
        if (session.plan.mode != PlaybackMode::direct && session.vod_plan) {
            durations.reserve(session.vod_plan->segment_durations.size());
            for (double seconds : session.vod_plan->segment_durations)
                durations.push_back(std::max<uint64_t>(1, static_cast<uint64_t>(std::llround(seconds * 1000.0))));
            return durations;
        }

        const auto total_ms = static_cast<uint64_t>(std::max(0.0, session.probe.duration_seconds) * 1000.0);
        const auto segment_ms =
            static_cast<uint64_t>(std::max<int64_t>(1, current_config().segment_duration.count()));
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
            Lock lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end() || !stream_token_matches(it->second->token, token))
                return http_error(404, "not_found", "stream not found");
            session = it->second;
        }
        if (request.method != "GET" && request.method != "HEAD") return http_error(405, "method", "GET or HEAD required");
        if (rest == "direct") {
            {
                Lock lock(mutex);
                auto it = sessions.find(id);
                if (it == sessions.end() || it->second != session)
                    return http_error(404, "not_found", "stream not found");
                session->touched = time.now();
                // One ranged body can outlive several idle windows, so direct
                // play counts as use.
                session->stream_served = true;
                signal_cleanup_locked();
            }
            return ranged_response(request, session->source_entry.size,
                                   source_mime(session->probe, session->source.logical_path),
                                   [this, path = session->source.logical_path, entry = session->source_entry](uint64_t offset, uint64_t length) {
                                       return std::make_shared<LogicalBody>(fs.open_read(entry, path, false, FrameType::foreground), offset, length);
                                   });
        }
        auto slash3 = rest.find('/');
        if (slash3 == std::string_view::npos) return http_error(404, "not_found", "stream object not found");
        uint64_t generation = 0;
        auto generation_text = rest.substr(0, slash3);
        auto [end, ec] = std::from_chars(generation_text.data(), generation_text.data() + generation_text.size(), generation);
        if (ec != std::errc{} || end != generation_text.data() + generation_text.size())
            return http_error(404, "not_found", "stream generation not found");
        if (generation != session->generation)
            return generation_gone(generation, session->generation);
        auto name = std::string(rest.substr(slash3 + 1));
        if (name.empty() || name == "." || name == ".." || name.find("..") != std::string::npos)
            return http_error(400, "bad_path", "invalid stream object");
        {
            Lock lock(mutex);
            auto it = sessions.find(id);
            // A removed session is 404. A replaced record (subtitle change,
            // fast-path seek, mode change) is supersession of the client's
            // generation.
            if (it == sessions.end())
                return http_error(404, "not_found", "stream generation not found");
            if (it->second != session)
                return generation_gone(generation, it->second->generation);
            if (session->generation != generation)
                return generation_gone(generation, session->generation);
            const auto now = time.now();
            session->touched = now;
            session->stream_touched = now;
            session->stream_served = true;
            ++session->active_stream_requests;
            signal_cleanup_locked();
        }
        [[maybe_unused]] auto stream_request = std::shared_ptr<void>(nullptr, [this, session](void*) {
            Lock lock(mutex);
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
                auto& cache = *session->subtitle_cache;
                Lock subtitle_lock(cache.mutex);
                auto key = std::make_pair(stream_index, *index);
                auto cached = cache.segments.find(key);
                if (cached != cache.segments.end()) {
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
            // A real master playlist carrying CODECS, so players need not infer
            // source-buffer codecs from the init segment.
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
            // No readiness gate: the playlist is complete once the plan exists.
            // Waits happen on fragments, whose failure (fragLoadError) clients
            // do not weigh as node health the way they weigh levelLoadError.
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
        // The VOD playlist promises every fragment up front, so a request for
        // an unproduced one is ordinary. It is held, but only when admitted and
        // only for media production is working toward.
        const auto index = segment_index(name);
        const bool holdable_init =
            name == "init.mp4" && store->container() != MediaContainer::mpegts;

        std::optional<Bytes> object = store->object(name);
        if (!object && (index || holdable_init)) {
            auto state = store->snapshot();
            if (!state.error.empty()) return stream_failed(state.error);
            // Past the plan: never promised, a genuine miss.
            if (index && state.planned_segments && *index >= state.planned_segments)
                return http_error(404, "not_found", "stream object not found");
            // A resumed request still owns its original hold; nothing is
            // re-admitted.
            auto held = request.resumed
                            ? std::static_pointer_cast<HeldRequest>(request.resumed_state)
                            : std::shared_ptr<HeldRequest>{};
            if (!held) {
                // Beyond the window nothing is working toward this fragment.
                if (index && *index >= state.segment_count + current_config().segment_hold_window)
                    return segment_not_ready(session->id, *index, "beyond_hold_window");
                auto why = SegmentHoldArbiter::Refusal::budget_exhausted;
                auto hold = segment_holds.try_acquire(session->id, &why);
                if (!hold)
                    return segment_not_ready(session->id, index.value_or(0),
                                             why == SegmentHoldArbiter::Refusal::session_limit
                                                 ? "session_hold_limit"
                                                 : "hold_budget_exhausted");
                held = std::make_shared<HeldRequest>(std::move(*hold));
                // Noted only once admitted, so a refused request cannot drag
                // the producer's window forward.
                if (index) active->note_segment_requested(*index);
            }
            // The wait costs no thread: fetch-or-subscribe is one locked step,
            // then the server is handed a deferral and re-runs the request when
            // the store fires or the deadline passes. The hold rides in the
            // deferral's state and is released with it.
            const auto segment_timeout = current_config().segment_timeout;
            const auto deadline =
                request.resumed ? request.resume_deadline
                : segment_timeout.count() > 0
                    ? Clock::now() + segment_timeout
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
            // Promised by the playlist: "not yet", never 404.
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
        if (!current_config().enabled)
            return http_error(503, "streaming_disabled", "streaming is disabled");
        // HttpServer has already refused unauthenticated requests (this route
        // is not exempt in capability_request); checked anyway.
        if (!request.session) return http_error(401, "unauthorized", "a valid session bearer token is required");
        auto trace = hex_token(4);
        auto request_started = Clock::now();
        Log::info("playback[" + trace + "] session create start");
        Json root = Json::parse(std::string_view(reinterpret_cast<const char*>(request.body.data()), request.body.size()));
        if (!root.isObject()) return http_error(400, "bad_request", "JSON object required");
        // Playback is by media_id only: choosing a title's file is the client's.
        if (root.find("item_id"))
            return http_error(400, "item_id_not_accepted",
                              "playback is by media_id: read the title's files from "
                              "GET /api/v1/playback/media?item_id= and send the chosen media_id");
        std::string media_id;
        if (auto v = root.find("media_id"); v && v->isString()) media_id = v->asString();
        if (media_id.empty()) return http_error(400, "media_id_required", "media_id is required");
        auto prefs = parse_preferences(root.find("preferences"));
        std::optional<int64_t> seek_ms;
        if (auto seek = root.find("seek_ms")) {
            try {
                auto value = seek->asInt64();
                if (value >= 0) seek_ms = value;
            } catch (...) {}
            if (!seek_ms) return http_error(400, "bad_seek", "seek_ms must be non-negative");
        }
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
        // Opt-in: answer once admitted and report start progress, rather than
        // blocking until the first fragment exists.
        bool async_start = false;
        if (auto it = request.query.find("start"); it != request.query.end()) {
            if (it->second != "async") return http_error(400, "bad_start", "start must be async");
            async_start = true;
        }

        std::string fingerprint;
        std::string idempotency_scope;
        std::shared_ptr<IdempotentCreation> idempotent;
        bool idempotent_owner = false;
        if (!idempotency_key.empty()) {
            fingerprint = creation_fingerprint(media_id, prefs, seek_ms,
                                               request.session->id);
            // Scoped to the account: keys are client-chosen and predictable,
            // so a shared namespace would let one account turn another's retry
            // into a 409.
            idempotency_scope = account + '\0' + idempotency_key;
            {
                Lock lock(mutex);
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
                Lock lock(idempotent->mutex);
                if (!idempotent->complete) {
                    const auto settings = current_config();
                    const auto deadline =
                        Clock::now() + settings.probe_timeout + settings.startup_timeout;
                    if (!idempotent->cv.wait_until(lock.native(), deadline,
                                                   [&]() MACHA_REQUIRES(idempotent->mutex) {
                                                       return idempotent->complete;
                                                   }))
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
                    Lock sessions_lock(mutex);
                    auto active = sessions.find(existing_id);
                    if (active == sessions.end())
                        return http_error(409, "idempotency_expired",
                                          "the prior playback session has expired");
                    existing = active->second;
                    existing->touched = time.now();
                    signal_cleanup_locked();
                }
                return creation_response(*existing, trace, "replayed");
            }
        }
        // A POST always creates a member; the per-account cap bounds them.
        // Each session is its own logical viewer, so viewers sharing a login
        // do not share a transcode entitlement.
        auto logical_session = std::make_shared<LogicalViewerSession>();
        Lock logical_operation(logical_session->operation_mutex);
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
            session = resolve_session(media_id, std::move(prefs), trace,
                                      std::move(deterministic_id), std::move(deterministic_token));
            session->logical_session = logical_session;
            session->account = account;
            if (seek_ms) session->plan.request_seek(std::chrono::milliseconds(*seek_ms));
            if (async_start && transformed(session->plan)) {
                // Admitted: every refusal decidable now has been made. The
                // placeholder takes the session's place and slot while the
                // start runs beside it. Direct play has no pipeline to wait for.
                resource_reservation = reserve_resources(*session);
                auto start = std::make_shared<StartState>();
                start->candidate = copy_for_start(*session);
                start->candidate->start = start;
                session->start = start;
                {
                    Lock lock(mutex);
                    sessions[session->id] = session;
                    signal_cleanup_locked();
                    if (pending_sessions) --pending_sessions;
                    release_pending_account_locked(account);
                    commit_resources_locked(resource_reservation);
                    resource_reservation = {};
                    ++start_workers;
                }
                try {
                    std::thread([this, session, start, trace] { run_start(session, start, trace); }).detach();
                } catch (...) {
                    Lock lock(mutex);
                    sessions.erase(session->id);
                    --start_workers;
                    start_workers_cv.notify_all();
                    throw;
                }
            } else {
                prepare_transformed_vod(*session, trace);
                resource_reservation = reserve_resources(*session);
                start_pipeline(*session, trace);
                {
                    Lock lock(mutex);
                    sessions[session->id] = session;
                    signal_cleanup_locked();
                    if (pending_sessions) --pending_sessions;
                    release_pending_account_locked(account);
                    commit_resources_locked(resource_reservation);
                    resource_reservation = {};
                }
            }
        } catch (...) {
            auto error = std::current_exception();
            if (session) stop_pipeline(*session);
            if (session && (resource_reservation.video || resource_reservation.audio))
                rollback_resources(*session, resource_reservation);
            release_session_slot(account);
            if (idempotent) {
                {
                    Lock lock(idempotent->mutex);
                    idempotent->error = error;
                    idempotent->complete = true;
                }
                idempotent->cv.notify_all();
                Lock lock(mutex);
                auto it = idempotent_creations.find(idempotency_scope);
                if (it != idempotent_creations.end() && it->second == idempotent)
                    idempotent_creations.erase(it);
            }
            std::rethrow_exception(error);
        }
        if (idempotent) {
            {
                Lock lock(idempotent->mutex);
                idempotent->session_id = session->id;
                idempotent->complete = true;
            }
            idempotent->cv.notify_all();
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - request_started).count();
        observations().record("playback.create_us", elapsed_us(request_started));
        // The negotiated transforms and codecs, for diagnosis.
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

    // Control routes check ownership: session ids are listed, so without this
    // any account could read, seek or DELETE another's session. A mismatch is
    // 404, not 403, so one account cannot learn another's ids exist.
    bool caller_owns(const Session& session, const HttpRequest& request) const {
        return request.session && session.account == account_key(*request.session);
    }

    Json account_state_json(std::string_view account) const {
        Json::Object out;
        {
            Lock lock(mutex);
            out["sessions"] = static_cast<uint64_t>(sessions_held_by_locked(account));
            out["transcodes"] = static_cast<uint64_t>(account_transcodes_locked(account));
            out["max_sessions"] = static_cast<uint64_t>(config.max_sessions_per_account);
            out["max_transcodes"] = static_cast<uint64_t>(config.max_transcodes_per_account);
        }
        return Json(std::move(out));
    }

    HttpResponse list_sessions(const HttpRequest& request) {
        if (!request.session)
            return http_error(401, "unauthorized", "a valid session bearer token is required");
        // Node-local by design: a session's resources live on this node, and
        // asking each node gives the client every session's provenance.
        const auto account = account_key(*request.session);
        std::vector<std::shared_ptr<Session>> owned;
        size_t transcodes = 0;
        size_t max_sessions = 0;
        size_t max_transcodes = 0;
        {
            Lock lock(mutex);
            for (const auto& [_, session] : sessions)
                if (session->account == account) owned.push_back(session);
            transcodes = account_transcodes_locked(account);
            max_sessions = config.max_sessions_per_account;
            max_transcodes = config.max_transcodes_per_account;
        }
        PageQuery page;
        if (auto bad = read_page_query(request, page)) return *bad;
        Json::Array out;
        for (const auto& session : owned) out.push_back(session_json(*session));
        Json::Object body;
        const auto held = out.size();
        page_json(out, "session_id", page, body);
        body["items"] = std::move(out);
        // The caps, so a client can plan against them rather than meet them.
        Json::Object account_info;
        account_info["sessions"] = static_cast<uint64_t>(held);
        account_info["max_sessions"] = static_cast<uint64_t>(max_sessions);
        account_info["transcodes"] = static_cast<uint64_t>(transcodes);
        account_info["max_transcodes"] = static_cast<uint64_t>(max_transcodes);
        body["account"] = std::move(account_info);
        return http_json(200, Json(std::move(body)).dump());
    }

    HttpResponse get_session(std::string_view id, const HttpRequest& request) {
        std::shared_ptr<Session> session;
        {
            Lock lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end()) {
                // A failed async start stays readable for a while.
                prune_failed_starts_locked();
                auto failed = failed_starts.find(id);
                if (failed != failed_starts.end() && request.session &&
                    failed->second.account == account_key(*request.session))
                    return http_json(200, failed->second.payload.dump());
                return http_error(404, "not_found", "playback session not found");
            }
            if (!caller_owns(*it->second, request))
                return http_error(404, "not_found", "playback session not found");
            session = it->second;
            session->touched = time.now();
            signal_cleanup_locked();
        }
        std::shared_ptr<PendingReplacement> pending;
        {
            Lock lock(mutex);
            pending = pending_locked(*session);
        }
        const auto watched = session->start && start_pending(*session) ? session->start
                             : pending                                  ? pending->start
                                                                        : session->start;
        if (watched) {
            // Long-poll: answer when progress_seq passes `after`, or at the
            // wait, whichever comes first.
            auto number = [&](std::string_view name) -> std::optional<uint64_t> {
                auto it = request.query.find(name);
                if (it == request.query.end() || it->second.empty()) return std::nullopt;
                uint64_t value = 0;
                auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), value);
                if (ec != std::errc{} || end != it->second.data() + it->second.size())
                    throw std::invalid_argument(std::string(name) + " must be a non-negative integer");
                return value;
            };
            const auto after = number("after");
            const auto wait_ms = std::min<uint64_t>(number("wait_ms").value_or(0),
                                                    static_cast<uint64_t>(current_config().start_wait_max.count()));
            const auto deadline = request.resumed ? request.resume_deadline
                                                  : Clock::now() + std::chrono::milliseconds(wait_ms);
            std::shared_ptr<HttpWaker> waker;
            bool unfinished = false;
            {
                Lock lock(watched->mutex);
                unfinished = !watched->finished();
                if (after && unfinished && watched->seq <= *after && Clock::now() < deadline) {
                    waker = std::make_shared<HttpWaker>();
                    watched->waiters.push_back(waker);
                }
            }
            if (waker) {
                HttpResponse deferred;
                deferred.defer = HttpDeferral{std::move(waker), deadline, {}};
                return deferred;
            }
            if (unfinished && watched == session->start)
                return http_json(200, pending_json(*session, *session->start).dump());
        }
        auto result = session_json(*session);
        if (session->start) {
            Lock lock(session->start->mutex);
            result["start"] = start_json_locked(*session->start);
        }
        if (pending) {
            Lock lock(pending->start->mutex);
            result["pending"] = Json(Json::Object{{"start", start_json_locked(*pending->start)}});
        }
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
            Lock lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end()) return http_error(404, "not_found", "playback session not found");
            if (!caller_owns(*it->second, request))
                return http_error(404, "not_found", "playback session not found");
            old = it->second;
        }
        if (!old->logical_session)
            throw std::logic_error("playback session has no logical viewer session");
        Lock logical_operation(old->logical_session->operation_mutex);
        {
            Lock lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end())
                return http_error(404, "not_found", "playback session not found");
            old = it->second;
        }
        if (start_pending(*old))
            return http_error(409, "playback_starting",
                              "this session's start is not ready: wait for it, or DELETE it");
        bool async_start = false;
        if (auto it = request.query.find("start"); it != request.query.end()) {
            if (it->second != "async") return http_error(400, "bad_start", "start must be async");
            async_start = true;
        }
        // A second PATCH replaces a pending one.
        cancel_pending(*old);
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
        if (root.find("item_id"))
            return http_error(400, "item_id_not_accepted",
                              "playback is by media_id: send the media_id to play");
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
                Lock lock(mutex);
                auto it = sessions.find(std::string(id));
                if (it == sessions.end() || it->second != old)
                    throw std::runtime_error("playback session changed during subtitle update");
                it->second = replacement;
            }
            return http_json(200, session_json(*replacement).dump());
        }

        // A seek with unchanged preferences (clients often resend them) reuses
        // the prepared VOD plan rather than re-probing and re-indexing the
        // source while the viewer waits.
        const bool seek_only = seek_ms.has_value() && media_override.empty() &&
                               prefs == old->preferences;
        std::shared_ptr<Session> replacement;
        if (seek_only)
            replacement = reuse_seek_session(*old, std::chrono::milliseconds(*seek_ms), trace);
        else if (seek_ms)
            // Say why this seek takes the slow path.
            Log::info("playback[" + trace + "] seek fast-path skipped media=" +
                      old->source.media_id + " requested_ms=" + std::to_string(*seek_ms) +
                      " reason=" + (media_override.empty() ? "preferences-changed"
                                                           : "media-override"));

        if (!replacement) {
            replacement = resolve_session(media_override.empty() ? old->source.media_id : media_override,
                                          prefs, trace, old->id, old->token);
            replacement->logical_session = old->logical_session;
            // Ownership carries over, as in the subtitle and seek replacements.
            replacement->account = old->account;
            replacement->generation = old->generation;
            if (seek_ms) replacement->plan.request_seek(std::chrono::milliseconds(*seek_ms));
            if (!async_start || !transformed(replacement->plan)) prepare_transformed_vod(*replacement, trace);
        }
        if (async_start && transformed(replacement->plan)) {
            // Built beside the playing generation, which keeps serving until
            // the replacement is ready.
            auto pending = std::make_shared<PendingReplacement>();
            pending->needs_plan = !replacement->vod_plan;
            pending->reservation = std::make_shared<ResourceReservation>(reserve_resources(*replacement, old->id));
            pending->start = std::make_shared<StartState>();
            pending->start->candidate = replacement;
            {
                Lock lock(mutex);
                auto it = sessions.find(std::string(id));
                if (it == sessions.end() || it->second != old) {
                    rollback_resources(*replacement, *pending->reservation);
                    throw std::runtime_error("playback session changed during update");
                }
                old->pending = pending;
                ++start_workers;
            }
            try {
                std::thread([this, old, pending, trace] { run_replacement(old, pending, trace); }).detach();
            } catch (...) {
                {
                    Lock lock(mutex);
                    old->pending.reset();
                    --start_workers;
                    start_workers_cv.notify_all();
                }
                rollback_resources(*replacement, *pending->reservation);
                throw;
            }
            auto payload = session_json(*old);
            payload["status"] = std::string("playback_starting");
            {
                Lock lock(pending->start->mutex);
                payload["pending"] = Json(Json::Object{{"start", start_json_locked(*pending->start)}});
            }
            return http_json(202, payload.dump());
        }
        ResourceReservation resource_reservation;
        // start_pipeline() may block up to startup_timeout before
        // stop_pipeline(*old) runs. Marking old superseded now wakes requests
        // parked on its unproduced segments at once; unlike stop_pipeline it
        // is reversible, so on failure old keeps serving.
        auto old_active = active_engine(*old);
        if (old_active) old_active->segments()->mark_superseded(true);
        try {
            resource_reservation = reserve_resources(*replacement, old->id);
            start_pipeline(*replacement, trace);
            // Hold the reservation until old's pipeline stops, so a concurrent
            // request cannot take the apparently free slot mid-handover.
            stop_pipeline(*old);
            {
                Lock lock(mutex);
                auto it = sessions.find(std::string(id));
                if (it == sessions.end() || it->second != old)
                    throw std::runtime_error("playback session changed during update");
                it->second = replacement;
                signal_cleanup_locked();
                commit_resources_locked(resource_reservation);
                resource_reservation = {};
                release_unused_transcode_entitlements_locked(*replacement);
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

    // Abandon a pending replacement; the playing generation is untouched.
    HttpResponse erase_pending(std::string_view id, const HttpRequest& request) {
        std::shared_ptr<Session> session;
        {
            Lock lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end() || !caller_owns(*it->second, request))
                return http_error(404, "not_found", "playback session not found");
            session = it->second;
        }
        cancel_pending(*session);
        return {204, "application/json; charset=utf-8", {}, {}, {}};
    }

    HttpResponse erase_session(std::string_view id, const HttpRequest& request) {
        std::shared_ptr<Session> session;
        {
            Lock lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end()) return http_error(404, "not_found", "playback session not found");
            if (!caller_owns(*it->second, request))
                return http_error(404, "not_found", "playback session not found");
            session = it->second;
        }
        if (!tear_down_session(id, session))
            return http_error(404, "not_found", "playback session not found");
        return {204, "application/json; charset=utf-8", {}, {}, {}};
    }

    // Page-exit close. An unloading page cannot complete a preflighted
    // cross-origin DELETE, so this is authorised by the stream token in the
    // path: a CORS simple request (sendBeacon, fetch keepalive). Idempotent:
    // a session already gone is success.
    HttpResponse close_by_capability(std::string_view id, std::string_view token) {
        std::shared_ptr<Session> session;
        {
            Lock lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end()) return {204, "application/json; charset=utf-8", {}, {}, {}};
            if (!stream_token_matches(it->second->token, std::string(token)))
                return http_error(404, "not_found", "stream not found");
            session = it->second;
        }
        (void)tear_down_session(id, session);
        return {204, "application/json; charset=utf-8", {}, {}, {}};
    }

    // Removes an already-authorised session and releases everything it holds.
    // False when it was removed meanwhile.
    bool tear_down_session(std::string_view id, std::shared_ptr<Session> session) {
        std::shared_ptr<Session> removed;
        Lock logical_operation(session->logical_session->operation_mutex);
        {
            Lock lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end()) return false;
            // Whatever holds the id now, possibly a recreate. `session` keeps
            // the locked logical session alive until the lock is released.
            removed = it->second;
            sessions.erase(it);
            erase_idempotency_for_session_locked(id);
            if (!removed->logical_session->client_key.empty())
                logical_sessions.erase(removed->logical_session->client_key);
            signal_cleanup_locked();
        }
        // A start running beside a placeholder stops now, freeing its slot.
        if (removed->start) {
            removed->start->cancelled.store(true);
            if (removed->start->candidate && removed->start->candidate != removed)
                stop_pipeline(*removed->start->candidate);
        }
        cancel_pending(*removed);
        stop_pipeline(*removed);
        std::error_code ec;
        std::filesystem::remove_all(*current_config().temp_path / removed->id, ec);
        return true;
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
        StreamingConfig settings;
        {
            Lock lock(mutex);
            settings = config;
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
        // Per-session reads happen outside the global mutex, so a slow WebVTT
        // extraction cannot stall every playback operation. try_lock: a session
        // mid-extraction contributes zero rather than blocking status().
        for (const auto& session : active_sessions) {
            subtitle_cache_usage(*session->subtitle_cache, cached_subtitle_segments,
                                 cached_subtitle_bytes);
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
                         {"enabled", settings.enabled},
                         {"sessions", static_cast<uint64_t>(session_count)},
                         {"max_sessions", static_cast<uint64_t>(settings.max_sessions)},
                         {"max_sessions_per_account",
                          static_cast<uint64_t>(settings.max_sessions_per_account)},
                         {"max_transcodes_per_account",
                          static_cast<uint64_t>(settings.max_transcodes_per_account)},
                         {"startup_no_progress_ms", static_cast<uint64_t>(settings.startup_no_progress.count())},
                         {"start_wait_max_ms", static_cast<uint64_t>(settings.start_wait_max.count())},
                         {"start_failed_retention_ms",
                          static_cast<uint64_t>(settings.start_failed_retention.count())},
                         {"video_transcodes", static_cast<uint64_t>(video_transcodes)},
                         {"max_video_transcodes", static_cast<uint64_t>(settings.max_video_transcodes)},
                         {"audio_transcodes", static_cast<uint64_t>(audio_transcodes)},
                         {"max_audio_transcodes", static_cast<uint64_t>(settings.max_audio_transcodes)},
                         {"running_video_transcode_pipelines",
                          static_cast<uint64_t>(running_video_transcode_pipelines)},
                         {"running_audio_transcode_pipelines",
                          static_cast<uint64_t>(running_audio_transcode_pipelines)},
                         {"video_decoder_threads",
                          static_cast<uint64_t>(settings.video_decoder_threads)},
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
                         // Constant fields older diagnostics UIs still parse.
                         {"ffmpeg_available", false},
                         {"ffprobe_available", false},
                         {"ffmpeg_version", ""}};
        return http_json(200, Json(std::move(out)).dump());
    }

    // GET /api/v1/playback/media?media_id= (or item_id=): what the media is
    // and what can be done with it. No session, no pipeline, no capability
    // matching; the client reads these facts and then instructs.
    HttpResponse media_facts(const HttpRequest& request) {
        const auto settings = current_config();
        if (!settings.enabled) return http_error(503, "streaming_disabled", "streaming is disabled");
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

        Json::Array reported;
        // Unreadable here differs from nonexistent; both are reported per
        // media so the client can decide whether to ask another node.
        Json::Array unavailable;
        std::string last_error;
        std::string last_reason;
        for (const auto& id : media_ids) {
            try {
                auto lease = create_source(id);
                // Each file gets the whole probe allowance, so a slow file
                // cannot push the rest into `unavailable`.
                auto probe = probe_source(lease, "facts", Clock::now() + settings.probe_timeout);
                // Facts of media and muxers, not of any client: direct is
                // always possible, copy_into is per stream, transcode needs the
                // encoders.
                Json::Array streams;
                for (const auto& stream : probe.streams) {
                    auto value = stream_json(stream);
                    if (!stream.attached_picture && stream.type == MediaStreamType::video)
                        value.asObject()["copy_into"] = Json::Object{
                            {"fmp4", fmp4_video_copy_supported(lower(stream.codec))},
                            {"mpegts", mpegts_video_copy_supported(lower(stream.codec))}};
                    else if (stream.type == MediaStreamType::audio)
                        value.asObject()["copy_into"] = Json::Object{
                            {"fmp4", fmp4_audio_copy_supported(lower(stream.codec))},
                            {"mpegts", mpegts_audio_copy_supported(lower(stream.codec))}};
                    streams.emplace_back(std::move(value));
                }
                const auto engine_state = engine ? engine->status() : MediaEngineStatus{};
                Json::Object operations{
                    {"direct", true},
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
                if (extra_media_facts)
                    extra_media_facts(entry, lease.media_id);
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
            if (rest.substr(slash == std::string_view::npos ? rest.size() : slash) == "/pending") {
                if (request.method != "DELETE") return http_error(405, "method", "DELETE required");
                return erase_pending(id, request);
            }
            if (slash == std::string_view::npos) {
                if (request.method == "GET") return get_session(id, request);
                if (request.method == "PATCH") return update_session(id, request);
                if (request.method == "DELETE") return erase_session(id, request);
                return http_error(405, "method", "GET, PATCH or DELETE required");
            }
            auto stream = parse_stream_route(request.path);
            if (!stream)
                return http_error(404, "not_found", "endpoint not found");
            if (const auto close = stream->stream_path.find('/');
                close != std::string_view::npos && stream->stream_path.substr(close) == "/close") {
                if (request.method != "POST") return http_error(405, "method", "POST required");
                return close_by_capability(stream->session_id, stream->stream_path.substr(0, close));
            }
            return session_stream_response(request, stream->session_id, stream->stream_path);
        }
        if (request.path == "/api/v1/playback/media" && request.method == "GET")
            return media_facts(request);
        return http_error(404, "not_found", "endpoint not found");
    }

    void signal_cleanup_locked() MACHA_REQUIRES(mutex) {
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
            std::chrono::milliseconds entitlement_idle{};
            bool reclaim_heap = false;
            std::filesystem::path temp_path;
            {
                Lock lock(mutex);
                const auto now = time.now();
                temp_path = *config.temp_path;
                idle_timeout = config.pipeline_idle;
                unused_idle_timeout = config.session_unused_idle;
                // Clamped to [pipeline_idle, session_idle]: reconfigure() can
                // receive an unvalidated StreamingConfig.
                entitlement_idle = std::clamp(config.transcode_entitlement_idle,
                                              config.pipeline_idle, config.session_idle);
                for (auto it = sessions.begin(); it != sessions.end();) {
                    // A session that never served a stream object expires on
                    // the shorter session_unused_idle. Both clocks run from
                    // `touched`, so a client still polling or PATCHing is never
                    // evicted. Clamped so an unused session never outlives a
                    // used one, since reconfigure() is unvalidated.
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
                            Lock pipeline_lock(it->second->pipeline_mutex);
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
                        // The entitlement has its own, longer clock than the
                        // pipeline. Checked every pass, with next_expiry
                        // carrying the wake-up so the loop does not spin.
                        if (holds_transcode_entitlement_locked(*it->second)) {
                            const auto entitlement_expires =
                                it->second->stream_touched + entitlement_idle;
                            if (now >= entitlement_expires) {
                                if (sole_session_for_logical_locked(*it->second))
                                    release_transcode_entitlements_locked(*it->second);
                            } else if (!next_expiry || entitlement_expires < *next_expiry) {
                                next_expiry = entitlement_expires;
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
                        Lock pipeline_lock(session->pipeline_mutex);
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
                    const auto changed = [&]() MACHA_REQUIRES(mutex) {
                        return cleanup_revision != observed_revision;
                    };
                    if (next_expiry)
                        cleanup_cv.wait_for(lock.native(), stop, *next_expiry - now, changed);
                    else
                        cleanup_cv.wait(lock.native(), stop, changed);
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
                std::filesystem::remove_all(temp_path / session->id, ec);
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
                    Lock lock(mutex);
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

PlaybackManager::PlaybackManager(FileSystem& fs, TranscodeRateBook& transcode_rates,
                                 RetainedMemoryLedger& retained_memory,
                                 CatalogueManager& catalogue, CatalogueApiConfig api,
                                 StreamingConfig streaming, std::shared_ptr<MediaEngine> engine,
                                 std::function<size_t(const std::vector<std::string>&)> request_profiles,
                                 MediaInformationService* media_information,
                                 MediaFacts media_facts, const TimeSource& time)
    : impl_(std::make_unique<Impl>(fs, transcode_rates, retained_memory, catalogue, std::move(api), std::move(streaming),
                                  std::move(engine), std::move(request_profiles),
                                  media_information, time)) {
    impl_->extra_media_facts = std::move(media_facts);
}

PlaybackManager::~PlaybackManager() { stop(); }

void PlaybackManager::start() {
    const auto config = impl_->current_config();
    if (impl_->started || !config.enabled) return;
    std::filesystem::create_directories(*config.temp_path);
    if (!impl_->engine) throw std::runtime_error("streaming media engine is unavailable");
    auto status = impl_->engine->status();
    if (!status.available) throw std::runtime_error("streaming media engine is unavailable");
    impl_->cleanup_thread = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("playback-cleanup", stop, [this, stop] { impl_->cleanup(stop); });
    });
    impl_->profile_publish_thread = std::jthread([this](std::stop_token stop) {
        run_supervised_loop("playback-profile-publish", stop, [this, stop] { impl_->publish_profiles(stop); });
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
    std::vector<std::shared_ptr<Impl::StartState>> starts;
    {
        Lock lock(impl_->mutex);
        for (auto& [_, session] : impl_->sessions) {
            if (session->start) starts.push_back(session->start);
            if (session->pending) starts.push_back(session->pending->start);
        }
    }
    for (auto& start : starts) {
        start->cancelled.store(true);
        if (start->candidate) impl_->stop_pipeline(*start->candidate);
    }
    {
        Lock lock(impl_->mutex);
        impl_->start_workers_cv.wait(lock.native(), [&]() MACHA_REQUIRES(impl_->mutex) {
            return impl_->start_workers == 0;
        });
    }
    std::vector<std::shared_ptr<Impl::Session>> sessions;
    {
        Lock lock(impl_->mutex);
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
    Lock lock(impl_->mutex);
    // Policy limits and timing apply at once; backend, probe, buffering and
    // temp-path changes need a restart.
    impl_->config.max_sessions = config.max_sessions;
    impl_->config.max_sessions_per_account = config.max_sessions_per_account;
    impl_->config.max_transcodes_per_account = config.max_transcodes_per_account;
    impl_->config.max_video_transcodes = config.max_video_transcodes;
    impl_->config.max_audio_transcodes = config.max_audio_transcodes;
    impl_->config.session_idle = config.session_idle;
    impl_->config.session_unused_idle = config.session_unused_idle;
    impl_->config.pipeline_idle = config.pipeline_idle;
    impl_->config.startup_timeout = config.startup_timeout;
    impl_->config.startup_no_progress = config.startup_no_progress;
    impl_->config.start_wait_max = config.start_wait_max;
    impl_->config.start_failed_retention = config.start_failed_retention;
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
    } catch (const PlaybackChoiceError& e) {
        // Every node would say the same, so do not walk.
        Json::Object error{{"code", e.code()},
                           {"message", std::string(e.what())},
                           {"choice", e.choice()},
                           {"choices", Json(e.choices())},
                           {"scope", std::string("request")},
                           {"node_healthy", true},
                           {"alternative_may_succeed", false}};
        Json::Object root;
        root["status"] = e.code();
        root["error"] = std::move(error);
        return http_json(400, Json(std::move(root)).dump());
    } catch (const PlaybackCapabilityError& e) {
        // 422, not 400: nothing is malformed. Another node may accept it, and
        // a transcode would succeed here.
        FailureAxes axes;
        axes.scope = FailureScope::node;
        axes.node_healthy = true;
        axes.alternative_may_succeed = true;
        return http_error(422, "copy_not_supported", e.what(), {}, axes);
    } catch (const std::invalid_argument& e) {
        // Every node would refuse it the same way: do not walk.
        FailureAxes axes;
        axes.scope = FailureScope::request;
        axes.node_healthy = true;
        axes.alternative_may_succeed = false;
        return http_error(400, "bad_playback_request", e.what(), {}, axes);
    } catch (const std::out_of_range& e) {
        return http_error(404, "not_found", e.what());
    } catch (const AccountSessionLimitError& e) {
        // Not resource_limit: an account cap is identical on every node, so
        // scope=request tells the client not to walk. The remedy is the
        // caller's, not another node's.
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
    } catch (const AccountTranscodeLimitError& e) {
        // Same on every node: scope=request. Remux or direct needs no
        // entitlement and may succeed here.
        Json::Object error{{"code", std::string("account_transcode_limit")},
                           {"message", std::string(e.what())},
                           {"scope", std::string("request")},
                           {"node_healthy", true},
                           {"alternative_may_succeed", true},
                           {"transcodes", static_cast<uint64_t>(e.held())},
                           {"max_transcodes", static_cast<uint64_t>(e.limit())}};
        Json::Object root;
        root["error"] = std::move(error);
        return http_json(429, Json(std::move(root)).dump());
    } catch (const ResourceLimitError& e) {
        // This node is full. On create nothing exists yet, so scope=node:
        // walk. On update the session lives here and is still serving, so
        // scope=request: the remedy is a different instruction on this node
        // (remux, a lower height), not another node.
        const bool updating = request.method == "PATCH";
        FailureAxes axes;
        axes.scope = updating ? FailureScope::request : FailureScope::node;
        axes.node_healthy = true;
        axes.alternative_may_succeed = true;
        return http_error(429, "resource_limit", e.what(), {}, axes);
    } catch (const PlaybackStageError& e) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count();
        Log::warn("playback[" + e.trace() + "] request failed stage=" + e.stage() +
                  " method=" + request.method + " path=" + request.path +
                  " elapsed_ms=" + std::to_string(elapsed) + " error=" + e.what());
        // The common error envelope: error.code to branch on, error.message
        // for a human, everything else on the same object.
        Json::Object error{{"code", "playback_" + e.stage() + "_failed"},
                           {"message", std::string(e.what())},
                           {"trace", e.trace()},
                           {"stage", e.stage()}};
        int status = 503;
        // A per-title fault leaves the node healthy. Without the engine's
        // reason, scope is left unstated rather than guessed.
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
        return http_json(status, Json(std::move(body)).dump());
    } catch (const std::exception& e) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count();
        Log::warn("playback request failed method=" + request.method + " path=" + request.path +
                  " elapsed_ms=" + std::to_string(elapsed) + " error=" + e.what());
        return http_error(503, "playback_unavailable", e.what());
    }
}

bool PlaybackManager::capability_request(const HttpRequest& request) const {
    // The stream subresource is authorised by the token in its path, so it is
    // exempt from the bearer. Same parse as the router: see parse_stream_route.
    return parse_stream_route(request.path).has_value();
}

} // namespace macha
