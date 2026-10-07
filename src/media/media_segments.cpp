// SPDX-License-Identifier: GPL-3.0-or-later
#include "media/media_engine.hpp"

#include "contract/thread_safety.hpp"
#include "write_behind.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cerrno>
#include <condition_variable>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <optional>
#include <sstream>
#include <unistd.h>

namespace macha {

namespace {
// A spilled fragment is on the control filesystem: its writeback starts at
// once rather than leaving it dirty (write_behind.hpp).
bool spill_file(const std::filesystem::path& path, const Bytes& bytes) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;
    size_t done = 0;
    while (done < bytes.size()) {
        const auto n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += static_cast<size_t>(n);
    }
    if (done == bytes.size())
        start_writeback(fd);
    return ::close(fd) == 0 && done == bytes.size();
}
} // namespace

struct MediaSegmentStore::Impl {
    struct Segment {
        double duration{};
        uint64_t size{};
        std::shared_ptr<Bytes> memory;
        std::filesystem::path spill;
    };

    // Held across spilling fragments to files (maybe_spill_locked).
    mutable IoMutex mutex;
    mutable std::condition_variable_any cv;
    std::shared_ptr<Bytes> init MACHA_GUARDED_BY(mutex);
    std::vector<Segment> segments MACHA_GUARDED_BY(mutex);
    // This and the settings up to memory_limit, and spill_directory and
    // target_duration, are fixed at construction.
    std::vector<double> vod_segment_durations;
    MediaContainer container{MediaContainer::fmp4};
    const TimeSource* time{&steady_time_source()};
    bool finished MACHA_GUARDED_BY(mutex){};
    bool cancelled MACHA_GUARDED_BY(mutex){};
    bool superseded MACHA_GUARDED_BY(mutex){};
    std::string error MACHA_GUARDED_BY(mutex);
    uint64_t highest_requested MACHA_GUARDED_BY(mutex){};
    size_t max_ahead{8};
    uint64_t memory_limit{64ULL * 1024 * 1024};
    uint64_t memory_bytes MACHA_GUARDED_BY(mutex){};
    // Media produced and encoder time spent, parked intervals excluded (see
    // Snapshot). Both cover the same fragments: the clock starts at construction
    // so the first fragment is timed too. Timing only gaps between publications
    // would overstate the rate by n/(n-1); including start-up reads low early,
    // the conservative direction (a deferred handover, not a stalled viewer).
    std::chrono::duration<double> produced_media MACHA_GUARDED_BY(mutex){};
    std::chrono::duration<double> producing MACHA_GUARDED_BY(mutex){};
    std::optional<std::chrono::steady_clock::time_point> produced_since MACHA_GUARDED_BY(mutex);
    uint64_t spill_bytes MACHA_GUARDED_BY(mutex){};
    std::filesystem::path spill_directory;
    std::chrono::milliseconds target_duration{4000};
    std::optional<RetainedMemoryLedger::Lease> retained_memory MACHA_GUARDED_BY(mutex);
    // Deferred requests awaiting the next publication or ending; each fires once.
    // Fired after `mutex` is released.
    mutable std::vector<std::function<void()>> wakers MACHA_GUARDED_BY(mutex);

    // Collects owed wakers and fires them at scope end: declared before the lock,
    // destroyed after it, so no waker runs under the store mutex.
    struct Wakeups {
        std::vector<std::function<void()>> list;
        Wakeups() = default;
        Wakeups(const Wakeups&) = delete;
        Wakeups& operator=(const Wakeups&) = delete;
        ~Wakeups() {
            for (auto& wake : list) {
                try {
                    wake();
                } catch (...) {
                }
            }
        }
    };

    void notify_locked(Wakeups& wakeups) MACHA_REQUIRES(mutex) {
        cv.notify_all();
        if (!wakers.empty()) {
            for (auto& wake : wakers) wakeups.list.push_back(std::move(wake));
            wakers.clear();
        }
    }

