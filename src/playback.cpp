// SPDX-License-Identifier: GPL-3.0-or-later
#include "playback.hpp"
#include "diagnostics.hpp"

#include "crypto.hpp"
#include "json.hpp"
#include "log.hpp"
#include "macha_version.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace macha {
namespace {

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

std::string extension(std::string_view path) {
    auto slash = path.find_last_of('/');
    auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    return lower(std::string(path.substr(dot)));
}

std::string direct_mime(std::string_view path) {
    auto ext = extension(path);
    if (ext == ".mp4" || ext == ".m4v" || ext == ".mov") return "video/mp4";
    if (ext == ".webm") return "video/webm";
    if (ext == ".mp3") return "audio/mpeg";
    if (ext == ".m4a") return "audio/mp4";
    if (ext == ".flac") return "audio/flac";
    if (ext == ".ogg" || ext == ".oga") return "audio/ogg";
    return "application/octet-stream";
}

std::string direct_container(std::string_view path) {
    auto ext = extension(path);
    if (ext == ".mp4" || ext == ".m4v" || ext == ".m4a" || ext == ".mov") return "mp4";
    if (ext == ".webm") return "webm";
    if (ext == ".mp3") return "mp3";
    if (ext == ".flac") return "flac";
    if (ext == ".ogg" || ext == ".oga") return "ogg";
    return {};
}

bool fmp4_video_copy_supported(std::string_view codec) {
    return codec == "h264" || codec == "hevc" || codec == "av1";
}

bool webvtt_subtitle_supported(const MediaStreamInfo& stream) {
    if (stream.type != MediaStreamType::subtitle) return false;
    const auto codec = lower(stream.codec);
    static constexpr std::array<std::string_view, 6> codecs{
        "ass", "mov_text", "ssa", "subrip", "text", "webvtt"
    };
    return std::find(codecs.begin(), codecs.end(), codec) != codecs.end();
}

std::string file_mime(std::string_view name) {
    auto ext = extension(name);
    if (ext == ".m3u8") return "application/vnd.apple.mpegurl";
    if (ext == ".m4s" || ext == ".mp4") return "video/mp4";
    if (ext == ".vtt") return "text/vtt; charset=utf-8";
    return "application/octet-stream";
}

struct ByteRange {
    uint64_t offset{};
    uint64_t length{};
    bool partial{};
};

class ResourceLimitError final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class PlaybackStageError final : public std::runtime_error {
    std::string trace_;
    std::string stage_;

