// SPDX-License-Identifier: GPL-3.0-or-later
#include "durable_file.hpp"

#include "types.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace macha {
namespace {

void fsync_checked(int fd, const char* what) {
    int rc;
    do { rc = ::fsync(fd); } while (rc != 0 && errno == EINTR);
    if (rc != 0)
        throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

void write_all(int fd, std::string_view contents) {
    size_t done = 0;
    while (done < contents.size()) {
        const auto n = ::write(fd, contents.data() + done, contents.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            throw std::runtime_error(std::string("durable state write failed: ") +
                                     std::strerror(errno));
        done += static_cast<size_t>(n);
    }
}

} // namespace

void durable_replace_file(const std::filesystem::path& path, std::string_view contents) {
    const auto directory = path.parent_path().empty() ? std::filesystem::path(".")
                                                      : path.parent_path();
    std::filesystem::create_directories(directory);
    const auto temp = path.string() + ".tmp." + std::to_string(getpid()) + "." +
                      std::to_string(unix_ms());
    int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        throw std::runtime_error("cannot create durable state " + temp + ": " +
                                 std::strerror(errno));
    try {
        write_all(fd, contents);
        fsync_checked(fd, "cannot sync durable state");
        if (::close(fd) != 0)
            throw std::runtime_error("cannot close durable state: " +
                                     std::string(std::strerror(errno)));
        fd = -1;
        if (::rename(temp.c_str(), path.c_str()) != 0)
            throw std::runtime_error("cannot replace durable state " + path.string() + ": " +
                                     std::strerror(errno));

        const int dir = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
        if (dir < 0)
            throw std::runtime_error("cannot open durable state directory " +
                                     directory.string() + ": " + std::strerror(errno));
        try {
            fsync_checked(dir, "cannot sync durable state directory");
        } catch (...) {
            ::close(dir);
            throw;
        }
        if (::close(dir) != 0)
            throw std::runtime_error("cannot close durable state directory: " +
                                     std::string(std::strerror(errno)));
    } catch (...) {
        if (fd >= 0)
            ::close(fd);
        std::error_code error;
        std::filesystem::remove(temp, error);
        throw;
    }
}

} // namespace macha
