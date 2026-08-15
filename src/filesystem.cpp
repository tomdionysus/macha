// SPDX-License-Identifier: GPL-3.0-or-later
#include "filesystem.hpp"
#include "crypto.hpp"
#include "codec.hpp"
#include "hydration.hpp"
#include "log.hpp"
#include "placement.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
namespace macha {
namespace {
std::atomic_uint64_t next_write_handle_diagnostic_id{1};

[[noreturn]] void fail(int c, const std::string& s) {
    throw FsError(c, s);
}
bool child(const std::string& c, const std::string& p) {
    if (p == "/")
        return c.size() > 1 && c.find('/', 1) == std::string::npos;
    return c.size() > p.size() + 1 && c.compare(0, p.size(), p) == 0 && c[p.size()] == '/' &&
           c.find('/', p.size() + 1) == std::string::npos;
}
bool under(const std::string& p, const std::string& r) {
    return p == r || (p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == '/');
}
void pwa(int f, std::span<const uint8_t> b, uint64_t o) {
    size_t p = 0;
    while (p < b.size()) {
        auto n = pwrite(f, b.data() + p, b.size() - p, o + p);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fail(EIO, strerror(errno));
        }
        p += n;
    }
}
size_t pra(int f, std::span<uint8_t> b, uint64_t o) {
    size_t p = 0;
    while (p < b.size()) {
        auto n = pread(f, b.data() + p, b.size() - p, o + p);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fail(EIO, strerror(errno));
        }
        if (!n)
            break;
        p += n;
    }
    return p;
}

bool all_zero(std::span<const uint8_t> data) {
    return std::all_of(data.begin(), data.end(), [](uint8_t value) { return value == 0; });
}

std::string edge_hex(std::span<const uint8_t> data, bool first) {
    constexpr size_t edge = 16;
    if (data.empty())
        return "";
    if (data.size() <= edge)
        return hex(data);
    return first ? hex(data.first(edge)) : hex(data.last(edge));
}

uint64_t fd_size(int fd) {
    struct stat st {};
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 0)
        return 0;
    return static_cast<uint64_t>(st.st_size);
}

void queue_garbage(MetadataSnapshot& snapshot, const ObjectId& id) {
    auto existing = std::find_if(snapshot.garbage.begin(), snapshot.garbage.end(),
                                 [&](const GarbageRef& garbage) { return garbage.id == id; });
    if (existing == snapshot.garbage.end())
        snapshot.garbage.push_back({id});
}

void queue_garbage(MetadataSnapshot& snapshot, const FsEntry& entry) {
    if (entry.type != EntryType::file)
        return;
    for (const auto& extent : entry.extents) {
        if (!extent.hole)
            queue_garbage(snapshot, extent.id);
    }
}
} // namespace
ReadHandle::ReadHandle(DistributedStore& s, FsEntry e, PlaybackTracker* playback,
                       std::string path)
    : s_(s), e_(std::move(e)), playback_(playback) {
    if (playback_)
        playback_session_ = playback_->open(std::move(path), e_);
    Log::debug("DIAG read-handle open ptr=" +
               std::to_string(reinterpret_cast<uintptr_t>(this)) +
               " size=" + std::to_string(e_.size) +
               " extents=" + std::to_string(e_.extents.size()));
    for (size_t i = 0; i < e_.extents.size(); ++i) {
        const auto& x = e_.extents[i];
        Log::debug("DIAG read-manifest ptr=" +
                   std::to_string(reinterpret_cast<uintptr_t>(this)) +
                   " index=" + std::to_string(i) +
                   " offset=" + std::to_string(x.offset) +
                   " length=" + std::to_string(x.length) +
                   " hole=" + std::to_string(x.hole ? 1 : 0) +
                   (x.hole ? std::string{} : " id=" + to_string(x.id)));
    }
}

ReadHandle::~ReadHandle() {
    if (playback_ && playback_session_)
        playback_->close(playback_session_);
}

const Bytes& ReadHandle::extent(size_t i) {
    if (cached_index_ == i) {
        Log::debug("DIAG read-extent cache-hit ptr=" +
                   std::to_string(reinterpret_cast<uintptr_t>(this)) +
                   " index=" + std::to_string(i));
        return cached_extent_;
    }

    auto& x = e_.extents.at(i);
    auto started = Clock::now();
    auto data = s_.get(x.id, i);
    if (!data || data->size() != x.length)
        fail(EIO, "extent unavailable/corrupt");

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    Log::debug("DIAG read-extent load ptr=" +
               std::to_string(reinterpret_cast<uintptr_t>(this)) +
               " index=" + std::to_string(i) +
               " offset=" + std::to_string(x.offset) +
               " length=" + std::to_string(x.length) +
               " id=" + to_string(x.id) +
               " source=direct ms=" + std::to_string(elapsed.count()));

    cached_extent_ = std::move(*data);
    cached_index_ = i;
    return cached_extent_;
}

