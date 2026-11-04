// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalogue/book_conversion.hpp"

#include "log.hpp"
#include "media/media_containers.hpp"
#include "supervised.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace macha {
namespace {

// "macha:<hex>" names a file in the cache by its hex part alone.
std::string cache_stem(std::string_view media_id) {
    std::string out;
    for (const char c : media_id.substr(media_id.find(':') + 1))
        if (std::isxdigit(static_cast<unsigned char>(c))) out.push_back(c);
    return out;
}

bool supported_format(std::string_view format) {
    return std::find(BookConversions::supported.begin(), BookConversions::supported.end(), format) !=
           BookConversions::supported.end();
}

bool supported_source(const std::string& name) { return mobi_extension(path_extension(name)); }

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Whether the Palm database's first record says the text is encrypted (DRM).
// An unreadable header is left to the converter to refuse.
bool encrypted_mobi(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    uint8_t header[86]{};
    if (!in.read(reinterpret_cast<char*>(header), sizeof header)) return false;
    const auto record0 = be32(header + 78);
    uint8_t palmdoc[14]{};
    in.seekg(record0);
    if (!in.read(reinterpret_cast<char*>(palmdoc), sizeof palmdoc)) return false;
    return (uint16_t(palmdoc[12]) << 8 | palmdoc[13]) != 0;
}

// Runs `argv` in its own process group at low priority, output to `log`.
// The pid, or -1.
pid_t spawn(const std::vector<std::string>& argv, const std::filesystem::path& log) {
    std::vector<char*> args;
    for (const auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);
    const auto pid = fork();
    if (pid != 0) return pid;
    // Child: only async-signal-safe calls from here.
    setpgid(0, 0);
    setpriority(PRIO_PROCESS, 0, 10);
    const int out = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out >= 0) {
        dup2(out, STDOUT_FILENO);
        dup2(out, STDERR_FILENO);
        close(out);
    }
    const int null = open("/dev/null", O_RDONLY);
    if (null >= 0) {
        dup2(null, STDIN_FILENO);
        close(null);
    }
    execve(args[0], args.data(), environ);
    _exit(127);
}

// The last bytes of the child's log, for the journal.
std::string log_tail(const std::filesystem::path& log) {
    std::ifstream in(log, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() > 400) text = text.substr(text.size() - 400);
    for (auto& c : text)
        if (c == '\n' || c == '\r') c = ' ';
    return text;
}

} // namespace

BookConversions::BookConversions(BookConversionConfig config,
                                 const std::filesystem::path& state_path)
    : config_(std::move(config)),
      cache_path_(config_.cache_path.empty() ? state_path / "book-conversions" : config_.cache_path) {}

BookConversions::~BookConversions() { stop(); }

void BookConversions::start() {
    std::error_code ec;
    std::filesystem::create_directories(cache_path_ / "work", ec);
    // A working file left by a stop or a crash is never a finished copy.
    for (const auto& entry : std::filesystem::directory_iterator(cache_path_ / "work", ec))
        std::filesystem::remove_all(entry.path(), ec);
    {
        Lock lock(mutex_);
        stopping_ = false;
    }
    for (size_t i = 0; i < config_.max_concurrent; ++i)
        workers_.emplace_back([this](std::stop_token stop) {
            run_supervised_loop("macha-book-convert", stop, [&] { work(stop); });
        });
}

void BookConversions::stop() {
    {
        Lock lock(mutex_);
        stopping_ = true;
        for (const auto& [_, pid] : running_) kill(-pid, SIGKILL);
    }
    wake_.notify_all();
    for (auto& worker : workers_) worker.request_stop();
    wake_.notify_all();
    workers_.clear();
}

std::filesystem::path BookConversions::output_path(const Key& key) const {
    return cache_path_ / (cache_stem(key.media_id) + "." + key.format);
}

std::vector<std::string> BookConversions::formats() const {
    return {supported.begin(), supported.end()};
}

