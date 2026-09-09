// SPDX-License-Identifier: GPL-3.0-or-later
#include "filesystem.hpp"
#include "crypto.hpp"
#include "codec.hpp"
#include "hydration.hpp"
#include "log.hpp"
#include "macos_unicode.hpp"
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

MemoryClass filesystem_memory_class(FrameType frame_type) {
    switch (frame_type) {
    case FrameType::control: return MemoryClass::control;
    case FrameType::foreground:
    case FrameType::read_ahead: return MemoryClass::viewer;
    case FrameType::loader: return MemoryClass::loader;
    case FrameType::speculative: return MemoryClass::speculative;
    }
    return MemoryClass::speculative;
}

[[noreturn]] void fail(int c, const std::string& s) {
    throw FsError(c, s);
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

void record_garbage_upsert(MetadataDelta& delta, const GarbageRef& garbage) {
    auto existing = std::find_if(delta.upsert_garbage.begin(), delta.upsert_garbage.end(),
                                 [&](const GarbageRef& value) { return value.id == garbage.id; });
    if (existing == delta.upsert_garbage.end())
        delta.upsert_garbage.push_back(garbage);
    else
        *existing = garbage;
}

void queue_garbage_batch(MetadataSnapshot& snapshot, std::vector<ObjectId> ids,
                         MetadataDelta& delta) {
    if (ids.empty())
        return;

    // File publication can retire hundreds of extents at once.  Do not perform
    // one linear scan of the (potentially very large) garbage vector per extent.
    // Keep the transient index bounded by this file's extent count and scan the
    // committed garbage set once.
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::vector<bool> found(ids.size(), false);

    for (auto& garbage : snapshot.garbage) {
        auto target = std::lower_bound(ids.begin(), ids.end(), garbage.id);
        if (target == ids.end() || *target != garbage.id)
            continue;
        const auto index = static_cast<size_t>(target - ids.begin());
        if (found[index])
            continue;

        auto retired = wall_time_ns();
        if (retired <= garbage.retired_at_ns &&
            garbage.retired_at_ns < std::numeric_limits<int64_t>::max())
            retired = garbage.retired_at_ns + 1;
        garbage.retired_at_ns = retired;
        garbage.retirement_id = random_node_id();
        record_garbage_upsert(delta, garbage);
        found[index] = true;
    }

    for (size_t i = 0; i < ids.size(); ++i) {
        if (found[i])
            continue;
        snapshot.garbage.push_back({ids[i], wall_time_ns(), random_node_id()});
        record_garbage_upsert(delta, snapshot.garbage.back());
    }
}

} // namespace
ReadHandle::ReadHandle(DistributedStore& s, FsEntry e, PlaybackTracker* playback,
                       std::string path, FrameType frame_type)
    : s_(s), e_(std::move(e)), playback_(playback), frame_type_(frame_type) {
    if (playback_)
        playback_session_ = playback_->open(std::move(path), e_);
    if (Log::enabled(LogLevel::all))
        Log::trace("DIAG read-handle open ptr=" +
               std::to_string(reinterpret_cast<uintptr_t>(this)) +
               " size=" + std::to_string(e_.size) +
               " extents=" + std::to_string(e_.extents.size()));
    for (size_t i = 0; i < e_.extents.size(); ++i) {
        const auto& x = e_.extents[i];
        if (Log::enabled(LogLevel::all))
            Log::trace("DIAG read-manifest ptr=" +
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

void ReadHandle::promote(FrameType requested) noexcept {
    auto current = frame_type_.load(std::memory_order_relaxed);
    while (frame_type_priority(requested) < frame_type_priority(current) &&
           !frame_type_.compare_exchange_weak(current, requested,
                                              std::memory_order_relaxed)) {}
}

const Bytes& ReadHandle::extent(size_t i, Clock::time_point deadline,
                                std::atomic_bool* cancelled) {
    if (cached_index_ == i) {
        if (Log::enabled(LogLevel::all))
            Log::trace("DIAG read-extent cache-hit ptr=" +
                   std::to_string(reinterpret_cast<uintptr_t>(this)) +
                   " index=" + std::to_string(i));
        return cached_extent_->bytes;
    }

    auto& x = e_.extents.at(i);
    // The previous extent is no longer observable once this handle advances.
    // Release it before reserving the replacement so a two-buffer handoff
    // cannot consume the viewer headroom indefinitely.
    cached_extent_.reset();
    cached_index_ = static_cast<size_t>(-1);
    auto started = Clock::now();
    auto data =
        s_.get_shared(x.id, i, frame_type_.load(std::memory_order_relaxed), deadline, cancelled);
    if (!data) {
        if (cancelled && cancelled->load())
            fail(ECANCELED, "extent read cancelled");
        if (deadline != Clock::time_point{} && Clock::now() >= deadline)
            fail(ETIMEDOUT, "extent read timed out");
        fail(EIO, "extent unavailable");
    }
    if (data->bytes.size() != x.length)
        fail(EIO, "extent corrupt");

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    if (Log::enabled(LogLevel::all))
        Log::trace("DIAG read-extent load ptr=" +
               std::to_string(reinterpret_cast<uintptr_t>(this)) +
               " index=" + std::to_string(i) +
               " offset=" + std::to_string(x.offset) +
               " length=" + std::to_string(x.length) +
               " id=" + to_string(x.id) +
               " source=direct ms=" + std::to_string(elapsed.count()));

    cached_extent_ = std::move(data);
    cached_index_ = i;
    return cached_extent_->bytes;
}

size_t ReadHandle::read(uint64_t off, std::span<uint8_t> out, Clock::time_point deadline,
                        std::atomic_bool* cancelled) {
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
                const auto& d = extent(idx, deadline, cancelled);
                std::copy_n(d.begin() + (a - it->offset), n, out.begin() + done);
            }
            done += n;
            last_extent = idx;
        }
        ++it;
    }
    bool seq = off == last_;
    last_ = off + done;
    if (done) {
        const auto frame_type = frame_type_.load(std::memory_order_relaxed);
        if (frame_type == FrameType::foreground)
            s_.foreground_activity(done);
        else if (frame_type == FrameType::read_ahead)
            s_.interactive_activity(done);
    }
    if (seq && done && playback_ && playback_session_ && last_extent != static_cast<size_t>(-1))
        playback_->progress(playback_session_, last_extent);
    return done;
}

WriteHandle::WriteHandle(FileSystem& f, std::string p, FsEntry b, bool trunc, bool cache_puts,
                         WriteDurability durability, uint64_t publication_pipeline_bytes,
                         DataWorkContext work_context)
    : fs_(f), path_(std::move(p)), base_(std::move(b)), expected_(base_.version),
      sequential_(true), cache_puts_(cache_puts), durability_(durability),
      work_context_(work_context),
      publication_pipeline_bytes_(publication_pipeline_bytes),
      logical_(trunc ? 0 : base_.size), staged_(trunc ? 0 : base_.size),
      diagnostic_id_(next_write_handle_diagnostic_id.fetch_add(1, std::memory_order_relaxed)) {
    // Existing files are append-capable without rematerialising the prefix.
    // Keep every complete immutable extent by reference.  If EOF lands inside
    // the final extent, fetch only that tail lazily when the first append
    // arrives so it can seed the normal sequential extent buffer.
    if (!trunc && base_.size) {
        extents_ = base_.extents;
        const auto extent_size = fs_.extent_size();
        const auto tail_length = static_cast<size_t>(base_.size % extent_size);
        if (tail_length) {
            const auto tail_offset = base_.size - tail_length;
            if (!extents_.empty()) {
                const auto& tail = extents_.back();
                if (tail.offset == tail_offset && tail.length == tail_length) {
                    append_tail_ = tail;
                    extents_.pop_back();
                    staged_ = tail_offset;
                } else {
                    // A non-canonical manifest remains writable through the
                    // generic staging fallback; do not guess at its tail.
                    sequential_ = false;
                }
            } else {
                sequential_ = false;
            }
        }
    }

    if (Log::enabled(LogLevel::all))
        Log::trace("WRITE handle-open id=" + std::to_string(diagnostic_id_) +
               " path=" + path_ +
               " base_size=" + std::to_string(base_.size) +
               " base_version=" + std::to_string(base_.version) +
               " sequential=" + std::to_string(sequential_ ? 1 : 0) +
               " retained_extents=" + std::to_string(extents_.size()) +
               " append_tail=" + std::to_string(append_tail_ ? 1 : 0));
}

void WriteHandle::ensure_buffer_memory() {
    if (buffer_memory_)
        return;
    const auto memory_class = filesystem_memory_class(work_context_.frame_type());
    auto& ledger = fs_.node().retained_memory();

    // Without a no-progress budget this is the historical behaviour: wait on
    // the caller's own deadline, which for publication was no deadline at all.
    const auto* progress = work_context_.progress();
    if (!progress) {
        auto memory = ledger.acquire(memory_class, MemoryOwner::publication, fs_.extent_size(),
                                     work_context_.deadline(), work_context_.cancellation());
        if (!memory)
            fail(EAGAIN, "write extent retained-memory admission saturated");
        buffer_memory_.emplace(std::move(*memory));
        buffer_.reserve(fs_.extent_size());
        return;
    }

    // With one, wait in slices and watch the shared counter. Any worker in
    // this pipeline making progress re-arms the window, so a slow-but-moving
    // node is never punished for being slow; only a pipeline where nothing at
    // all advances within the budget fails. That failure is an ordinary EAGAIN,
    // which the publication retry policy already backs off and eventually
    // parks -- turning a permanent silent deadlock into visible, bounded,
    // self-healing failure. Before this, all eight workers held their buffers
    // and blocked forever, so `parked_publications` stayed 0 on a node that
    // had published nothing for hours (es-1, 2026-09-09).
    constexpr auto slice = std::chrono::milliseconds(500);
    const auto budget = work_context_.no_progress_budget();
    auto seen = progress->load(std::memory_order_relaxed);
    auto window_started = DataWorkContext::Clock::now();
    while (true) {
        const auto absolute = work_context_.deadline();
        auto until = DataWorkContext::Clock::now() + slice;
        if (absolute != DataWorkContext::Clock::time_point{} && absolute < until)
            until = absolute;
        auto memory = ledger.acquire(memory_class, MemoryOwner::publication, fs_.extent_size(),
                                     until, work_context_.cancellation());
        if (memory) {
            buffer_memory_.emplace(std::move(*memory));
            buffer_.reserve(fs_.extent_size());
            return;
        }
        if (work_context_.cancelled() ||
            (absolute != DataWorkContext::Clock::time_point{} &&
             DataWorkContext::Clock::now() >= absolute))
            fail(EAGAIN, "write extent retained-memory admission saturated");

        const auto now_seen = progress->load(std::memory_order_relaxed);
        if (now_seen != seen) {
            seen = now_seen;
            window_started = DataWorkContext::Clock::now();
            continue;
        }
        if (DataWorkContext::Clock::now() - window_started >= budget)
            fail(EAGAIN, "write extent retained-memory admission made no progress within budget");
    }
}
WriteHandle::~WriteHandle() {
    try {
        std::lock_guard lock(m_);
        (void)drain_staging_locked();
    } catch (...) {
        // An abandoned or failed generation has no visible metadata. Joining
        // its bounded provisional puts is nevertheless required before the
        // handle releases references captured by those tasks.
    }
    cleanup();
}

