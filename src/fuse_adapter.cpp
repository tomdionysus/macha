// SPDX-License-Identifier: GPL-3.0-or-later
#define FUSE_USE_VERSION 31
#if defined(__APPLE__)
#define FUSE_DARWIN_ENABLE_EXTENSIONS 0
#endif
#include "fuse_adapter.hpp"
#include "crypto.hpp"
#include "diagnostics.hpp"
#include "fuse_frontend.hpp"
#include "log.hpp"
#include "macos_unicode.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#pragma clang diagnostic ignored "-Wnested-anon-types"
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#endif
#include <fuse.h>
#include <fuse_lowlevel.h>
#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/mount.h>
#endif

namespace macha {
namespace {

FuseFrontend& frontend() {
    return *static_cast<FuseFrontend*>(fuse_get_context()->private_data);
}

struct Handle {
    FuseOpenHandle file;
};

Handle* handle(fuse_file_info* fi) {
    return fi && fi->fh ? reinterpret_cast<Handle*>(fi->fh) : nullptr;
}

std::string hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string byte_hex(std::string_view value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    if (!value.empty()) out.reserve(value.size() * 3 - 1);
    for (unsigned char c : value) {
        if (!out.empty()) out.push_back(' ');
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0f]);
    }
    return out;
}

std::string request_identity() {
    auto* c = fuse_get_context();
    if (!c) return "uid=? gid=? pid=?";
    return "uid=" + std::to_string(c->uid) + " gid=" + std::to_string(c->gid) +
           " pid=" + std::to_string(c->pid);
}

void trace_request(const char* op, const char* path, const fuse_file_info* fi = nullptr) {
    if (!Log::enabled(LogLevel::all)) return;
    std::string message = std::string("FUSE TRACE request op=") + op + " path=" +
                          (path ? path : "<null>");
    if (path) message += " path_hex=" + byte_hex(path);
    message += " " + request_identity();
    if (fi)
        message += " flags=" + hex(static_cast<unsigned int>(fi->flags)) +
                   " fh=" + std::to_string(fi->fh);
    Log::trace(message);
}

struct FuseLatency {
    const char* op;
    const char* path;
    bool enabled{Log::enabled(LogLevel::debug)};
    Clock::time_point started{};

    FuseLatency(const char* operation, const char* pathname)
        : op(operation), path(pathname), started(enabled ? Clock::now() : Clock::time_point{}) {
        frontend().note_interactive_activity();
    }

    ~FuseLatency() noexcept {
        try {
            if (!enabled) return;
            const auto ms = elapsed_ms(started);
            const auto threshold_ms = std::strcmp(op, "write") == 0 ? 250 : 100;
            if (ms >= threshold_ms)
                Log::debug(std::string("DIAG slow-fuse op=") + op + " path=" +
                           (path ? path : "<null>") + " elapsed_ms=" + std::to_string(ms));
        } catch (...) {
        }
    }
};

int fail(const char* op, const std::exception& e) {
    if (auto* f = dynamic_cast<const FsError*>(&e)) {
        if (Log::enabled(LogLevel::all))
            Log::trace(std::string("FUSE TRACE failure op=") + op + " " + request_identity() +
                       " errno=" + std::to_string(f->code()) + " message=" + e.what());
        return -f->code();
    }
    Log::warn(std::string("FUSE failure op=") + op + " " + request_identity() +
              " errno=" + std::to_string(EIO) + " message=" + e.what());
    return -EIO;
}

template <class Fn> int guarded(const char* op, Fn&& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        return fail(op, e);
    }
}

void set_time(struct stat& st, int64_t ns) {
    time_t sec = static_cast<time_t>(ns / 1000000000LL);
    long nsec = static_cast<long>(ns % 1000000000LL);
#if defined(__APPLE__)
    st.st_mtimespec = {sec, nsec};
    st.st_ctimespec = {sec, nsec};
    st.st_atimespec = {sec, nsec};
#else
    st.st_mtim = {sec, nsec};
    st.st_ctim = {sec, nsec};
    st.st_atim = {sec, nsec};
#endif
}