  public:
    PlaybackStageError(std::string trace, std::string stage, std::string message)
        : std::runtime_error(std::move(message)), trace_(std::move(trace)),
          stage_(std::move(stage)) {}
    const std::string& trace() const noexcept { return trace_; }
    const std::string& stage() const noexcept { return stage_; }
};

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

struct ClientCapabilities {
    std::set<std::string> containers{"mp4"};
    std::set<std::string> video_codecs{"h264"};
    std::set<std::string> audio_codecs{"aac", "mp3"};
    bool hls_fmp4{true};
    std::optional<int> max_width;
    std::optional<int> max_height;
};

struct PlaybackPreferences {
    std::string mode{"auto"};
    std::optional<int> max_height;
    std::optional<uint64_t> max_bitrate;
    std::optional<int> audio_stream;
    std::optional<int> subtitle_stream;
    std::string audio_language;
    std::string subtitle_language;
};

void read_string_set(const Json* object, std::string_view key, std::set<std::string>& out) {
    if (!object || !object->isObject()) return;
    auto value = object->find(key);
    if (!value || !value->isArray()) return;
    out.clear();
    for (const auto& element : value->asArray())
        if (element.isString()) out.insert(lower(element.asString()));
}

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

ClientCapabilities parse_capabilities(const Json* value) {
    ClientCapabilities caps;
    if (!value || !value->isObject()) return caps;
    read_string_set(value, "containers", caps.containers);
    read_string_set(value, "video_codecs", caps.video_codecs);
    read_string_set(value, "audio_codecs", caps.audio_codecs);
    if (auto hls = value->find("hls_fmp4"); hls && hls->isBool()) caps.hls_fmp4 = hls->asBool();
    caps.max_width = optional_int(value->find("max_width"));
    caps.max_height = optional_int(value->find("max_height"));
    if (caps.max_width && *caps.max_width <= 0) throw std::invalid_argument("capabilities.max_width must be positive");
    if (caps.max_height && *caps.max_height <= 0) throw std::invalid_argument("capabilities.max_height must be positive");
    return caps;
}

PlaybackPreferences parse_preferences(const Json* value, PlaybackPreferences current = {}) {
    if (!value || !value->isObject()) return current;
    if (auto mode = value->find("mode"); mode && mode->isString()) current.mode = lower(mode->asString());
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

PlaybackPlan negotiate(const MediaProbeResult& probe, std::string_view logical_path,
                       const ClientCapabilities& caps, const PlaybackPreferences& prefs) {
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

    const auto source_container = direct_container(logical_path);
    bool direct_container_ok = !source_container.empty() && caps.containers.contains(source_container);
    bool video_direct = !video || caps.video_codecs.contains(lower(video->codec));
    bool audio_direct = !audio || caps.audio_codecs.contains(lower(audio->codec));
    bool size_direct = true;
    auto max_height = prefs.max_height ? prefs.max_height : caps.max_height;
    if (video && max_height && video->height > *max_height) size_direct = false;
    if (video && caps.max_width && video->width > *caps.max_width) size_direct = false;
    bool bitrate_direct = !prefs.max_bitrate || !probe.bitrate || probe.bitrate <= *prefs.max_bitrate;
    bool can_direct = direct_container_ok && video_direct && audio_direct && size_direct && bitrate_direct;

    if (prefs.mode != "auto" && prefs.mode != "direct" && prefs.mode != "remux" && prefs.mode != "transcode")
        throw std::invalid_argument("preferences.mode must be auto, direct, remux or transcode");
    plan.video = video ? MediaTransform::copy : MediaTransform::omit;
    plan.audio = audio ? MediaTransform::copy : MediaTransform::omit;
    plan.video_codec = video ? lower(video->codec) : std::string{};
    plan.audio_codec = audio ? lower(audio->codec) : std::string{};
    if (prefs.mode == "direct") {
        if (!can_direct) throw std::invalid_argument("requested direct play is incompatible with this source/capability set");
        plan.mode = PlaybackMode::direct;
        return plan;
    }
    if (prefs.mode == "auto" && can_direct) {
        plan.mode = PlaybackMode::direct;
        return plan;
    }
    if (!caps.hls_fmp4) throw std::invalid_argument("client cannot accept fragmented-MP4 HLS");

    const auto source_video_codec = video ? lower(video->codec) : std::string{};
    const auto source_audio_codec = audio ? lower(audio->codec) : std::string{};
    bool video_copy = !video || (caps.video_codecs.contains(source_video_codec) &&
                                 fmp4_video_copy_supported(source_video_codec));
    bool audio_copy = !audio || (source_audio_codec == "aac" && caps.audio_codecs.contains("aac"));
    std::optional<int> target_height = max_height;
    if (video && caps.max_width && video->width > *caps.max_width && video->width > 0 && video->height > 0) {
        auto by_width = static_cast<int>(std::floor(static_cast<double>(video->height) *
                                                    static_cast<double>(*caps.max_width) /
                                                    static_cast<double>(video->width)));
        by_width = std::max(2, by_width & ~1);
        target_height = target_height ? std::min(*target_height, by_width) : by_width;
    }
    if (video && target_height && video->height > *target_height) {
        video_copy = false;
        plan.target_height = *target_height;
    }
    if (prefs.max_bitrate) {
        video_copy = false;
        plan.target_video_bitrate = *prefs.max_bitrate;
    }
    if (prefs.mode == "transcode") {
        if (video) video_copy = false;
        if (audio) audio_copy = false;
    }
    if (prefs.mode == "remux" && (!video_copy || !audio_copy))
        throw std::invalid_argument("requested remux requires copy-compatible video and AAC audio without quality conversion");
    if (video && !video_copy && !caps.video_codecs.contains("h264"))
        throw std::invalid_argument("client cannot decode the H.264 transcode target");
    if (audio && !audio_copy && !caps.audio_codecs.contains("aac"))
        throw std::invalid_argument("client cannot decode the AAC transcode target");

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
                     {"max_height", preferences.max_height ? Json(*preferences.max_height) : Json(nullptr)},
                     {"max_bitrate", preferences.max_bitrate ? Json(*preferences.max_bitrate) : Json(nullptr)},
                     {"audio_stream", preferences.audio_stream ? Json(*preferences.audio_stream) : Json(nullptr)},
                     {"subtitle_stream", preferences.subtitle_stream ? Json(*preferences.subtitle_stream) : Json(nullptr)},
                     {"audio_language", preferences.audio_language},
                     {"subtitle_language", preferences.subtitle_language}};
    return Json(std::move(out));
}

Json output_json(const MediaProbeResult& probe, const PlaybackPlan& plan,
                 std::string_view source_format) {
    Json::Object out{{"format", plan.mode == PlaybackMode::direct ? std::string(source_format) : "mp4"}};

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
        }
        if (plan.video == MediaTransform::transcode && plan.target_video_bitrate)
            value["bitrate"] = *plan.target_video_bitrate;
        out["video"] = Json(std::move(value));
    }

    if (const auto* audio = stream_at(probe, plan.audio_stream); audio && plan.audio != MediaTransform::omit) {
        Json::Object value{{"source_stream", audio->index},
                           {"transform", transform_name(plan.audio)},
                           {"codec", plan.audio_codec}};
        if (plan.audio == MediaTransform::transcode) {
            value["channels"] = 2;
            value["sample_rate"] = audio->sample_rate > 0 ? audio->sample_rate : 48000;
            value["bitrate"] = static_cast<uint64_t>(192000);
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
    struct SourceLease {
        std::string media_id;
        std::string path;
        FsEntry entry;
    };

    struct Session {
        std::string id;
        std::string token;
        std::string item_id;
        ClientCapabilities capabilities;
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
        };
        std::string subtitle_url;
        std::shared_ptr<SubtitleCache> subtitle_cache{std::make_shared<SubtitleCache>()};
        Clock::time_point touched{Clock::now()};
    };

    FileSystem& fs;
    CatalogueManager& catalogue;
    StreamingConfig config;
    std::unique_ptr<MediaEngine> engine;
    std::jthread cleanup_thread;
    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<Session>, std::less<>> sessions;
    std::map<std::string, MediaProbeResult, std::less<>> probe_cache;
    // Session/pipeline admission happens before a newly-created pipeline is
    // visible in `sessions`.  Reserve those slots explicitly so concurrent
    // POST/PATCH requests cannot all pass the same resource-limit check.
    size_t pending_sessions{};
    size_t reserved_video_transcodes{};
    size_t reserved_audio_transcodes{};
    bool started{};

    Impl(FileSystem& filesystem, CatalogueManager& cat, CatalogueApiConfig api,
         StreamingConfig streaming, std::unique_ptr<MediaEngine> media_engine)
        : fs(filesystem), catalogue(cat), config(std::move(streaming)),
          engine(media_engine ? std::move(media_engine) :
                (config.enabled ? make_libav_media_engine(config) : nullptr)) {
        (void)api;
    }

    std::string public_stream_prefix(const Session& session) const {
        return "/api/v1/playback/stream/" + session.id + "/" + session.token;
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
            }};
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
        {
            std::lock_guard lock(mutex);
            if (auto it = probe_cache.find(key); it != probe_cache.end()) {
                Log::debug("playback[" + std::string(trace) + "] probe cache-hit media=" + lease.media_id);
                return it->second;
            }
        }
        auto started_at = Clock::now();
        if (started_at >= resolve_deadline)
            throw std::runtime_error("media inspection deadline exhausted before probing candidate");
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(resolve_deadline - started_at);
        remaining = std::max(std::chrono::milliseconds(1), remaining);
        Log::debug("playback[" + std::string(trace) + "] probe start media=" + lease.media_id +
                   " path=" + lease.path + " bytes=" + std::to_string(lease.entry.size) +
                   " deadline_ms=" + std::to_string(remaining.count()));
        MediaProbeResult probed;
        try {
            probed = engine->probe(media_source(lease), remaining);
        } catch (const PlaybackStageError&) {
            throw;
        } catch (const std::exception& e) {
            throw PlaybackStageError(std::string(trace), "probe", e.what());
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count();
        Log::info("playback[" + std::string(trace) + "] probe complete media=" + lease.media_id +
                  " elapsed_ms=" + std::to_string(elapsed) + " format=" + probed.format +
                  " streams=" + std::to_string(probed.streams.size()));
        {
            std::lock_guard lock(mutex);
            probe_cache[std::move(key)] = probed;
        }
        return probed;
    }

    size_t video_transcodes_locked(std::string_view excluding = {}) const {
        size_t count = 0;
        for (const auto& [id, session] : sessions) {
            if (id == excluding) continue;
            if (session->plan.video == MediaTransform::transcode) {
                std::lock_guard pipeline_lock(session->pipeline_mutex);
                if (session->engine_session && session->engine_session->running()) ++count;
            }
        }
        return count;
    }

    size_t audio_transcodes_locked(std::string_view excluding = {}) const {
        size_t count = 0;
        for (const auto& [id, session] : sessions) {
            if (id == excluding) continue;
            if (session->plan.audio == MediaTransform::transcode) {
                std::lock_guard pipeline_lock(session->pipeline_mutex);
                if (session->engine_session && session->engine_session->running()) ++count;
            }
        }
        return count;
    }

    void reserve_session_slot() {
        std::lock_guard lock(mutex);
        if (sessions.size() + pending_sessions >= config.max_sessions)
            throw ResourceLimitError("playback session limit reached");
        ++pending_sessions;
    }

    void release_session_slot() {
        std::lock_guard lock(mutex);
        if (pending_sessions) --pending_sessions;
    }

    void reserve_resources(const PlaybackPlan& plan, std::string_view excluding = {}) {
        std::lock_guard lock(mutex);
        const bool video = plan.video == MediaTransform::transcode;
        const bool audio = plan.audio == MediaTransform::transcode;
        if (video && video_transcodes_locked(excluding) + reserved_video_transcodes >=
                         config.max_video_transcodes)
            throw ResourceLimitError("video transcode limit reached");
        if (audio && audio_transcodes_locked(excluding) + reserved_audio_transcodes >=
                         config.max_audio_transcodes)
            throw ResourceLimitError("audio transcode limit reached");
        if (video) ++reserved_video_transcodes;
        if (audio) ++reserved_audio_transcodes;
    }

    void release_resources_locked(const PlaybackPlan& plan) {
        if (plan.video == MediaTransform::transcode && reserved_video_transcodes)
            --reserved_video_transcodes;
        if (plan.audio == MediaTransform::transcode && reserved_audio_transcodes)
            --reserved_audio_transcodes;
    }

    void release_resources(const PlaybackPlan& plan) {
        std::lock_guard lock(mutex);
        release_resources_locked(plan);
    }

    std::shared_ptr<MediaEngineSession> active_engine(const Session& session) const {
        std::lock_guard lock(session.pipeline_mutex);
        return session.engine_session;
    }

    void stop_pipeline(Session& session) {
        std::shared_ptr<MediaEngineSession> active;
        {
            std::lock_guard lock(session.pipeline_mutex);
            active.swap(session.engine_session);
        }
        if (active) active->stop();
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

    void prepare_transformed_vod(Session& session, std::string_view trace) {
        session.vod_plan.reset();
        if (session.plan.mode == PlaybackMode::direct) return;
        auto started = Clock::now();
        auto prepared = engine->prepare_hls_vod(session.source, session.plan,
                                                session.probe.duration_seconds, config.segment_duration,
                                                session.preferences.mode != "remux" &&
                                                    session.capabilities.video_codecs.contains("h264"),
                                                config.probe_timeout);
        session.plan = prepared.playback;
        Log::debug("playback[" + std::string(trace) + "] VOD plan ready mode=" +
                   playback_mode_name(session.plan.mode) +
                   " seek_ms=" + std::to_string(session.plan.seek.count()) +
                   " segments=" + std::to_string(prepared.segment_durations.size()) +
                   " elapsed_ms=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                       Clock::now() - started).count()));
        session.vod_plan = std::move(prepared);
    }

    void start_pipeline(Session& session, std::string_view trace) {
        stop_pipeline(session);
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
                throw PlaybackStageError(std::string(trace), "pipeline_start", e.what());
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
                                             ClientCapabilities capabilities, PlaybackPreferences preferences,
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
                auto probe = probe_source(lease, trace, resolve_deadline);
                auto plan = negotiate(probe, lease.path, capabilities, preferences);
                require_plan_supported(plan);
                int rank = plan.mode == PlaybackMode::direct ? 0 : (plan.mode == PlaybackMode::remux ? 1 : 2);
                Log::debug("playback[" + std::string(trace) + "] candidate media=" + media_id +
                           " mode=" + playback_mode_name(plan.mode) + " rank=" + std::to_string(rank));
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
        session->capabilities = std::move(capabilities);
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
        if (old.plan.mode == PlaybackMode::direct || !old.vod_plan) return {};
        auto reseeked = reseek_hls_vod(*old.vod_plan, requested_seek);
        if (!reseeked) return {};

        auto session = std::make_shared<Session>();
        session->id = old.id;
        session->token = old.token;
        session->item_id = old.item_id;
        session->capabilities = old.capabilities;
        session->preferences = old.preferences;
        session->source = old.source;
        session->source_entry = old.source_entry;
        session->probe = old.probe;
        session->plan = reseeked->playback;
        session->vod_plan = std::move(*reseeked);
        session->generation = old.generation;
        session->touched = Clock::now();
        Log::info("playback[" + std::string(trace) + "] seek fast-path media=" +
                  session->source.media_id + " requested_ms=" +
                  std::to_string(requested_seek.count()) + " aligned_ms=" +
                  std::to_string(session->plan.seek.count()) + " segments=" +
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
        session->capabilities = old.capabilities;
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
        Json::Array modes;
        for (const auto* candidate : {"direct", "remux", "transcode"}) {
            auto preferences = session.preferences;
            preferences.mode = candidate;
            try {
                const auto plan = negotiate(session.probe, session.source.logical_path,
                                            session.capabilities, preferences);
                if (plan_supported(plan)) modes.emplace_back(candidate);
            } catch (...) {}
        }
        Json::Array quality_heights;
        if (const auto* video = stream_at(session.probe, session.plan.video_stream); video && video->height > 0) {
            static constexpr std::array<int, 6> candidates{2160, 1440, 1080, 720, 480, 360};
            for (const auto height : candidates) {
                if (height >= video->height) continue;
                auto preferences = session.preferences;
                preferences.max_height = height;
                try {
                    const auto plan = negotiate(session.probe, session.source.logical_path,
                                                session.capabilities, preferences);
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
                    const auto plan = negotiate(session.probe, session.source.logical_path,
                                                session.capabilities, preferences);
                    if (plan_supported(plan)) audio_streams.emplace_back(stream_json(stream));
                } catch (...) {}
            }
            if (stream.type == MediaStreamType::subtitle) {
                auto preferences = session.preferences;
                preferences.subtitle_stream = stream.index;
                preferences.subtitle_language.clear();
                try {
                    const auto plan = negotiate(session.probe, session.source.logical_path,
                                                session.capabilities, preferences);
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
        Json::Object stream{{"url", session.stream_url},
                            {"mime_type", mime_type},
                            {"subtitle_url", session.subtitle_url.empty() ? Json(nullptr) : Json(session.subtitle_url)}};
        Json::Object out{{"session_id", session.id},
                         {"media_id", session.source.media_id},
                         {"mode", playback_mode_name(session.plan.mode)},
                         {"duration_ms", static_cast<uint64_t>(std::max(0.0, session.probe.duration_seconds) * 1000.0)},
                         {"seek_ms", static_cast<uint64_t>(std::max<int64_t>(0, session.plan.seek.count()))},
                         {"preferences", preferences_json(session.preferences)},
                         {"selection", Json(std::move(selected))},
                         {"source", Json(std::move(source))},
                         {"output", output_json(session.probe, session.plan, session.probe.format)},
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
        constexpr std::string_view suffix = ".m4s";
        if (!name.starts_with(prefix) || !name.ends_with(suffix)) return {};
        auto number = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
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

    HttpResponse public_stream_response(const HttpRequest& request) {
        constexpr std::string_view prefix = "/api/v1/playback/stream/";
        auto rest = std::string_view(request.path).substr(prefix.size());
        auto slash1 = rest.find('/');
        if (slash1 == std::string_view::npos) return http_error(404, "not_found", "stream not found");
        auto id = std::string(rest.substr(0, slash1));
        rest.remove_prefix(slash1 + 1);
        auto slash2 = rest.find('/');
        if (slash2 == std::string_view::npos) return http_error(404, "not_found", "stream not found");
        auto token = std::string(rest.substr(0, slash2));
        rest.remove_prefix(slash2 + 1);
        std::shared_ptr<Session> session;
        {
            std::lock_guard lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end() || it->second->token != token)
                return http_error(404, "not_found", "stream not found");
            session = it->second;
            session->touched = Clock::now();
        }
        if (request.method != "GET" && request.method != "HEAD") return http_error(405, "method", "GET or HEAD required");
        if (rest == "direct") {
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
                        session->subtitle_cache->segments.emplace(key, data);
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
            auto playlist = store->playlist();
            auto state = store->snapshot();
            Log::debug("playback stream playlist session=" + session->id +
                       " generation=" + std::to_string(session->generation) +
                       " segments=" + std::to_string(state.segment_count) +
                       " highest_requested=" + std::to_string(state.highest_requested) +
                       " finished=" + std::string(state.finished ? "true" : "false"));
            if (playlist.empty()) {
                if (!state.error.empty()) return http_error(503, "stream_failed", state.error);
                return http_error(404, "not_ready", "playlist not ready");
            }
            Bytes bytes(playlist.begin(), playlist.end());
            auto response = bytes_response(request, std::move(bytes), "application/vnd.apple.mpegurl");
            response.headers["Cache-Control"] = "no-store";
            return response;
        }
        std::optional<Bytes> object;
        if (auto index = segment_index(name)) {
            // A VOD playlist is complete and immutable from first publication,
            // so clients are allowed to ask for a valid future fragment. Demand
            // wakes the sequential producer and this HTTP request waits for that
            // fragment instead of returning a transient 404.
            active->note_segment_requested(*index);
            object = store->wait_object(name, {});
            auto state = store->snapshot();
            if (object) {
                Log::debug("playback stream segment session=" + session->id +
                           " generation=" + std::to_string(session->generation) +
                           " index=" + std::to_string(*index) +
                           " bytes=" + std::to_string(object->size()) +
                           " segments_ready=" + std::to_string(state.segment_count));
            }
        } else {
            object = store->object(name);
        }
        if (!object) {
            auto state = store->snapshot();
            if (!state.error.empty()) return http_error(503, "stream_failed", state.error);
            return http_error(404, state.finished ? "not_found" : "not_ready",
                              state.finished ? "stream object not found" : "stream object not ready");
        }
        return bytes_response(request, std::move(*object), file_mime(name));
    }

    std::vector<std::string> item_media(std::string_view item_id) const {
        auto item = catalogue.get(item_id);
        if (!item) throw std::out_of_range("catalogue item not found");
        return item->media_ids;
    }

    HttpResponse create(const HttpRequest& request) {
        if (!config.enabled) return http_error(503, "streaming_disabled", "streaming is disabled");
        auto trace = hex_token(4);
        auto request_started = Clock::now();
        Log::info("playback[" + trace + "] session create start");
        Json root = Json::parse(std::string_view(reinterpret_cast<const char*>(request.body.data()), request.body.size()));
        if (!root.isObject()) return http_error(400, "bad_request", "JSON object required");
        std::string item_id, media_id;
        if (auto v = root.find("item_id"); v && v->isString()) item_id = v->asString();
        if (auto v = root.find("media_id"); v && v->isString()) media_id = v->asString();
        if (item_id.empty() && media_id.empty()) return http_error(400, "bad_request", "item_id or media_id is required");
        auto caps = parse_capabilities(root.find("capabilities"));
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
        reserve_session_slot();
        bool resources_reserved = false;
        std::shared_ptr<Session> session;
        try {
            session = resolve_session(item_id, std::move(media), std::move(caps), std::move(prefs), trace);
            if (seek_ms) session->plan.seek = std::chrono::milliseconds(*seek_ms);
            prepare_transformed_vod(*session, trace);
            reserve_resources(session->plan);
            resources_reserved = true;
            start_pipeline(*session, trace);
            {
                std::lock_guard lock(mutex);
                sessions[session->id] = session;
                if (pending_sessions) --pending_sessions;
                release_resources_locked(session->plan);
                resources_reserved = false;
            }
        } catch (...) {
            if (session) stop_pipeline(*session);
            if (resources_reserved && session) release_resources(session->plan);
            release_session_slot();
            throw;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - request_started).count();
        Log::info("playback[" + trace + "] session create complete id=" + session->id +
                  " mode=" + playback_mode_name(session->plan.mode) + " elapsed_ms=" + std::to_string(elapsed));
        auto payload = session_json(*session);
        payload["trace_id"] = trace;
        auto response = http_json(201, payload.dump());
        response.headers["Location"] = "/api/v1/playback/sessions/" + session->id;
        response.headers["X-Macha-Playback-Trace"] = trace;
        return response;
    }

    HttpResponse get_session(std::string_view id) {
        std::shared_ptr<Session> session;
        {
            std::lock_guard lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end()) return http_error(404, "not_found", "playback session not found");
            session = it->second;
            session->touched = Clock::now();
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
        const bool seek_only = seek_ms.has_value() && preference_patch == nullptr &&
                               media_override.empty();
        std::shared_ptr<Session> replacement;
        if (seek_only)
            replacement = reuse_seek_session(*old, std::chrono::milliseconds(*seek_ms), trace);

        if (!replacement) {
            auto media = media_override.empty()
                             ? (old->item_id.empty() ? std::vector<std::string>{old->source.media_id}
                                                     : item_media(old->item_id))
                             : std::vector<std::string>{media_override};
            replacement = resolve_session(old->item_id, std::move(media), old->capabilities, prefs,
                                          trace, old->id, old->token);
            replacement->generation = old->generation;
            if (seek_ms) replacement->plan.seek = std::chrono::milliseconds(*seek_ms);
            prepare_transformed_vod(*replacement, trace);
        }
        bool resources_reserved = false;
        try {
            reserve_resources(replacement->plan, old->id);
            resources_reserved = true;
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
                release_resources_locked(replacement->plan);
                resources_reserved = false;
            }
        } catch (...) {
            stop_pipeline(*replacement);
            if (resources_reserved) release_resources(replacement->plan);
            throw;
        }
        std::error_code ec;
        if (!old->generation_dir.empty() && old->generation_dir != replacement->generation_dir)
            std::filesystem::remove_all(old->generation_dir, ec);
        return http_json(200, session_json(*replacement).dump());
    }

    HttpResponse erase_session(std::string_view id) {
        std::shared_ptr<Session> session;
        {
            std::lock_guard lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end()) return http_error(404, "not_found", "playback session not found");
            session = it->second;
            sessions.erase(it);
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
        {
            std::lock_guard lock(mutex);
            session_count = sessions.size();
            video_transcodes = video_transcodes_locked();
            audio_transcodes = audio_transcodes_locked();
        }
        Json::Object out{{"server_version", std::string(kServerVersion)},
                         {"enabled", config.enabled},
                         {"sessions", static_cast<uint64_t>(session_count)},
                         {"max_sessions", static_cast<uint64_t>(config.max_sessions)},
                         {"video_transcodes", static_cast<uint64_t>(video_transcodes)},
                         {"max_video_transcodes", static_cast<uint64_t>(config.max_video_transcodes)},
                         {"audio_transcodes", static_cast<uint64_t>(audio_transcodes)},
                         {"max_audio_transcodes", static_cast<uint64_t>(config.max_audio_transcodes)},
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

    HttpResponse handle_api(const HttpRequest& request) {
        if (request.path == "/api/v1/playback/status" && request.method == "GET") return status();
        if (request.path == "/api/v1/playback/sessions" && request.method == "POST") return create(request);
        constexpr std::string_view sessions_prefix = "/api/v1/playback/sessions/";
        if (request.path.starts_with(sessions_prefix)) {
            auto id = std::string_view(request.path).substr(sessions_prefix.size());
            if (id.empty() || id.find('/') != std::string_view::npos) return http_error(404, "not_found", "endpoint not found");
            if (request.method == "GET") return get_session(id);
            if (request.method == "PATCH") return update_session(id, request);
            if (request.method == "DELETE") return erase_session(id);
            return http_error(405, "method", "GET, PATCH or DELETE required");
        }
        if (request.path.starts_with("/api/v1/playback/stream/")) return public_stream_response(request);
        return http_error(404, "not_found", "endpoint not found");
    }

    void cleanup(std::stop_token stop) {
        set_thread_name("macha-play-gc");
        while (!stop.stop_requested()) {
            std::vector<std::shared_ptr<Session>> expired;
            {
                std::lock_guard lock(mutex);
                auto now = Clock::now();
                for (auto it = sessions.begin(); it != sessions.end();) {
                    if (now - it->second->touched >= config.session_idle) {
                        expired.push_back(it->second);
                        it = sessions.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (auto& session : expired) {
                stop_pipeline(*session);
                std::error_code ec;
                std::filesystem::remove_all(*config.temp_path / session->id, ec);
            }
            for (int i = 0; i < 10 && !stop.stop_requested(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};

PlaybackManager::PlaybackManager(FileSystem& fs, CatalogueManager& catalogue, CatalogueApiConfig api,
                                 StreamingConfig streaming, std::unique_ptr<MediaEngine> engine)
    : impl_(std::make_unique<Impl>(fs, catalogue, std::move(api), std::move(streaming), std::move(engine))) {}

PlaybackManager::~PlaybackManager() { stop(); }

void PlaybackManager::start() {
    if (impl_->started || !impl_->config.enabled) return;
    std::filesystem::create_directories(*impl_->config.temp_path);
    if (!impl_->engine) throw std::runtime_error("streaming media engine is unavailable");
    auto status = impl_->engine->status();
    if (!status.available) throw std::runtime_error("streaming media engine is unavailable");
    impl_->cleanup_thread = std::jthread([this](std::stop_token stop) { impl_->cleanup(stop); });
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
    std::vector<std::shared_ptr<Impl::Session>> sessions;
    {
        std::lock_guard lock(impl_->mutex);
        for (auto& [_, session] : impl_->sessions) sessions.push_back(session);
        impl_->sessions.clear();
    }
    for (auto& session : sessions) impl_->stop_pipeline(*session);
    impl_->started = false;
}

void PlaybackManager::request_stop() {
    if (!impl_ || !impl_->started) return;
    if (impl_->cleanup_thread.joinable())
        impl_->cleanup_thread.request_stop();
}

void PlaybackManager::reconfigure(StreamingConfig config) {
    std::lock_guard lock(impl_->mutex);
    // Backend, probe, buffering and temp-path changes require a service restart.
    // Policy limits and playback timing apply to subsequent sessions immediately.
    impl_->config.max_sessions = config.max_sessions;
    impl_->config.max_video_transcodes = config.max_video_transcodes;
    impl_->config.max_audio_transcodes = config.max_audio_transcodes;
    impl_->config.session_idle = config.session_idle;
    impl_->config.startup_timeout = config.startup_timeout;
    impl_->config.segment_duration = config.segment_duration;
    impl_->config.max_ahead_segments = config.max_ahead_segments;
}

HttpResponse PlaybackManager::handle(const HttpRequest& request) {
    auto began = Clock::now();
    try {
        return impl_->handle_api(request);
    } catch (const JsonError& e) {
        return http_error(400, "bad_json", e.what());
    } catch (const std::invalid_argument& e) {
        return http_error(400, "bad_playback_request", e.what());
    } catch (const std::out_of_range& e) {
        return http_error(404, "not_found", e.what());
    } catch (const ResourceLimitError& e) {
        return http_error(429, "resource_limit", e.what());
    } catch (const PlaybackStageError& e) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count();
        Log::warn("playback[" + e.trace() + "] request failed stage=" + e.stage() +
                  " method=" + request.method + " path=" + request.path +
                  " elapsed_ms=" + std::to_string(elapsed) + " error=" + e.what());
        Json::Object body{{"error", "playback_" + e.stage() + "_failed"},
                          {"message", std::string(e.what())},
                          {"trace", e.trace()},
                          {"stage", e.stage()}};
        auto response = http_json(503, Json(std::move(body)).dump());
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
    return request.path.starts_with("/api/v1/playback/stream/");
}

} // namespace macha
