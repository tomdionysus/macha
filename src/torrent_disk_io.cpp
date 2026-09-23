// SPDX-License-Identifier: GPL-3.0-or-later
#include "torrent_disk_io.hpp"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/disk_buffer_holder.hpp>
#include <libtorrent/disk_interface.hpp>
#include <libtorrent/disk_observer.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/io_context.hpp>
#include <libtorrent/operations.hpp>
#include <libtorrent/performance_counters.hpp>
#include <libtorrent/session_handle.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/storage_defs.hpp>
#include <libtorrent/version.hpp>

#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace macha {
namespace lt = libtorrent;

namespace {

// libtorrent 2.1 turned status_t from an enum into flag bits in
// lt::disk_status, with success as the empty set. The nodes run 2.0.11 and
// the developer's machine 2.1, so name the three outcomes once.
#if LIBTORRENT_VERSION_NUM >= 20100
const lt::status_t status_ok{};
const lt::status_t status_fatal = lt::disk_status::fatal_disk_error;
const lt::status_t status_full_check = lt::disk_status::need_full_check;
#else
const lt::status_t status_ok = lt::status_t::no_error;
const lt::status_t status_fatal = lt::status_t::fatal_disk_error;
const lt::status_t status_full_check = lt::status_t::need_full_check;
#endif

// 2.1 dropped the size argument: every disk buffer is one block.
lt::disk_buffer_holder hold(lt::buffer_allocator_interface& allocator, char* buffer) {
#if LIBTORRENT_VERSION_NUM >= 20100
    return lt::disk_buffer_holder(allocator, buffer);
#else
    return lt::disk_buffer_holder(allocator, buffer, lt::default_block_size);
#endif
}

// libtorrent's error_code is boost::system::error_code, which Boost 1.83 (the
// nodes') cannot construct from a std::error_code. Every error here is an
// errno, so carry the number.
lt::error_code errno_code(int value) {
    return lt::error_code(value, boost::system::generic_category());
}
lt::error_code errno_code(const std::error_code& ec) { return errno_code(ec.value()); }

// A cached descriptor per file, bounded: a discography is thousands of files
// and one torrent must not hold a descriptor for each.
constexpr size_t max_open_files_per_torrent = 64;

using Clock = std::chrono::steady_clock;

struct OpenFile {
    int fd{-1};
    bool writable{};
};

struct Storage {
    const lt::file_storage& files;
    std::string save_path;
    // Touched only by the job currently running for this storage, and jobs
    // for one storage never run concurrently (see MachaDiskIo::worker).
    std::map<lt::file_index_t, OpenFile> open;
    // Guarded by MachaDiskIo::mutex_.
    std::deque<std::function<void()>> jobs;
    bool scheduled{};

    Storage(const lt::file_storage& f, std::string path) : files(f), save_path(std::move(path)) {}
    ~Storage() { close_all(); }

    void close_all() {
        for (auto& [_, file] : open)
            if (file.fd >= 0) ::close(file.fd);
        open.clear();
    }
};

class MachaDiskIo final : public lt::disk_interface, public lt::buffer_allocator_interface {
  public:
    MachaDiskIo(lt::io_context& ios, const lt::settings_interface& settings, TorrentDiskHooks hooks)
        : ios_(ios), settings_(settings), hooks_(std::move(hooks)) {
        settings_updated();
        const auto threads = std::max<size_t>(1, hooks_.threads);
        workers_.reserve(threads);
        for (size_t i = 0; i < threads; ++i)
            workers_.emplace_back([this] { worker(); });
    }

    ~MachaDiskIo() override { shut_down(); }

    // buffer_allocator_interface: every buffer this backend hands libtorrent
    // came from new char[].
    void free_disk_buffer(char* buffer) override { delete[] buffer; }
#if LIBTORRENT_VERSION_NUM >= 20100
    void free_multiple_buffers(lt::span<char*> buffers) override {
        for (char* buffer : buffers) delete[] buffer;
    }
#endif

    void settings_updated() override {
        const auto limit = settings_.get_int(lt::settings_pack::max_queued_disk_bytes);
        queue_limit_.store(static_cast<uint64_t>(std::max(limit, lt::default_block_size)),
                           std::memory_order_relaxed);
    }

