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
#include <unistd.h>

#if defined(__linux__)
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

void prepare_fuse_mountpoint(const std::filesystem::path& mount_path, const FuseConfig& config) {
    const auto mount = mount_path.string();
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

} // namespace macha