ConversionState BookConversions::request(const std::string& media_id, const std::string& format,
                                         const MediaContentSource& source) {
    const Key key{media_id, format};
    ConversionState state{ConversionState::Phase::converting, media_id, format, 0, {}};
    if (!supported_format(format)) {
        state.phase = ConversionState::Phase::failed;
        state.error_code = "conversion_failed";
        return state;
    }
    if (!supported_source(source.name)) {
        state.phase = ConversionState::Phase::failed;
        state.error_code = "unsupported_source";
        return state;
    }
    if (const auto ready = this->state(media_id, format);
        ready && ready->phase != ConversionState::Phase::failed)
        return *ready;
    if (source.size > config_.max_source_bytes) {
        state.phase = ConversionState::Phase::failed;
        state.error_code = "source_too_large";
        Lock lock(mutex_);
        states_[key] = state;
        return state;
    }
    {
        Lock lock(mutex_);
        states_[key] = state;
        queue_.push_back({key, source});
    }
    wake_.notify_one();
    return state;
}

std::optional<ConversionState> BookConversions::state(const std::string& media_id,
                                                      const std::string& format) {
    const Key key{media_id, format};
    {
        Lock lock(mutex_);
        if (auto found = states_.find(key); found != states_.end()) return found->second;
    }
    // A copy made before a restart is still ready.
    std::error_code ec;
    const auto path = output_path(key);
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || !size) return std::nullopt;
    ConversionState ready{ConversionState::Phase::ready, media_id, format, size, {}};
    Lock lock(mutex_);
    states_.emplace(key, ready);
    return ready;
}

std::optional<MediaContentSource> BookConversions::content(const std::string& media_id,
                                                           const std::string& format) {
    const auto current = state(media_id, format);
    if (!current || current->phase != ConversionState::Phase::ready) return std::nullopt;
    const auto path = output_path({media_id, format});
    auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
    if (!*file) return std::nullopt;
    // Served now: the eviction keeps the most recently served.
    std::error_code ec;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
    auto mutex = std::make_shared<std::mutex>();
    return MediaContentSource{
        current->size, path.filename().string(),
        [file, mutex](uint64_t offset, std::span<uint8_t> into) -> size_t {
            std::lock_guard guard(*mutex);
            file->clear();
            file->seekg(static_cast<std::streamoff>(offset));
            file->read(reinterpret_cast<char*>(into.data()), static_cast<std::streamsize>(into.size()));
            return static_cast<size_t>(file->gcount());
        }};
}

bool BookConversions::erase(const std::string& media_id, const std::string& format) {
    const Key key{media_id, format};
    std::error_code ec;
    {
        Lock lock(mutex_);
        const auto found = states_.find(key);
        if (found != states_.end() && found->second.phase == ConversionState::Phase::converting)
            return false;
        const bool known = found != states_.end();
        if (known) states_.erase(found);
        const bool removed = std::filesystem::remove(output_path(key), ec);
        return known || removed;
    }
}

