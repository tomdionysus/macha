// SPDX-License-Identifier: GPL-3.0-or-later
#include "playback.hpp"

#include "crypto.hpp"
#include "json.hpp"
#include "log.hpp"

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

class LocalFileBody final : public HttpBodySource {
    int fd_{-1};
    uint64_t base_{};
    uint64_t size_{};
  public:
    LocalFileBody(const std::filesystem::path& path, uint64_t base, uint64_t size)
        : base_(base), size_(size) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error("cannot open stream file");
    }
    ~LocalFileBody() override { if (fd_ >= 0) ::close(fd_); }
    uint64_t size() const override { return size_; }
    size_t read(uint64_t offset, std::span<uint8_t> destination) override {
        if (offset >= size_) return 0;
        auto wanted = static_cast<size_t>(std::min<uint64_t>(destination.size(), size_ - offset));
        while (true) {
            auto n = pread(fd_, destination.data(), wanted, static_cast<off_t>(base_ + offset));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return 0;
            return static_cast<size_t>(n);
        }
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
                     {"forced", stream.forced}};
    if (stream.width) out["width"] = stream.width;
    if (stream.height) out["height"] = stream.height;
    if (stream.channels) out["channels"] = stream.channels;
    if (stream.sample_rate) out["sample_rate"] = stream.sample_rate;
    if (stream.bit_depth) out["bit_depth"] = stream.bit_depth;
    return Json(std::move(out));
}

} // namespace

struct PlaybackManager::Impl {
    struct SourceLease {
        std::string token;
        std::string media_id;
        std::string path;
        FsEntry entry;
        Clock::time_point touched{Clock::now()};
    };

    struct Session {
        std::string id;
        std::string token;
        std::string item_id;
        ClientCapabilities capabilities;
        PlaybackPreferences preferences;
        MediaSource source;
        MediaProbeResult probe;
        PlaybackPlan plan;
        std::string source_token;
        uint64_t generation{};
        std::filesystem::path generation_dir;
        std::unique_ptr<MediaEngineSession> engine_session;
        std::string stream_url;
        std::string subtitle_url;
        uint64_t highest_segment_requested{};
        bool producer_paused{};
        Clock::time_point touched{Clock::now()};
    };

    FileSystem& fs;
    CatalogueManager& catalogue;
    CatalogueApiConfig api_config;
    StreamingConfig config;
    std::unique_ptr<MediaEngine> engine;
    std::unique_ptr<HttpServer> source_http;
    std::jthread cleanup_thread;
    mutable std::mutex mutex;
    std::map<std::string, SourceLease, std::less<>> sources;
    std::map<std::string, std::shared_ptr<Session>, std::less<>> sessions;
    std::map<std::string, MediaProbeResult, std::less<>> probe_cache;
    bool started{};

    Impl(FileSystem& filesystem, CatalogueManager& cat, CatalogueApiConfig api,
         StreamingConfig streaming, std::unique_ptr<MediaEngine> media_engine)
        : fs(filesystem), catalogue(cat), api_config(std::move(api)), config(std::move(streaming)),
          engine(media_engine ? std::move(media_engine) :
                (config.enabled ? make_ffmpeg_process_engine(config) : nullptr)) {}

    std::string source_url(std::string_view token) const {
        return "http://127.0.0.1:" + std::to_string(source_http->bound_port()) + "/source/" + std::string(token);
    }

    std::string public_stream_prefix(const Session& session) const {
        return "/api/v1/playback/stream/" + session.id + "/" + session.token;
    }

    SourceLease create_source(std::string_view media_id) {
        auto found = fs.find_media(media_id);
        if (!found) throw std::out_of_range("media object not found in filesystem");
        SourceLease lease;
        lease.token = hex_token();
        lease.media_id = std::string(media_id);
        lease.path = found->first;
        lease.entry = found->second;
        {
            std::lock_guard lock(mutex);
            sources[lease.token] = lease;
        }
        return lease;
    }

    void remove_source(std::string_view token) {
        std::lock_guard lock(mutex);
        sources.erase(std::string(token));
    }

    std::string probe_key(const SourceLease& lease) const {
        if (lease.media_id.starts_with("path:") || (!lease.media_id.empty() && lease.media_id.front() == '/')) {
            return lease.media_id + "#" + std::to_string(lease.entry.version) + ":" +
                   std::to_string(lease.entry.size) + ":" + std::to_string(lease.entry.mtime_ns);
        }
        return lease.media_id;
    }