void WriteHandle::diagnostic_stage_extent(const char* label, size_t index, uint64_t offset,
                                          size_t length) {
    if (!Log::enabled(LogLevel::all) || temp_ < 0 || !length)
        return;
    try {
        Bytes bytes(length);
        const auto got = pra(temp_, bytes, offset);
        if (got != length) {
            if (Log::enabled(LogLevel::all))
                Log::trace("WRITE stage-check id=" + std::to_string(diagnostic_id_) +
                       " label=" + label + " index=" + std::to_string(index) +
                       " offset=" + std::to_string(offset) +
                       " length=" + std::to_string(length) +
                       " read=" + std::to_string(got) + " result=SHORT");
            return;
        }
        const auto hash = sha256(bytes);
        if (Log::enabled(LogLevel::all))
            Log::trace("WRITE stage-check id=" + std::to_string(diagnostic_id_) +
                   " label=" + label + " index=" + std::to_string(index) +
                   " offset=" + std::to_string(offset) +
                   " length=" + std::to_string(length) +
                   " sha256=" + to_string(hash) +
                   " all_zero=" + std::to_string(all_zero(bytes) ? 1 : 0) +
                   " first16=" + edge_hex(bytes, true) +
                   " last16=" + edge_hex(bytes, false));
    } catch (const std::exception& error) {
        if (Log::enabled(LogLevel::all))
            Log::trace("WRITE stage-check id=" + std::to_string(diagnostic_id_) +
                   " label=" + label + " index=" + std::to_string(index) +
                   " result=DIAGNOSTIC_ERROR message=" + error.what());
    }
}