void fill_stat(const FuseEntryAttributes& e, struct stat& st) {
    std::memset(&st, 0, sizeof(st));
    st.st_mode = (e.type == EntryType::directory ? S_IFDIR : S_IFREG) | e.mode;
    st.st_nlink = e.type == EntryType::directory ? 2 : 1;
    st.st_uid = e.uid;
    st.st_gid = e.gid;
    st.st_size = static_cast<off_t>(e.size);
    st.st_blksize = 128 * 1024;
    st.st_blocks = static_cast<blkcnt_t>((e.size + 511) / 512);
    set_time(st, e.mtime_ns);
}

int op_getattr(const char* path, struct stat* st, fuse_file_info* fi) {
    trace_request("getattr", path, fi);
    FuseLatency latency{"getattr", path};
    return guarded("getattr", [&] {
        auto entry = frontend().getattr(path);
        fill_stat(entry, *st);
        return 0;
    });
}

int op_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t, fuse_file_info* fi,
               enum fuse_readdir_flags) {
    trace_request("readdir", path, fi);
    FuseLatency latency{"readdir", path};
    return guarded("readdir", [&] {
        filler(buf, ".", nullptr, 0, FUSE_FILL_DIR_DEFAULTS);
        filler(buf, "..", nullptr, 0, FUSE_FILL_DIR_DEFAULTS);
        auto entries = frontend().readdir(path);
        for (auto& [name, entry] : entries) {
            struct stat st{};
            fill_stat(entry, st);
            const auto fuse_name = macos_fuse_decomposed_name(name);
            if (filler(buf, fuse_name.c_str(), &st, 0, FUSE_FILL_DIR_DEFAULTS)) break;
        }
        return 0;
    });
}

int op_mkdir(const char* path, mode_t mode) {
    trace_request("mkdir", path);
    FuseLatency latency{"mkdir", path};
    return guarded("mkdir", [&] {
        auto* c = fuse_get_context();
        frontend().mkdir(path, mode, c->uid, c->gid);
        return 0;
    });
}

int op_rmdir(const char* path) {
    trace_request("rmdir", path);
    FuseLatency latency{"rmdir", path};
    return guarded("rmdir", [&] { frontend().rmdir(path); return 0; });
}

int op_unlink(const char* path) {
    trace_request("unlink", path);
    FuseLatency latency{"unlink", path};
    return guarded("unlink", [&] { frontend().unlink(path); return 0; });
}

int op_rename(const char* from, const char* to, unsigned int flags) {
    trace_request("rename", from);
    FuseLatency latency{"rename", from};
    return guarded("rename", [&] {
        bool noreplace = false;
#ifdef RENAME_NOREPLACE
        noreplace = (flags & RENAME_NOREPLACE) != 0;
        flags &= ~static_cast<unsigned int>(RENAME_NOREPLACE);
#endif
        if (flags) return -EINVAL;
        frontend().rename(from, to, noreplace);
        return 0;
    });
}

int op_chmod(const char* path, mode_t mode, fuse_file_info* fi) {
    trace_request("chmod", path, fi);
    FuseLatency latency{"chmod", path};
    return guarded("chmod", [&] { frontend().chmod(path, mode); return 0; });
}

int op_chown(const char* path, uid_t uid, gid_t gid, fuse_file_info* fi) {
    trace_request("chown", path, fi);
    FuseLatency latency{"chown", path};
    return guarded("chown", [&] {
        frontend().chown(path, uid, gid, uid != static_cast<uid_t>(-1),
                         gid != static_cast<gid_t>(-1));
        return 0;
    });
}

int op_utimens(const char* path, const struct timespec tv[2], fuse_file_info* fi) {
    trace_request("utimens", path, fi);
    FuseLatency latency{"utimens", path};
    return guarded("utimens", [&] {
        if (tv[1].tv_nsec == UTIME_OMIT) return 0;
        const int64_t ns = tv[1].tv_nsec == UTIME_NOW
                               ? wall_time_ns()
                               : static_cast<int64_t>(tv[1].tv_sec) * 1000000000LL + tv[1].tv_nsec;
        frontend().utimens(path, ns);
        return 0;
    });
}

void set_file_flags(fuse_file_info* fi) {
    fi->direct_io = 0;
    fi->keep_cache = 0;
}

int op_open(const char* path, fuse_file_info* fi) {
    trace_request("open", path, fi);
    FuseLatency latency{"open", path};
    return guarded("open", [&] {
        const int access = fi->flags & O_ACCMODE;
        auto h = std::make_unique<Handle>();
        h->file = frontend().open(path, access == O_RDONLY || access == O_RDWR,
                                  access == O_WRONLY || access == O_RDWR,
                                  (fi->flags & O_APPEND) != 0, (fi->flags & O_TRUNC) != 0);
        set_file_flags(fi);
        fi->fh = reinterpret_cast<uint64_t>(h.release());
        return 0;
    });
}