size_t ReadHandle::read(uint64_t off, std::span<uint8_t> out) {
    std::lock_guard g(m_);
    if (off >= e_.size || out.empty())
        return 0;
    size_t want = std::min<uint64_t>(out.size(), e_.size - off), done = 0;
    uint64_t end = off + want;
    auto it =
        std::lower_bound(e_.extents.begin(), e_.extents.end(), off,
                         [](const ExtentRef& x, uint64_t v) { return x.offset + x.length <= v; });
    size_t last_extent = static_cast<size_t>(-1);
    while (it != e_.extents.end() && it->offset < end) {
        size_t idx = std::distance(e_.extents.begin(), it);
        uint64_t a = std::max(off, it->offset), b = std::min(end, it->offset + it->length);
        size_t n = b - a;
        if (n) {
            if (it->hole)
                std::fill_n(out.begin() + done, n, 0);
            else {
                const auto& d = extent(idx);
                std::copy_n(d.begin() + (a - it->offset), n, out.begin() + done);
            }
            done += n;
            last_extent = idx;
        }
        ++it;
    }
    bool seq = off == last_;
    last_ = off + done;
    if (done)
        s_.foreground_activity(done);
    if (seq && done && playback_ && playback_session_ && last_extent != static_cast<size_t>(-1))
        playback_->progress(playback_session_, last_extent);
    return done;
}

WriteHandle::WriteHandle(FileSystem& f, std::string p, FsEntry b, bool trunc)
    : fs_(f), path_(std::move(p)), base_(std::move(b)), expected_(base_.version),
      sequential_(trunc), logical_(trunc ? 0 : base_.size), staged_(trunc ? 0 : base_.size),
      diagnostic_id_(next_write_handle_diagnostic_id.fetch_add(1, std::memory_order_relaxed)) {
    Log::debug("WRITE handle-open id=" + std::to_string(diagnostic_id_) +
               " path=" + path_ +
               " base_size=" + std::to_string(base_.size) +
               " base_version=" + std::to_string(base_.version) +
               " sequential=" + std::to_string(sequential_ ? 1 : 0));
    if (trunc)
        buffer_.reserve(fs_.extent_size());
}
WriteHandle::~WriteHandle() {
    cleanup();
}

void WriteHandle::diagnostic_stage_extent(const char* label, size_t index, uint64_t offset,
                                          size_t length) {
    if (!Log::enabled(LogLevel::debug) || temp_ < 0 || !length)
        return;
    try {
        Bytes bytes(length);
        const auto got = pra(temp_, bytes, offset);
        if (got != length) {
            Log::debug("WRITE stage-check id=" + std::to_string(diagnostic_id_) +
                       " label=" + label + " index=" + std::to_string(index) +
                       " offset=" + std::to_string(offset) +
                       " length=" + std::to_string(length) +
                       " read=" + std::to_string(got) + " result=SHORT");
            return;
        }
        const auto hash = sha256(bytes);
        Log::debug("WRITE stage-check id=" + std::to_string(diagnostic_id_) +
                   " label=" + label + " index=" + std::to_string(index) +
                   " offset=" + std::to_string(offset) +
                   " length=" + std::to_string(length) +
                   " sha256=" + to_string(hash) +
                   " all_zero=" + std::to_string(all_zero(bytes) ? 1 : 0) +
                   " first16=" + edge_hex(bytes, true) +
                   " last16=" + edge_hex(bytes, false));
    } catch (const std::exception& error) {
        Log::debug("WRITE stage-check id=" + std::to_string(diagnostic_id_) +
                   " label=" + label + " index=" + std::to_string(index) +
                   " result=DIAGNOSTIC_ERROR message=" + error.what());
    }
}