void WriteHandle::diagnostic_stage_checkpoint(const char* label) {
    if (!Log::enabled(LogLevel::all) || temp_ < 0)
        return;
    const auto physical = fd_size(temp_);
    Log::trace("WRITE stage-checkpoint id=" + std::to_string(diagnostic_id_) +
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
std::chrono::milliseconds WriteHandle::flush() {
    if (buffer_.empty())
        return {};
    const auto offset = staged_;
    const auto length = buffer_.size();
    const bool diagnostics = Log::enabled(LogLevel::all);
    const bool zero = diagnostics && all_zero(buffer_);
    const auto first = diagnostics ? edge_hex(buffer_, true) : std::string{};
    const auto last = diagnostics ? edge_hex(buffer_, false) : std::string{};
    if (fs_.io_cancellation_requested())
        fail(EINTR, "write cancelled");
    auto started = Clock::now();
    if (durability_ == WriteDurability::publication_generation &&
        publication_pipeline_bytes_) {
        std::chrono::milliseconds waited{};
        while (pending_extent_bytes_ + length > publication_pipeline_bytes_)
            waited += drain_one_extent();

        auto bytes = std::make_shared<const Bytes>(std::move(buffer_));
        auto memory = std::move(buffer_memory_);
        buffer_memory_.reset();
        buffer_.clear();
        staged_ += length;
        pending_extent_bytes_ += length;
        PendingExtent pending{length, offset, cache_puts_, std::move(bytes), {},
                              std::move(memory)};
        launch_pending_extent(pending);
        pending_extents_.push_back(std::move(pending));
        peak_pending_extents_ = std::max(peak_pending_extents_, pending_extents_.size());
        return waited;
    }
    ObjectId id;
    try {
        if (durability_ == WriteDurability::publication_generation)
            id = fs_.store().put_deferred(buffer_, durability_batch_,
                                          work_context_.frame_type(), &fs_.io_cancelled_);
        else
            id = fs_.store().put(buffer_, work_context_.frame_type(), &fs_.io_cancelled_);
    } catch (...) {
        if (fs_.io_cancellation_requested())
            fail(EINTR, "write cancelled");
        throw;
    }
    if (cache_puts_)
        (void)fs_.store().cache_local(id, buffer_);
    ++new_extent_puts_;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    if (elapsed >= std::chrono::milliseconds(500) && Log::enabled(LogLevel::debug)) {
        Log::debug("write stage id=" + std::to_string(diagnostic_id_) +
                   " path=" + path_ +
                   " stage=extent-put offset=" + std::to_string(offset) +
                   " bytes=" + std::to_string(length) +
                   " object=" + to_string(id) +
                   " elapsed_ms=" + std::to_string(elapsed.count()));
    }
    if (diagnostics) {
        if (Log::enabled(LogLevel::all))
            Log::trace("WRITE extent-put id=" + std::to_string(diagnostic_id_) +
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
    return elapsed;
}

void WriteHandle::launch_pending_extent(PendingExtent& pending) {
    auto* store = &fs_.store();
    auto* cancelled = &fs_.io_cancelled_;
    const auto cache_put = pending.cache_put;
    const auto offset = pending.offset;
    const auto bytes = pending.payload;
    const auto frame_type = work_context_.frame_type();
    pending.result = fs_.submit_extent_task(
        [store, cancelled, cache_put, offset, frame_type, bytes] {
            const auto put_started = Clock::now();
            DistributedStore::DurabilityBatch batch;
            auto id = store->put_deferred(*bytes, batch, frame_type, cancelled);
            if (cache_put)
                (void)store->cache_local(id, *bytes);
            return StagedExtentResult{
                {offset, bytes->size(), id, false}, std::move(batch),
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - put_started)};
        });
}

std::chrono::milliseconds WriteHandle::drain_one_extent() {
    if (pending_extents_.empty())
        return {};
    auto& pending = pending_extents_.front();
    // A failed asynchronous put leaves its payload and exact manifest offset
    // at the queue head. The next publication attempt relaunches that bounded,
    // content-addressed operation instead of discarding the complete writer
    // and replaying every earlier spool byte.
    if (!pending.result.valid())
        launch_pending_extent(pending);
    auto result = pending.result.get();
    durability_batch_.requirements.insert(
        durability_batch_.requirements.end(),
        std::make_move_iterator(result.durability.requirements.begin()),
        std::make_move_iterator(result.durability.requirements.end()));
    extents_.push_back(result.extent);
    ++new_extent_puts_;
    if (result.elapsed >= std::chrono::milliseconds(500) && Log::enabled(LogLevel::debug)) {
        Log::debug("write stage id=" + std::to_string(diagnostic_id_) +
                   " path=" + path_ + " stage=extent-put offset=" +
                   std::to_string(result.extent.offset) + " bytes=" +
                   std::to_string(result.extent.length) + " object=" +
                   to_string(result.extent.id) + " elapsed_ms=" +
                   std::to_string(result.elapsed.count()));
    }
    pending_extent_bytes_ -= pending.bytes;
    pending_extents_.pop_front();
    // The lease this extent held is now back in the ledger. That is the event
    // a writer waiting on admission is waiting for.
    fs_.note_write_progress();
    return result.elapsed;
}

std::chrono::milliseconds WriteHandle::drain_staging_locked() {
    std::chrono::milliseconds elapsed{};
    // Stop at the first failed offset. Later pipelined futures may finish in
    // parallel, but cannot enter the manifest ahead of the missing extent.
    while (!pending_extents_.empty())
        elapsed += drain_one_extent();
    return elapsed;
}

void WriteHandle::drain_staging() {
    std::lock_guard lock(m_);
    (void)drain_staging_locked();
}
void WriteHandle::prepare_append_tail() {
    if (!append_tail_)
        return;
    if (fs_.io_cancellation_requested())
        fail(EINTR, "write cancelled");

    const auto tail = *append_tail_;
    ensure_buffer_memory();
    Bytes bytes;
    if (tail.hole) {
        bytes.assign(static_cast<size_t>(tail.length), 0);
    } else {
        auto data = fs_.store().get(tail.id, static_cast<size_t>(tail.offset / fs_.extent_size()),
                                    work_context_.frame_type(), {}, &fs_.io_cancelled_);
        if (!data) {
            if (fs_.io_cancellation_requested())
                fail(EINTR, "write cancelled");
            fail(EIO, "cannot read append tail extent");
        }
        if (data->size() != tail.length)
            fail(EIO, "append tail extent length mismatch");
        bytes = std::move(*data);
    }
    ++append_tail_fetches_;
    buffer_ = std::move(bytes);
    staged_ = tail.offset;
    append_tail_.reset();

    if (Log::enabled(LogLevel::debug))
        Log::debug("write stage id=" + std::to_string(diagnostic_id_) +
                   " path=" + path_ +
                   " stage=append-tail-fetch offset=" + std::to_string(tail.offset) +
                   " bytes=" + std::to_string(tail.length));
}

bool WriteHandle::canonical_base() const {
    uint64_t offset = 0;
    for (const auto& extent : base_.extents) {
        if (extent.offset != offset || !extent.length || extent.length > fs_.extent_size())
            return false;
        const auto remaining = base_.size - std::min(base_.size, offset);
        const auto expected = std::min<uint64_t>(fs_.extent_size(), remaining);
        if (extent.length != expected)
            return false;
        offset += extent.length;
    }
    return offset == base_.size;
}

void WriteHandle::note_changed_range(uint64_t begin, uint64_t end) {
    if (begin >= end)
        return;
    ChangedRange merged{begin, end};
    auto it = changed_ranges_.begin();
    while (it != changed_ranges_.end() && it->end < merged.begin)
        ++it;
    while (it != changed_ranges_.end() && it->begin <= merged.end) {
        merged.begin = std::min(merged.begin, it->begin);
        merged.end = std::max(merged.end, it->end);
        it = changed_ranges_.erase(it);
    }
    changed_ranges_.insert(it, merged);
}

bool WriteHandle::range_changed(uint64_t begin, uint64_t end) const {
    auto it = std::lower_bound(changed_ranges_.begin(), changed_ranges_.end(), begin,
                               [](const ChangedRange& range, uint64_t value) {
                                   return range.end <= value;
                               });
    return it != changed_ranges_.end() && it->begin < end;
}

void WriteHandle::begin_sparse_overlay() {
    if (temp_ >= 0)
        return;
    if (!canonical_base())
        fail(EINVAL, "sparse overlay requires a canonical base manifest");
    (void)drain_staging_locked();
    if (fs_.io_cancellation_requested())
        fail(EINTR, "write cancelled");
    auto directory = fs_.node().config().state_path / "tmp";
    std::filesystem::create_directories(directory);
    auto pattern =
        (directory / ("write." + to_string(fs_.node().node_id()) + ".XXXXXX")).string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    temp_ = mkstemp(name.data());
    if (temp_ < 0)
        fail(EIO, "cannot create sparse overlay file");
    temp_path_ = name.data();
    sparse_overlay_ = true;
    sequential_ = false;
    rebuild_prepared_ = false;

    // Sequential work may have staged an incomplete new tail before a later
    // write changed direction. Preserve only that tail as overlay data; full
    // immutable extents already present in extents_ remain reusable by ID.
    if (!buffer_.empty()) {
        pwa(temp_, buffer_, staged_);
        note_changed_range(staged_, staged_ + buffer_.size());
        buffer_.clear();
    }
    append_tail_.reset();
    if (ftruncate(temp_, logical_))
        fail(EIO, "sparse overlay truncate failed");
}

WritePreparation WriteHandle::materialize_step(uint64_t byte_budget) {
    if (!materializing_) {
        (void)drain_staging_locked();
        if (temp_ >= 0)
            return {true, 0};
        if (sequential_ && append_tail_) {
            // Non-sequential fallback needs the old tail as immutable source
            // data, not as an eagerly fetched append seed. Restore the manifest
            // reference so its fetch and staging write occur inside this
            // resumable materialisation budget.
            extents_.push_back(*append_tail_);
            staged_ = logical_;
            append_tail_.reset();
        }
        if (fs_.io_cancellation_requested())
            fail(EINTR, "write cancelled");
        auto d = fs_.node().config().state_path / "tmp";
        std::filesystem::create_directories(d);
        auto pattern = (d / ("write." + to_string(fs_.node().node_id()) + ".XXXXXX")).string();
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        temp_ = mkstemp(name.data());
        if (temp_ < 0)
            fail(EIO, "cannot create staging file");
        temp_path_ = name.data();
        materializing_ = true;
        materialize_offset_ = 0;
        materialize_extent_index_ = 0;
        if (Log::enabled(LogLevel::all))
            Log::trace("WRITE materialize-begin id=" + std::to_string(diagnostic_id_) +
                   " path=" + path_ +
                   " logical=" + std::to_string(logical_) +
                   " staged=" + std::to_string(staged_) +
                   " buffer=" + std::to_string(buffer_.size()) +
                   " extents=" + std::to_string(extents_.size()) +
                   " sequential=" + std::to_string(sequential_ ? 1 : 0) +
                   " temp=" + temp_path_.string());
    }

    const bool unlimited = byte_budget == 0;
    if (!unlimited && byte_budget < fs_.extent_size())
        fail(EINVAL, "materialization budget must be zero or at least one extent");
    uint64_t processed = 0;
    const auto source_size = logical_;
    while (materialize_offset_ < source_size &&
           (unlimited || processed < byte_budget)) {
        if (fs_.io_cancellation_requested())
            fail(EINTR, "write cancelled");
        const auto remaining_budget = unlimited
                                          ? std::numeric_limits<uint64_t>::max()
                                          : byte_budget - processed;

        if (sequential_ && materialize_offset_ < staged_) {
            if (materialize_extent_index_ >= extents_.size())
                fail(EIO, "staged extent manifest has a gap");
            const auto& extent = extents_[materialize_extent_index_];
            if (extent.offset != materialize_offset_ || extent.length > remaining_budget)
                fail(EIO, "staged extent exceeds materialization quantum");
            if (extent.hole) {
                Bytes zeros(static_cast<size_t>(extent.length), 0);
                pwa(temp_, zeros, extent.offset);
            } else {
                auto bytes = fs_.store().get(extent.id, materialize_extent_index_,
                                             work_context_.frame_type(), {},
                                             &fs_.io_cancelled_);
                ++materialize_source_reads_;
                if (!bytes || bytes->size() != extent.length) {
                    if (fs_.io_cancellation_requested())
                        fail(EINTR, "write cancelled");
                    fail(EIO, "cannot rematerialize staged extent");
                }
                pwa(temp_, *bytes, extent.offset);
            }
            materialize_offset_ += extent.length;
            processed += extent.length;
            ++materialize_extent_index_;
            continue;
        }

        if (sequential_) {
            if (materialize_offset_ < staged_ || materialize_offset_ - staged_ >= buffer_.size())
                fail(EIO, "staged write buffer has a gap");
            const auto buffer_offset = static_cast<size_t>(materialize_offset_ - staged_);
            const auto n = static_cast<size_t>(std::min<uint64_t>(
                {buffer_.size() - buffer_offset, source_size - materialize_offset_,
                 remaining_budget}));
            pwa(temp_, {buffer_.data() + buffer_offset, n}, materialize_offset_);
            materialize_offset_ += n;
            processed += n;
            continue;
        }

        const auto n = static_cast<size_t>(std::min<uint64_t>(
            {fs_.extent_size(), source_size - materialize_offset_, remaining_budget}));
        ReadHandle reader(fs_.store(), base_, nullptr, {}, work_context_.frame_type());
        Bytes bytes(n);
        ++materialize_source_reads_;
        if (reader.read(materialize_offset_, bytes, {}, &fs_.io_cancelled_) != n) {
            if (fs_.io_cancellation_requested())
                fail(EINTR, "write cancelled");
            fail(EIO, "short source read");
        }
        pwa(temp_, bytes, materialize_offset_);
        materialize_offset_ += n;
        processed += n;
    }

    materialize_source_bytes_ += processed;
    ++materialize_steps_;
    if (materialize_offset_ < source_size)
        return {false, processed};

    if (ftruncate(temp_, logical_))
        fail(EIO, "staging truncate failed");
    materializing_ = false;
    sequential_ = false;
    diagnostic_stage_checkpoint("post-materialize");
    diagnostic_completed_extents_ = static_cast<size_t>(logical_ / fs_.extent_size());
    if (Log::enabled(LogLevel::all))
        Log::trace("WRITE materialize-end id=" + std::to_string(diagnostic_id_) +
               " physical=" + std::to_string(fd_size(temp_)) +
               " completed_extents=" + std::to_string(diagnostic_completed_extents_));
    return {true, processed};
}

void WriteHandle::materialize() {
    while (!materialize_step(0).ready) {
    }
}

WritePreparation WriteHandle::prepare_write(uint64_t offset, uint64_t byte_budget) {
    std::lock_guard lock(m_);
    if (!materializing_ && ((sequential_ && offset == logical_) || temp_ >= 0))
        return {true, 0};
    if (!materializing_ && temp_ < 0 && canonical_base()) {
        begin_sparse_overlay();
        return {true, 0};
    }
    return materialize_step(byte_budget);
}
size_t WriteHandle::write(uint64_t off, std::span<const uint8_t> d) {
    const auto operation_started = Clock::now();
    std::unique_lock g(m_);
    const auto lock_wait =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - operation_started);
    if (d.empty())
        return 0;

    const bool diagnostics = Log::enabled(LogLevel::all);
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
        if (Log::enabled(LogLevel::all))
            Log::trace("WRITE ingress id=" + std::to_string(diagnostic_id_) +
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
            if (Log::enabled(LogLevel::all))
                Log::trace("WRITE overlap id=" + std::to_string(diagnostic_id_) +
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
        if (diagnostic_writes_.size() >= diagnostic_write_limit_) {
            const auto& victim = diagnostic_writes_.front();
            auto exact = diagnostic_exact_writes_.find({victim.offset, victim.length});
            if (exact != diagnostic_exact_writes_.end() &&
                exact->second.first == victim.sequence)
                diagnostic_exact_writes_.erase(exact);
            diagnostic_writes_.pop_front();
        }
        diagnostic_exact_writes_[key] = {sequence, input_hash};
        diagnostic_writes_.push_back({sequence, off, d.size(), input_hash});
    }

    std::chrono::milliseconds extent_put_time{};
    if (sequential_ && off == logical_) {
        prepare_append_tail();
        size_t p = 0;
        while (p < d.size()) {
            ensure_buffer_memory();
            size_t n = std::min(fs_.extent_size() - buffer_.size(), d.size() - p);
            buffer_.insert(buffer_.end(), d.begin() + p, d.begin() + p + n);
            p += n;
            logical_ += n;
            if (buffer_.size() == fs_.extent_size())
                extent_put_time += flush();
        }
        if (diagnostics) {
            if (Log::enabled(LogLevel::all))
                Log::trace("WRITE accepted id=" + std::to_string(diagnostic_id_) +
                       " seq=" + std::to_string(sequence) +
                       " mode=sequential logical_after=" + std::to_string(logical_) +
                       " staged_after=" + std::to_string(staged_) +
                       " buffer_after=" + std::to_string(buffer_.size()));
        }
    } else {
        if (materializing_)
            materialize();
        if (rebuilding_)
            rebuild();
        if (temp_ < 0) {
            if (Log::enabled(LogLevel::all))
                Log::trace("WRITE nonsequential id=" + std::to_string(diagnostic_id_) +
                       " seq=" + std::to_string(sequence) +
                       " requested_offset=" + std::to_string(off) +
                       " logical=" + std::to_string(logical_) +
                       " bytes=" + std::to_string(d.size()) + " materializing=1");
            auto started = Clock::now();
            if (canonical_base())
                begin_sparse_overlay();
            else
                materialize();
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
            if (Log::enabled(LogLevel::all))
                Log::trace("WRITE materialized id=" + std::to_string(diagnostic_id_) +
                       " seq=" + std::to_string(sequence) +
                       " ms=" + std::to_string(elapsed.count()));
            sequential_ = false;
        }

        rebuild_prepared_ = false;

        pwa(temp_, d, off);
        if (sparse_overlay_)
            note_changed_range(off, off + d.size());
        logical_ = std::max<uint64_t>(logical_, off + d.size());

        if (diagnostics) {
            try {
                Bytes readback(d.size());
                const auto got = pra(temp_, readback, off);
                if (got == d.size()) {
                    const auto readback_hash = sha256(readback);
                    if (Log::enabled(LogLevel::all))
                        Log::trace("WRITE stage-write id=" + std::to_string(diagnostic_id_) +
                               " seq=" + std::to_string(sequence) +
                               " offset=" + std::to_string(off) +
                               " length=" + std::to_string(d.size()) +
                               " input_sha256=" + to_string(input_hash) +
                               " readback_sha256=" + to_string(readback_hash) +
                               " all_zero=" + std::to_string(all_zero(readback) ? 1 : 0) +
                               " verify=" + (readback_hash == input_hash ? "OK" : "MISMATCH"));
                } else {
                    if (Log::enabled(LogLevel::all))
                        Log::trace("WRITE stage-write id=" + std::to_string(diagnostic_id_) +
                               " seq=" + std::to_string(sequence) +
                               " offset=" + std::to_string(off) +
                               " length=" + std::to_string(d.size()) +
                               " read=" + std::to_string(got) + " verify=SHORT");
                }
            } catch (const std::exception& error) {
                if (Log::enabled(LogLevel::all))
                    Log::trace("WRITE stage-write id=" + std::to_string(diagnostic_id_) +
                           " seq=" + std::to_string(sequence) +
                           " verify=DIAGNOSTIC_ERROR message=" + error.what());
            }

            const auto full_extents = static_cast<size_t>(logical_ / fs_.extent_size());
            while (diagnostic_completed_extents_ < full_extents) {
                const auto index = diagnostic_completed_extents_++;
                const auto offset = static_cast<uint64_t>(index) * fs_.extent_size();
                diagnostic_stage_extent("extent-complete", index, offset, fs_.extent_size());
            }
            if (Log::enabled(LogLevel::all))
                Log::trace("WRITE accepted id=" + std::to_string(diagnostic_id_) +
                       " seq=" + std::to_string(sequence) +
                       " mode=staged logical_after=" + std::to_string(logical_) +
                       " physical_after=" + std::to_string(fd_size(temp_)));
        }
    }
    dirty_ = true;
    if (work_context_.frame_type() == FrameType::foreground)
        fs_.store().foreground_activity(d.size());
    else if (work_context_.frame_type() == FrameType::read_ahead)
        fs_.store().interactive_activity(d.size());
    const auto total =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - operation_started);
    if (total >= std::chrono::milliseconds(500) && Log::enabled(LogLevel::debug)) {
        Log::debug("write stage id=" + std::to_string(diagnostic_id_) +
                   " path=" + path_ +
                   " stage=write bytes=" + std::to_string(d.size()) +
                   " offset=" + std::to_string(off) +
                   " lock_wait_ms=" + std::to_string(lock_wait.count()) +
                   " extent_put_ms=" + std::to_string(extent_put_time.count()) +
                   " total_ms=" + std::to_string(total.count()));
    }
    return d.size();
}
void WriteHandle::truncate(uint64_t z) {
    std::lock_guard g(m_);
    (void)drain_staging_locked();
    const bool diagnostics = Log::enabled(LogLevel::all);
    if (diagnostics) {
        Log::trace("WRITE truncate-begin id=" + std::to_string(diagnostic_id_) +
                   " requested=" + std::to_string(z) +
                   " logical_before=" + std::to_string(logical_) +
                   " staged_before=" + std::to_string(staged_) +
                   " buffer_before=" + std::to_string(buffer_.size()) +
                   " sequential=" + std::to_string(sequential_ ? 1 : 0) +
                   " temp_open=" + std::to_string(temp_ >= 0 ? 1 : 0) +
                   " temp_size=" + std::to_string(fd_size(temp_)));
    }
    if (z == logical_) {
        if (Log::enabled(LogLevel::all))
            Log::trace("WRITE truncate-end id=" + std::to_string(diagnostic_id_) +
                   " result=NOOP logical_after=" + std::to_string(logical_));
        return;
    }
    if (z == 0 && sequential_ && temp_ < 0) {
        extents_.clear();
        buffer_.clear();
        append_tail_.reset();
        staged_ = logical_ = 0;
        dirty_ = true;
        if (Log::enabled(LogLevel::all))
            Log::trace("WRITE truncate-end id=" + std::to_string(diagnostic_id_) +
                   " result=SEQUENTIAL_RESET logical_after=0 staged_after=0");
        return;
    }
    if (rebuilding_)
        rebuild();
    if (temp_ < 0) {
        if (canonical_base())
            begin_sparse_overlay();
        else {
            materialize();
            sequential_ = false;
        }
    }
    rebuild_prepared_ = false;
    if (sparse_overlay_ && z < logical_)
        note_changed_range(z, logical_);
    if (ftruncate(temp_, z))
        fail(EIO, "truncate failed");
    logical_ = z;
    dirty_ = true;
    if (diagnostics)
        diagnostic_stage_checkpoint("post-truncate");
    if (Log::enabled(LogLevel::all))
        Log::trace("WRITE truncate-end id=" + std::to_string(diagnostic_id_) +
               " result=OK logical_after=" + std::to_string(logical_) +
               " physical_after=" + std::to_string(fd_size(temp_)));
}
WritePreparation WriteHandle::rebuild_step(uint64_t byte_budget) {
    if (rebuild_prepared_ || temp_ < 0)
        return {true, 0};
    if (materializing_)
        fail(EINVAL, "cannot rebuild before materialization completes");

    if (!rebuilding_) {
        rebuilding_ = true;
        rebuild_offset_ = 0;
        rebuild_index_ = 0;
        rebuild_handle_extents_ = std::move(extents_);
        extents_.clear();
        staged_ = 0;
        if (Log::enabled(LogLevel::all)) {
            Log::trace("WRITE rebuild-begin id=" + std::to_string(diagnostic_id_) +
                   " logical=" + std::to_string(logical_) +
                   " physical=" + std::to_string(fd_size(temp_)));
            // Preserve the exhaustive diagnostic for synchronous callers. A
            // bounded loader step emits per-extent diagnostics below instead
            // of hiding a second whole-file scan inside tracing.
            if (!byte_budget)
                diagnostic_stage_checkpoint("pre-rebuild");
        }
    }

    const bool unlimited = byte_budget == 0;
    if (!unlimited && byte_budget < fs_.extent_size())
        fail(EINVAL, "rebuild budget must be zero or at least one extent");
    uint64_t processed = 0;
    const auto candidate_at = [](const std::vector<ExtentRef>& candidates,
                                 uint64_t offset, uint64_t length)
        -> const ExtentRef* {
        auto found = std::lower_bound(candidates.begin(), candidates.end(), offset,
                                      [](const ExtentRef& extent, uint64_t value) {
                                          return extent.offset < value;
                                      });
        if (found == candidates.end() || found->offset != offset || found->length != length)
            return nullptr;
        return &*found;
    };

    Bytes bytes(fs_.extent_size());
    uint64_t source_bytes = 0;
    while (rebuild_offset_ < logical_ && (unlimited || processed < byte_budget)) {
        if (fs_.io_cancellation_requested())
            fail(EINTR, "write cancelled");
        const auto remaining_budget = unlimited
                                          ? std::numeric_limits<uint64_t>::max()
                                          : byte_budget - processed;
        const auto n = static_cast<size_t>(
            std::min<uint64_t>(bytes.size(), logical_ - rebuild_offset_));
        if (!n || n > remaining_budget)
            break;
        // Handle-local immutable extents override the committed base, matching
        // the previous map insertion order without constructing an O(file)
        // lookup table before the first yield point.
        const ExtentRef* candidate =
            candidate_at(rebuild_handle_extents_, rebuild_offset_, n);
        if (!candidate)
            candidate = candidate_at(base_.extents, rebuild_offset_, n);
        const bool changed = sparse_overlay_ &&
                             range_changed(rebuild_offset_, rebuild_offset_ + n);
        bool have_data = !sparse_overlay_ || changed || !candidate;
        ObjectId id{};
        bool zero_data = false;
        if (have_data) {
            std::fill_n(bytes.data(), n, 0);
            if (sparse_overlay_) {
                const ExtentRef* source_extent = candidate;
                if (!source_extent) {
                    auto base_extent = std::lower_bound(
                        base_.extents.begin(), base_.extents.end(), rebuild_offset_,
                        [](const ExtentRef& extent, uint64_t value) {
                            return extent.offset < value;
                        });
                    if (base_extent != base_.extents.end() &&
                        base_extent->offset == rebuild_offset_)
                        source_extent = &*base_extent;
                }
                if (rebuild_offset_ < base_.size && !source_extent)
                        fail(EIO, "canonical base extent missing during sparse rebuild");
                if (source_extent) {
                    const auto copy = static_cast<size_t>(
                        std::min<uint64_t>(n, source_extent->length));
                    if (!source_extent->hole) {
                        auto source = fs_.store().get(
                            source_extent->id, rebuild_index_, work_context_.frame_type(), {},
                            &fs_.io_cancelled_);
                        if (!source || source->size() != source_extent->length ||
                            source->size() < copy)
                            fail(EIO, "cannot read changed source extent");
                        std::copy_n(source->data(), copy, bytes.data());
                        source_bytes += source->size();
                    }
                }
                for (const auto& range : changed_ranges_) {
                    const auto begin = std::max(range.begin, rebuild_offset_);
                    const auto end = std::min(range.end, rebuild_offset_ + n);
                    if (begin >= end)
                        continue;
                    const auto length = static_cast<size_t>(end - begin);
                    auto destination = std::span<uint8_t>{
                        bytes.data() + static_cast<size_t>(begin - rebuild_offset_), length};
                    if (pra(temp_, destination, begin) != length)
                        fail(EIO, "short sparse overlay read");
                    source_bytes += length;
                }
            } else {
                if (pra(temp_, {bytes.data(), n}, rebuild_offset_) != n)
                    fail(EIO, "short staging read");
                source_bytes += n;
            }
            const auto data = std::span<const uint8_t>{bytes.data(), n};
            id = object_id(data);
            zero_data = all_zero(data);
        } else if (candidate && !candidate->hole) {
            id = candidate->id;
        }
        const bool reused = candidate &&
                            (!have_data || (!candidate->hole && candidate->id == id) ||
                             (candidate->hole && zero_data));
        ExtentRef result;
        std::chrono::milliseconds elapsed{};
        if (reused) {
            result = *candidate;
            ++rebuild_reused_extents_;
        } else {
            const auto data = std::span<const uint8_t>{bytes.data(), n};
            const auto started = Clock::now();
            const bool ok = durability_ == WriteDurability::publication_generation
                                ? fs_.store().put_deferred(id, data, durability_batch_,
                                                           work_context_.frame_type(),
                                                           &fs_.io_cancelled_)
                                : fs_.store().put(id, data, work_context_.frame_type(),
                                                  &fs_.io_cancelled_);
            elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
            if (!ok) {
                if (fs_.io_cancellation_requested())
                    fail(EINTR, "write cancelled");
                fail(EIO, "object replication quorum unavailable");
            }
            if (cache_puts_)
                (void)fs_.store().cache_local(id, data);
            result = {rebuild_offset_, n, id, false};
            ++rebuild_put_extents_;
            if (elapsed >= std::chrono::milliseconds(500) && Log::enabled(LogLevel::debug))
                Log::debug("write stage id=" + std::to_string(diagnostic_id_) +
                           " path=" + path_ +
                           " stage=rebuild-extent-put index=" +
                           std::to_string(rebuild_index_) +
                           " offset=" + std::to_string(rebuild_offset_) +
                           " bytes=" + std::to_string(n) +
                           " object=" + to_string(id) +
                           " elapsed_ms=" + std::to_string(elapsed.count()));
        }

        if (Log::enabled(LogLevel::all) && have_data)
            Log::trace("WRITE rebuild-extent id=" + std::to_string(diagnostic_id_) +
                   " index=" + std::to_string(rebuild_index_) +
                   " offset=" + std::to_string(rebuild_offset_) +
                   " length=" + std::to_string(n) +
                   " object_id=" + to_string(id) +
                   " reused=" + std::to_string(reused ? 1 : 0) +
                   " all_zero=" + std::to_string(zero_data ? 1 : 0) +
                   " first16=" + edge_hex(std::span<const uint8_t>{bytes.data(), n}, true) +
                   " last16=" + edge_hex(std::span<const uint8_t>{bytes.data(), n}, false) +
                   " ms=" + std::to_string(elapsed.count()));
        extents_.push_back(result);
        rebuild_offset_ += n;
        processed += n;
        ++rebuild_index_;
    }

    rebuild_source_bytes_ += source_bytes;
    ++rebuild_steps_;
    if (rebuild_offset_ < logical_)
        return {false, processed};

    staged_ = logical_;
    buffer_.clear();
    append_tail_.reset();
    rebuild_handle_extents_.clear();
    rebuilding_ = false;
    rebuild_prepared_ = true;
    if (Log::enabled(LogLevel::all))
        Log::trace("WRITE rebuild-end id=" + std::to_string(diagnostic_id_) +
               " extents=" + std::to_string(extents_.size()) +
               " reused=" + std::to_string(rebuild_reused_extents_) +
               " put=" + std::to_string(rebuild_put_extents_));
    return {true, processed};
}

void WriteHandle::rebuild() {
    while (!rebuild_step(0).ready) {
    }
}

WritePreparation WriteHandle::prepare_commit(uint64_t byte_budget) {
    std::lock_guard lock(m_);
    if (!dirty_)
        return {true, 0};
    if (materializing_) {
        auto preparation = materialize_step(byte_budget);
        // Do not combine materialisation and rebuild under one opaque call. The
        // scheduler sees the exact completed unit before admitting another.
        if (preparation.bytes_processed || !preparation.ready)
            return {false, preparation.bytes_processed};
    }
    if (temp_ >= 0)
        return rebuild_step(byte_budget);
    (void)flush();
    (void)drain_staging_locked();
    return {true, 0};
}
void WriteHandle::commit() {
    const auto operation_started = Clock::now();
    std::unique_lock g(m_);
    const auto lock_wait =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - operation_started);
    if (!dirty_) {
        if (lock_wait >= std::chrono::milliseconds(500) && Log::enabled(LogLevel::debug)) {
            Log::debug("write stage id=" + std::to_string(diagnostic_id_) +
                       " path=" + path_ +
                       " stage=commit-clean lock_wait_ms=" +
                       std::to_string(lock_wait.count()));
        }
        if (Log::enabled(LogLevel::all))
            Log::trace("WRITE commit-clean id=" + std::to_string(diagnostic_id_));
        return;
    }
    auto commit_started = Clock::now();
    if (Log::enabled(LogLevel::all))
        Log::trace("WRITE commit-begin id=" + std::to_string(diagnostic_id_) +
               " logical=" + std::to_string(logical_) +
               " staged=" + std::to_string(staged_) +
               " buffer=" + std::to_string(buffer_.size()) +
               " extents=" + std::to_string(extents_.size()) +
               " temp=" + std::to_string(temp_ >= 0 ? 1 : 0) +
               " base_version=" + std::to_string(base_.version));
    const auto data_started = Clock::now();
    if (temp_ >= 0)
        rebuild();
    else {
        (void)flush();
        (void)drain_staging_locked();
    }
    const auto data_time =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - data_started);
    for (size_t i = 0; i < extents_.size(); ++i) {
        const auto& x = extents_[i];
        if (Log::enabled(LogLevel::all))
            Log::trace("WRITE manifest id=" + std::to_string(diagnostic_id_) +
                   " index=" + std::to_string(i) +
                   " offset=" + std::to_string(x.offset) +
                   " length=" + std::to_string(x.length) +
                   " id=" + to_string(x.id));
    }
    if (durability_ == WriteDurability::publication_generation && !durability_batch_.empty()) {
        const auto durability_started = Clock::now();
        std::vector<ObjectId> unsatisfiable;
        if (!fs_.store().durability_barrier(durability_batch_, work_context_.frame_type(),
                                            &unsatisfiable)) {
            // The barrier re-derives dead placement tokens itself; what is
            // left here is an object a peer genuinely no longer holds. Re-put
            // it from a local copy when there is one, else tell the caller the
            // generation must be replayed from its own WAL (ESTALE): this
            // writer cannot recover the bytes. Never retry the same batch.
            bool reput_all = !unsatisfiable.empty();
            for (const auto& id : unsatisfiable) {
                const auto data = fs_.node().local_store().get(id);
                if (!data) {
                    reput_all = false;
                    break;
                }
                std::erase_if(durability_batch_.requirements,
                              [&](const auto& requirement) { return requirement.id == id; });
                if (!fs_.store().put_deferred(id, *data, durability_batch_,
                                              work_context_.frame_type(), &fs_.io_cancelled_)) {
                    reput_all = false;
                    break;
                }
            }
            if (!reput_all)
                fail(unsatisfiable.empty() ? EIO : ESTALE,
                     unsatisfiable.empty()
                         ? "object durability quorum unavailable before publication"
                         : "object durability lost on a peer and no local copy; "
                           "publication must be replayed");
            Log::info("write stage id=" + std::to_string(diagnostic_id_) + " path=" + path_ +
                      " re-put " + std::to_string(unsatisfiable.size()) +
                      " extent(s) a peer no longer held");
            unsatisfiable.clear();
            if (!fs_.store().durability_barrier(durability_batch_, work_context_.frame_type(),
                                                &unsatisfiable))
                fail(EIO, "object durability quorum unavailable after re-put");
        }
        durability_batch_.clear();
        const auto durability_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - durability_started);
        if (durability_elapsed >= std::chrono::milliseconds(500) && Log::enabled(LogLevel::debug))
            Log::debug("write stage id=" + std::to_string(diagnostic_id_) +
                       " path=" + path_ + " stage=durability-barrier ms=" +
                       std::to_string(durability_elapsed.count()));
    }

    FsEntry committed;
    const auto metadata_started = Clock::now();
    fs_.commit_write(*this, base_, logical_, extents_, &committed, committed_mtime_);
    const auto metadata_time =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - metadata_started);
    base_ = std::move(committed);
    expected_ = base_.version;
    dirty_ = false;
    if (buffer_.empty()) {
        Bytes{}.swap(buffer_);
        buffer_memory_.reset();
    }
    // A committed generation is the other way publication-owned memory comes
    // back: this handle is now retirable and its remaining leases go with it.
    fs_.note_write_progress();

    if (sparse_overlay_) {
        // The committed manifest is now the immutable authority. Discard the
        // process-local overlay and make a still-open handle append-capable
        // again, just like a freshly opened handle on this generation.
        cleanup();
        temp_path_.clear();
        sparse_overlay_ = false;
        changed_ranges_.clear();
        rebuild_prepared_ = false;
        sequential_ = true;
        staged_ = logical_;
    }

    // flush() publishes a partial final extent at commit time.  The handle may
    // remain open and receive more append writes after flush/fsync, so re-arm
    // that committed tail for the same one-extent lazy seed used at open.
    if (sequential_ && temp_ < 0) {
        append_tail_.reset();
        staged_ = logical_;
        const auto tail_length = static_cast<size_t>(logical_ % fs_.extent_size());
        if (tail_length && !extents_.empty()) {
            const auto tail_offset = logical_ - tail_length;
            const auto& tail = extents_.back();
            if (tail.offset == tail_offset && tail.length == tail_length) {
                append_tail_ = tail;
                extents_.pop_back();
                staged_ = tail_offset;
            }
        }
    }
    auto commit_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - commit_started);
    const auto total =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - operation_started);
    if (total >= std::chrono::milliseconds(500) && Log::enabled(LogLevel::debug)) {
        Log::debug("write stage id=" + std::to_string(diagnostic_id_) +
                   " path=" + path_ +
                   " stage=commit logical_bytes=" + std::to_string(logical_) +
                   " extents=" + std::to_string(extents_.size()) +
                   " lock_wait_ms=" + std::to_string(lock_wait.count()) +
                   " data_ms=" + std::to_string(data_time.count()) +
                   " metadata_ms=" + std::to_string(metadata_time.count()) +
                   " total_ms=" + std::to_string(total.count()));
    }
    if (Log::enabled(LogLevel::all))
        Log::trace("WRITE commit-end id=" + std::to_string(diagnostic_id_) +
               " committed_size=" + std::to_string(base_.size) +
               " committed_version=" + std::to_string(base_.version) +
               " ms=" + std::to_string(commit_elapsed.count()));
}