int op_create(const char* path, mode_t mode, fuse_file_info* fi) {
    trace_request("create", path, fi);
    FuseLatency latency{"create", path};
    return guarded("create", [&] {
        const int access = fi->flags & O_ACCMODE;
        auto* c = fuse_get_context();
        auto h = std::make_unique<Handle>();
        h->file = frontend().create(path, mode, c->uid, c->gid,
                                    access == O_RDONLY || access == O_RDWR,
                                    access == O_WRONLY || access == O_RDWR,
                                    (fi->flags & O_APPEND) != 0);
        set_file_flags(fi);
        fi->fh = reinterpret_cast<uint64_t>(h.release());
        return 0;
    });
}

int op_read(const char* path, char* buf, size_t size, off_t off, fuse_file_info* fi) {
    trace_request("read", path, fi);
    FuseLatency latency{"read", path};
    return guarded("read", [&] {
        if (off < 0) return -EINVAL;
        auto* h = handle(fi);
        uint64_t inode = 0;
        if (h) {
            if (!h->file.readable) return -EBADF;
            inode = h->file.inode;
        } else {
            auto found = frontend().inode_for_path(path);
            if (!found) return -ENOENT;
            inode = *found;
        }
        auto n = frontend().read(inode, static_cast<uint64_t>(off),
                                 {reinterpret_cast<uint8_t*>(buf), size});
        return static_cast<int>(n);
    });
}

int op_write(const char* path, const char* buf, size_t size, off_t off, fuse_file_info* fi) {
    trace_request("write", path, fi);
    FuseLatency latency{"write", path};
    return guarded("write", [&] {
        if (off < 0) return -EINVAL;
        auto* h = handle(fi);
        if (!h || !h->file.writable) return -EBADF;
        auto n = frontend().write(h->file.inode, static_cast<uint64_t>(off),
                                  {reinterpret_cast<const uint8_t*>(buf), size}, h->file.append);
        return static_cast<int>(n);
    });
}

int op_truncate(const char* path, off_t size, fuse_file_info* fi) {
    trace_request("truncate", path, fi);
    FuseLatency latency{"truncate", path};
    return guarded("truncate", [&] {
        if (size < 0) return -EINVAL;
        if (auto* h = handle(fi); h && h->file.writable)
            frontend().truncate(h->file.inode, static_cast<uint64_t>(size));
        else
            frontend().truncate(path, static_cast<uint64_t>(size));
        return 0;
    });
}

int op_flush(const char* path, fuse_file_info* fi) {
    trace_request("flush", path, fi);
    FuseLatency latency{"flush", path};
    return guarded("flush", [&] {
        if (auto* h = handle(fi); h && h->file.writable) frontend().flush(h->file.inode);
        return 0;
    });
}

int op_fsync(const char* path, int, fuse_file_info* fi) {
    trace_request("fsync", path, fi);
    FuseLatency latency{"fsync", path};
    return guarded("fsync", [&] {
        if (auto* h = handle(fi); h && h->file.writable) frontend().fsync(h->file.inode);
        return 0;
    });
}

int op_release(const char* path, fuse_file_info* fi) {
    trace_request("release", path, fi);
    FuseLatency latency{"release", path};
    auto* h = handle(fi);
    if (!h) return 0;
    int rc = 0;
    try {
        frontend().release(h->file.inode, h->file.writable);
    } catch (const std::exception& e) {
        rc = fail("release", e);
    }
    delete h;
    fi->fh = 0;
    return rc;
}

int op_statfs(const char* path, struct statvfs* st) {
    trace_request("statfs", path);
    FuseLatency latency{"statfs", path};
    return guarded("statfs", [&] {
        auto [total, used] = frontend().logical_capacity();
        constexpr uint64_t block = 4096;
        std::memset(st, 0, sizeof(*st));
        st->f_bsize = block;
        st->f_frsize = block;
        st->f_blocks = total / block;
        st->f_bfree = total > used ? (total - used) / block : 0;
        st->f_bavail = st->f_bfree;
        st->f_namemax = 255;
        st->f_files = 1000000000ULL;
        st->f_ffree = 999999999ULL;
        return 0;
    });
}

