// SPDX-License-Identifier: GPL-3.0-or-later
#define FUSE_USE_VERSION 31
#if defined(__APPLE__)
// macFUSE enables Darwin-specific libfuse3 callback signatures by default.
// This filesystem deliberately uses the portable libfuse3 API on both Linux
// and macOS.  Disabling the extensions keeps getattr/readdir/statfs ABI-compatible
// with upstream libfuse3 (stat/statvfs) instead of fuse_darwin_attr/statfs.
#define FUSE_DARWIN_ENABLE_EXTENSIONS 0
#endif
#include "fuse_adapter.hpp"
#include "crypto.hpp"
#include "diagnostics.hpp"
#include "log.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#if defined(__APPLE__) && defined(__clang__)
// macFUSE headers themselves use anonymous struct/union extensions.  Keep
// -Werror for our code without promoting warnings in the third-party header.
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
#include <iomanip>
#include <memory>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <sys/statvfs.h>
#include <unistd.h>

namespace macha {
namespace {
FileSystem& fs();

struct Handle {
    std::string path;
    std::shared_ptr<ReadHandle> read;
    std::shared_ptr<WriteHandle> write;
};

struct FuseLatency {
    const char* op;
    const char* path;
    bool enabled{Log::enabled(LogLevel::debug)};
    Clock::time_point started{};

    FuseLatency(const char* operation, const char* pathname)
        : op(operation), path(pathname), started(enabled ? Clock::now() : Clock::time_point{}) {
        // Metadata-only mount activity (rsync readdir/getattr in particular)
        // must preempt maintenance even though it transfers no extent bytes.
        fs().note_interactive_activity();
    }