void WriteHandle::diagnostic_stage_checkpoint(const char* label) {
    if (!Log::enabled(LogLevel::debug) || temp_ < 0)
        return;
    const auto physical = fd_size(temp_);
    Log::debug("WRITE stage-checkpoint id=" + std::to_string(diagnostic_id_) +
               " label=" + label +
               " logical=" + std::to_string(logical_) +
               " staged=" + std::to_string(staged_) +
               " physical=" + std::to_string(physical));
    const auto extent_size = fs_.extent_size();
    for (uint64_t offset = 0, index = 0; offset < logical_; offset += extent_size, ++index) {
        const auto length = static_cast<size_t>(std::min<uint64_t>(extent_size, logical_ - offset));
        diagnostic_stage_extent(label, static_cast<size_t>(index), offset, length);
    }
}
void WriteHandle::flush() {
    if (buffer_.empty())
        return;
    const auto offset = staged_;
    const auto length = buffer_.size();
    const bool diagnostics = Log::enabled(LogLevel::debug);
    const bool zero = diagnostics && all_zero(buffer_);
    const auto first = diagnostics ? edge_hex(buffer_, true) : std::string{};
    const auto last = diagnostics ? edge_hex(buffer_, false) : std::string{};
    auto started = Clock::now();
    auto id = fs_.store().put(buffer_);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    if (diagnostics) {
        Log::debug("WRITE extent-put id=" + std::to_string(diagnostic_id_) +
                   " offset=" + std::to_string(offset) +
                   " length=" + std::to_string(length) +
                   " sha256=" + to_string(id) +
                   " all_zero=" + std::to_string(zero ? 1 : 0) +
                   " first16=" + first + " last16=" + last +
                   " ms=" + std::to_string(elapsed.count()));
    }
    extents_.push_back({staged_, buffer_.size(), id, false});
    staged_ += buffer_.size();
    buffer_.clear();
}
void WriteHandle::materialize() {
    if (temp_ >= 0)
        return;
    auto d = fs_.node().config().state_path / "tmp";
    std::filesystem::create_directories(d);
    auto pattern = (d / ("write." + to_string(fs_.node().node_id()) + ".XXXXXX")).string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    temp_ = mkstemp(name.data());
    if (temp_ < 0)
        fail(EIO, "cannot create staging file");
    temp_path_ = name.data();
    Log::debug("WRITE materialize-begin id=" + std::to_string(diagnostic_id_) +
               " path=" + path_ +
               " logical=" + std::to_string(logical_) +
               " staged=" + std::to_string(staged_) +
               " buffer=" + std::to_string(buffer_.size()) +
               " extents=" + std::to_string(extents_.size()) +
               " sequential=" + std::to_string(sequential_ ? 1 : 0) +
               " temp=" + temp_path_.string());
    if (sequential_) {
        for (size_t i = 0; i < extents_.size(); ++i) {
            auto x = fs_.store().get(extents_[i].id, i);
            if (!x)
                fail(EIO, "cannot rematerialize staged extent");
            pwa(temp_, *x, extents_[i].offset);
        }
        if (!buffer_.empty())
            pwa(temp_, buffer_, staged_);
    } else {
        ReadHandle r(fs_.store(), base_, 0);
        Bytes b(std::min<size_t>(fs_.extent_size(), 4 * 1024 * 1024));
        uint64_t o = 0;
        while (o < base_.size) {
            size_t n = std::min<uint64_t>(b.size(), base_.size - o);
            if (r.read(o, {b.data(), n}) != n)
                fail(EIO, "short source read");
            pwa(temp_, {b.data(), n}, o);
            o += n;
        }
    }
    if (ftruncate(temp_, logical_))
        fail(EIO, "staging truncate failed");
    diagnostic_stage_checkpoint("post-materialize");
    diagnostic_completed_extents_ = static_cast<size_t>(logical_ / fs_.extent_size());
    Log::debug("WRITE materialize-end id=" + std::to_string(diagnostic_id_) +
               " physical=" + std::to_string(fd_size(temp_)) +
               " completed_extents=" + std::to_string(diagnostic_completed_extents_));
}
size_t WriteHandle::write(uint64_t off, std::span<const uint8_t> d) {
    std::lock_guard g(m_);
    if (d.empty())
        return 0;

    const bool diagnostics = Log::enabled(LogLevel::debug);
    const uint64_t sequence = ++diagnostic_write_sequence_;
    Hash256 input_hash{};
    if (diagnostics) {
        input_hash = sha256(d);
        auto key = std::make_pair(off, d.size());
        auto previous = diagnostic_exact_writes_.find(key);
        std::string repeat = " exact_repeat=0";
        if (previous != diagnostic_exact_writes_.end()) {
            repeat = " exact_repeat=1 previous_seq=" + std::to_string(previous->second.first) +
                     " previous_sha256=" + to_string(previous->second.second) +
                     " same_hash=" +
                     std::to_string(previous->second.second == input_hash ? 1 : 0);
        }
        size_t overlap_count = 0;
        for (const auto& previous_write : diagnostic_writes_) {
            const auto previous_end = previous_write.offset + previous_write.length;
            const auto current_end = off + d.size();
            if (off >= previous_end || previous_write.offset >= current_end)
                continue;
            ++overlap_count;
        }
        Log::debug("WRITE ingress id=" + std::to_string(diagnostic_id_) +
                   " seq=" + std::to_string(sequence) +
                   " offset=" + std::to_string(off) +
                   " length=" + std::to_string(d.size()) +
                   " sha256=" + to_string(input_hash) +
                   " all_zero=" + std::to_string(all_zero(d) ? 1 : 0) +
                   " first16=" + edge_hex(d, true) +
                   " last16=" + edge_hex(d, false) +
                   " logical_before=" + std::to_string(logical_) +
                   " staged_before=" + std::to_string(staged_) +
                   " buffer_before=" + std::to_string(buffer_.size()) +
                   " sequential=" + std::to_string(sequential_ ? 1 : 0) +
                   " overlap_count=" + std::to_string(overlap_count) + repeat);
        size_t overlap_logged = 0;
        for (auto i = diagnostic_writes_.rbegin();
             i != diagnostic_writes_.rend() && overlap_logged < 8; ++i) {
            const auto previous_end = i->offset + i->length;
            const auto current_end = off + d.size();
            if (off >= previous_end || i->offset >= current_end)
                continue;
            const auto overlap_begin = std::max(off, i->offset);
            const auto overlap_end = std::min(current_end, previous_end);
            const bool exact = off == i->offset && d.size() == i->length;
            Log::debug("WRITE overlap id=" + std::to_string(diagnostic_id_) +
                       " seq=" + std::to_string(sequence) +
                       " previous_seq=" + std::to_string(i->sequence) +
                       " current_offset=" + std::to_string(off) +
                       " current_length=" + std::to_string(d.size()) +
                       " previous_offset=" + std::to_string(i->offset) +
                       " previous_length=" + std::to_string(i->length) +
                       " overlap_offset=" + std::to_string(overlap_begin) +
                       " overlap_length=" + std::to_string(overlap_end - overlap_begin) +
                       " previous_sha256=" + to_string(i->hash) +
                       " exact=" + std::to_string(exact ? 1 : 0) +
                       (exact ? " same_hash=" +
                                    std::to_string(i->hash == input_hash ? 1 : 0)
                              : std::string{}));
            ++overlap_logged;
        }
        diagnostic_exact_writes_[key] = {sequence, input_hash};
        diagnostic_writes_.push_back({sequence, off, d.size(), input_hash});
    }

    if (sequential_ && off == logical_) {
        size_t p = 0;
        while (p < d.size()) {
            size_t n = std::min(fs_.extent_size() - buffer_.size(), d.size() - p);
            buffer_.insert(buffer_.end(), d.begin() + p, d.begin() + p + n);
            p += n;
            logical_ += n;
            if (buffer_.size() == fs_.extent_size())
                flush();
        }
        if (diagnostics) {
            Log::debug("WRITE accepted id=" + std::to_string(diagnostic_id_) +
                       " seq=" + std::to_string(sequence) +
                       " mode=sequential logical_after=" + std::to_string(logical_) +
                       " staged_after=" + std::to_string(staged_) +
                       " buffer_after=" + std::to_string(buffer_.size()));
        }
    } else {
        if (temp_ < 0) {
            Log::debug("WRITE nonsequential id=" + std::to_string(diagnostic_id_) +
                       " seq=" + std::to_string(sequence) +
                       " requested_offset=" + std::to_string(off) +
                       " logical=" + std::to_string(logical_) +
                       " bytes=" + std::to_string(d.size()) + " materializing=1");
            auto started = Clock::now();
            materialize();
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
            Log::debug("WRITE materialized id=" + std::to_string(diagnostic_id_) +
                       " seq=" + std::to_string(sequence) +
                       " ms=" + std::to_string(elapsed.count()));
            sequential_ = false;
        }

        pwa(temp_, d, off);
        logical_ = std::max<uint64_t>(logical_, off + d.size());

        if (diagnostics) {
            try {
                Bytes readback(d.size());
                const auto got = pra(temp_, readback, off);
                if (got == d.size()) {
                    const auto readback_hash = sha256(readback);
                    Log::debug("WRITE stage-write id=" + std::to_string(diagnostic_id_) +
                               " seq=" + std::to_string(sequence) +
                               " offset=" + std::to_string(off) +
                               " length=" + std::to_string(d.size()) +
                               " input_sha256=" + to_string(input_hash) +
                               " readback_sha256=" + to_string(readback_hash) +
                               " all_zero=" + std::to_string(all_zero(readback) ? 1 : 0) +
                               " verify=" + (readback_hash == input_hash ? "OK" : "MISMATCH"));
                } else {
                    Log::debug("WRITE stage-write id=" + std::to_string(diagnostic_id_) +
                               " seq=" + std::to_string(sequence) +
                               " offset=" + std::to_string(off) +
                               " length=" + std::to_string(d.size()) +
                               " read=" + std::to_string(got) + " verify=SHORT");
                }
            } catch (const std::exception& error) {
                Log::debug("WRITE stage-write id=" + std::to_string(diagnostic_id_) +
                           " seq=" + std::to_string(sequence) +
                           " verify=DIAGNOSTIC_ERROR message=" + error.what());
            }

            const auto full_extents = static_cast<size_t>(logical_ / fs_.extent_size());
            while (diagnostic_completed_extents_ < full_extents) {
                const auto index = diagnostic_completed_extents_++;
                const auto offset = static_cast<uint64_t>(index) * fs_.extent_size();
                diagnostic_stage_extent("extent-complete", index, offset, fs_.extent_size());
            }
            Log::debug("WRITE accepted id=" + std::to_string(diagnostic_id_) +
                       " seq=" + std::to_string(sequence) +
                       " mode=staged logical_after=" + std::to_string(logical_) +
                       " physical_after=" + std::to_string(fd_size(temp_)));
        }
    }
    dirty_ = true;
    fs_.store().foreground_activity(d.size());
    return d.size();
}
void WriteHandle::truncate(uint64_t z) {
    std::lock_guard g(m_);
    const bool diagnostics = Log::enabled(LogLevel::debug);
    if (diagnostics) {
        Log::debug("WRITE truncate-begin id=" + std::to_string(diagnostic_id_) +
                   " requested=" + std::to_string(z) +
                   " logical_before=" + std::to_string(logical_) +
                   " staged_before=" + std::to_string(staged_) +
                   " buffer_before=" + std::to_string(buffer_.size()) +
                   " sequential=" + std::to_string(sequential_ ? 1 : 0) +
                   " temp_open=" + std::to_string(temp_ >= 0 ? 1 : 0) +
                   " temp_size=" + std::to_string(fd_size(temp_)));
    }
    if (z == logical_) {
        Log::debug("WRITE truncate-end id=" + std::to_string(diagnostic_id_) +
                   " result=NOOP logical_after=" + std::to_string(logical_));
        return;
    }
    if (z == 0 && sequential_ && temp_ < 0) {
        extents_.clear();
        buffer_.clear();
        staged_ = logical_ = 0;
        dirty_ = true;
        Log::debug("WRITE truncate-end id=" + std::to_string(diagnostic_id_) +
                   " result=SEQUENTIAL_RESET logical_after=0 staged_after=0");
        return;
    }
    if (temp_ < 0) {
        materialize();
        sequential_ = false;
    }
    if (ftruncate(temp_, z))
        fail(EIO, "truncate failed");
    logical_ = z;
    dirty_ = true;
    if (diagnostics)
        diagnostic_stage_checkpoint("post-truncate");
    Log::debug("WRITE truncate-end id=" + std::to_string(diagnostic_id_) +
               " result=OK logical_after=" + std::to_string(logical_) +
               " physical_after=" + std::to_string(fd_size(temp_)));
}
void WriteHandle::rebuild() {
    auto rebuild_started = Clock::now();
    Log::debug("WRITE rebuild-begin id=" + std::to_string(diagnostic_id_) +
               " logical=" + std::to_string(logical_) +
               " physical=" + std::to_string(fd_size(temp_)));
    diagnostic_stage_checkpoint("pre-rebuild");
    extents_.clear();
    staged_ = 0;
    Bytes b(fs_.extent_size());
    uint64_t o = 0;
    size_t index = 0;
    while (o < logical_) {
        size_t n = std::min<uint64_t>(b.size(), logical_ - o);
        if (pra(temp_, {b.data(), n}, o) != n)
            fail(EIO, "short staging read");
        const bool diagnostics = Log::enabled(LogLevel::debug);
        Hash256 plaintext_hash{};
        bool zero = false;
        std::string first, last;
        if (diagnostics) {
            plaintext_hash = sha256({b.data(), n});
            zero = all_zero({b.data(), n});
            first = edge_hex({b.data(), n}, true);
            last = edge_hex({b.data(), n}, false);
        }
        auto started = Clock::now();
        auto id = fs_.store().put({b.data(), n});
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        if (diagnostics) {
            Log::debug("WRITE rebuild-extent id=" + std::to_string(diagnostic_id_) +
                       " index=" + std::to_string(index) +
                       " offset=" + std::to_string(o) +
                       " length=" + std::to_string(n) +
                       " plaintext_sha256=" + to_string(plaintext_hash) +
                       " object_id=" + to_string(id) +
                       " hash_match=" + std::to_string(plaintext_hash == id ? 1 : 0) +
                       " all_zero=" + std::to_string(zero ? 1 : 0) +
                       " first16=" + first + " last16=" + last +
                       " ms=" + std::to_string(elapsed.count()));
        }
        extents_.push_back({o, n, id, false});
        o += n;
        ++index;
    }
    staged_ = logical_;
    auto rebuild_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - rebuild_started);
    Log::debug("WRITE rebuild-end id=" + std::to_string(diagnostic_id_) +
               " extents=" + std::to_string(extents_.size()) +
               " ms=" + std::to_string(rebuild_elapsed.count()));
}
void WriteHandle::commit() {
    std::lock_guard g(m_);
    if (!dirty_) {
        Log::debug("WRITE commit-clean id=" + std::to_string(diagnostic_id_));
        return;
    }
    auto commit_started = Clock::now();
    Log::debug("WRITE commit-begin id=" + std::to_string(diagnostic_id_) +
               " logical=" + std::to_string(logical_) +
               " staged=" + std::to_string(staged_) +
               " buffer=" + std::to_string(buffer_.size()) +
               " extents=" + std::to_string(extents_.size()) +
               " temp=" + std::to_string(temp_ >= 0 ? 1 : 0) +
               " base_version=" + std::to_string(base_.version));
    if (temp_ >= 0)
        rebuild();
    else
        flush();
    for (size_t i = 0; i < extents_.size(); ++i) {
        const auto& x = extents_[i];
        Log::debug("WRITE manifest id=" + std::to_string(diagnostic_id_) +
                   " index=" + std::to_string(i) +
                   " offset=" + std::to_string(x.offset) +
                   " length=" + std::to_string(x.length) +
                   " id=" + to_string(x.id));
    }
    FsEntry committed;
    fs_.commit_write(*this, base_, logical_, extents_, &committed);
    base_ = std::move(committed);
    expected_ = base_.version;
    dirty_ = false;
    auto commit_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - commit_started);
    Log::debug("WRITE commit-end id=" + std::to_string(diagnostic_id_) +
               " committed_size=" + std::to_string(base_.size) +
               " committed_version=" + std::to_string(base_.version) +
               " ms=" + std::to_string(commit_elapsed.count()));
}