    lt::storage_holder new_torrent(const lt::storage_params& params,
                                   const std::shared_ptr<void>&) override {
        auto storage = std::make_shared<Storage>(params.files, std::string(params.path));
        int index;
        if (!free_slots_.empty()) {
            index = free_slots_.back();
            free_slots_.pop_back();
            storages_[static_cast<size_t>(index)] = std::move(storage);
        } else {
            index = static_cast<int>(storages_.size());
            storages_.push_back(std::move(storage));
        }
        return lt::storage_holder(lt::storage_index_t(static_cast<std::uint32_t>(index)), *this);
    }

    void remove_torrent(lt::storage_index_t index) override {
        // Queued jobs hold their own reference; the storage closes its files
        // when the last of them has run.
        storages_[slot(index)].reset();
        free_slots_.push_back(static_cast<int>(slot(index)));
    }

    void abort(bool) override { shut_down(); }

    void async_read(lt::storage_index_t index, const lt::peer_request& request,
                    std::function<void(lt::disk_buffer_holder, const lt::storage_error&)> handler,
                    lt::disk_job_flags_t) override {
        auto storage = at(index);
        enqueue(storage, [this, storage, request, handler = std::move(handler)] {
            auto admitted = admit(static_cast<uint64_t>(request.length));
            // Always a whole block: libtorrent 2.1 assumes it.
            char* buffer = new char[static_cast<size_t>(lt::default_block_size)];
            lt::storage_error error;
            if (!admitted && aborting_.load())
                error = aborted();
            else
                read(*storage, request.piece, request.start, buffer, request.length, error);
            auto holder = hold(*this, buffer);
            boost::asio::post(ios_, [handler, holder = std::move(holder), error]() mutable {
                handler(std::move(holder), error);
            });
        });
    }

    bool async_write(lt::storage_index_t index, const lt::peer_request& request, const char* data,
                     std::shared_ptr<lt::disk_observer> observer,
                     std::function<void(const lt::storage_error&)> handler,
                     lt::disk_job_flags_t) override {
        // `data` is valid only for the duration of this call.
        auto bytes = std::make_shared<std::vector<char>>(data, data + request.length);
        const auto size = static_cast<uint64_t>(request.length);
        const auto queued = queued_write_bytes_.fetch_add(size) + size;
        const bool exceeded = queued > queue_limit_.load(std::memory_order_relaxed);
        if (exceeded && observer) {
            std::lock_guard lock(mutex_);
            observers_.push_back(std::move(observer));
        }
        auto storage = at(index);
        enqueue(storage, [this, storage, request, bytes, size, handler = std::move(handler)] {
            auto admitted = admit(size);
            lt::storage_error error;
            if (!admitted && aborting_.load())
                error = aborted();
            else
                write(*storage, request.piece, request.start, bytes->data(), request.length, error);
            boost::asio::post(ios_, [handler, error] { handler(error); });
            write_drained(size);
        });
        // True tells libtorrent to stop reading from peers until on_disk():
        // the network waits for the disk, and the disk waits for admission.
        return exceeded;
    }