WriteHandleDiagnostics WriteHandle::diagnostics() const {
    std::lock_guard g(m_);
    return {diagnostic_id_, logical_, staged_, buffer_.size(), sequential_, temp_ >= 0,
            fd_size(temp_), append_tail_fetches_, materialize_source_reads_, new_extent_puts_,
            rebuild_reused_extents_, rebuild_put_extents_, pending_extents_.size(),
            peak_pending_extents_, work_context_.frame_type(), work_context_.quantum_bytes(),
            materialize_source_bytes_, materialize_steps_, rebuild_source_bytes_,
            rebuild_steps_};
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
    : n_(n), s_(s), m_(m), playback_(playback) {
    extent_worker_limit_ = std::max<size_t>(1, n_.config().fuse.commit_workers);
    extent_task_limit_ = extent_worker_limit_ * 2;
    extent_workers_.reserve(extent_worker_limit_);
}

void FileSystem::extent_worker(std::stop_token stop) {
    while (true) {
        std::shared_ptr<ExtentTask> task;
        {
            std::unique_lock lock(extent_tasks_mutex_);
            extent_tasks_cv_.wait(lock, stop,
                                  [&] { return !extent_tasks_.empty(); });
            if (extent_tasks_.empty()) {
                if (stop.stop_requested())
                    return;
                continue;
            }
            task = std::move(extent_tasks_.front());
            extent_tasks_.pop_front();
        }
        extent_tasks_cv_.notify_all();
        const auto active = extent_tasks_active_.fetch_add(1, std::memory_order_relaxed) + 1;
        auto peak = extent_tasks_peak_active_.load(std::memory_order_relaxed);
        while (peak < active && !extent_tasks_peak_active_.compare_exchange_weak(
                                    peak, active, std::memory_order_relaxed)) {
        }
        (*task)();
        extent_tasks_active_.fetch_sub(1, std::memory_order_relaxed);
    }
}

std::future<WriteHandle::StagedExtentResult> FileSystem::submit_extent_task(
    std::function<WriteHandle::StagedExtentResult()> fn) {
    auto task = std::make_shared<ExtentTask>(std::move(fn));
    auto result = task->get_future();
    {
        std::unique_lock lock(extent_tasks_mutex_);
        if (extent_workers_.empty()) {
            for (size_t i = 0; i < extent_worker_limit_; ++i)
                extent_workers_.emplace_back(
                    [this](std::stop_token stop) { extent_worker(stop); });
        }
        extent_tasks_cv_.wait(lock, [&] {
            return io_cancellation_requested() || extent_tasks_.size() < extent_task_limit_;
        });
        if (io_cancellation_requested())
            throw std::runtime_error("extent publication cancelled");
        extent_tasks_.push_back(std::move(task));
        extent_tasks_submitted_.fetch_add(1, std::memory_order_relaxed);
        const auto queued = extent_tasks_.size();
        auto peak = extent_tasks_peak_queued_.load(std::memory_order_relaxed);
        while (peak < queued && !extent_tasks_peak_queued_.compare_exchange_weak(
                                    peak, queued, std::memory_order_relaxed)) {
        }
    }
    extent_tasks_cv_.notify_one();
    return result;
}

ExtentExecutorDiagnostics FileSystem::extent_executor_diagnostics() const {
    uint64_t queued = 0;
    uint64_t workers = 0;
    {
        std::lock_guard lock(extent_tasks_mutex_);
        queued = extent_tasks_.size();
        workers = extent_workers_.size();
    }
    return {workers, queued,
            extent_tasks_active_.load(std::memory_order_relaxed),
            extent_tasks_peak_queued_.load(std::memory_order_relaxed),
            extent_tasks_peak_active_.load(std::memory_order_relaxed),
            extent_tasks_submitted_.load(std::memory_order_relaxed)};
}
MetadataSnapshot FileSystem::snap() {
    return m_.snapshot();
}

std::shared_ptr<const FileSystem::NamespaceIndex> FileSystem::namespace_index() {
    auto view = m_.snapshot_view();
    {
        std::lock_guard lock(namespace_index_mutex_);
        if (namespace_index_ && namespace_index_->generation == view.generation &&
            namespace_index_->hash == view.hash)
            return namespace_index_;
    }

    auto built = std::make_shared<NamespaceIndex>();
    built->generation = view.generation;
    built->hash = view.hash;
    built->snapshot = std::move(view.snapshot);
    for (const auto& [path, entry] : built->snapshot->entries) {
        const auto canonical = macos_fuse_composed_name(path);
        auto [canonical_it, inserted] = built->canonical_paths.emplace(canonical, path);
        if (!inserted && canonical_it->second != path) {
            built->ambiguous_canonical_paths.insert(canonical);
            if (Log::enabled(LogLevel::debug))
                Log::debug("namespace contains canonically-equivalent duplicate paths canonical=" +
                           canonical + " first=" + canonical_it->second + " second=" + path);
        }
        if (path == "/")
            continue;
        built->children[parent_path(path)].push_back({base_name(path), path});
    }

    std::lock_guard lock(namespace_index_mutex_);
    if (!namespace_index_ || namespace_index_->generation < built->generation ||
        (namespace_index_->generation == built->generation && namespace_index_->hash != built->hash))
        namespace_index_ = built;
    return namespace_index_;
}

std::optional<std::string> FileSystem::resolve_existing_path(const std::string& p) {
    const auto q = normalize_path(p);
    auto index = namespace_index();
    if (index->snapshot->entries.contains(q))
        return q;

    const auto canonical = macos_fuse_composed_name(q);
    if (index->ambiguous_canonical_paths.contains(canonical))
        return {};
    auto alias = index->canonical_paths.find(canonical);
    if (alias == index->canonical_paths.end())
        return {};
    return alias->second;
}

std::string FileSystem::resolve_new_path(const std::string& p) {
    const auto q = normalize_path(p);
    if (auto existing = resolve_existing_path(q))
        return *existing;
    if (q == "/")
        return q;

    auto index = namespace_index();
    const auto canonical = macos_fuse_composed_name(q);
    if (index->ambiguous_canonical_paths.contains(canonical))
        fail(EEXIST, "canonically equivalent name is ambiguous");

    const auto requested_parent = parent_path(q);
    const auto actual_parent = resolve_existing_path(requested_parent).value_or(requested_parent);
    const auto leaf = macos_fuse_composed_name(base_name(q));
    return actual_parent == "/" ? "/" + leaf : actual_parent + "/" + leaf;
}

void FileSystem::require_parent(const MetadataSnapshot& s, const std::string& p) {
    auto i = s.entries.find(parent_path(p));
    if (i == s.entries.end())
        fail(ENOENT, "parent missing");
    if (i->second.type != EntryType::directory)
        fail(ENOTDIR, "parent not directory");
}
FsEntry FileSystem::getattr(const std::string& p) {
    auto index = namespace_index();
    auto resolved = resolve_existing_path(p);
    if (!resolved)
        fail(ENOENT, "not found");
    auto i = index->snapshot->entries.find(*resolved);
    if (i == index->snapshot->entries.end())
        fail(ENOENT, "not found");
    return i->second;
}
std::vector<std::pair<std::string, FsEntry>> FileSystem::readdir(const std::string& p) {
    auto resolved = resolve_existing_path(p);
    if (!resolved)
        fail(ENOENT, "not found");
    auto q = *resolved;
    auto index = namespace_index();
    auto entry = index->snapshot->entries.find(q);
    if (entry == index->snapshot->entries.end())
        fail(ENOENT, "not found");
    if (entry->second.type != EntryType::directory)
        fail(ENOTDIR, "not directory");
    auto children = index->children.find(q);
    if (children == index->children.end())
        return {};
    std::vector<std::pair<std::string, FsEntry>> result;
    result.reserve(children->second.size());
    for (const auto& [name, child_path] : children->second) {
        auto child = index->snapshot->entries.find(child_path);
        if (child != index->snapshot->entries.end())
            result.emplace_back(name, child->second);
    }
    return result;
}
void FileSystem::mkdir(const std::string& p, uint32_t mode, uint32_t uid, uint32_t gid) {
    auto q = resolve_new_path(p);
    const FilesystemNamespaceMutation op{
        FilesystemNamespaceMutation::Kind::mkdir, q, {}, false, mode, uid, gid};
    (void)apply_namespace_batch({&op, 1});
}

std::optional<FsEntry> FileSystem::apply_namespace_mutation(
    MetadataSnapshot& s, MetadataDelta& delta, const FilesystemNamespaceMutation& op) {
    const auto& q = op.from;
    switch (op.kind) {
    case FilesystemNamespaceMutation::Kind::mkdir: {
        require_parent(s, q);
        if (s.entries.contains(q))
            fail(EEXIST, "exists");
        FsEntry e;
        e.type = EntryType::directory;
        e.mode = op.mode & 07777;
        e.uid = op.uid;
        e.gid = op.gid;
        e.ctime_ns = e.mtime_ns = wall_time_ns();
        s.entries[q] = e;
        delta.upsert_entries[q] = e;
        return e;
    }
    case FilesystemNamespaceMutation::Kind::create: {
        require_parent(s, q);
        if (s.entries.contains(q))
            fail(EEXIST, "exists");
        FsEntry e;
        e.type = EntryType::file;
        e.mode = op.mode & 07777;
        e.uid = op.uid;
        e.gid = op.gid;
        e.ctime_ns = e.mtime_ns = wall_time_ns();
        s.entries[q] = e;
        delta.upsert_entries[q] = e;
        return e;
    }
    case FilesystemNamespaceMutation::Kind::rmdir: {
        if (q == "/")
            fail(EBUSY, "root");
        auto i = s.entries.find(q);
        if (i == s.entries.end())
            fail(ENOENT, "missing");
        if (i->second.type != EntryType::directory)
            fail(ENOTDIR, "not directory");
        // Namespace entries are ordered by path. Any child/subtree entry is
        // immediately after its directory, so emptiness is one indexed lookup
        // rather than a full namespace scan per recovered rmdir.
        const auto child = s.entries.upper_bound(q);
        if (child != s.entries.end() && under(child->first, q))
            fail(ENOTEMPTY, "not empty");
        s.entries.erase(i);
        delta.erase_entries.push_back(q);
        return {};
    }
    case FilesystemNamespaceMutation::Kind::unlink: {
        auto i = s.entries.find(q);
        if (i == s.entries.end())
            fail(ENOENT, "missing");
        if (i->second.type != EntryType::file)
            fail(EISDIR, "directory");
        std::vector<ObjectId> retiring;
        retiring.reserve(i->second.extents.size());
        for (const auto& extent : i->second.extents)
            if (!extent.hole)
                retiring.push_back(extent.id);
        queue_garbage_batch(s, std::move(retiring), delta);
        s.entries.erase(i);
        delta.erase_entries.push_back(q);
        return {};
    }
    case FilesystemNamespaceMutation::Kind::rename: {
        const auto& x = op.from;
        const auto& y = op.to;
        if (x == "/" || y == "/")
            fail(EBUSY, "root");
        if (x == y)
            return {};
        if (under(y, x))
            fail(EINVAL, "recursive rename");
        auto src = s.entries.find(x);
        if (src == s.entries.end())
            fail(ENOENT, "source missing");
        require_parent(s, y);
        auto dst = s.entries.find(y);
        if (dst != s.entries.end()) {
            if (op.noreplace)
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
                std::vector<ObjectId> retiring;
                retiring.reserve(dst->second.extents.size());
                for (const auto& extent : dst->second.extents)
                    if (!extent.hole)
                        retiring.push_back(extent.id);
                queue_garbage_batch(s, std::move(retiring), delta);
            }
            s.entries.erase(dst);
            delta.erase_entries.push_back(y);
        }
        std::vector<std::pair<std::string, FsEntry>> mv;
        for (auto i = s.entries.begin(); i != s.entries.end();) {
            if (under(i->first, x)) {
                const auto target = y + i->first.substr(x.size());
                delta.erase_entries.push_back(i->first);
                delta.upsert_entries[target] = i->second;
                mv.push_back({target, i->second});
                i = s.entries.erase(i);
            } else {
                ++i;
            }
        }
        for (auto& v : mv)
            s.entries.emplace(std::move(v));
        std::sort(delta.erase_entries.begin(), delta.erase_entries.end());
        delta.erase_entries.erase(
            std::unique(delta.erase_entries.begin(), delta.erase_entries.end()),
            delta.erase_entries.end());
        return {};
    }
    case FilesystemNamespaceMutation::Kind::chmod: {
        auto j = s.entries.find(q);
        if (j == s.entries.end())
            fail(ENOENT, "missing");
        auto& i = j->second;
        i.mode = op.mode & 07777;
        ++i.version;
        i.ctime_ns = wall_time_ns();
        delta.upsert_entries[q] = i;
        return {};
    }
    case FilesystemNamespaceMutation::Kind::chown: {
        auto i = s.entries.find(q);
        if (i == s.entries.end())
            fail(ENOENT, "missing");
        if (op.set_uid)
            i->second.uid = op.uid;
        if (op.set_gid)
            i->second.gid = op.gid;
        ++i->second.version;
        i->second.ctime_ns = wall_time_ns();
        delta.upsert_entries[q] = i->second;
        return {};
    }
    case FilesystemNamespaceMutation::Kind::utimens: {
        auto i = s.entries.find(q);
        if (i == s.entries.end())
            fail(ENOENT, "missing");
        i->second.mtime_ns = op.mtime_ns;
        i->second.ctime_ns = wall_time_ns();
        ++i->second.version;
        delta.upsert_entries[q] = i->second;
        return {};
    }
    }
    throw std::logic_error("unknown filesystem namespace mutation");
}