WriteHandleDiagnostics WriteHandle::diagnostics() const {
    std::lock_guard g(m_);
    return {diagnostic_id_, logical_, staged_, buffer_.size(), sequential_, temp_ >= 0,
            fd_size(temp_)};
}

void WriteHandle::cleanup() {
    if (temp_ >= 0) {
        close(temp_);
        temp_ = -1;
    }
    if (!temp_path_.empty()) {
        std::error_code e;
        std::filesystem::remove(temp_path_, e);
    }
}
FileSystem::FileSystem(NodeRuntime& n, DistributedStore& s, MetadataManager& m, PlaybackTracker* playback)
    : n_(n), s_(s), m_(m), playback_(playback) {}
MetadataSnapshot FileSystem::snap() {
    return m_.snapshot();
}

void FileSystem::require_parent(const MetadataSnapshot& s, const std::string& p) {
    auto i = s.entries.find(parent_path(p));
    if (i == s.entries.end())
        fail(ENOENT, "parent missing");
    if (i->second.type != EntryType::directory)
        fail(ENOTDIR, "parent not directory");
}
FsEntry FileSystem::getattr(const std::string& p) {
    auto s = snap();
    auto i = s.entries.find(normalize_path(p));
    if (i == s.entries.end())
        fail(ENOENT, "not found");
    return i->second;
}
std::vector<std::pair<std::string, FsEntry>> FileSystem::readdir(const std::string& p) {
    auto q = normalize_path(p);
    auto s = snap();
    auto i = s.entries.find(q);
    if (i == s.entries.end())
        fail(ENOENT, "not found");
    if (i->second.type != EntryType::directory)
        fail(ENOTDIR, "not directory");
    std::vector<std::pair<std::string, FsEntry>> o;
    for (auto& [x, e] : s.entries)
        if (child(x, q))
            o.push_back({base_name(x), e});
    return o;
}
void FileSystem::mkdir(const std::string& p, uint32_t mode, uint32_t uid, uint32_t gid) {
    auto q = normalize_path(p);
    m_.mutate([&](MetadataSnapshot& s) {
        require_parent(s, q);
        if (s.entries.contains(q))
            fail(EEXIST, "exists");
        FsEntry e;
        e.type = EntryType::directory;
        e.mode = mode & 07777;
        e.uid = uid;
        e.gid = gid;
        e.ctime_ns = e.mtime_ns = wall_time_ns();
        s.entries[q] = e;
    });
}
void FileSystem::rmdir(const std::string& p) {
    auto q = normalize_path(p);
    if (q == "/")
        fail(EBUSY, "root");
    m_.mutate([&](MetadataSnapshot& s) {
        auto i = s.entries.find(q);
        if (i == s.entries.end())
            fail(ENOENT, "missing");
        if (i->second.type != EntryType::directory)
            fail(ENOTDIR, "not directory");
        for (auto& [x, _] : s.entries)
            if (x != q && under(x, q))
                fail(ENOTEMPTY, "not empty");
        s.entries.erase(i);
    });
}
FsEntry FileSystem::create_file(const std::string& p, uint32_t mode, uint32_t uid, uint32_t gid) {
    auto q = normalize_path(p);
    FsEntry e;
    e.type = EntryType::file;
    e.mode = mode & 07777;
    e.uid = uid;
    e.gid = gid;
    e.ctime_ns = e.mtime_ns = wall_time_ns();
    m_.mutate([&](MetadataSnapshot& s) {
        require_parent(s, q);
        if (s.entries.contains(q))
            fail(EEXIST, "exists");
        s.entries[q] = e;
    });
    return e;
}
void FileSystem::unlink(const std::string& p) {
    auto q = normalize_path(p);
    m_.mutate([&](MetadataSnapshot& s) {
        auto i = s.entries.find(q);
        if (i == s.entries.end())
            fail(ENOENT, "missing");
        if (i->second.type != EntryType::file)
            fail(EISDIR, "directory");
        queue_garbage(s, i->second);
        s.entries.erase(i);
    });
}
void FileSystem::rename(const std::string& a, const std::string& b, bool nr) {
    auto x = normalize_path(a), y = normalize_path(b);
    if (x == "/" || y == "/")
        fail(EBUSY, "root");
    if (x == y)
        return;
    if (under(y, x))
        fail(EINVAL, "recursive rename");

    // A write handle is currently path-backed rather than inode-backed. Keep
    // namespace rename and handle-path migration serialized with write commit so
    // a close/flush cannot observe the source path after it has moved.
    std::lock_guard handles(open_writes_mutex_);
    m_.mutate([&](MetadataSnapshot& s) {
        auto src = s.entries.find(x);
        if (src == s.entries.end())
            fail(ENOENT, "source missing");
        require_parent(s, y);
        auto dst = s.entries.find(y);
        if (dst != s.entries.end()) {
            if (nr)
                fail(EEXIST, "target exists");
            if (src->second.type == EntryType::directory &&
                dst->second.type != EntryType::directory)
                fail(ENOTDIR, "cannot replace file with directory");
            if (src->second.type != EntryType::directory &&
                dst->second.type == EntryType::directory)
                fail(EISDIR, "cannot replace directory with file");
            if (dst->second.type == EntryType::directory) {
                for (auto& [z, _] : s.entries) {
                    if (z != y && under(z, y))
                        fail(ENOTEMPTY, "target not empty");
                }
            } else {
                queue_garbage(s, dst->second);
            }
            s.entries.erase(dst);
        }
        std::vector<std::pair<std::string, FsEntry>> mv;
        for (auto i = s.entries.begin(); i != s.entries.end();)
            if (under(i->first, x)) {
                mv.push_back({y + i->first.substr(x.size()), i->second});
                i = s.entries.erase(i);
            } else
                ++i;
        for (auto& v : mv)
            s.entries.emplace(std::move(v));
    });

    for (auto i = open_writes_.begin(); i != open_writes_.end();) {
        auto handle = i->lock();
        if (!handle) {
            i = open_writes_.erase(i);
            continue;
        }
        if (under(handle->path_, x)) {
            const auto before = handle->path_;
            handle->path_ = y + handle->path_.substr(x.size());
            Log::debug("WRITE rename id=" + std::to_string(handle->diagnostic_id_) +
                       " from=" + before + " to=" + handle->path_);
        }
        ++i;
    }
}
void FileSystem::chmod(const std::string& p, uint32_t mode) {
    auto q = normalize_path(p);
    m_.mutate([&](MetadataSnapshot& s) {
        auto j = s.entries.find(q);
        if (j == s.entries.end())
            fail(ENOENT, "missing");
        auto& i = j->second;
        i.mode = mode & 07777;
        ++i.version;
        i.ctime_ns = wall_time_ns();
    });
}
void FileSystem::chown(const std::string& p, uint32_t u, uint32_t g, bool su, bool sg) {
    auto q = normalize_path(p);
    m_.mutate([&](MetadataSnapshot& s) {
        auto i = s.entries.find(q);
        if (i == s.entries.end())
            fail(ENOENT, "missing");
        if (su)
            i->second.uid = u;
        if (sg)
            i->second.gid = g;
        ++i->second.version;
        i->second.ctime_ns = wall_time_ns();
    });
}
void FileSystem::utimens(const std::string& p, int64_t mt) {
    auto q = normalize_path(p);
    m_.mutate([&](MetadataSnapshot& s) {
        auto i = s.entries.find(q);
        if (i == s.entries.end())
            fail(ENOENT, "missing");
        i->second.mtime_ns = mt;
        i->second.ctime_ns = wall_time_ns();
        ++i->second.version;
    });
}
void FileSystem::truncate_file(const std::string& p, uint64_t z) {
    auto q = normalize_path(p);
    auto e = getattr(q);
    if (e.type != EntryType::file)
        fail(EISDIR, "directory");
    if (z == e.size)
        return;
    std::vector<ExtentRef> xs;
    if (z < e.size) {
        for (size_t i = 0; i < e.extents.size(); ++i) {
            auto x = e.extents[i];
            if (x.offset >= z)
                break;
            if (x.offset + x.length <= z) {
                xs.push_back(x);
                continue;
            }
            size_t keep = z - x.offset;
            if (x.hole)
                xs.push_back({x.offset, keep, {}, true});
            else {
                auto d = s_.get(x.id, i);
                if (!d || d->size() < keep)
                    fail(EIO, "truncate source unavailable");
                auto id = s_.put({d->data(), keep});
                xs.push_back({x.offset, keep, id, false});
            }
            break;
        }
    } else {
        xs = e.extents;
        xs.push_back({e.size, z - e.size, {}, true});
    }
    commit_file(q, e, z, xs, nullptr);
}
std::string file_media_id(const FsEntry& entry) {
    if (entry.type != EntryType::file)
        return {};
    Writer writer;
    constexpr std::array<uint8_t, 8> magic{'M', 'F', 'I', 'L', 'E', '0', '0', '1'};
    writer.raw(magic);
    writer.u64(entry.size);
    writer.u32(entry.extents.size());
    for (const auto& extent : entry.extents) {
        writer.u64(extent.offset);
        writer.u64(extent.length);
        writer.u8(extent.hole);
        if (!extent.hole)
            writer.fixed(extent.id.bytes);
    }
    return "macha:" + to_string(object_id(writer.data()));
}