    void async_hash(lt::storage_index_t index, lt::piece_index_t piece,
                    lt::span<lt::sha256_hash> block_hashes, lt::disk_job_flags_t flags,
                    std::function<void(lt::piece_index_t, const lt::sha1_hash&,
                                       const lt::storage_error&)> handler) override {
        auto storage = at(index);
        enqueue(storage, [this, storage, piece, block_hashes, flags, handler = std::move(handler)] {
            const auto& files = storage->files;
            const bool v1 = bool(flags & lt::disk_interface::v1_hash);
            const bool v2 = !block_hashes.empty();
            const int piece_size = v1 ? files.piece_size(piece) : 0;
            const int piece_size2 = v2 ? files.piece_size2(piece) : 0;
            const int blocks = v1 ? (piece_size + lt::default_block_size - 1) / lt::default_block_size
                                  : 0;
            const int blocks2 = v2 ? files.blocks_in_piece2(piece) : 0;
            auto admitted = admit(static_cast<uint64_t>(std::max(piece_size, piece_size2)));
            lt::storage_error error;
            lt::hasher v1_hasher;
            if (!admitted && aborting_.load()) {
                error = aborted();
            } else {
                std::vector<char> block(static_cast<size_t>(lt::default_block_size));
                int offset = 0;
                for (int i = 0; i < std::max(blocks, blocks2); ++i) {
                    const bool v2_block = i < blocks2;
                    const int length = v1 ? std::min(lt::default_block_size, piece_size - offset) : 0;
                    const int length2 =
                        v2_block ? std::min(lt::default_block_size, piece_size2 - offset) : 0;
                    const int want = std::max(length, length2);
                    const int got = read(*storage, piece, offset, block.data(), want, error);
                    offset += lt::default_block_size;
                    if (got <= 0) break;
                    if (v1) v1_hasher.update({block.data(), std::min(got, length)});
                    if (v2_block)
                        block_hashes[i] =
                            lt::hasher256(lt::span<const char>{block.data(), std::min(got, length2)})
                                .final();
                }
            }
            const lt::sha1_hash hash = v1 ? v1_hasher.final() : lt::sha1_hash();
            boost::asio::post(ios_, [handler, piece, hash, error] { handler(piece, hash, error); });
        });
    }

    void async_hash2(lt::storage_index_t index, lt::piece_index_t piece, int offset,
                     lt::disk_job_flags_t,
                     std::function<void(lt::piece_index_t, const lt::sha256_hash&,
                                        const lt::storage_error&)> handler) override {
        auto storage = at(index);
        enqueue(storage, [this, storage, piece, offset, handler = std::move(handler)] {
            const int length =
                std::min(lt::default_block_size, storage->files.piece_size2(piece) - offset);
            auto admitted = admit(static_cast<uint64_t>(length));
            lt::storage_error error;
            lt::hasher256 hasher;
            if (!admitted && aborting_.load()) {
                error = aborted();
            } else {
                std::vector<char> block(static_cast<size_t>(length));
                const int got = read(*storage, piece, offset, block.data(), length, error);
                if (got > 0) hasher.update({block.data(), got});
            }
            const auto hash = hasher.final();
            boost::asio::post(ios_, [handler, piece, hash, error] { handler(piece, hash, error); });
        });
    }

    void async_move_storage(lt::storage_index_t, std::string path, lt::move_flags_t,
                            std::function<void(lt::status_t, const std::string&,
                                               const lt::storage_error&)> handler) override {
        // macha never moves a torrent: its save path is the job's staging
        // directory for the job's whole life.
        const lt::storage_error error(errno_code(EOPNOTSUPP), lt::operation_t::file_rename);
        boost::asio::post(ios_, [handler = std::move(handler), path = std::move(path), error] {
            handler(status_fatal, path, error);
        });
    }

    void async_release_files(lt::storage_index_t index, std::function<void()> handler) override {
        auto storage = at(index);
        enqueue(storage, [this, storage, handler = std::move(handler)] {
            storage->close_all();
            if (handler) boost::asio::post(ios_, handler);
        });
    }

    void async_check_files(lt::storage_index_t index, const lt::add_torrent_params* resume,
                           lt::aux::vector<std::string, lt::file_index_t>,
                           std::function<void(lt::status_t, const lt::storage_error&)> handler)
        override {
        auto storage = at(index);
        // Copy what is needed now: the resume data is only valid during this
        // call.
        std::shared_ptr<lt::typed_bitfield<lt::piece_index_t>> have;
        if (resume && !resume->have_pieces.empty() && !resume->have_pieces.none_set())
            have = std::make_shared<lt::typed_bitfield<lt::piece_index_t>>(resume->have_pieces);
        enqueue(storage, [this, storage, have, handler = std::move(handler)] {
            lt::storage_error error;
            const auto status = check(*storage, have.get(), error);
            boost::asio::post(ios_, [handler, status, error] { handler(status, error); });
        });
    }

    void async_stop_torrent(lt::storage_index_t index, std::function<void()> handler) override {
        auto storage = at(index);
        enqueue(storage, [this, storage, handler = std::move(handler)] {
            storage->close_all();
            if (handler) boost::asio::post(ios_, handler);
        });
    }

