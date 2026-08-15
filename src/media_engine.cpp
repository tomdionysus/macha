// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_engine.hpp"

#include "json.hpp"
#include "log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace macha {
namespace {

struct CaptureResult {
    int exit_code{-1};
    std::string out;
    std::string err;
};

std::vector<char*> argv_ptrs(std::vector<std::string>& args) {
    std::vector<char*> out;
    out.reserve(args.size() + 1);
    for (auto& arg : args) out.push_back(arg.data());
    out.push_back(nullptr);
    return out;
}

CaptureResult run_capture(std::vector<std::string> args, size_t max_bytes = 16 * 1024 * 1024) {
    if (args.empty()) throw std::runtime_error("empty process argv");
    int out_pipe[2]{-1, -1}, err_pipe[2]{-1, -1};
    if (pipe(out_pipe) || pipe(err_pipe)) throw std::runtime_error("pipe failed");
    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        (void)dup2(out_pipe[1], STDOUT_FILENO);
        (void)dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
        auto ptrs = argv_ptrs(args);
        execvp(ptrs[0], ptrs.data());
        _exit(127);
    }
    close(out_pipe[1]); close(err_pipe[1]);
    (void)fcntl(out_pipe[0], F_SETFL, fcntl(out_pipe[0], F_GETFL) | O_NONBLOCK);
    (void)fcntl(err_pipe[0], F_SETFL, fcntl(err_pipe[0], F_GETFL) | O_NONBLOCK);

    CaptureResult result;
    bool out_open = true, err_open = true;
    std::array<char, 8192> buffer{};
    while (out_open || err_open) {
        pollfd fds[2]{{out_pipe[0], static_cast<short>(POLLIN | POLLHUP), 0},
                      {err_pipe[0], static_cast<short>(POLLIN | POLLHUP), 0}};
        (void)poll(fds, 2, 100);
        auto drain = [&](int fd, std::string& target, bool& open, short events) {
            if (!open || !(events & (POLLIN | POLLHUP | POLLERR))) return;
            while (true) {
                auto n = read(fd, buffer.data(), buffer.size());
                if (n > 0) {
                    if (target.size() + static_cast<size_t>(n) > max_bytes) {
                        kill(pid, SIGKILL);
                        throw std::runtime_error("process output exceeded limit");
                    }
                    target.append(buffer.data(), static_cast<size_t>(n));
                    continue;
                }
                if (n == 0) { close(fd); open = false; }
                if (n < 0 && errno != EAGAIN && errno != EINTR) { close(fd); open = false; }
                break;
            }
        };
        drain(out_pipe[0], result.out, out_open, fds[0].revents);
        drain(err_pipe[0], result.err, err_open, fds[1].revents);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exit_code = 128 + WTERMSIG(status);
    return result;
}

std::optional<std::string> json_string(const Json* value) {
    if (!value || !value->isString()) return {};
    return value->asString();
}

int json_int(const Json* value, int fallback = 0) {
    if (!value) return fallback;
    try { return static_cast<int>(value->asInt64()); } catch (...) {}
    try { return static_cast<int>(value->asUInt64()); } catch (...) {}
    if (value->isString()) {
        int parsed{};
        auto text = value->asString();
        auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (ec == std::errc{} && end == text.data() + text.size()) return parsed;
    }
    return fallback;
}

uint64_t json_u64(const Json* value, uint64_t fallback = 0) {
    if (!value) return fallback;
    try { return value->asUInt64(); } catch (...) {}
    try {
        auto n = value->asInt64();
        return n >= 0 ? static_cast<uint64_t>(n) : fallback;
    } catch (...) {}
    if (value->isString()) {
        uint64_t parsed{};
        auto text = value->asString();
        auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (ec == std::errc{} && end == text.data() + text.size()) return parsed;
    }
    return fallback;
}

double json_double(const Json* value, double fallback = 0.0) {
    if (!value) return fallback;
    try { return value->asNumber(); } catch (...) {}
    if (value->isString()) {
        char* end = nullptr;
        auto text = value->asString();
        auto parsed = std::strtod(text.c_str(), &end);
        if (end == text.c_str() + text.size()) return parsed;
    }
    return fallback;
}

MediaStreamType stream_type(std::string_view value) {
    if (value == "video") return MediaStreamType::video;
    if (value == "audio") return MediaStreamType::audio;
    if (value == "subtitle") return MediaStreamType::subtitle;
    return MediaStreamType::other;
}

class ProcessSession final : public MediaEngineSession {
    mutable std::mutex process_mutex_;
    mutable pid_t pid_{-1};
    int stderr_fd_{-1};
    std::jthread stderr_thread_;
    mutable std::mutex diagnostics_mutex_;
    std::string diagnostics_;
    mutable bool running_{true};
    mutable bool paused_{};
    mutable int exit_code_{-1};

