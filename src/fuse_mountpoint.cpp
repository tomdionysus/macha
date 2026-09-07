// SPDX-License-Identifier: GPL-3.0-or-later
#include "fuse_mountpoint.hpp"
#include "log.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#endif

namespace macha {

MountTableProbe probe_macha_mountpoint(const std::string& mount) {
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

    errno = 0;
    std::ifstream input("/proc/self/mountinfo");
    if (!input.is_open())
        return {MountTableState::probe_error, errno ? errno : EIO};

    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string id, parent, device, root, mounted;
        if (!(fields >> id >> parent >> device >> root >> mounted)) continue;
        if (unescape_mount_field(mounted) != mount) continue;
        const auto separator = line.find(" - ");
        if (separator == std::string::npos)
            return {MountTableState::probe_error, EPROTO};
        const auto tail = line.substr(separator + 3);
        std::istringstream type_fields(tail);
        std::string type, source;
        type_fields >> type >> source;
        const bool ours = source == "macha" || type == "fuse.macha" || type == "fuse3.macha";
        return {ours ? MountTableState::macha_fuse : MountTableState::other, 0};
    }
    if (input.bad())
        return {MountTableState::probe_error, errno ? errno : EIO};
    return {MountTableState::missing, 0};
#elif defined(__APPLE__)
    errno = 0;
    struct statfs* mounts = nullptr;
    const int count = getmntinfo(&mounts, MNT_NOWAIT);
    if (count <= 0)
        return {MountTableState::probe_error, errno ? errno : EIO};
    for (int i = 0; i < count; ++i) {
        if (mount == mounts[i].f_mntonname) {
            const std::string source = mounts[i].f_mntfromname;
            const bool ours = source == "macha" || source.starts_with("macha@");
            return {ours ? MountTableState::macha_fuse : MountTableState::other, 0};
        }
    }
    return {MountTableState::missing, 0};
#else
    (void)mount;
    return {MountTableState::probe_error, ENOTSUP};
#endif
}

namespace {

MountpointPreparation g_mountpoint_preparation;

void recover_stale_mount(const std::string& mount, const FuseConfig& config);

} // namespace

const MountpointPreparation& fuse_mountpoint_preparation() {
    return g_mountpoint_preparation;
}

MountpointPreparation guard_covered_mountpoint(const std::filesystem::path& mount_path,
                                               bool fail_closed) {
    MountpointPreparation out;
    std::error_code ec;
    std::filesystem::create_directories(mount_path, ec);
    for (const auto& entry : std::filesystem::directory_iterator(mount_path, ec)) {
        (void)entry;
        ++out.stray_entries;
    }
    if (out.stray_entries)
        Log::error("stray entries under the FUSE mountpoint path=" + mount_path.string() +
                   " count=" + std::to_string(out.stray_entries) +
                   ": they are on the host filesystem, not in Macha, and the mount will cover"
                   " them (inspect with: mount --bind / /mnt/rootview)");

#if defined(__linux__)
    if (fail_closed) {
        const int fd = ::open(mount_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            out.immutable_error = std::strerror(errno);
        } else {
            int flags = 0;
            if (::ioctl(fd, FS_IOC_GETFLAGS, &flags) != 0) {
                out.immutable_error = std::strerror(errno);
            } else if (flags & FS_IMMUTABLE_FL) {
                out.immutable = true;
            } else {
                flags |= FS_IMMUTABLE_FL;
                if (::ioctl(fd, FS_IOC_SETFLAGS, &flags) != 0)
                    out.immutable_error = std::strerror(errno);
                else
                    out.immutable = true;
            }
            ::close(fd);
        }
        if (!out.immutable)
            Log::warn("FUSE mountpoint cannot be made immutable path=" + mount_path.string() +
                      " error=" + out.immutable_error +
                      ": root can still write into the host directory while the mount is"
                      " absent");
    }
#else
    (void)fail_closed;
#endif
    return out;
}

void prepare_fuse_mountpoint(const std::filesystem::path& mount_path, const FuseConfig& config) {
    recover_stale_mount(mount_path.string(), config);
    // The covered directory is a trap while no mount covers it: a writer that
    // arrives before the mount (an rsync started 25 s after the daemon, while
    // local services were still coming up) fills the host disk with files the
    // mount then hides -- 52 GB on a shared host's root disk, 2026-09-07. A
    // mode change does not stop root; the immutable flag does, and it stays
    // in place across restarts so the pre-mount window is closed for good.
    g_mountpoint_preparation = guard_covered_mountpoint(mount_path, config.fail_closed_mountpoint);
}

namespace {

void recover_stale_mount(const std::string& mount, const FuseConfig& config) {
    const auto probe = probe_macha_mountpoint(mount);
    if (probe.state == MountTableState::missing)
        return;
    if (probe.state == MountTableState::probe_error)
        throw std::runtime_error("cannot inspect FUSE mountpoint " + mount + ": " +
                                 std::string(std::strerror(probe.error)));
    if (probe.state == MountTableState::other)
        throw std::runtime_error("refusing to unmount non-Macha filesystem at " + mount);
    if (!config.unmount_if_mounted)
        throw std::runtime_error("Macha FUSE mount already exists at " + mount +
                                 " (set fuse.unmount_if_mounted: true to recover it)");

    Log::warn("recovering stale Macha FUSE mount path=" + mount);
#if defined(__linux__)
    if (::umount2(mount.c_str(), MNT_DETACH) != 0)
        throw std::runtime_error("cannot unmount stale Macha FUSE mount " + mount + ": " +
                                 std::string(std::strerror(errno)));
#elif defined(__APPLE__)
    if (::unmount(mount.c_str(), MNT_FORCE) != 0)
        throw std::runtime_error("cannot unmount stale Macha FUSE mount " + mount + ": " +
                                 std::string(std::strerror(errno)));
#else
    throw std::runtime_error("stale FUSE unmount is unsupported on this platform");
#endif

    for (int attempt = 0; attempt < 20; ++attempt) {
        const auto after = probe_macha_mountpoint(mount);
        if (after.state == MountTableState::missing)
            return;
        if (after.state == MountTableState::other)
            throw std::runtime_error("mountpoint changed to a non-Macha filesystem during recovery");
        if (after.state == MountTableState::probe_error)
            throw std::runtime_error("cannot verify stale FUSE unmount");
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    throw std::runtime_error("stale Macha FUSE mount remained mounted after unmount");
}

} // namespace

} // namespace macha