    void async_rename_file(lt::storage_index_t, lt::file_index_t file, std::string name,
                           std::function<void(const std::string&, lt::file_index_t,
                                              const lt::storage_error&)> handler) override {
        const lt::storage_error error(errno_code(EOPNOTSUPP), file, lt::operation_t::file_rename);
        boost::asio::post(ios_, [handler = std::move(handler), name = std::move(name), file, error] {
            handler(name, file, error);
        });
    }

    void async_delete_files(lt::storage_index_t index, lt::remove_flags_t options,
                            std::function<void(const lt::storage_error&)> handler) override {
        auto storage = at(index);
        enqueue(storage, [this, storage, options, handler = std::move(handler)] {
            lt::storage_error error;
            storage->close_all();
            if (options & lt::session_handle::delete_files) remove_payload(*storage, error);
            boost::asio::post(ios_, [handler, error] { handler(error); });
        });
    }

    void async_set_file_priority(
        lt::storage_index_t, lt::aux::vector<lt::download_priority_t, lt::file_index_t> priorities,
        std::function<void(const lt::storage_error&,
                           lt::aux::vector<lt::download_priority_t, lt::file_index_t>)> handler)
        override {
        // macha downloads every file of a torrent; priorities are accepted and
        // echoed, and every piece is written where it belongs.
        boost::asio::post(ios_, [handler = std::move(handler),
                                 priorities = std::move(priorities)]() mutable {
            handler(lt::storage_error(), std::move(priorities));
        });
    }

    void async_clear_piece(lt::storage_index_t, lt::piece_index_t piece,
                           std::function<void(lt::piece_index_t)> handler) override {
        boost::asio::post(ios_, [handler = std::move(handler), piece] { handler(piece); });
    }

    void update_stats_counters(lt::counters&) const override {}

    std::vector<lt::open_file_state> get_status(lt::storage_index_t) const override { return {}; }

    void submit_jobs() override {}

  private:
    lt::io_context& ios_;
    const lt::settings_interface& settings_;
    TorrentDiskHooks hooks_;

    // Touched only on libtorrent's network thread.
    std::vector<std::shared_ptr<Storage>> storages_;
    std::vector<int> free_slots_;

    std::mutex mutex_;
    std::condition_variable cv_;
    // Storages with at least one queued job and none running, in turn order.
    std::deque<std::shared_ptr<Storage>> ready_;
    std::vector<std::shared_ptr<lt::disk_observer>> observers_;
    bool stopping_{};
    std::atomic_bool aborting_{false};
    std::vector<std::thread> workers_;

    std::atomic<uint64_t> queued_write_bytes_{0};
    std::atomic<uint64_t> queue_limit_{0};

    static size_t slot(lt::storage_index_t index) {
        return static_cast<size_t>(static_cast<std::uint32_t>(index));
    }

    static lt::storage_error aborted() {
        return lt::storage_error(lt::error_code(boost::asio::error::operation_aborted));
    }

    std::shared_ptr<Storage> at(lt::storage_index_t index) const {
        return storages_[slot(index)];
    }

    std::shared_ptr<void> admit(uint64_t bytes) {
        if (!hooks_.admit) return std::make_shared<int>(0);
        return hooks_.admit(bytes, aborting_);
    }

    // Jobs for one storage run strictly in the order libtorrent issued them,
    // one at a time. libtorrent may ask for a piece's hash before the writes
    // of its blocks have completed; running them in order is what makes the
    // hash read what the writes wrote.
    void enqueue(const std::shared_ptr<Storage>& storage, std::function<void()> job) {
        {
            std::lock_guard lock(mutex_);
            storage->jobs.push_back(std::move(job));
            if (!storage->scheduled) {
                storage->scheduled = true;
                ready_.push_back(storage);
            }
        }
        cv_.notify_one();
    }

    void worker() {
        std::unique_lock lock(mutex_);
        while (true) {
            cv_.wait(lock, [this] { return stopping_ || !ready_.empty(); });
            if (ready_.empty()) return;
            auto storage = std::move(ready_.front());
            ready_.pop_front();
            auto job = std::move(storage->jobs.front());
            storage->jobs.pop_front();
            lock.unlock();
            job();
            job = nullptr;
            lock.lock();
            if (storage->jobs.empty()) {
                storage->scheduled = false;
            } else {
                ready_.push_back(std::move(storage));
                cv_.notify_one();
            }
        }
    }