std::optional<std::pair<std::string, FsEntry>> FileSystem::find_media(std::string_view id) {
    if (id.starts_with("path:")) {
        auto path = std::string(id.substr(5));
        try {
            auto entry = getattr(path);
            if (entry.type == EntryType::file)
                return std::pair{normalize_path(path), entry};
        } catch (...) {
        }
        return {};
    }
    if (!id.empty() && id.front() == '/') {
        try {
            auto entry = getattr(std::string(id));
            if (entry.type == EntryType::file)
                return std::pair{normalize_path(std::string(id)), entry};
        } catch (...) {
        }
        return {};
    }
    auto snapshot = snap();
    for (const auto& [path, entry] : snapshot.entries) {
        if (entry.type == EntryType::file && file_media_id(entry) == id)
            return std::pair{path, entry};
    }
    return {};
}

std::shared_ptr<ReadHandle> FileSystem::open_read(const std::string& p) {
    auto e = getattr(p);
    if (e.type != EntryType::file)
        fail(EISDIR, "directory");
    return std::make_shared<ReadHandle>(s_, e, playback_, normalize_path(p));
}
std::shared_ptr<WriteHandle> FileSystem::open_write(const std::string& p, bool trunc) {
    // Serialize path lookup/registration with rename so an opening writer cannot
    // miss a rename between resolving the entry and joining the handle registry.
    std::lock_guard handles(open_writes_mutex_);
    auto e = getattr(p);
    if (e.type != EntryType::file)
        fail(EISDIR, "directory");
    if (trunc && e.size) {
        truncate_file(p, 0);
        e = getattr(p);
    }
    auto handle =
        std::make_shared<WriteHandle>(*this, normalize_path(p), e, trunc || !e.size);
    for (auto i = open_writes_.begin(); i != open_writes_.end();) {
        if (i->expired())
            i = open_writes_.erase(i);
        else
            ++i;
    }
    open_writes_.push_back(handle);
    return handle;
}