FilesystemNamespaceBatchResult FileSystem::apply_namespace_batch(
    std::span<const FilesystemNamespaceMutation> operations,
    std::optional<MetadataMutationIdentity> identity, bool atomic) {
    if (operations.empty())
        throw std::invalid_argument("filesystem namespace batch is empty");

    // Write handles are path-backed: after the transaction, every successful
    // rename re-points the open handles beneath it (under the registry lock,
    // which is not held across the mutation itself -- see open_write()).
    FilesystemNamespaceBatchResult result;
    result.entries.resize(operations.size());
    result.record = m_.mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
        result.applied = 0;
        result.failure_code.reset();
        result.failure_message.clear();
        std::fill(result.entries.begin(), result.entries.end(), std::nullopt);
        for (size_t i = 0; i < operations.size(); ++i) {
            try {
                result.entries[i] = apply_namespace_mutation(snapshot, delta, operations[i]);
                ++result.applied;
            } catch (const FsError& error) {
                // With no valid prefix there is nothing to publish. Preserve
                // the old error behaviour and leave the durable queue head in
                // place. Otherwise commit the largest valid prefix and report
                // the blocking operation to the caller -- unless the caller
                // asked for all-or-nothing.
                if (!result.applied || atomic)
                    throw;
                result.failure_code = error.code();
                result.failure_message = error.what();
                break;
            }
        }
        // Individual filesystem methods previously emitted at most one erase
        // (rename already canonicalised its subtree). A batch can accumulate
        // erases in syscall order, while the delta wire format deliberately
        // requires canonical sorted/unique paths.
        std::sort(delta.erase_entries.begin(), delta.erase_entries.end());
        delta.erase_entries.erase(
            std::unique(delta.erase_entries.begin(), delta.erase_entries.end()),
            delta.erase_entries.end());
    }, 8, identity);
    if (identity && !result.applied) {
        // mutate_delta() found the identity clock already at or past this
        // batch: an earlier attempt (or a peer's merge of it) committed the
        // whole batch. Report it as fully applied; entries are resolved from
        // the current snapshot by the caller if it needs them.
        result.applied = operations.size();
    }

    std::lock_guard handles(open_writes_mutex_);
    for (size_t op_index = 0; op_index < result.applied; ++op_index) {
        const auto& op = operations[op_index];
        if (op.kind != FilesystemNamespaceMutation::Kind::rename || op.from == op.to)
            continue;
        for (auto i = open_writes_.begin(); i != open_writes_.end();) {
            auto handle = i->lock();
            if (!handle) {
                i = open_writes_.erase(i);
                continue;
            }
            if (under(handle->path_, op.from)) {
                const auto before = handle->path_;
                handle->path_ = op.to + handle->path_.substr(op.from.size());
                if (Log::enabled(LogLevel::all))
                    Log::trace("WRITE rename id=" + std::to_string(handle->diagnostic_id_) +
                               " from=" + before + " to=" + handle->path_);
            }
            ++i;
        }
    }
    return result;
}