    MediaProbeResult probe_source(const SourceLease& lease) {
        auto key = probe_key(lease);
        {
            std::lock_guard lock(mutex);
            if (auto it = probe_cache.find(key); it != probe_cache.end()) return it->second;
        }
        MediaSource source{lease.media_id, lease.path, source_url(lease.token), lease.entry.size};
        auto probed = engine->probe(source);
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
            if (session->plan.video == MediaTransform::transcode &&
                session->engine_session && session->engine_session->running()) ++count;
        }
        return count;
    }

    size_t audio_transcodes_locked(std::string_view excluding = {}) const {
        size_t count = 0;
        for (const auto& [id, session] : sessions) {
            if (id == excluding) continue;
            if (session->plan.audio == MediaTransform::transcode &&
                session->engine_session && session->engine_session->running()) ++count;
        }
        return count;
    }

    void check_resources(const PlaybackPlan& plan, std::string_view excluding = {}) {
        std::lock_guard lock(mutex);
        if (plan.video == MediaTransform::transcode &&
            video_transcodes_locked(excluding) >= config.max_video_transcodes)
            throw ResourceLimitError("video transcode limit reached");
        if (plan.audio == MediaTransform::transcode &&
            audio_transcodes_locked(excluding) >= config.max_audio_transcodes)
            throw ResourceLimitError("audio transcode limit reached");
    }

    void wait_for_playlist(Session& session) {
        auto playlist = session.generation_dir / "master.m3u8";
        auto deadline = Clock::now() + config.startup_timeout;
        while (Clock::now() < deadline) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(playlist, ec) && std::filesystem::file_size(playlist, ec) > 0) return;
            if (session.engine_session && !session.engine_session->running()) {
                auto diagnostic = session.engine_session->diagnostics();
                throw std::runtime_error("ffmpeg exited before producing HLS: " + diagnostic);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        throw std::runtime_error("timed out waiting for first HLS playlist");
    }

    void stop_pipeline(Session& session) {
        if (session.engine_session) {
            session.engine_session->stop();
            session.engine_session.reset();
        }
    }

    void start_pipeline(Session& session) {
        stop_pipeline(session);
        ++session.generation;
        session.generation_dir = *config.temp_path / session.id / std::to_string(session.generation);
        std::filesystem::create_directories(session.generation_dir);
        session.subtitle_url.clear();
        session.highest_segment_requested = 0;
        session.producer_paused = false;
        auto prefix = public_stream_prefix(session);
        if (session.plan.mode == PlaybackMode::direct) {
            session.stream_url = prefix + "/direct";
        } else {
            check_resources(session.plan, session.id);
            session.engine_session = engine->start_hls(session.source, session.plan, session.generation_dir,
                                                       config.segment_duration);
            try {
                wait_for_playlist(session);
            } catch (...) {
                stop_pipeline(session);
                throw;
            }
            session.stream_url = prefix + "/" + std::to_string(session.generation) + "/master.m3u8";
        }
        if (session.plan.subtitle_stream >= 0) {
            auto subtitle_path = session.generation_dir / "subtitle.vtt";
            try {
                auto subtitle_seek = session.plan.mode == PlaybackMode::direct
                                         ? std::chrono::milliseconds{} : session.plan.seek;
                engine->extract_webvtt(session.source, session.plan.subtitle_stream, subtitle_path, subtitle_seek);
                session.subtitle_url = prefix + "/" + std::to_string(session.generation) + "/subtitle.vtt";
            } catch (const std::exception& e) {
                Log::debug("subtitle extraction skipped: " + std::string(e.what()));
            }
        }
    }

    std::shared_ptr<Session> resolve_session(std::string item_id, std::vector<std::string> media_ids,
                                             ClientCapabilities capabilities, PlaybackPreferences preferences,
                                             std::string existing_id = {}, std::string existing_token = {}) {
        if (media_ids.empty()) throw std::runtime_error("no media representations are available");
        struct Candidate {
            SourceLease lease;
            MediaProbeResult probe;
            PlaybackPlan plan;
            int rank{};
        };
        std::optional<Candidate> best;
        std::vector<std::string> temporary_tokens;
        std::exception_ptr last_exception;
        for (const auto& media_id : media_ids) {
            try {
                auto lease = create_source(media_id);
                temporary_tokens.push_back(lease.token);
                auto probe = probe_source(lease);
                auto plan = negotiate(probe, lease.path, capabilities, preferences);
                int rank = plan.mode == PlaybackMode::direct ? 0 : (plan.mode == PlaybackMode::remux ? 1 : 2);
                if (!best || rank < best->rank) best = Candidate{std::move(lease), std::move(probe), plan, rank};
                if (rank == 0) break;
            } catch (...) {
                last_exception = std::current_exception();
            }
        }
        if (!best) {
            for (const auto& token : temporary_tokens) remove_source(token);
            if (last_exception) std::rethrow_exception(last_exception);
            throw std::runtime_error("no playable media representation");
        }
        for (const auto& token : temporary_tokens)
            if (token != best->lease.token) remove_source(token);

        auto session = std::make_shared<Session>();
        session->id = existing_id.empty() ? hex_token(16) : std::move(existing_id);
        session->token = existing_token.empty() ? hex_token() : std::move(existing_token);
        session->item_id = std::move(item_id);
        session->capabilities = std::move(capabilities);
        session->preferences = std::move(preferences);
        session->source_token = best->lease.token;
        session->source = MediaSource{best->lease.media_id, best->lease.path, source_url(best->lease.token), best->lease.entry.size};
        session->probe = std::move(best->probe);
        session->plan = best->plan;
        session->touched = Clock::now();
        return session;
    }

    Json session_json(const Session& session) const {
        Json::Array streams;
        for (const auto& stream : session.probe.streams) streams.push_back(stream_json(stream));
        Json::Object selected{{"video_stream", session.plan.video_stream},
                              {"audio_stream", session.plan.audio_stream},
                              {"subtitle_stream", session.plan.subtitle_stream}};
        Json::Object transformations{{"video", session.plan.video == MediaTransform::copy ? "copy" :
                                                   session.plan.video == MediaTransform::transcode ? "transcode" : "omit"},
                                     {"audio", session.plan.audio == MediaTransform::copy ? "copy" :
                                                   session.plan.audio == MediaTransform::transcode ? "transcode" : "omit"}};
        Json::Array modes;
        for (const auto* candidate : {"direct", "remux", "transcode"}) {
            auto preferences = session.preferences;
            preferences.mode = candidate;
            try {
                (void)negotiate(session.probe, session.source.logical_path, session.capabilities, preferences);
                modes.emplace_back(candidate);
            } catch (...) {}
        }
        Json::Array audio_streams, subtitle_streams;
        for (const auto& stream : session.probe.streams) {
            if (stream.type == MediaStreamType::audio) audio_streams.emplace_back(stream_json(stream));
            if (stream.type == MediaStreamType::subtitle) subtitle_streams.emplace_back(stream_json(stream));
        }
        Json::Array media_ids;
        if (!session.item_id.empty()) {
            try {
                for (const auto& media_id : item_media(session.item_id)) media_ids.emplace_back(media_id);
            } catch (...) {}
        } else {
            media_ids.emplace_back(session.source.media_id);
        }
        Json::Object options{{"modes", Json(std::move(modes))},
                             {"media_ids", Json(std::move(media_ids))},
                             {"audio_streams", Json(std::move(audio_streams))},
                             {"subtitle_streams", Json(std::move(subtitle_streams))},
                             {"can_seek", true},
                             {"can_change_quality", true},
                             {"can_switch_media", !session.item_id.empty()}};
        Json::Object out{{"session_id", session.id},
                         {"media_id", session.source.media_id},
                         {"mode", playback_mode_name(session.plan.mode)},
                         {"mime_type", session.plan.mode == PlaybackMode::direct ? direct_mime(session.source.logical_path)
                                                                                : "application/vnd.apple.mpegurl"},
                         {"stream_url", session.stream_url},
                         {"subtitle_url", session.subtitle_url.empty() ? Json(nullptr) : Json(session.subtitle_url)},
                         {"selected", Json(std::move(selected))},
                         {"transform", Json(std::move(transformations))},
                         {"options", Json(std::move(options))},
                         {"streams", Json(std::move(streams))},
                         {"source_format", session.probe.format},
                         {"duration_ms", static_cast<uint64_t>(std::max(0.0, session.probe.duration_seconds) * 1000.0)},
                         {"source_bitrate", session.probe.bitrate}};
        if (!session.item_id.empty()) out["item_id"] = session.item_id;
        return Json(std::move(out));
    }

    HttpResponse source_response(const HttpRequest& request) {
        constexpr std::string_view prefix = "/source/";
        auto token = request.path.substr(prefix.size());
        SourceLease lease;
        {
            std::lock_guard lock(mutex);
            auto it = sources.find(token);
            if (it == sources.end()) return http_error(404, "not_found", "source lease not found");
            it->second.touched = Clock::now();
            lease = it->second;
        }
        if (request.method != "GET" && request.method != "HEAD") return http_error(405, "method", "GET or HEAD required");
        return ranged_response(request, lease.entry.size, "application/octet-stream",
                               [this, path = lease.path, entry = lease.entry](uint64_t offset, uint64_t length) {
                                   return std::make_shared<LogicalBody>(fs.open_read(entry, path), offset, length);
                               });
    }

    void note_segment_request(Session& session, std::string_view name) {
        constexpr std::string_view prefix = "segment-";
        constexpr std::string_view suffix = ".m4s";
        if (!name.starts_with(prefix) || !name.ends_with(suffix)) return;
        auto number = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        uint64_t index = 0;
        auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), index);
        if (ec != std::errc{} || end != number.data() + number.size()) return;
        session.highest_segment_requested = std::max(session.highest_segment_requested, index);
    }

    void manage_backpressure(Session& session) {
        if (!session.engine_session || session.plan.mode == PlaybackMode::direct || !session.engine_session->running()) return;
        uint64_t highest_generated = 0;
        bool found = false;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(session.generation_dir, ec)) {
            if (ec) break;
            auto name = entry.path().filename().string();
            constexpr std::string_view prefix = "segment-";
            constexpr std::string_view suffix = ".m4s";
            if (!std::string_view(name).starts_with(prefix) || !std::string_view(name).ends_with(suffix)) continue;
            auto number = std::string_view(name).substr(prefix.size(), name.size() - prefix.size() - suffix.size());
            uint64_t index = 0;
            auto [end, parse_ec] = std::from_chars(number.data(), number.data() + number.size(), index);
            if (parse_ec == std::errc{} && end == number.data() + number.size()) {
                highest_generated = std::max(highest_generated, index);
                found = true;
            }
        }
        if (!found) return;
        const auto ahead = config.max_ahead_segments;
        if (!session.producer_paused && highest_generated > session.highest_segment_requested + ahead) {
            session.engine_session->set_paused(true);
            session.producer_paused = true;
        } else if (session.producer_paused && highest_generated <= session.highest_segment_requested + std::max<size_t>(1, ahead / 2)) {
            session.engine_session->set_paused(false);
            session.producer_paused = false;
        }
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
        SourceLease source_lease;
        {
            std::lock_guard lock(mutex);
            auto it = sessions.find(id);
            if (it == sessions.end() || it->second->token != token)
                return http_error(404, "not_found", "stream not found");
            session = it->second;
            session->touched = Clock::now();
            auto source = sources.find(session->source_token);
            if (source == sources.end()) return http_error(404, "not_found", "media source lease expired");
            source->second.touched = Clock::now();
            source_lease = source->second;
        }
        if (request.method != "GET" && request.method != "HEAD") return http_error(405, "method", "GET or HEAD required");
        if (rest == "direct") {
            return ranged_response(request, source_lease.entry.size, direct_mime(source_lease.path),
                                   [this, path = source_lease.path, entry = source_lease.entry](uint64_t offset, uint64_t length) {
                                       return std::make_shared<LogicalBody>(fs.open_read(entry, path), offset, length);
                                   });
        }
        auto slash3 = rest.find('/');
        if (slash3 == std::string_view::npos) return http_error(404, "not_found", "stream file not found");
        uint64_t generation = 0;
        auto generation_text = rest.substr(0, slash3);
        auto [end, ec] = std::from_chars(generation_text.data(), generation_text.data() + generation_text.size(), generation);
        if (ec != std::errc{} || end != generation_text.data() + generation_text.size() || generation != session->generation)
            return http_error(404, "not_found", "stream generation not found");
        auto name = std::string(rest.substr(slash3 + 1));
        if (name.empty() || name.find('/') != std::string::npos || name == "." || name == ".." || name.find("..") != std::string::npos)
            return http_error(400, "bad_path", "invalid stream filename");
        {
            std::lock_guard lock(mutex);
            note_segment_request(*session, name);
        }
        auto path = session->generation_dir / name;
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) return http_error(404, "not_ready", "stream file not ready");
        auto size = std::filesystem::file_size(path, error);
        if (error) return http_error(404, "not_found", "stream file not found");
        return ranged_response(request, size, file_mime(name),
                               [path](uint64_t offset, uint64_t length) {
                                   return std::make_shared<LocalFileBody>(path, offset, length);
                               });
    }

    std::vector<std::string> item_media(std::string_view item_id) const {
        auto item = catalogue.get(item_id);
        if (!item) throw std::out_of_range("catalogue item not found");
        return item->media_ids;
    }

    HttpResponse create(const HttpRequest& request) {
        if (!config.enabled) return http_error(503, "streaming_disabled", "streaming is disabled");
        Json root = Json::parse(std::string_view(reinterpret_cast<const char*>(request.body.data()), request.body.size()));
        if (!root.isObject()) return http_error(400, "bad_request", "JSON object required");
        std::string item_id, media_id;
        if (auto v = root.find("item_id"); v && v->isString()) item_id = v->asString();
        if (auto v = root.find("media_id"); v && v->isString()) media_id = v->asString();
        if (item_id.empty() && media_id.empty()) return http_error(400, "bad_request", "item_id or media_id is required");
        auto caps = parse_capabilities(root.find("capabilities"));
        auto prefs = parse_preferences(root.find("preferences"));
        auto media = media_id.empty() ? item_media(item_id) : std::vector<std::string>{media_id};
        {
            std::lock_guard lock(mutex);
            if (sessions.size() >= config.max_sessions) return http_error(429, "session_limit", "playback session limit reached");
        }
        auto session = resolve_session(item_id, std::move(media), std::move(caps), std::move(prefs));
        check_resources(session->plan);
        start_pipeline(*session);
        {
            std::lock_guard lock(mutex);
            sessions[session->id] = session;
        }
        auto response = http_json(201, session_json(*session).dump());
        response.headers["Location"] = "/api/v1/playback/sessions/" + session->id;
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
        if (session->engine_session) {
            result["engine_running"] = session->engine_session->running();
            if (auto code = session->engine_session->exit_code()) result["engine_exit_code"] = *code;
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
        Json root = Json::parse(std::string_view(reinterpret_cast<const char*>(request.body.data()), request.body.size()));
        if (!root.isObject()) return http_error(400, "bad_request", "JSON object required");
        auto prefs = parse_preferences(root.find("preferences"), old->preferences);
        std::optional<uint64_t> seek_ms;
        if (auto seek = root.find("seek_ms")) {
            seek_ms = optional_u64(seek);
            if (!seek_ms) return http_error(400, "bad_seek", "seek_ms must be non-negative");
        }
        std::string media_override;
        if (auto media = root.find("media_id"); media && media->isString()) media_override = media->asString();
        auto media = media_override.empty() ? (old->item_id.empty() ? std::vector<std::string>{old->source.media_id}
                                                                   : item_media(old->item_id))
                                            : std::vector<std::string>{media_override};
        auto replacement = resolve_session(old->item_id, std::move(media), old->capabilities, prefs,
                                           old->id, old->token);
        replacement->generation = old->generation;
        if (seek_ms) replacement->plan.seek = std::chrono::milliseconds(*seek_ms);
        check_resources(replacement->plan, old->id);
        start_pipeline(*replacement);
        {
            std::lock_guard lock(mutex);
            sessions[std::string(id)] = replacement;
            sources.erase(old->source_token);
        }
        stop_pipeline(*old);
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
            sources.erase(session->source_token);
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
        Json::Object out{{"enabled", config.enabled},
                         {"sessions", static_cast<uint64_t>(session_count)},
                         {"max_sessions", static_cast<uint64_t>(config.max_sessions)},
                         {"video_transcodes", static_cast<uint64_t>(video_transcodes)},
                         {"max_video_transcodes", static_cast<uint64_t>(config.max_video_transcodes)},
                         {"audio_transcodes", static_cast<uint64_t>(audio_transcodes)},
                         {"max_audio_transcodes", static_cast<uint64_t>(config.max_audio_transcodes)},
                         {"ffmpeg_available", state.ffmpeg_available},
                         {"ffprobe_available", state.ffprobe_available},
                         {"ffmpeg_version", state.ffmpeg_version}};
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
        while (!stop.stop_requested()) {
            std::vector<std::shared_ptr<Session>> expired;
            {
                std::lock_guard lock(mutex);
                auto now = Clock::now();
                for (auto it = sessions.begin(); it != sessions.end();) {
                    if (now - it->second->touched >= config.session_idle) {
                        expired.push_back(it->second);
                        sources.erase(it->second->source_token);
                        it = sessions.erase(it);
                    } else {
                        ++it;
                    }
                }
                for (auto it = sources.begin(); it != sources.end();) {
                    if (now - it->second.touched >= config.session_idle) it = sources.erase(it);
                    else ++it;
                }
            }
            for (auto& session : expired) {
                stop_pipeline(*session);
                std::error_code ec;
                std::filesystem::remove_all(*config.temp_path / session->id, ec);
            }
            std::vector<std::shared_ptr<Session>> active;
            {
                std::lock_guard lock(mutex);
                for (const auto& [_, session] : sessions) active.push_back(session);
            }
            for (auto& session : active) {
                std::lock_guard lock(mutex);
                manage_backpressure(*session);
            }
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
    CatalogueApiConfig internal;
    internal.enabled = true;
    internal.listen = "127.0.0.1";
    internal.port = 0;
    internal.workers = 4;
    internal.max_queued_connections = 64;
    internal.stream_chunk_bytes = impl_->api_config.stream_chunk_bytes;
    impl_->source_http = std::make_unique<HttpServer>(internal, [this](const HttpRequest& request) {
        if (request.path.starts_with("/source/")) return impl_->source_response(request);
        return http_error(404, "not_found", "source endpoint not found");
    });
    impl_->source_http->start();
    auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!impl_->source_http->bound_port() && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!impl_->source_http->bound_port()) {
        impl_->source_http->stop();
        impl_->source_http.reset();
        throw std::runtime_error("playback source HTTP server failed to start");
    }
    impl_->cleanup_thread = std::jthread([this](std::stop_token stop) { impl_->cleanup(stop); });
    impl_->started = true;
    auto status = impl_->engine->status();
    Log::info("streaming enabled ffmpeg=" + std::string(status.ffmpeg_available ? "yes" : "no") +
              " ffprobe=" + std::string(status.ffprobe_available ? "yes" : "no"));
}

void PlaybackManager::stop() {
    if (!impl_ || !impl_->started) return;
    if (impl_->cleanup_thread.joinable()) {
        impl_->cleanup_thread.request_stop();
        impl_->cleanup_thread.join();
    }
    std::vector<std::shared_ptr<Impl::Session>> sessions;
    {
        std::lock_guard lock(impl_->mutex);
        for (auto& [_, session] : impl_->sessions) sessions.push_back(session);
        impl_->sessions.clear();
        impl_->sources.clear();
    }
    for (auto& session : sessions) impl_->stop_pipeline(*session);
    if (impl_->source_http) {
        impl_->source_http->stop();
        impl_->source_http.reset();
    }
    impl_->started = false;
}

void PlaybackManager::reconfigure(StreamingConfig config) {
    std::lock_guard lock(impl_->mutex);
    // Engine executable/backend changes deliberately require restart. Live reload
    // covers policy limits and idle/segment timing for subsequent sessions.
    impl_->config.max_sessions = config.max_sessions;
    impl_->config.max_video_transcodes = config.max_video_transcodes;
    impl_->config.max_audio_transcodes = config.max_audio_transcodes;
    impl_->config.session_idle = config.session_idle;
    impl_->config.startup_timeout = config.startup_timeout;
    impl_->config.segment_duration = config.segment_duration;
    impl_->config.max_ahead_segments = config.max_ahead_segments;
}

HttpResponse PlaybackManager::handle(const HttpRequest& request) {
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
    } catch (const std::exception& e) {
        return http_error(503, "playback_unavailable", e.what());
    }
}

bool PlaybackManager::capability_request(const HttpRequest& request) const {
    return request.path.starts_with("/api/v1/playback/stream/");
}

} // namespace macha