    void record_status_locked(int status) const {
        running_ = false;
        paused_ = false;
        pid_ = -1;
        if (WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) exit_code_ = 128 + WTERMSIG(status);
    }

    bool poll_locked() const {
        if (!running_ || pid_ <= 0) return false;
        int status = 0;
        auto result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            record_status_locked(status);
            return false;
        }
        if (result < 0 && errno == ECHILD) {
            running_ = false;
            paused_ = false;
            pid_ = -1;
            return false;
        }
        return true;
    }

    void read_stderr(std::stop_token stop) {
        std::array<char, 4096> buffer{};
        while (!stop.stop_requested()) {
            auto n = read(stderr_fd_, buffer.data(), buffer.size());
            if (n > 0) {
                std::lock_guard lock(diagnostics_mutex_);
                diagnostics_.append(buffer.data(), static_cast<size_t>(n));
                constexpr size_t keep = 64 * 1024;
                if (diagnostics_.size() > keep) diagnostics_.erase(0, diagnostics_.size() - keep);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        if (stderr_fd_ >= 0) {
            close(stderr_fd_);
            stderr_fd_ = -1;
        }
    }

  public:
    ProcessSession(pid_t pid, int stderr_fd) : pid_(pid), stderr_fd_(stderr_fd) {
        stderr_thread_ = std::jthread([this](std::stop_token stop) { read_stderr(stop); });
    }

    ~ProcessSession() override { stop(); }

    bool running() const override {
        std::lock_guard lock(process_mutex_);
        return poll_locked();
    }

    std::optional<int> exit_code() const override {
        std::lock_guard lock(process_mutex_);
        (void)poll_locked();
        if (exit_code_ < 0) return {};
        return exit_code_;
    }

    std::string diagnostics() const override {
        std::lock_guard lock(diagnostics_mutex_);
        return diagnostics_;
    }

    void set_paused(bool paused) override {
        std::lock_guard lock(process_mutex_);
        if (!poll_locked()) return;
        if (paused_ == paused || pid_ <= 0) return;
        if (::kill(pid_, paused ? SIGSTOP : SIGCONT) == 0) {
            paused_ = paused;
        } else if (errno != ESRCH) {
            Log::debug("ffmpeg pause/resume signal failed: " + std::string(std::strerror(errno)));
        }
    }

    void stop() override {
        {
            std::lock_guard lock(process_mutex_);
            if (poll_locked() && pid_ > 0) {
                if (paused_) {
                    (void)::kill(pid_, SIGCONT);
                    paused_ = false;
                }
                (void)::kill(pid_, SIGTERM);
                for (int i = 0; i < 20; ++i) {
                    int status = 0;
                    auto result = waitpid(pid_, &status, WNOHANG);
                    if (result == pid_) {
                        record_status_locked(status);
                        break;
                    }
                    if (result < 0 && errno == ECHILD) {
                        running_ = false;
                        pid_ = -1;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                if (pid_ > 0) {
                    (void)::kill(pid_, SIGKILL);
                    int status = 0;
                    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
                    running_ = false;
                    paused_ = false;
                    exit_code_ = 137;
                    pid_ = -1;
                }
            }
        }
        if (stderr_thread_.joinable()) {
            stderr_thread_.request_stop();
            stderr_thread_.join();
        }
    }
};
std::unique_ptr<MediaEngineSession> spawn_ffmpeg(std::vector<std::string> args) {
    int err_pipe[2]{-1, -1};
    if (pipe(err_pipe)) throw std::runtime_error("ffmpeg stderr pipe failed");
    pid_t pid = fork();
    if (pid < 0) {
        close(err_pipe[0]); close(err_pipe[1]);
        throw std::runtime_error("ffmpeg fork failed");
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        (void)dup2(err_pipe[1], STDERR_FILENO);
        close(err_pipe[0]); close(err_pipe[1]);
        auto ptrs = argv_ptrs(args);
        execvp(ptrs[0], ptrs.data());
        _exit(127);
    }
    close(err_pipe[1]);
    return std::make_unique<ProcessSession>(pid, err_pipe[0]);
}

class FfmpegProcessEngine final : public MediaEngine {
    StreamingConfig config_;
    MediaEngineStatus status_;

  public:
    explicit FfmpegProcessEngine(StreamingConfig config) : config_(std::move(config)) {
        auto ffmpeg = run_capture({config_.ffmpeg, "-version"}, 256 * 1024);
        status_.ffmpeg_available = ffmpeg.exit_code == 0;
        if (status_.ffmpeg_available) {
            auto newline = ffmpeg.out.find('\n');
            status_.ffmpeg_version = ffmpeg.out.substr(0, newline);
        }
        auto ffprobe = run_capture({config_.ffprobe, "-version"}, 256 * 1024);
        status_.ffprobe_available = ffprobe.exit_code == 0;
    }

    MediaEngineStatus status() const override { return status_; }

    MediaProbeResult probe(const MediaSource& source) override {
        if (!status_.ffprobe_available) throw std::runtime_error("ffprobe is unavailable");
        auto result = run_capture({config_.ffprobe, "-v", "error", "-print_format", "json",
                                   "-show_format", "-show_streams", source.url});
        if (result.exit_code != 0)
            throw std::runtime_error("ffprobe failed: " + result.err);
        auto root = Json::parse(result.out);
        MediaProbeResult out;
        if (auto format = root.find("format"); format && format->isObject()) {
            out.format = json_string(format->find("format_name")).value_or("");
            out.duration_seconds = json_double(format->find("duration"));
            out.bitrate = json_u64(format->find("bit_rate"));
        }
        if (auto streams = root.find("streams"); streams && streams->isArray()) {
            for (const auto& value : streams->asArray()) {
                if (!value.isObject()) continue;
                MediaStreamInfo stream;
                stream.index = json_int(value.find("index"), -1);
                stream.type = stream_type(json_string(value.find("codec_type")).value_or(""));
                stream.codec = json_string(value.find("codec_name")).value_or("");
                stream.profile = json_string(value.find("profile")).value_or("");
                stream.width = json_int(value.find("width"));
                stream.height = json_int(value.find("height"));
                stream.channels = json_int(value.find("channels"));
                stream.sample_rate = json_int(value.find("sample_rate"));
                stream.bit_depth = json_int(value.find("bits_per_raw_sample"));
                if (auto tags = value.find("tags"); tags && tags->isObject())
                    stream.language = json_string(tags->find("language")).value_or("");
                if (auto disposition = value.find("disposition"); disposition && disposition->isObject()) {
                    stream.default_stream = json_int(disposition->find("default")) != 0;
                    stream.forced = json_int(disposition->find("forced")) != 0;
                }
                out.streams.push_back(std::move(stream));
            }
        }
        return out;
    }

    std::unique_ptr<MediaEngineSession> start_hls(
        const MediaSource& source, const PlaybackPlan& plan,
        const std::filesystem::path& output_directory,
        std::chrono::milliseconds segment_duration) override {
        if (!status_.ffmpeg_available) throw std::runtime_error("ffmpeg is unavailable");
        std::filesystem::create_directories(output_directory);
        const auto playlist = output_directory / "master.m3u8";
        const auto segment_pattern = output_directory / "segment-%06d.m4s";
        std::vector<std::string> args{config_.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "warning", "-y"};
        if (plan.seek.count() > 0) {
            args.push_back("-ss");
            args.push_back(std::to_string(static_cast<double>(plan.seek.count()) / 1000.0));
        }
        args.insert(args.end(), {"-i", source.url});
        if (plan.video_stream >= 0) args.insert(args.end(), {"-map", "0:" + std::to_string(plan.video_stream)});
        if (plan.audio_stream >= 0) args.insert(args.end(), {"-map", "0:" + std::to_string(plan.audio_stream)});
        else args.push_back("-an");
        args.push_back("-sn");

        if (plan.video == MediaTransform::copy) {
            args.insert(args.end(), {"-c:v", "copy"});
            if (plan.video_codec == "hevc") args.insert(args.end(), {"-tag:v", "hvc1"});
        } else if (plan.video == MediaTransform::transcode) {
            args.insert(args.end(), {"-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p"});
            if (plan.target_height) args.insert(args.end(), {"-vf", "scale=-2:" + std::to_string(*plan.target_height)});
            if (plan.target_video_bitrate) {
                args.insert(args.end(), {"-b:v", std::to_string(*plan.target_video_bitrate),
                                         "-maxrate", std::to_string(*plan.target_video_bitrate),
                                         "-bufsize", std::to_string(*plan.target_video_bitrate * 2)});
            } else {
                args.insert(args.end(), {"-crf", "20"});
            }
        } else {
            args.push_back("-vn");
        }

        if (plan.audio_stream >= 0) {
            if (plan.audio == MediaTransform::copy)
                args.insert(args.end(), {"-c:a", "copy"});
            else
                args.insert(args.end(), {"-c:a", "aac", "-b:a", "192k", "-ac", "2"});
        }

        const auto segment_seconds = std::max(1.0, static_cast<double>(segment_duration.count()) / 1000.0);
        if (plan.video == MediaTransform::transcode) {
            args.insert(args.end(), {"-force_key_frames", "expr:gte(t,n_forced*" + std::to_string(segment_seconds) + ")"});
        }
        args.insert(args.end(), {"-f", "hls", "-hls_time", std::to_string(segment_seconds),
                                 "-hls_list_size", "0", "-hls_segment_type", "fmp4",
                                 "-hls_fmp4_init_filename", "init.mp4",
                                 "-hls_segment_filename", segment_pattern.string(),
                                 "-hls_flags", "independent_segments+temp_file", playlist.string()});
        Log::debug("media engine starting ffmpeg for " + source.media_id + " mode=" + playback_mode_name(plan.mode));
        return spawn_ffmpeg(std::move(args));
    }

    void extract_webvtt(const MediaSource& source, int subtitle_stream,
                        const std::filesystem::path& output_file,
                        std::chrono::milliseconds seek) override {
        if (!status_.ffmpeg_available) throw std::runtime_error("ffmpeg is unavailable");
        std::vector<std::string> args{config_.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "warning", "-y"};
        if (seek.count() > 0) {
            args.push_back("-ss");
            args.push_back(std::to_string(static_cast<double>(seek.count()) / 1000.0));
        }
        args.insert(args.end(), {"-i", source.url, "-map", "0:" + std::to_string(subtitle_stream),
                                 "-c:s", "webvtt", output_file.string()});
        auto result = run_capture(std::move(args), 4 * 1024 * 1024);
        if (result.exit_code != 0)
            throw std::runtime_error("subtitle conversion failed: " + result.err);
    }
};

} // namespace

std::unique_ptr<MediaEngine> make_ffmpeg_process_engine(const StreamingConfig& config) {
    return std::make_unique<FfmpegProcessEngine>(config);
}

std::string playback_mode_name(PlaybackMode mode) {
    switch (mode) {
    case PlaybackMode::direct: return "direct";
    case PlaybackMode::remux: return "remux";
    case PlaybackMode::transcode: return "transcode";
    }
    return "unknown";
}

std::string media_stream_type_name(MediaStreamType type) {
    switch (type) {
    case MediaStreamType::video: return "video";
    case MediaStreamType::audio: return "audio";
    case MediaStreamType::subtitle: return "subtitle";
    case MediaStreamType::other: return "other";
    }
    return "other";
}

} // namespace macha