void FileSystem::rmdir(const std::string& p) {
    auto resolved = resolve_existing_path(p);
    if (!resolved)
        fail(ENOENT, "missing");
    FilesystemNamespaceMutation op;
    op.kind = FilesystemNamespaceMutation::Kind::rmdir;
    op.from = *resolved;
    (void)apply_namespace_batch({&op, 1});
}

FsEntry FileSystem::create_file(const std::string& p, uint32_t mode, uint32_t uid, uint32_t gid) {
    auto q = resolve_new_path(p);
    const FilesystemNamespaceMutation op{
        FilesystemNamespaceMutation::Kind::create, q, {}, false, mode, uid, gid};
    auto result = apply_namespace_batch({&op, 1});
    return *result.entries.front();
}

void FileSystem::unlink(const std::string& p) {
    auto resolved = resolve_existing_path(p);
    if (!resolved)
        fail(ENOENT, "missing");
    FilesystemNamespaceMutation op;
    op.kind = FilesystemNamespaceMutation::Kind::unlink;
    op.from = *resolved;
    (void)apply_namespace_batch({&op, 1});
}

void FileSystem::rename(const std::string& a, const std::string& b, bool noreplace) {
    auto source = resolve_existing_path(a);
    if (!source)
        fail(ENOENT, "source missing");
    auto target = resolve_new_path(b);
    if (*source == target)
        return;
    const FilesystemNamespaceMutation op{
        FilesystemNamespaceMutation::Kind::rename, *source, target, noreplace};
    (void)apply_namespace_batch({&op, 1});
}

