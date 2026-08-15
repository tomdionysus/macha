// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_engine.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace macha {

struct MediaSegmentStore::Impl {
    struct Segment {
        double duration{};
        uint64_t size{};
        std::shared_ptr<Bytes> memory;
        std::filesystem::path spill;
    };

    mutable std::mutex mutex;
    mutable std::condition_variable_any cv;
    std::shared_ptr<Bytes> init;
    std::vector<Segment> segments;
    bool finished{};
    bool cancelled{};
    std::string error;
    uint64_t highest_requested{};
    size_t max_ahead{8};
    uint64_t memory_limit{64ULL * 1024 * 1024};
    uint64_t memory_bytes{};
    std::filesystem::path spill_directory;
    std::chrono::milliseconds target_duration{4000};

    void maybe_spill_locked() {
        if (!memory_limit || memory_bytes <= memory_limit || spill_directory.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(spill_directory, ec);
        if (ec) return;
        for (size_t i = 0; i < segments.size() && memory_bytes > memory_limit; ++i) {
            // Keep the most recently consumed fragments resident. Everything
            // older remains seekable through the spill file.
            if (i + 2 >= highest_requested || !segments[i].memory) continue;
            auto path = spill_directory / ("segment-" + [&] {
                std::ostringstream n;
                n << std::setfill('0') << std::setw(6) << i;
                return n.str();
            }() + ".m4s");
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) continue;
            out.write(reinterpret_cast<const char*>(segments[i].memory->data()),
                      static_cast<std::streamsize>(segments[i].memory->size()));
            out.close();
            if (!out) {
                std::filesystem::remove(path, ec);
                continue;
            }
            memory_bytes -= segments[i].memory->size();
            segments[i].spill = std::move(path);
            segments[i].memory.reset();
        }
    }

    bool publish_init(Bytes bytes) {
        std::lock_guard lock(mutex);
        if (cancelled) return false;
        init = std::make_shared<Bytes>(std::move(bytes));
        memory_bytes += init->size();
        cv.notify_all();
        return true;
    }

    bool publish_segment(Bytes bytes, double duration) {
        std::unique_lock lock(mutex);
        const auto index = static_cast<uint64_t>(segments.size());
        cv.wait(lock, [&] {
            return cancelled || index <= highest_requested + static_cast<uint64_t>(max_ahead);
        });
        if (cancelled) return false;
        Segment segment;
        segment.duration = std::max(0.001, duration);
        segment.size = bytes.size();
        segment.memory = std::make_shared<Bytes>(std::move(bytes));
        memory_bytes += segment.memory->size();
        segments.push_back(std::move(segment));
        maybe_spill_locked();
        cv.notify_all();
        return true;
    }

    void mark_finished() {
        std::lock_guard lock(mutex);
        finished = true;
        cv.notify_all();
    }

    void mark_failed(std::string message) {
        std::lock_guard lock(mutex);
        if (error.empty()) error = std::move(message);
        finished = true;
        cv.notify_all();
    }
};

MediaSegmentStore::MediaSegmentStore(size_t max_ahead_segments, uint64_t memory_limit,
                                     std::filesystem::path spill_directory,
                                     std::chrono::milliseconds target_duration)
    : impl_(std::make_unique<Impl>()) {
    impl_->max_ahead = std::max<size_t>(2, max_ahead_segments);
    impl_->memory_limit = memory_limit;
    impl_->spill_directory = std::move(spill_directory);
    impl_->target_duration = target_duration;
}

MediaSegmentStore::~MediaSegmentStore() = default;

bool MediaSegmentStore::wait_ready(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    impl_->cv.wait_for(lock, timeout, [&] {
        return impl_->cancelled || !impl_->error.empty() ||
               (impl_->init && !impl_->segments.empty()) || impl_->finished;
    });
    return impl_->init && !impl_->segments.empty();
}

std::string MediaSegmentStore::playlist() const {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->error.empty() && impl_->segments.empty()) return {};
    double longest = std::max(1.0, static_cast<double>(impl_->target_duration.count()) / 1000.0);
    for (const auto& segment : impl_->segments) longest = std::max(longest, segment.duration);
    std::ostringstream out;
    out << "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:" << static_cast<int>(std::ceil(longest))
        << "\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:EVENT\n"
        << "#EXT-X-INDEPENDENT-SEGMENTS\n"
        << "#EXT-X-MAP:URI=\"init.mp4\"\n";
    for (size_t i = 0; i < impl_->segments.size(); ++i) {
        out << "#EXTINF:" << std::fixed << std::setprecision(3) << impl_->segments[i].duration << ",\n"
            << "segment-" << std::setfill('0') << std::setw(6) << i << ".m4s\n";
    }
    if (impl_->finished && impl_->error.empty()) out << "#EXT-X-ENDLIST\n";
    return out.str();
}

std::optional<Bytes> MediaSegmentStore::object(std::string_view name) const {
    std::shared_ptr<Bytes> resident;
    std::filesystem::path spill;
    {
        std::lock_guard lock(impl_->mutex);
        if (name == "init.mp4") {
            if (!impl_->init) return {};
            return *impl_->init;
        }
        constexpr std::string_view prefix = "segment-";
        constexpr std::string_view suffix = ".m4s";
        if (!name.starts_with(prefix) || !name.ends_with(suffix)) return {};
        auto number = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        uint64_t index = 0;
        auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), index);
        if (ec != std::errc{} || end != number.data() + number.size() || index >= impl_->segments.size()) return {};
        resident = impl_->segments[static_cast<size_t>(index)].memory;
        spill = impl_->segments[static_cast<size_t>(index)].spill;
    }
    if (resident) return *resident;
    if (spill.empty()) return {};
    std::ifstream in(spill, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    if (size < 0) return {};
    in.seekg(0, std::ios::beg);
    Bytes bytes(static_cast<size_t>(size));
    if (!bytes.empty()) in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in && !bytes.empty()) return {};
    return bytes;
}

void MediaSegmentStore::note_requested(uint64_t index) {
    std::lock_guard lock(impl_->mutex);
    impl_->highest_requested = std::max(impl_->highest_requested, index);
    impl_->maybe_spill_locked();
    impl_->cv.notify_all();
}

MediaSegmentStore::Snapshot MediaSegmentStore::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    return {static_cast<bool>(impl_->init), impl_->finished, impl_->error,
            static_cast<uint64_t>(impl_->segments.size()), impl_->highest_requested};
}

void MediaSegmentStore::cancel() {
    std::lock_guard lock(impl_->mutex);
    impl_->cancelled = true;
    impl_->cv.notify_all();
}

bool MediaSegmentStore::publish_init(Bytes bytes) {
    return impl_->publish_init(std::move(bytes));
}

bool MediaSegmentStore::publish_segment(Bytes bytes, double duration_seconds) {
    return impl_->publish_segment(std::move(bytes), duration_seconds);
}

void MediaSegmentStore::finish() {
    impl_->mark_finished();
}

void MediaSegmentStore::fail(std::string message) {
    impl_->mark_failed(std::move(message));
}


} // namespace macha