    ~FuseLatency() noexcept {
        try {
            if (!enabled)
                return;
            const auto ms = elapsed_ms(started);
            // Normal large-file writes on FUSE commonly take tens of
            // milliseconds and logging every one obscures the actual stalls.
            // Keep namespace latency sensitive, but only report data writes
            // once they are clearly outside the normal streaming-write band.
            const auto threshold_ms = std::strcmp(op, "write") == 0 ? 250 : 100;
            if (ms >= threshold_ms)
                Log::debug(std::string("DIAG slow-fuse op=") + op + " path=" +
                           (path ? path : "<null>") + " elapsed_ms=" + std::to_string(ms));
        } catch (...) {
        }
    }
};

FileSystem& fs() {
    return *static_cast<FileSystem*>(fuse_get_context()->private_data);
}

Handle* handle(fuse_file_info* fi) {
    return fi && fi->fh ? reinterpret_cast<Handle*>(fi->fh) : nullptr;
}

std::string octal(uint64_t value) {
    std::ostringstream out;
    out << '0' << std::oct << value;
    return out.str();
}

std::string hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string request_identity() {
    auto* c = fuse_get_context();
    if (!c)
        return "uid=? gid=? pid=?";
    return "uid=" + std::to_string(c->uid) + " gid=" + std::to_string(c->gid) +
           " pid=" + std::to_string(c->pid);
}

void trace_request(const char* op, const char* path, const fuse_file_info* fi = nullptr) {
    if (!Log::enabled(LogLevel::all))
        return;
    std::string message = std::string("FUSE TRACE request op=") + op + " path=" +
                          (path ? path : "<null>") + " " + request_identity();
    if (fi)
        message += " flags=" + hex(static_cast<unsigned int>(fi->flags)) +
                   " fh=" + std::to_string(fi->fh);
    if (Log::enabled(LogLevel::all))
        Log::trace(message);
}

std::string entry_summary(const FsEntry& e) {
    return std::string("type=") + (e.type == EntryType::directory ? "directory" : "file") +
           " mode=" + octal(e.mode) + " uid=" + std::to_string(e.uid) +
           " gid=" + std::to_string(e.gid) + " size=" + std::to_string(e.size) +
           " version=" + std::to_string(e.version) + " ctime_ns=" +
           std::to_string(e.ctime_ns) + " mtime_ns=" + std::to_string(e.mtime_ns);
}

int fail(const char* op, const std::exception& e) {
    if (auto* f = dynamic_cast<const FsError*>(&e)) {
        if (Log::enabled(LogLevel::all))
            Log::trace(std::string("FUSE TRACE failure op=") + op + " " + request_identity() +
                   " errno=" + std::to_string(f->code()) + " message=" + e.what());
        return -f->code();
    }
    Log::warn(std::string("FUSE TRACE failure op=") + op + " " + request_identity() +
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

void fill_stat(const FsEntry& e, struct stat& st) {
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

void apply_active_write_size(const std::string& path, const FsEntry& e, struct stat& st) {
    if (e.type != EntryType::file)
        return;
    auto active_size = fs().active_write_size(path);
    if (!active_size)
        return;
    st.st_size = static_cast<off_t>(*active_size);
    st.st_blocks = static_cast<blkcnt_t>((*active_size + 511) / 512);
}

int op_getattr(const char* path, struct stat* st, fuse_file_info* fi) {
    trace_request("getattr", path, fi);
    FuseLatency latency{"getattr", path};
    return guarded("getattr", [&] {
        auto e = fs().getattr(path);
        fill_stat(e, *st);
        apply_active_write_size(path, e, *st);
        if (Log::enabled(LogLevel::all)) {
            auto active = fs().active_write_diagnostics(path);
            Log::trace(std::string("FUSE TRACE result op=getattr path=") + path + " " +
                       entry_summary(e) + " returned_size=" + std::to_string(st->st_size) +
                       " active_writes=" + std::to_string(active.size()));
            for (const auto& write : active) {
                Log::trace(std::string("WRITE getattr-view path=") + path +
                           " id=" + std::to_string(write.id) +
                           " metadata_size=" + std::to_string(e.size) +
                           " returned_size=" + std::to_string(st->st_size) +
                           " logical_size=" + std::to_string(write.logical_size) +
                           " staged_size=" + std::to_string(write.staged_size) +
                           " buffer_size=" + std::to_string(write.buffer_size) +
                           " sequential=" + std::to_string(write.sequential ? 1 : 0) +
                           " temp_open=" + std::to_string(write.temp_open ? 1 : 0) +
                           " temp_size=" + std::to_string(write.temp_size));
            }
        }
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
        auto entries = fs().readdir(path);
        if (Log::enabled(LogLevel::all))
            Log::trace(std::string("FUSE TRACE result op=readdir path=") + path +
                  " entries=" + std::to_string(entries.size()));
        for (auto& [name, entry] : entries) {
            if (Log::enabled(LogLevel::all))
                Log::trace(std::string("FUSE TRACE dirent parent=") + path + " name=" + name + " " +
                      entry_summary(entry));
            struct stat st{};
            fill_stat(entry, st);
            const std::string child_path =
                std::string(path) == "/" ? "/" + name : std::string(path) + "/" + name;
            apply_active_write_size(child_path, entry, st);
            if (filler(buf, name.c_str(), &st, 0, FUSE_FILL_DIR_DEFAULTS))
                break;
        }
        return 0;
    });
}

int op_mkdir(const char* path, mode_t mode) {
    trace_request("mkdir", path);
    FuseLatency latency{"mkdir", path};
    if (Log::enabled(LogLevel::all))
        Log::trace(std::string("FUSE TRACE argument op=mkdir mode=") + octal(mode));
    return guarded("mkdir", [&] {
        auto* c = fuse_get_context();
        fs().mkdir(path, mode, c->uid, c->gid);
        return 0;
    });
}

int op_rmdir(const char* path) {
    trace_request("rmdir", path);
    FuseLatency latency{"rmdir", path};
    return guarded("rmdir", [&] {
        fs().rmdir(path);
        return 0;
    });
}

int op_unlink(const char* path) {
    trace_request("unlink", path);
    FuseLatency latency{"unlink", path};
    return guarded("unlink", [&] {
        fs().unlink(path);
        return 0;
    });
}

int op_rename(const char* from, const char* to, unsigned int flags) {
    trace_request("rename", from);
    FuseLatency latency{"rename", from};
    if (Log::enabled(LogLevel::all))
        Log::trace(std::string("FUSE TRACE argument op=rename to=") + to + " flags=" + hex(flags));
    return guarded("rename", [&] {
        bool noreplace = false;
#ifdef RENAME_NOREPLACE
        noreplace = (flags & RENAME_NOREPLACE) != 0;
        flags &= ~static_cast<unsigned int>(RENAME_NOREPLACE);
#endif
        if (flags)
            return -EINVAL;
        fs().rename(from, to, noreplace);
        return 0;
    });
}

int op_chmod(const char* path, mode_t mode, fuse_file_info* fi) {
    trace_request("chmod", path, fi);
    FuseLatency latency{"chmod", path};
    if (Log::enabled(LogLevel::all))
        Log::trace(std::string("FUSE TRACE argument op=chmod mode=") + octal(mode));
    return guarded("chmod", [&] {
        fs().chmod(path, mode);
        return 0;
    });
}

int op_chown(const char* path, uid_t uid, gid_t gid, fuse_file_info* fi) {
    trace_request("chown", path, fi);
    FuseLatency latency{"chown", path};
    if (Log::enabled(LogLevel::all))
        Log::trace(std::string("FUSE TRACE argument op=chown uid=") + std::to_string(uid) +
              " gid=" + std::to_string(gid));
    return guarded("chown", [&] {
        fs().chown(path, uid, gid, uid != static_cast<uid_t>(-1), gid != static_cast<gid_t>(-1));
        return 0;
    });
}

int op_utimens(const char* path, const struct timespec tv[2], fuse_file_info* fi) {
    trace_request("utimens", path, fi);
    FuseLatency latency{"utimens", path};
    return guarded("utimens", [&] {
        int64_t ns;
        if (tv[1].tv_nsec == UTIME_NOW)
            ns = wall_time_ns();
        else if (tv[1].tv_nsec == UTIME_OMIT)
            return 0;
        else
            ns = static_cast<int64_t>(tv[1].tv_sec) * 1000000000LL + tv[1].tv_nsec;
        fs().utimens(path, ns);
        return 0;
    });
}

int op_open(const char* path, fuse_file_info* fi) {
    trace_request("open", path, fi);
    FuseLatency latency{"open", path};
    return guarded("open", [&] {
        auto e = fs().getattr(path);
        if (e.type != EntryType::file)
            return -EISDIR;
        auto h = std::make_unique<Handle>();
        h->path = path;
        int access = fi->flags & O_ACCMODE;
        if (access == O_RDONLY || access == O_RDWR)
            h->read = fs().open_read(path);
        if (access == O_WRONLY || access == O_RDWR)
            h->write = fs().open_write(path, (fi->flags & O_TRUNC) != 0);
        // Files in this filesystem always have a meaningful logical size.
        // Do not use direct I/O: it bypasses the kernel page/UBC cache and,
        // especially on macOS/macFUSE, breaks mmap-based applications and can
        // produce non-standard write behaviour.  keep_cache=0 still prevents
        // stale data from surviving across opens.
        fi->direct_io = 0;
        fi->keep_cache = 0;
        auto* read_ptr = h->read.get();
        auto* write_ptr = h->write.get();
        fi->fh = reinterpret_cast<uint64_t>(h.release());
        if (Log::enabled(LogLevel::all))
            Log::trace(std::string("FUSE TRACE result op=open path=") + path +
                  " fh=" + std::to_string(fi->fh));
        if (Log::enabled(LogLevel::all))
            Log::trace(std::string("FUSE handle op=open path=") + path +
                   " fh=" + std::to_string(fi->fh) +
                   " read_ptr=" + std::to_string(reinterpret_cast<uintptr_t>(read_ptr)) +
                   " write_id=" +
                   std::to_string(write_ptr ? write_ptr->diagnostic_id() : 0));
        return 0;
    });
}

int op_create(const char* path, mode_t mode, fuse_file_info* fi) {
    trace_request("create", path, fi);
    FuseLatency latency{"create", path};
    if (Log::enabled(LogLevel::all))
        Log::trace(std::string("FUSE TRACE argument op=create mode=") + octal(mode));
    return guarded("create", [&] {
        auto* c = fuse_get_context();
        fs().create_file(path, mode, c->uid, c->gid);
        auto h = std::make_unique<Handle>();
        h->path = path;
        int access = fi->flags & O_ACCMODE;
        if (access == O_RDONLY || access == O_RDWR)
            h->read = fs().open_read(path);
        if (access == O_WRONLY || access == O_RDWR)
            h->write = fs().open_write(path, true);
        // Files in this filesystem always have a meaningful logical size.
        // Do not use direct I/O: it bypasses the kernel page/UBC cache and,
        // especially on macOS/macFUSE, breaks mmap-based applications and can
        // produce non-standard write behaviour.  keep_cache=0 still prevents
        // stale data from surviving across opens.
        fi->direct_io = 0;
        fi->keep_cache = 0;
        auto* read_ptr = h->read.get();
        auto* write_ptr = h->write.get();
        fi->fh = reinterpret_cast<uint64_t>(h.release());
        if (Log::enabled(LogLevel::all))
            Log::trace(std::string("FUSE TRACE result op=create path=") + path +
                  " fh=" + std::to_string(fi->fh));
        if (Log::enabled(LogLevel::all))
            Log::trace(std::string("FUSE handle op=create path=") + path +
                   " fh=" + std::to_string(fi->fh) +
                   " read_ptr=" + std::to_string(reinterpret_cast<uintptr_t>(read_ptr)) +
                   " write_id=" +
                   std::to_string(write_ptr ? write_ptr->diagnostic_id() : 0));
        return 0;
    });
}

int op_read(const char* path, char* buf, size_t size, off_t off, fuse_file_info* fi) {
    trace_request("read", path, fi);
    FuseLatency latency{"read", path};
    if (Log::enabled(LogLevel::all))
        Log::trace(std::string("FUSE TRACE argument op=read size=") + std::to_string(size) +
              " offset=" + std::to_string(off));
    return guarded("read", [&] {
        if (off < 0)
            return -EINVAL;
        auto* h = handle(fi);
        if (h && h->write) {
            h->write->commit();
            h->path = path;
            h->read = fs().open_read(h->path);
        }
        auto r = h && h->read ? h->read : fs().open_read(path);
        auto n = r->read(static_cast<uint64_t>(off),
                         {reinterpret_cast<uint8_t*>(buf), size}, {},
                         fs().io_cancellation_flag());
        if (Log::enabled(LogLevel::all) && n) {
            auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(buf), n);
            if (Log::enabled(LogLevel::all))
                Log::trace(std::string("READ egress path=") + path +
                       " offset=" + std::to_string(off) +
                       " length=" + std::to_string(n) +
                       " sha256=" + to_string(sha256(bytes)) +
                       " all_zero=" +
                       std::to_string(std::all_of(bytes.begin(), bytes.end(),
                                                  [](uint8_t b) { return b == 0; })
                                          ? 1
                                          : 0));
        }
        if (Log::enabled(LogLevel::all))
            Log::trace(std::string("FUSE TRACE result op=read path=") + path +
                   " bytes=" + std::to_string(n));
        return static_cast<int>(n);
    });
}

int op_write(const char* path, const char* buf, size_t size, off_t off, fuse_file_info* fi) {
    trace_request("write", path, fi);
    FuseLatency latency{"write", path};
    if (Log::enabled(LogLevel::all))
        Log::trace(std::string("FUSE TRACE argument op=write size=") + std::to_string(size) +
              " offset=" + std::to_string(off));
    return guarded("write", [&] {
        auto* h = handle(fi);
        if (!h || !h->write)
            return -EBADF;
        if (off < 0)
            return -EINVAL;
        uint64_t offset = static_cast<uint64_t>(off);
        if ((fi->flags & O_APPEND) != 0)
            offset = h->write->size();
        auto n = h->write->write(offset, {reinterpret_cast<const uint8_t*>(buf), size});
        h->read.reset();
        if (Log::enabled(LogLevel::all))
            Log::trace(std::string("FUSE TRACE result op=write path=") + path +
                  " bytes=" + std::to_string(n));
        return static_cast<int>(n);
    });
}

int op_truncate(const char* path, off_t size, fuse_file_info* fi) {
    trace_request("truncate", path, fi);
    FuseLatency latency{"truncate", path};
    if (Log::enabled(LogLevel::all))
        Log::trace(std::string("FUSE TRACE argument op=truncate size=") + std::to_string(size));
    return guarded("truncate", [&] {
        if (size < 0)
            return -EINVAL;
        if (auto* h = handle(fi); h && h->write)
            h->write->truncate(static_cast<uint64_t>(size));
        else
            fs().truncate_file(path, static_cast<uint64_t>(size));
        return 0;
    });
}

int op_flush(const char* path, fuse_file_info* fi) {
    trace_request("flush", path, fi);
    FuseLatency latency{"flush", path};
    return guarded("flush", [&] {
        if (auto* h = handle(fi); h && h->write)
            h->write->commit();
        if (Log::enabled(LogLevel::all))
            Log::trace(std::string("FUSE TRACE result op=flush path=") + path + " rc=0");
        return 0;
    });
}

int op_fsync(const char* path, int, fuse_file_info* fi) {
    trace_request("fsync", path, fi);
    FuseLatency latency{"fsync", path};
    return guarded("fsync", [&] {
        if (auto* h = handle(fi); h && h->write)
            h->write->commit();
        return 0;
    });
}

int op_release(const char* path, fuse_file_info* fi) {
    trace_request("release", path, fi);
    FuseLatency latency{"release", path};
    auto* h = handle(fi);
    if (!h)
        return 0;
    int rc = 0;
    try {
        if (h->write)
            h->write->commit();
    } catch (const std::exception& e) {
        rc = fail("release", e);
    }
    delete h;
    fi->fh = 0;
    if (Log::enabled(LogLevel::all))
        Log::trace(std::string("FUSE TRACE result op=release path=") + path +
              " rc=" + std::to_string(rc));
    return rc;
}

int op_statfs(const char* path, struct statvfs* st) {
    trace_request("statfs", path);
    FuseLatency latency{"statfs", path};
    return guarded("statfs", [&] {
        auto [total, used] = fs().logical_capacity();
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
    if (Log::enabled(LogLevel::all))
        Log::trace(std::string("FUSE TRACE init ") + request_identity() +
              " process_uid=" + std::to_string(getuid()) +
              " process_gid=" + std::to_string(getgid()) +
              " process_euid=" + std::to_string(geteuid()) +
              " process_egid=" + std::to_string(getegid()));
    const auto& policy = fs().node().config().filesystem;
    const auto seconds = [](std::chrono::milliseconds value) {
        return std::chrono::duration<double>(value).count();
    };
    // Keep file-content caching conservative across opens, but allow short
    // namespace/attribute caches. Zero timeouts make macFUSE bounce every
    // lookup/stat through userspace and can turn a pathname failure into a
    // kernel/userspace request storm.
    cfg->kernel_cache = 0;
    cfg->entry_timeout = seconds(policy.entry_timeout);
    cfg->attr_timeout = seconds(policy.attr_timeout);
    cfg->negative_timeout = seconds(policy.negative_timeout);
    Log::debug("FUSE cache policy entry_timeout_ms=" +
               std::to_string(policy.entry_timeout.count()) +
               " attr_timeout_ms=" + std::to_string(policy.attr_timeout.count()) +
               " negative_timeout_ms=" + std::to_string(policy.negative_timeout.count()) +
               " kernel_cache=0");
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
} // namespace

int run_fuse(FileSystem& filesystem, const std::filesystem::path& mount_path, bool allow_other,
             std::function<void()> request_shutdown) {
    auto mount = mount_path.string();
    const std::string options = allow_other ? "default_permissions,allow_other,fsname=macha"
                                            : "default_permissions,fsname=macha";

    char cwd[4096]{};
    std::string cwd_text = getcwd(cwd, sizeof(cwd)) ? cwd : "<getcwd failed>";
    if (Log::enabled(LogLevel::all))
        Log::trace(std::string("FUSE TRACE mount begin path=") + mount + " options=" + options +
              " process_uid=" + std::to_string(getuid()) +
              " process_gid=" + std::to_string(getgid()) +
              " process_euid=" + std::to_string(geteuid()) +
              " process_egid=" + std::to_string(getegid()) + " cwd=" + cwd_text);
    struct stat mount_stat {};
    if (lstat(mount.c_str(), &mount_stat) == 0) {
        if (Log::enabled(LogLevel::all))
            Log::trace(std::string("FUSE TRACE mountpoint before mount mode=") +
                  octal(static_cast<uint64_t>(mount_stat.st_mode & 07777)) +
                  " uid=" + std::to_string(mount_stat.st_uid) +
                  " gid=" + std::to_string(mount_stat.st_gid));
    } else {
        Log::warn(std::string("FUSE TRACE mountpoint lstat failed errno=") +
                  std::to_string(errno) + " message=" + std::strerror(errno));
    }

    // fuse_main() hides the session handle until it returns, which prevents a
    // filesystem callback blocked in remote I/O from observing daemon shutdown.
    // Use the equivalent high-level lifecycle directly so a watcher can observe
    // fuse_session_exit() (set by libfuse's signal handlers) and cooperatively
    // cancel outstanding mounted-filesystem writes.
    std::vector<std::string> fuse_arg_storage{"macha", "-o", options};
    std::vector<char*> fuse_argv;
    fuse_argv.reserve(fuse_arg_storage.size());
    for (auto& arg : fuse_arg_storage)
        fuse_argv.push_back(arg.data());
    struct fuse_args args = FUSE_ARGS_INIT(static_cast<int>(fuse_argv.size()), fuse_argv.data());
    auto ops = operations();
    struct fuse* instance = fuse_new(&args, &ops, sizeof(ops), &filesystem);
    if (!instance) {
        fuse_opt_free_args(&args);
        return 3;
    }
    if (fuse_mount(instance, mount.c_str()) != 0) {
        fuse_destroy(instance);
        fuse_opt_free_args(&args);
        return 4;
    }

    auto* session = fuse_get_session(instance);
    if (fuse_set_signal_handlers(session) != 0) {
        fuse_unmount(instance);
        fuse_destroy(instance);
        fuse_opt_free_args(&args);
        return 6;
    }

    filesystem.reset_io_cancellation();
    std::jthread shutdown_watcher([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (fuse_session_exited(session)) {
                Log::debug("shutdown: FUSE session exit observed; requesting service shutdown");
                filesystem.request_io_cancellation();
                if (request_shutdown)
                    request_shutdown();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    Log::debug("shutdown: entering FUSE main loop");
    // FUSE_USE_VERSION=31 gives the stable fuse_loop_mt(fuse, clone_fd)
    // interface on libfuse3/macFUSE while retaining the concurrent callbacks
    // previously selected by fuse_main() (we deliberately did not pass -s).
    int loop_rc = fuse_loop_mt(instance, 0);
    filesystem.request_io_cancellation();
    if (request_shutdown)
        request_shutdown();
    shutdown_watcher.request_stop();
    if (shutdown_watcher.joinable())
        shutdown_watcher.join();

    fuse_remove_signal_handlers(session);
    fuse_unmount(instance);
    fuse_destroy(instance);
    fuse_opt_free_args(&args);

    const int rc = loop_rc == 0 ? 0 : 8;
    if (Log::enabled(LogLevel::all))
        Log::trace(std::string("FUSE TRACE mount end rc=") + std::to_string(rc));
    Log::debug("shutdown: FUSE main loop returned rc=" + std::to_string(rc));
    return rc;
}

} // namespace macha