    void maybe_spill_locked() MACHA_REQUIRES(mutex) {
        if (!memory_limit || memory_bytes <= memory_limit || spill_directory.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(spill_directory, ec);
        if (ec) return;
        for (size_t i = 0; i < segments.size() && memory_bytes > memory_limit; ++i) {
            // Keep the most recently consumed fragments resident; older ones stay
            // seekable through the spill file.
            if (i + 2 >= highest_requested || !segments[i].memory) continue;
            auto path = spill_directory / ("segment-" + [&] {
                std::ostringstream n;
                n << std::setfill('0') << std::setw(6) << i;
                return n.str();
            }() + ".m4s");
            if (!spill_file(path, *segments[i].memory)) {
                std::filesystem::remove(path, ec);
                continue;
            }
            memory_bytes -= segments[i].memory->size();
            spill_bytes += segments[i].memory->size();
            segments[i].spill = std::move(path);
            segments[i].memory.reset();
        }
    }

    bool publish_init(Bytes bytes) {
        Wakeups wakeups;
        Lock lock(mutex);
        if (cancelled) return false;
        init = std::make_shared<Bytes>(std::move(bytes));
        memory_bytes += init->size();
        notify_locked(wakeups);
        return true;
    }

    bool publish_segment(Bytes bytes, double duration) {
        Wakeups wakeups;
        // Encode time is the gap since the previous call returned; the demand wait
        // comes after this point, so parked time is excluded.
        const auto entered = time->now();
        Lock lock(mutex);
        if (produced_since) producing += entered - *produced_since;
        const auto index = static_cast<uint64_t>(segments.size());
        if (!vod_segment_durations.empty() && index >= vod_segment_durations.size()) {
            if (error.empty()) error = "media pipeline produced more fragments than the VOD plan";
            finished = true;
            notify_locked(wakeups);
            return false;
        }
        cv.wait(lock.native(), [&]() MACHA_REQUIRES(mutex) {
            return cancelled || index <= highest_requested + static_cast<uint64_t>(max_ahead);
        });
        if (cancelled) return false;
        Segment segment;
        segment.duration = std::max(0.001, duration);
        segment.size = bytes.size();
        segment.memory = std::make_shared<Bytes>(std::move(bytes));
        memory_bytes += segment.memory->size();
        produced_media += std::chrono::duration<double>(segment.duration);
        segments.push_back(std::move(segment));
        maybe_spill_locked();
        notify_locked(wakeups);
        produced_since = time->now();
        return true;
    }

    void mark_finished() {
        Wakeups wakeups;
        Lock lock(mutex);
        // The invariant is planned media published, not fragment count: a boundary
        // can pass without a fragment (the delayed-moov flush) and its length is
        // carried into the next. publish_duration() consumes the plan, so equal
        // totals account for every planned length. An error here makes playlist()
        // withhold the playlist.
        if (!vod_segment_durations.empty()) {
            double planned = 0.0;
            for (const auto duration : vod_segment_durations) planned += duration;
            double published = 0.0;
            for (const auto& segment : segments) published += segment.duration;
            // One fragment of slack: a source may end a little short of its container
            // duration.
            const double tolerance = std::max(1.0, vod_segment_durations.back());
            if (published + tolerance < planned && error.empty())
                error = "media pipeline published " + std::to_string(published) +
                        "s across " + std::to_string(segments.size()) + " fragments for a " +
                        std::to_string(planned) + "s VOD plan";
        }
        finished = true;
        notify_locked(wakeups);
    }

    void mark_failed(std::string message) {
        Wakeups wakeups;
        Lock lock(mutex);
        if (error.empty()) error = std::move(message);
        finished = true;
        notify_locked(wakeups);
    }
};

MediaSegmentStore::MediaSegmentStore(size_t max_ahead_segments, uint64_t memory_limit,
                                     std::filesystem::path spill_directory,
                                     std::chrono::milliseconds target_duration,
                                     std::vector<double> vod_segment_durations,
                                     MediaContainer container, const TimeSource& time)
    : impl_(std::make_unique<Impl>()) {
    impl_->time = &time;
    impl_->produced_since = time.now();
    impl_->container = container;
    impl_->max_ahead = std::max<size_t>(2, max_ahead_segments);
    impl_->memory_limit = memory_limit;
    impl_->spill_directory = std::move(spill_directory);
    impl_->target_duration = target_duration;
    impl_->vod_segment_durations = std::move(vod_segment_durations);
    for (auto& duration : impl_->vod_segment_durations)
        duration = std::max(0.001, duration);
}

MediaSegmentStore::~MediaSegmentStore() = default;

MediaContainer MediaSegmentStore::container() const noexcept {
    return impl_->container;
}

namespace {
// "segment-000123.m4s" / "segment-000123.ts" -> 123
std::optional<uint64_t> segment_number(std::string_view name) {
    constexpr std::string_view prefix = "segment-";
    if (!name.starts_with(prefix)) return std::nullopt;
    std::string_view rest = name.substr(prefix.size());
    if (rest.ends_with(".m4s")) rest.remove_suffix(4);
    else if (rest.ends_with(".ts")) rest.remove_suffix(3);
    else return std::nullopt;
    uint64_t index = 0;
    auto [end, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), index);
    if (ec != std::errc{} || end != rest.data() + rest.size()) return std::nullopt;
    return index;
}
} // namespace