void BookConversions::work(std::stop_token stop) {
    for (;;) {
        Job job;
        {
            Lock lock(mutex_);
            while (queue_.empty() && !stopping_ && !stop.stop_requested())
                wake_.wait(lock.native());
            if (stopping_ || stop.stop_requested()) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        auto result = convert(job);
        // Evicted first: a conversion reported ready finds the cache within its limit.
        if (result.phase == ConversionState::Phase::ready) evict();
        {
            Lock lock(mutex_);
            states_[job.key] = result;
        }
    }
}

ConversionState BookConversions::convert(const Job& job) {
    ConversionState result{ConversionState::Phase::failed, job.key.media_id, job.key.format, 0, {}};
    const auto work = cache_path_ / "work" / (cache_stem(job.key.media_id) + "-" + job.key.format);
    // mobitool names its output after the input: book.<ext> becomes out/book.epub.
    const auto input = work / ("book" + path_extension(job.source.name));
    const auto output = work / "out" / ("book." + job.key.format);
    const auto log = work / "log";
    std::error_code ec;
    const auto cleanup = [&] { std::filesystem::remove_all(work, ec); };
    std::filesystem::remove_all(work, ec);
    std::filesystem::create_directories(work / "out", ec);

    // The source, copied whole first: the child reads a plain file.
    {
        std::ofstream out(input, std::ios::binary | std::ios::trunc);
        std::vector<uint8_t> buffer(1024 * 1024);
        uint64_t done = 0;
        try {
            while (done < job.source.size) {
                const auto want = static_cast<size_t>(
                    std::min<uint64_t>(buffer.size(), job.source.size - done));
                const auto read = job.source.read(done, std::span<uint8_t>(buffer.data(), want));
                if (!read) break;
                out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(read));
                done += read;
            }
        } catch (const std::exception& e) {
            Log::warn("book conversion source unreadable media=" + job.key.media_id + ": " + e.what());
        }
        if (!out || done != job.source.size) {
            cleanup();
            result.error_code = "source_unreadable";
            return result;
        }
    }
    if (encrypted_mobi(input)) {
        cleanup();
        result.error_code = "encrypted";
        Log::info("book conversion refused media=" + job.key.media_id + ": the book is encrypted");
        return result;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto pid = spawn({config_.mobitool.string(), "-e", "-o", (work / "out").string(), input.string()}, log);
    if (pid < 0) {
        cleanup();
        result.error_code = "conversion_failed";
        Log::warn("book conversion could not start media=" + job.key.media_id);
        return result;
    }
    {
        Lock lock(mutex_);
        running_[job.key] = pid;
    }
    int status = 0;
    bool timed_out = false;
    for (;;) {
        const auto waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) break;
        if (waited < 0 && errno != EINTR) break;
        bool stopping = false;
        {
            Lock lock(mutex_);
            stopping = stopping_;
        }
        if (stopping || std::chrono::steady_clock::now() - started > config_.timeout) {
            timed_out = !stopping;
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        usleep(100 * 1000);
    }
    {
        Lock lock(mutex_);
        running_.erase(job.key);
    }
    const auto size = std::filesystem::file_size(output, ec);
    const bool ok = !timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0 && !ec && size;
    if (!ok) {
        result.error_code = timed_out ? "conversion_timed_out" : "conversion_failed";
        Log::warn("book conversion failed media=" + job.key.media_id + " format=" + job.key.format +
                  " code=" + result.error_code + " exit=" +
                  (WIFEXITED(status) ? std::to_string(WEXITSTATUS(status)) : std::string("signal")) +
                  " output=\"" + log_tail(log) + "\"");
        cleanup();
        return result;
    }
    std::filesystem::rename(output, output_path(job.key), ec);
    cleanup();
    if (ec) {
        result.error_code = "conversion_failed";
        return result;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    Log::info("book converted media=" + job.key.media_id + " format=" + job.key.format +
              " bytes=" + std::to_string(size) + " elapsed_ms=" + std::to_string(elapsed));
    result.phase = ConversionState::Phase::ready;
    result.size = size;
    return result;
}

void BookConversions::evict() {
    std::error_code ec;
    struct Entry {
        std::filesystem::path path;
        uint64_t size;
        std::filesystem::file_time_type served;
    };
    std::vector<Entry> entries;
    uint64_t total = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path_, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const auto size = entry.file_size(ec);
        entries.push_back({entry.path(), size, entry.last_write_time(ec)});
        total += size;
    }
    if (total <= config_.cache_limit) return;
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.served < b.served; });
    for (const auto& entry : entries) {
        if (total <= config_.cache_limit) break;
        const auto name = entry.path.filename().string();
        const auto dot = name.find('.');
        if (dot != std::string::npos) {
            Lock lock(mutex_);
            for (auto it = states_.begin(); it != states_.end();) {
                if (cache_stem(it->first.media_id) == name.substr(0, dot) &&
                    it->first.format == name.substr(dot + 1) &&
                    it->second.phase == ConversionState::Phase::ready)
                    it = states_.erase(it);
                else
                    ++it;
            }
        }
        std::filesystem::remove(entry.path, ec);
        total -= entry.size;
    }
}

} // namespace macha