void FileSystem::chmod(const std::string& p, uint32_t mode) {
    auto resolved = resolve_existing_path(p);
    if (!resolved)
        fail(ENOENT, "missing");
    const FilesystemNamespaceMutation op{
        FilesystemNamespaceMutation::Kind::chmod, *resolved, {}, false, mode};
    (void)apply_namespace_batch({&op, 1});
}

void FileSystem::chown(const std::string& p, uint32_t uid, uint32_t gid,
                       bool set_uid, bool set_gid) {
    auto resolved = resolve_existing_path(p);
    if (!resolved)
        fail(ENOENT, "missing");
    FilesystemNamespaceMutation op;
    op.kind = FilesystemNamespaceMutation::Kind::chown;
    op.from = *resolved;
    op.uid = uid;
    op.gid = gid;
    op.set_uid = set_uid;
    op.set_gid = set_gid;
    (void)apply_namespace_batch({&op, 1});
}

void FileSystem::utimens(const std::string& p, int64_t mtime_ns) {
    auto resolved = resolve_existing_path(p);
    if (!resolved)
        fail(ENOENT, "missing");
    FilesystemNamespaceMutation op;
    op.kind = FilesystemNamespaceMutation::Kind::utimens;
    op.from = *resolved;
    op.mtime_ns = mtime_ns;
    (void)apply_namespace_batch({&op, 1});
}
void FileSystem::truncate_file(const std::string& p, uint64_t z) {
    auto resolved = resolve_existing_path(p);
    if (!resolved)
        fail(ENOENT, "missing");
    auto q = *resolved;
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
    // Media ids are content-addressed from the immutable file manifest. Once an
    // id has been resolved, unrelated namespace generations cannot make that
    // manifest point at different bytes. Serve cache hits from the snapshot that
    // built the index; only a miss consults current metadata. This keeps rsync's
    // mkdir/create/chmod generation churn out of playback negotiation.
    {
        std::lock_guard lock(media_index_mutex_);
        if (media_index_valid_ && media_index_snapshot_) {
            auto found = media_index_.find(std::string(id));
            if (found != media_index_.end()) {
                auto entry = media_index_snapshot_->entries.find(found->second);
                if (entry != media_index_snapshot_->entries.end() &&
                    entry->second.type == EntryType::file &&
                    file_media_id(entry->second) == id)
                    return std::pair{found->second, entry->second};
            }
        }
    }

    auto install_and_lookup = [&](const MetadataSnapshotView& view)
        -> std::optional<std::pair<std::string, FsEntry>> {
        std::lock_guard lock(media_index_mutex_);

        // Another lookup may have populated this id while the snapshot was
        // acquired. Prefer that immutable hit first.
        if (media_index_valid_ && media_index_snapshot_) {
            auto found = media_index_.find(std::string(id));
            if (found != media_index_.end()) {
                auto entry = media_index_snapshot_->entries.find(found->second);
                if (entry != media_index_snapshot_->entries.end() &&
                    entry->second.type == EntryType::file &&
                    file_media_id(entry->second) == id)
                    return std::pair{found->second, entry->second};
            }
            if (media_index_namespace_revision_ == view.namespace_revision)
                return {};
        }

        std::map<std::string, std::string> next;
        for (const auto& [path, entry] : view.snapshot->entries) {
            if (entry.type != EntryType::file)
                continue;
            next.emplace(file_media_id(entry), path);
        }
        media_index_ = std::move(next);
        media_index_namespace_revision_ = view.namespace_revision;
        media_index_snapshot_ = view.snapshot;
        media_index_valid_ = true;
        Log::debug("filesystem media index rebuilt namespace_revision=" +
                   std::to_string(view.namespace_revision) + " metadata_generation=" +
                   std::to_string(view.generation) + " files=" +
                   std::to_string(media_index_.size()) + " source=memory");

        auto found = media_index_.find(std::string(id));
        if (found == media_index_.end())
            return {};
        auto entry = media_index_snapshot_->entries.find(found->second);
        if (entry == media_index_snapshot_->entries.end() || entry->second.type != EntryType::file)
            return {};
        return std::pair{found->second, entry->second};
    };

    // Playback resolution is a data-plane operation. MetadataManager already
    // owns a decoded immutable view in normal settled operation; use it before
    // doing any quorum read. A hit is safe even if the view is slightly old
    // because media IDs are content-derived. A miss is definitive only when the
    // available view has caught up with every generation this node knows about.
    if (auto available = m_.available_snapshot_view()) {
        if (auto found = install_and_lookup(*available))
            return found;
        if (available->generation >= n_.known_metadata_generation())
            return {};
    }

    // Only a genuinely stale/missing decoded view may require authoritative
    // metadata I/O. This preserves correctness for a just-published media ID
    // without putting routine cold playback behind a multi-second quorum read.
    const auto authoritative = m_.snapshot_view();
    return install_and_lookup(authoritative);
}