void* op_init(struct fuse_conn_info*, struct fuse_config* cfg) {
    const auto& policy = frontend().config();
    const auto seconds = [](std::chrono::milliseconds value) {
        return std::chrono::duration<double>(value).count();
    };
    cfg->kernel_cache = 0;
    cfg->entry_timeout = seconds(policy.entry_timeout);
    cfg->attr_timeout = seconds(policy.attr_timeout);
    cfg->negative_timeout = seconds(policy.negative_timeout);
    Log::debug("FUSE bounded frontend entry_timeout_ms=" + std::to_string(policy.entry_timeout.count()) +
               " attr_timeout_ms=" + std::to_string(policy.attr_timeout.count()) +
               " absolute_request_timeout_ms=" +
               std::to_string(policy.absolute_request_timeout.count()) +
               " commit_workers=" + std::to_string(policy.commit_workers) +
               " foreground_commit_workers=" +
               std::to_string(policy.foreground_commit_workers) +
               " publication_quiet_ms=" +
               std::to_string(policy.publication_quiet.count()));
    return fuse_get_context()->private_data;
}

fuse_operations operations() {
    fuse_operations o{};
    o.init = op_init;
    o.getattr = op_getattr;
    o.readdir = op_readdir;
    o.mkdir = op_mkdir;
    o.rmdir = op_rmdir;
    o.unlink = op_unlink;
    o.rename = op_rename;
    o.chmod = op_chmod;
    o.chown = op_chown;
    o.utimens = op_utimens;
    o.open = op_open;
    o.create = op_create;
    o.read = op_read;
    o.write = op_write;
    o.truncate = op_truncate;
    o.flush = op_flush;
    o.fsync = op_fsync;
    o.release = op_release;
    o.statfs = op_statfs;
    return o;
}

bool mount_table_contains(const std::string& mount) {
#if defined(__linux__)
    const auto unescape_mount_field = [](std::string value) {
        for (const auto& [escaped, plain] :
             std::array<std::pair<std::string_view, char>, 4>{{{"\\040", ' '}, {"\\011", '\t'},
                                                               {"\\012", '\n'}, {"\\134", '\\'}}}) {
            size_t pos = 0;
            while ((pos = value.find(escaped, pos)) != std::string::npos) {
                value.replace(pos, escaped.size(), 1, plain);
                ++pos;
            }
        }
        return value;
    };
    std::ifstream input("/proc/self/mountinfo");
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string id, parent, device, root, mounted;
        if (!(fields >> id >> parent >> device >> root >> mounted)) continue;
        if (unescape_mount_field(mounted) != mount) continue;
        auto separator = line.find(" - ");
        if (separator == std::string::npos) return true;
        auto tail = line.substr(separator + 3);
        std::istringstream type_fields(tail);
        std::string type, source;
        type_fields >> type >> source;
        return source == "macha" || type.starts_with("fuse");
    }
    return false;
#elif defined(__APPLE__)
    struct statfs* mounts = nullptr;
    const int count = getmntinfo(&mounts, MNT_NOWAIT);
    for (int i = 0; i < count; ++i) {
        if (mount == mounts[i].f_mntonname) {
            std::string source = mounts[i].f_mntfromname;
            std::string type = mounts[i].f_fstypename;
            return source.find("macha") != std::string::npos || type.find("fuse") != std::string::npos ||
                   type.find("macfuse") != std::string::npos;
        }
    }
    return false;
#else
    (void)mount;
    return true;
#endif
}

class CoveredMountpointGuard {
    int fd_{-1};
    mode_t original_mode_{};
    bool protected_{};
    bool restore_{};

  public:
    explicit CoveredMountpointGuard(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd_ < 0) throw std::runtime_error("cannot open FUSE mountpoint for fail-closed guard");
        struct stat st{};
        if (::fstat(fd_, &st) != 0) {
            const auto error = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("cannot stat FUSE mountpoint: " + std::string(std::strerror(error)));
        }
        original_mode_ = st.st_mode & 07777;
    }

    ~CoveredMountpointGuard() {
        if (fd_ >= 0) {
            if (restore_ && protected_) (void)::fchmod(fd_, original_mode_);
            ::close(fd_);
        }
    }

    bool protect() {
        if (fd_ < 0) return false;
        const auto fail_closed_mode = static_cast<mode_t>(original_mode_ & ~0222);
        if (::fchmod(fd_, fail_closed_mode) != 0) return false;
        protected_ = true;
        return true;
    }

    void restore_on_exit(bool value) { restore_ = value; }
};

} // namespace