std::optional<uint64_t> FileSystem::active_write_size(const std::string& p) {
    const auto q = normalize_path(p);
    std::vector<std::shared_ptr<WriteHandle>> matches;
    {
        std::lock_guard handles(open_writes_mutex_);
        for (auto i = open_writes_.begin(); i != open_writes_.end();) {
            auto handle = i->lock();
            if (!handle) {
                i = open_writes_.erase(i);
                continue;
            }
            // path_ is changed only while open_writes_mutex_ is held by rename().
            if (handle->path_ == q)
                matches.push_back(std::move(handle));
            ++i;
        }
    }

    std::optional<uint64_t> visible;
    for (const auto& handle : matches) {
        const auto size = handle->size();
        if (!visible || size > *visible)
            visible = size;
    }
    return visible;
}

std::vector<WriteHandleDiagnostics> FileSystem::active_write_diagnostics(const std::string& p) {
    const auto q = normalize_path(p);
    std::vector<std::shared_ptr<WriteHandle>> matches;
    {
        std::lock_guard handles(open_writes_mutex_);
        for (auto i = open_writes_.begin(); i != open_writes_.end();) {
            auto handle = i->lock();
            if (!handle) {
                i = open_writes_.erase(i);
                continue;
            }
            // path_ is changed only while open_writes_mutex_ is held by rename().
            if (handle->path_ == q)
                matches.push_back(std::move(handle));
            ++i;
        }
    }

    std::vector<WriteHandleDiagnostics> out;
    out.reserve(matches.size());
    for (const auto& handle : matches)
        out.push_back(handle->diagnostics());
    return out;
}