bool MediaSegmentStore::attach_memory_ledger(RetainedMemoryLedger& ledger) {
    Lock lock(impl_->mutex);
    if (impl_->retained_memory)
        return true;
    auto lease = ledger.try_acquire(WorkClass::viewer, MemoryOwner::playback_segment,
                                    impl_->memory_limit);
    if (!lease)
        return false;
    impl_->retained_memory.emplace(std::move(*lease));
    return true;
}

bool MediaSegmentStore::wait_ready(std::chrono::milliseconds timeout) {
    Lock lock(impl_->mutex);
    impl_->cv.wait_for(lock.native(), timeout, [&]() MACHA_REQUIRES(impl_->mutex) {
        return impl_->cancelled || !impl_->error.empty() ||
               ((impl_->init || impl_->container == MediaContainer::mpegts) &&
                !impl_->segments.empty()) ||
               impl_->finished;
    });
    return (impl_->init || impl_->container == MediaContainer::mpegts) && !impl_->segments.empty();
}

std::string MediaSegmentStore::playlist() const {
    Lock lock(impl_->mutex);
    if (!impl_->error.empty()) return {};
    const auto& durations = impl_->vod_segment_durations;
    if (durations.empty()) return {};
    double longest = 1.0;
    for (const double duration : durations) longest = std::max(longest, duration);
    // A complete, closed VOD list served on first fetch with no readiness gate:
    // the duration is known, and waiting for unproduced media is the server's
    // problem. With ENDLIST a player fetches the playlist once, and a production
    // failure surfaces as fragLoadError (own retry policy) rather than
    // levelLoadError, which clients weigh as node health.
    // EXTINF is the plan: a VOD playlist is immutable, so produced and unproduced
    // fragments are described alike. This holds because the plan predicts the
    // output exactly, one fragment per entry (tests/test_transcode_timeline.cpp).
    std::ostringstream out;
    const bool mpegts = impl_->container == MediaContainer::mpegts;
    out << "#EXTM3U\n#EXT-X-VERSION:" << (mpegts ? 3 : 7)
        << "\n#EXT-X-TARGETDURATION:" << static_cast<int>(std::ceil(longest))
        << "\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n"
        << "#EXT-X-INDEPENDENT-SEGMENTS\n";
    if (!mpegts) out << "#EXT-X-MAP:URI=\"init.mp4\"\n";
    for (size_t i = 0; i < durations.size(); ++i) {
        out << "#EXTINF:" << std::fixed << std::setprecision(3) << durations[i] << ",\n"
            << "segment-" << std::setfill('0') << std::setw(6) << i
            << (mpegts ? ".ts" : ".m4s") << "\n";
    }
    out << "#EXT-X-ENDLIST\n";
    return out.str();
}