std::shared_ptr<ReadHandle> FileSystem::open_read(const std::string& p) {
    auto resolved = resolve_existing_path(p);
    if (!resolved)
        fail(ENOENT, "missing");
    auto e = getattr(*resolved);
    if (e.type != EntryType::file)
        fail(EISDIR, "directory");
    return open_read(e, *resolved, false, FrameType::read_ahead);
}

std::shared_ptr<ReadHandle> FileSystem::open_read(const FsEntry& entry, const std::string& logical_path,
                                                  bool track_playback, FrameType frame_type) {
    if (entry.type != EntryType::file)
        fail(EISDIR, "directory");
    return std::make_shared<ReadHandle>(s_, entry, track_playback ? playback_ : nullptr,
                                        normalize_path(logical_path), frame_type);
}
std::shared_ptr<WriteHandle> FileSystem::open_write(const std::string& p, bool trunc,
                                                    bool cache_puts, WriteDurability durability,
                                                    uint64_t publication_pipeline_bytes,
                                                    DataWorkContext work_context) {
    // open_writes_mutex_ serializes path lookup/registration with rename's
    // handle fix-up so an opening writer cannot miss a rename between
    // resolving the entry and joining the registry. It is never held across
    // a metadata mutation (0.32.4): truncation is a cluster round trip and
    // ran under it, as did every commit, so all publications on a node were
    // serialized behind one WAN-bound commit at a time.
    if (trunc) {
        auto existing = resolve_existing_path(p);
        if (!existing)
            fail(ENOENT, "missing");
        auto current = getattr(*existing);
        if (current.type != EntryType::file)
            fail(EISDIR, "directory");
        if (current.size)
            truncate_file(*existing, 0);
    }
    std::lock_guard handles(open_writes_mutex_);
    auto resolved = resolve_existing_path(p);
    if (!resolved)
        fail(ENOENT, "missing");
    auto e = getattr(*resolved);
    if (e.type != EntryType::file)
        fail(EISDIR, "directory");
    auto handle =
        std::make_shared<WriteHandle>(*this, *resolved, e, trunc || !e.size, cache_puts,
                                      durability, publication_pipeline_bytes, work_context);
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
    const auto q = resolve_existing_path(p).value_or(normalize_path(p));
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
    const auto q = resolve_existing_path(p).value_or(normalize_path(p));
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
                              const std::vector<ExtentRef>& xs, FsEntry* out,
                              std::optional<int64_t> mtime_override) {
    // Snapshot the handle's current path under the registry lock and commit
    // outside it. A rename that lands between the two moves the entry away
    // from `path`: the mutation then finds no entry ("removed while open"),
    // the caller retries, and by then rename's fix-up has updated path_.
    std::string path;
    {
        std::lock_guard handles(open_writes_mutex_);
        path = handle.path_;
    }
    commit_file(path, expected, z, xs, out, mtime_override);
}
void FileSystem::commit_file(const std::string& p, const FsEntry& expected, uint64_t z,
                             const std::vector<ExtentRef>& xs, FsEntry* out,
                             std::optional<int64_t> mtime_override) {
    auto q = normalize_path(p);
    FsEntry committed;
    m_.mutate_delta([&](MetadataSnapshot& s, MetadataDelta& delta) {
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
        std::vector<ObjectId> retiring;
        retiring.reserve(i->second.extents.size());
        for (const auto& extent : i->second.extents) {
            if (!extent.hole && !retained.contains(extent.id))
                retiring.push_back(extent.id);
        }
        queue_garbage_batch(s, std::move(retiring), delta);

        const FsEntry previous = i->second;
        i->second.size = z;
        i->second.extents = xs;
        const auto now = wall_time_ns();
        if (mtime_override)
            i->second.mtime_ns = *mtime_override;
        else if (!explicit_mtime)
            i->second.mtime_ns = now;
        i->second.ctime_ns = now;
        ++i->second.version;
        committed = i->second;
        record_entry_change(delta, q, &previous, committed);
    });
    if (out)
        *out = std::move(committed);
}
MetadataSnapshot FileSystem::local_snapshot() const {
    const auto record = n_.metadata_replica().current();
    if (!valid_metadata_record(record))
        throw std::runtime_error("local metadata replica unavailable");
    return decode_snapshot(record.payload);
}

MetadataSnapshotView FileSystem::local_snapshot_view() {
    const auto record = n_.metadata_replica().current();
    if (!valid_metadata_record(record))
        throw std::runtime_error("local metadata replica unavailable");

    std::lock_guard lock(local_snapshot_mutex_);
    if (!local_snapshot_cache_ || local_snapshot_generation_ != record.generation ||
        local_snapshot_hash_ != record.hash) {
        local_snapshot_generation_ = record.generation;
        local_snapshot_hash_ = record.hash;
        if (auto materialized = n_.metadata_replica().materialized(record.hash);
            materialized && materialized->record.generation == record.generation &&
            materialized->record.payload == record.payload) {
            local_snapshot_cache_ = materialized->snapshot;
        } else {
            local_snapshot_cache_ =
                std::make_shared<const MetadataSnapshot>(decode_snapshot(record.payload));
        }
    }
    return MetadataSnapshotView{record.generation, 0, record.hash, local_snapshot_cache_};
}

std::optional<MetadataSnapshotView> FileSystem::available_snapshot_view() const {
    return m_.available_snapshot_view();
}

std::pair<uint64_t, uint64_t> FileSystem::logical_capacity() const {
    auto ns = n_.membership().active();
    // In the first seconds after a start the membership view can be empty
    // or carry peers whose capacity has not been exchanged yet; statfs then
    // answered 0 blocks and `df` showed a 0-byte filesystem (gbni-2,
    // 2026-09-07). Fall back to this node's own store so the mount never
    // reports less than what it can hold by itself.
    const auto local_fallback = [&]() -> std::pair<uint64_t, uint64_t> {
        const uint64_t limit = n_.local_store().limit();
        const uint64_t used = std::min(limit, n_.local_store().used());
        return {limit, used};
    };
    if (ns.empty())
        return local_fallback();
    const size_t r = std::max<size_t>(1, std::min(n_.config().replication, ns.size()));
    const uint64_t total = placement_logical_capacity(ns, r);
    if (!total)
        return local_fallback();

    __uint128_t physical_used = 0;
    for (const auto& node : ns)
        physical_used += node.used;
    uint64_t used = static_cast<uint64_t>(std::min<__uint128_t>(
        physical_used / r, std::numeric_limits<uint64_t>::max()));
    used = std::min(used, total);
    return {total, used};
}
std::vector<ObjectId> FileSystem::live_objects() {
    auto cached = maintenance_objects_cached();
    return {cached->live.begin(), cached->live.end()};
}

Hash256 FileSystem::namespace_signature(uint64_t* metadata_generation) {
    const auto view = m_.snapshot_view();
    if (metadata_generation) *metadata_generation = view.generation;
    return metadata_namespace_signature(*view.snapshot);
}

std::optional<Hash256> FileSystem::available_namespace_signature(
    uint64_t* metadata_generation) const {
    auto view = m_.available_snapshot_view();
    if (!view)
        return {};
    if (metadata_generation) *metadata_generation = view->generation;
    return metadata_namespace_signature(*view->snapshot);
}

std::shared_ptr<const MaintenanceObjects> FileSystem::maintenance_objects_cached() {
    const auto known_generation = n_.known_metadata_generation();
    {
        std::lock_guard lock(maintenance_index_mutex_);
        if (maintenance_index_ && maintenance_index_generation_ >= known_generation)
            return maintenance_index_;
    }

    auto view = m_.snapshot_view();
    const auto& snapshot = *view.snapshot;
    std::vector<ObjectId> live;
    size_t extents = 0;
    auto add_entry_extents = [&](const FsEntry& entry) {
        extents += entry.extents.size();
        for (const auto& extent : entry.extents) {
            if (!extent.hole)
                live.push_back(extent.id);
        }
    };
    for (const auto& [_, entry] : snapshot.entries)
        add_entry_extents(entry);

    // Unresolved conflict alternatives are reachability roots just as surely as
    // the effective namespace. Count their physical extents for diagnostics and
    // protect their immutable objects from GC until explicit resolution.
    for (const auto& [_, conflict] : snapshot.conflicts) {
        if (conflict.kind != MetadataConflictKind::namespace_entry)
            continue;
        for (const auto* candidate : {&conflict.base_entry, &conflict.left_entry,
                                      &conflict.right_entry}) {
            if (*candidate)
                extents += (**candidate).extents.size();
        }
    }
    const auto conflict_live = metadata_conflict_extent_roots(snapshot);
    live.insert(live.end(), conflict_live.begin(), conflict_live.end());
    std::sort(live.begin(), live.end());
    live.erase(std::unique(live.begin(), live.end()), live.end());

    // Preserve committed tombstone identity here. Service maintenance combines
    // filesystem and catalogue liveness before deciding whether a retirement is
    // a collection candidate; retaining retired_at_ns also makes pruning ABA-safe.
    auto garbage = snapshot.garbage;
    std::sort(garbage.begin(), garbage.end(), [](const GarbageRef& a, const GarbageRef& b) {
        if (a.id != b.id)
            return a.id < b.id;
        return a.retired_at_ns > b.retired_at_ns;
    });
    garbage.erase(std::unique(garbage.begin(), garbage.end(),
                              [](const GarbageRef& a, const GarbageRef& b) {
                                  return a.id == b.id;
                              }),
                  garbage.end());

    auto built = std::make_shared<MaintenanceObjects>();
    built->live = std::move(live);
    built->garbage = std::move(garbage);
    built->metadata_generation = view.generation;
    built->observed_mutations = snapshot.mutation_sequences;
    built->entries = snapshot.entries.size();
    built->extents = extents;

    std::lock_guard lock(maintenance_index_mutex_);
    if (!maintenance_index_ || view.generation >= maintenance_index_generation_) {
        maintenance_index_generation_ = view.generation;
        maintenance_index_ = built;
    }
    return maintenance_index_;
}

MaintenanceObjects FileSystem::maintenance_objects() {
    return *maintenance_objects_cached();
}
} // namespace macha