int run_fuse(FileSystem& filesystem, CacheHydrator& hydrator,
             const std::filesystem::path& mount_path, const FuseConfig& config,
             std::function<void()> request_shutdown) {
    auto mount = mount_path.string();
    std::string options = config.allow_other ? "default_permissions,allow_other,fsname=macha"
                                             : "default_permissions,fsname=macha";

    std::unique_ptr<CoveredMountpointGuard> mount_guard;
    if (config.fail_closed_mountpoint)
        mount_guard = std::make_unique<CoveredMountpointGuard>(mount);

    auto fuse_frontend = std::make_shared<FuseFrontend>(filesystem, config);

    std::vector<std::string> fuse_arg_storage{"macha", "-o", options};
    std::vector<char*> fuse_argv;
    for (auto& arg : fuse_arg_storage) fuse_argv.push_back(arg.data());
    struct fuse_args args = FUSE_ARGS_INIT(static_cast<int>(fuse_argv.size()), fuse_argv.data());
    auto ops = operations();
    struct fuse* instance = fuse_new(&args, &ops, sizeof(ops), fuse_frontend.get());
    if (!instance) {
        fuse_opt_free_args(&args);
        return 3;
    }
    if (fuse_mount(instance, mount.c_str()) != 0) {
        fuse_destroy(instance);
        fuse_opt_free_args(&args);
        return 4;
    }

    if (mount_guard && !mount_guard->protect()) {
        Log::error("FUSE fail-closed mountpoint protection could not be installed");
        fuse_unmount(instance);
        fuse_destroy(instance);
        fuse_opt_free_args(&args);
        return 5;
    }

    hydrator.add_provider(fuse_frontend);
    auto* session = fuse_get_session(instance);
    if (fuse_set_signal_handlers(session) != 0) {
        hydrator.remove_provider(fuse_frontend.get());
        fuse_unmount(instance);
        fuse_destroy(instance);
        fuse_opt_free_args(&args);
        return 6;
    }

    filesystem.reset_io_cancellation();
    std::atomic_bool unexpected_mount_loss{false};
    std::atomic_bool mount_seen{mount_table_contains(mount)};

    std::jthread mount_watchdog([&](std::stop_token stop) {
        size_t consecutive_misses = 0;
        constexpr size_t missing_threshold = 3;
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(config.watchdog_interval);
            if (stop.stop_requested()) break;
            const bool mounted = mount_table_contains(mount);
            if (mounted) {
                mount_seen.store(true);
                consecutive_misses = 0;
                continue;
            }
            if (!mount_seen.load()) continue;
            if (++consecutive_misses < missing_threshold) {
                Log::debug("FUSE mount-table watchdog miss " +
                           std::to_string(consecutive_misses) + "/" +
                           std::to_string(missing_threshold) + " mount=" + mount);
                continue;
            }
            unexpected_mount_loss.store(true);
            Log::error("FUSE mount disappeared for three consecutive watchdog checks; namespace is fail-closed and service shutdown is requested");
            filesystem.request_io_cancellation();
            if (request_shutdown) request_shutdown();
            fuse_session_exit(session);
            return;
        }
    });

    Log::debug("shutdown: entering bounded FUSE main loop");
    const int loop_rc = fuse_loop_mt(instance, 0);
    if (loop_rc != 0) unexpected_mount_loss.store(true);
    filesystem.request_io_cancellation();
    if (request_shutdown) request_shutdown();

    mount_watchdog.request_stop();
    if (mount_watchdog.joinable()) mount_watchdog.join();

    hydrator.remove_provider(fuse_frontend.get());
    fuse_frontend->stop();
    fuse_remove_signal_handlers(session);
    fuse_unmount(instance);

    if (mount_guard) mount_guard->restore_on_exit(!unexpected_mount_loss.load());

    fuse_destroy(instance);
    fuse_opt_free_args(&args);

    if (unexpected_mount_loss.load()) {
        Log::error("FUSE frontend terminated unexpectedly; covered mountpoint remains non-writable");
        return 8;
    }
    Log::debug("shutdown: FUSE main loop returned cleanly");
    return 0;
}

} // namespace macha