    // Queued jobs still run after an abort; each one finds aborting_ set,
    // skips admission and its I/O, and answers with operation_aborted.
    // Nothing is marked downloaded unless it hashed, so nothing is lost.
    void shut_down() {
        aborting_.store(true);
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& thread : workers_)
            if (thread.joinable()) thread.join();
    }

    void write_drained(uint64_t bytes) {
        const auto queued = queued_write_bytes_.fetch_sub(bytes) - bytes;
        if (queued > queue_limit_.load(std::memory_order_relaxed) / 2) return;
        std::vector<std::shared_ptr<lt::disk_observer>> waiting;
        {
            std::lock_guard lock(mutex_);
            waiting.swap(observers_);
        }
        for (auto& observer : waiting)
            boost::asio::post(ios_, [observer] { observer->on_disk(); });
    }

    void observe(Clock::time_point started, uint64_t bytes) const {
        if (hooks_.observe) hooks_.observe(Clock::now() - started, bytes);
    }

    int descriptor(Storage& storage, lt::file_index_t file, bool writable, lt::storage_error& error) {
        auto found = storage.open.find(file);
        if (found != storage.open.end()) {
            if (found->second.writable || !writable) return found->second.fd;
            ::close(found->second.fd);
            storage.open.erase(found);
        }
        const auto path = storage.files.file_path(file, storage.save_path);
        if (writable) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
            if (ec) {
                error = lt::storage_error(errno_code(ec), file, lt::operation_t::mkdir);
                return -1;
            }
        }
        const int flags = writable ? (O_RDWR | O_CREAT | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC);
        const int fd = ::open(path.c_str(), flags, 0644);
        if (fd < 0) {
            error = lt::storage_error(errno_code(errno), file,
                                      lt::operation_t::file_open);
            return -1;
        }
        if (storage.open.size() >= max_open_files_per_torrent) {
            ::close(storage.open.begin()->second.fd);
            storage.open.erase(storage.open.begin());
        }
        storage.open.emplace(file, OpenFile{fd, writable});
        return fd;
    }

    // Returns bytes read. Like libtorrent's posix backend: a missing file is
    // an open error, a read that returns nothing is file_too_short, and a pad
    // file reads as zeroes.
    int read(Storage& storage, lt::piece_index_t piece, int start, char* out, int length,
             lt::storage_error& error) {
        int total = 0;
        for (const auto& slice : storage.files.map_block(piece, start, length)) {
            char* at_buffer = out + total;
            const auto want = static_cast<size_t>(slice.size);
            if (storage.files.pad_file_at(slice.file_index)) {
                std::fill_n(at_buffer, want, 0);
                total += static_cast<int>(want);
                continue;
            }
            const int fd = descriptor(storage, slice.file_index, false, error);
            if (fd < 0) return total;
            const auto started = Clock::now();
            size_t done = 0;
            while (done < want) {
                const auto got = ::pread(fd, at_buffer + done, want - done,
                                         static_cast<off_t>(slice.offset + static_cast<int64_t>(done)));
                if (got < 0 && errno == EINTR) continue;
                if (got < 0) {
                    error = lt::storage_error(errno_code(errno),
                                              slice.file_index, lt::operation_t::file_read);
                    return total + static_cast<int>(done);
                }
                if (got == 0) break;
                done += static_cast<size_t>(got);
            }
            observe(started, done);
            total += static_cast<int>(done);
            if (done < want) {
                if (done == 0)
                    error = lt::storage_error(lt::errors::make_error_code(lt::errors::file_too_short),
                                              slice.file_index, lt::operation_t::file_read);
                return total;
            }
        }
        return total;
    }

    void write(Storage& storage, lt::piece_index_t piece, int start, const char* in, int length,
               lt::storage_error& error) {
        int total = 0;
        for (const auto& slice : storage.files.map_block(piece, start, length)) {
            const char* from = in + total;
            const auto want = static_cast<size_t>(slice.size);
            total += static_cast<int>(want);
            if (storage.files.pad_file_at(slice.file_index)) continue;
            const int fd = descriptor(storage, slice.file_index, true, error);
            if (fd < 0) return;
            const auto started = Clock::now();
            size_t done = 0;
            while (done < want) {
                const auto put = ::pwrite(fd, from + done, want - done,
                                          static_cast<off_t>(slice.offset + static_cast<int64_t>(done)));
                if (put < 0 && errno == EINTR) continue;
                if (put <= 0) {
                    error = lt::storage_error(errno_code(put < 0 ? errno : EIO),
                                              slice.file_index, lt::operation_t::file_write);
                    return;
                }
                done += static_cast<size_t>(put);
            }
            observe(started, done);
        }
    }

    // With resume data: every piece it claims must still be backed by files
    // long enough to hold it, or the torrent is re-checked in full. Without
    // resume data, any existing payload file means a full check.
    lt::status_t check(Storage& storage, const lt::typed_bitfield<lt::piece_index_t>* have,
                       lt::storage_error& error) {
        std::error_code ec;
        std::filesystem::create_directories(storage.save_path, ec);
        if (ec) {
            error = lt::storage_error(errno_code(ec), lt::operation_t::mkdir);
            return status_fatal;
        }
        const auto& files = storage.files;
        auto size_on_disk = [&](lt::file_index_t file) -> int64_t {
            struct stat st {};
            if (::stat(files.file_path(file, storage.save_path).c_str(), &st) != 0) return -1;
            return static_cast<int64_t>(st.st_size);
        };
        if (!have) {
            for (const auto file : files.file_range())
                if (!files.pad_file_at(file) && size_on_disk(file) >= 0) return status_full_check;
            return status_ok;
        }
        for (const auto piece : files.piece_range()) {
            if (!have->get_bit(piece)) continue;
            for (const auto& slice : files.map_block(piece, 0, files.piece_size(piece))) {
                if (files.pad_file_at(slice.file_index)) continue;
                if (size_on_disk(slice.file_index) < slice.offset + slice.size)
                    return status_full_check;
            }
        }
        return status_ok;
    }

    void remove_payload(Storage& storage, lt::storage_error& error) {
        const auto& files = storage.files;
        std::vector<std::filesystem::path> parents;
        for (const auto file : files.file_range()) {
            if (files.pad_file_at(file)) continue;
            const std::filesystem::path path = files.file_path(file, storage.save_path);
            std::error_code ec;
            std::filesystem::remove(path, ec);
            if (ec && !error.ec)
                error = lt::storage_error(errno_code(ec), file, lt::operation_t::file_remove);
            parents.push_back(path.parent_path());
        }
        // Prune directories the payload created, deepest first, stopping at
        // the save path; a directory still holding anything stays.
        std::sort(parents.begin(), parents.end(), [](const auto& a, const auto& b) {
            return a.native().size() > b.native().size();
        });
        const std::filesystem::path root(storage.save_path);
        for (auto dir : parents) {
            while (!dir.empty() && dir != root && dir.native().size() > root.native().size()) {
                std::error_code ec;
                if (!std::filesystem::remove(dir, ec)) break;
                dir = dir.parent_path();
            }
        }
    }
};

} // namespace

std::function<std::shared_ptr<void>(uint64_t, const std::atomic_bool&)>
loader_admission(DataResourceArbiter& arbiter) {
    return [&arbiter](uint64_t bytes, const std::atomic_bool& aborting) -> std::shared_ptr<void> {
        while (!aborting.load()) {
            const auto deadline = DataWorkContext::Clock::now() + std::chrono::seconds(1);
            auto lease = arbiter.acquire(DataWorkContext(FrameType::loader, bytes, deadline), bytes);
            if (lease) return std::make_shared<DataResourceArbiter::Lease>(std::move(*lease));
            if (DataWorkContext::Clock::now() < deadline) return nullptr;
        }
        return nullptr;
    };
}

lt::disk_io_constructor_type macha_disk_io_constructor(TorrentDiskHooks hooks) {
    return [hooks = std::move(hooks)](lt::io_context& ios, const lt::settings_interface& settings,
                                      lt::counters&) -> std::unique_ptr<lt::disk_interface> {
        return std::make_unique<MachaDiskIo>(ios, settings, hooks);
    };
}

} // namespace macha