void FileSystem::commit_write(WriteHandle& handle, const FsEntry& expected, uint64_t z,
                              const std::vector<ExtentRef>& xs, FsEntry* out) {
    std::lock_guard handles(open_writes_mutex_);
    commit_file(handle.path_, expected, z, xs, out);
}
void FileSystem::commit_file(const std::string& p, const FsEntry& expected, uint64_t z,
                             const std::vector<ExtentRef>& xs, FsEntry* out) {
    auto q = normalize_path(p);
    FsEntry committed;
    m_.mutate([&](MetadataSnapshot& s) {
        auto i = s.entries.find(q);
        if (i == s.entries.end())
            fail(ENOENT, "removed while open");

        // chmod/chown/utimens may legitimately run against an open write handle
        // (macOS cp does exactly this).  Those operations advance the inode
        // version but do not change file content, so they must not invalidate
        // the data writer.  Reject only if the content observed when the handle
        // opened has actually changed.
        if (i->second.version != expected.version &&
            (i->second.size != expected.size || i->second.extents != expected.extents))
            fail(EAGAIN, "concurrent file content change");

        // If somebody explicitly changed mtime while this handle was open, keep
        // that value.  This is required for cp -p / macOS copyfile semantics,
        // which can set timestamps before the final flush/close.  Otherwise a
        // successful data write updates mtime normally.
        const bool explicit_mtime = i->second.mtime_ns != expected.mtime_ns;

        std::set<ObjectId> retained;
        for (const auto& extent : xs) {
            if (!extent.hole)
                retained.insert(extent.id);
        }
        for (const auto& extent : i->second.extents) {
            if (!extent.hole && !retained.contains(extent.id))
                queue_garbage(s, extent.id);
        }

        i->second.size = z;
        i->second.extents = xs;
        const auto now = wall_time_ns();
        if (!explicit_mtime)
            i->second.mtime_ns = now;
        i->second.ctime_ns = now;
        ++i->second.version;
        committed = i->second;
    });
    if (out)
        *out = std::move(committed);
}
std::pair<uint64_t, uint64_t> FileSystem::logical_capacity() const {
    auto ns = n_.membership().active();
    if (ns.empty())
        return {0, 0};
    const size_t r = std::max<size_t>(1, std::min(n_.config().replication, ns.size()));
    const uint64_t total = placement_logical_capacity(ns, r);

    __uint128_t physical_used = 0;
    for (const auto& node : ns)
        physical_used += node.used;
    uint64_t used = static_cast<uint64_t>(std::min<__uint128_t>(
        physical_used / r, std::numeric_limits<uint64_t>::max()));
    used = std::min(used, total);
    return {total, used};
}
std::vector<ObjectId> FileSystem::live_objects() {
    return maintenance_objects().live;
}

MaintenanceObjects FileSystem::maintenance_objects() {
    auto snapshot = m_.snapshot();
    std::set<ObjectId> live;
    for (const auto& [_, entry] : snapshot.entries) {
        for (const auto& extent : entry.extents) {
            if (!extent.hole)
                live.insert(extent.id);
        }
    }

    std::set<ObjectId> garbage;
    for (const auto& candidate : snapshot.garbage) {
        if (!live.contains(candidate.id))
            garbage.insert(candidate.id);
    }

    return {{live.begin(), live.end()}, {garbage.begin(), garbage.end()}};
}
} // namespace macha