std::optional<Bytes> MediaSegmentStore::object(std::string_view name) const {
    std::shared_ptr<Bytes> resident;
    std::filesystem::path spill;
    {
        Lock lock(impl_->mutex);
        if (name == "init.mp4") {
            if (!impl_->init) return {};
            return *impl_->init;
        }
        const auto parsed = segment_number(name);
        if (!parsed || *parsed >= impl_->segments.size()) return {};
        const uint64_t index = *parsed;
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

std::optional<Bytes> MediaSegmentStore::wait_object(std::string_view name,
                                                    std::chrono::milliseconds timeout) const {
    const auto parsed = segment_number(name);
    // One hold path for every object kind: init.mp4 waits like a fragment, since
    // the playlist can send a client for it before the muxer writes it. MPEG-TS
    // has no init, so a miss there is genuine.
    const bool init_object =
        !parsed && name == "init.mp4" && impl_->container != MediaContainer::mpegts;
    if (!parsed && !init_object) return object(name);
    const uint64_t index = parsed ? *parsed : 0;

    std::shared_ptr<Bytes> resident;
    std::filesystem::path spill;
    {
        Lock lock(impl_->mutex);
        if (!init_object && !impl_->vod_segment_durations.empty() &&
            index >= impl_->vod_segment_durations.size())
            return {};
        // Kinds differ only in presence; everything that ends a wait is shared.
        const auto present = [&]() MACHA_REQUIRES(impl_->mutex) {
            return init_object ? static_cast<bool>(impl_->init) : index < impl_->segments.size();
        };
        const auto ready = [&]() MACHA_REQUIRES(impl_->mutex) {
            return impl_->cancelled || impl_->superseded || !impl_->error.empty() || present() ||
                   impl_->finished;
        };
        if (timeout.count() > 0) impl_->cv.wait_for(lock.native(), timeout, ready);
        else impl_->cv.wait(lock.native(), ready);
        if (!present()) return {};
        if (init_object) return *impl_->init;
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
    Lock lock(impl_->mutex);
    if (!impl_->vod_segment_durations.empty() && index >= impl_->vod_segment_durations.size()) return;
    impl_->highest_requested = std::max(impl_->highest_requested, index);
    impl_->maybe_spill_locked();
    impl_->cv.notify_all();
}

MediaSegmentStore::Snapshot MediaSegmentStore::snapshot() const {
    Lock lock(impl_->mutex);
    return {static_cast<bool>(impl_->init), impl_->finished, impl_->error,
            static_cast<uint64_t>(impl_->segments.size()), impl_->highest_requested,
            impl_->memory_bytes, impl_->spill_bytes,
            static_cast<uint64_t>(impl_->segments.capacity() * sizeof(Impl::Segment) +
                                  impl_->vod_segment_durations.capacity() * sizeof(double)),
            static_cast<uint64_t>(impl_->vod_segment_durations.size()),
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(impl_->produced_media)
                    .count()),
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(impl_->producing).count()),
            impl_->produced_since
                ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            impl_->time->now() - *impl_->produced_since)
                                            .count())
                : 0,
            impl_->segments.size() > impl_->highest_requested + impl_->max_ahead};
}

void MediaSegmentStore::cancel() {
    Impl::Wakeups wakeups;
    Lock lock(impl_->mutex);
    impl_->cancelled = true;
    impl_->notify_locked(wakeups);
}

void MediaSegmentStore::mark_superseded(bool superseded) {
    Impl::Wakeups wakeups;
    Lock lock(impl_->mutex);
    impl_->superseded = superseded;
    impl_->notify_locked(wakeups);
}

MediaSegmentStore::Awaited MediaSegmentStore::object_or_subscribe(
    std::string_view name, std::function<void()> wake) const {
    const auto parsed = segment_number(name);
    const bool init_object =
        !parsed && name == "init.mp4" && impl_->container != MediaContainer::mpegts;
    // As wait_object: anything but a fragment or holdable init is an immediate
    // lookup.
    if (!parsed && !init_object) return {object(name), true};
    const uint64_t index = parsed ? *parsed : 0;

    std::shared_ptr<Bytes> resident;
    std::filesystem::path spill;
    {
        Lock lock(impl_->mutex);
        if (!init_object && !impl_->vod_segment_durations.empty() &&
            index >= impl_->vod_segment_durations.size())
            return {std::nullopt, true};
        const bool present =
            init_object ? static_cast<bool>(impl_->init) : index < impl_->segments.size();
        if (!present) {
            const bool ended = impl_->cancelled || impl_->superseded ||
                               !impl_->error.empty() || impl_->finished;
            if (!ended && wake) impl_->wakers.push_back(std::move(wake));
            return {std::nullopt, ended};
        }
        if (init_object) return {*impl_->init, false};
        resident = impl_->segments[static_cast<size_t>(index)].memory;
        spill = impl_->segments[static_cast<size_t>(index)].spill;
    }
    if (resident) return {*resident, false};
    if (spill.empty()) return {std::nullopt, true};
    std::ifstream in(spill, std::ios::binary);
    if (!in) return {std::nullopt, true};
    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    if (size < 0) return {std::nullopt, true};
    in.seekg(0, std::ios::beg);
    Bytes bytes(static_cast<size_t>(size));
    if (!bytes.empty())
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in && !bytes.empty()) return {std::nullopt, true};
    return {std::move(bytes), false};
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
