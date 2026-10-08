// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage/sealed_journal.hpp"

#include "crypto.hpp"
#include "durable_file.hpp"
#include "log.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace macha {
namespace {

void write_all(int fd, std::span<const uint8_t> bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        const auto n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            throw std::runtime_error(std::string("journal write failed: ") +
                                     std::strerror(errno));
        done += static_cast<size_t>(n);
    }
}

bool pread_all(int fd, std::span<uint8_t> bytes, uint64_t offset) {
    size_t done = 0;
    while (done < bytes.size()) {
        const auto n = ::pread(fd, bytes.data() + done, bytes.size() - done,
                               static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

void fsync_checked(int fd) {
    int rc;
    do { rc = ::fsync(fd); } while (rc != 0 && errno == EINTR);
    if (rc != 0)
        throw std::runtime_error(std::string("journal sync failed: ") + std::strerror(errno));
}

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24U) | (static_cast<uint32_t>(p[1]) << 16U) |
           (static_cast<uint32_t>(p[2]) << 8U) | static_cast<uint32_t>(p[3]);
}

constexpr uint64_t frame_header = 4 + 12 + 16;

} // namespace

SealedJournal::SealedJournal(std::filesystem::path path, std::array<uint8_t, 32> key,
                             std::array<uint8_t, 8> aad, uint32_t max_frame)
    : path_(std::move(path)), key_(key), aad_(aad), max_frame_(max_frame) {
    // A file read by offset and never replayed (the ledger's nodes) appends
    // after what is already there.
    std::error_code ec;
    if (const auto size = std::filesystem::file_size(path_, ec); !ec)
        bytes_ = size;
}

SealedJournal::~SealedJournal() {
    close_fd();
}

int SealedJournal::fd() const {
    if (fd_ < 0) {
        fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd_ < 0)
            throw std::runtime_error("cannot open journal " + path_.string() + ": " +
                                     std::strerror(errno));
    }
    return fd_;
}

void SealedJournal::close_fd() const noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

size_t SealedJournal::replay(const std::function<void(std::span<const uint8_t>)>& apply,
                             uint32_t max_read_frame, std::string_view what) {
    close_fd();
    frames_ = 0;
    bytes_ = 0;
    if (!std::filesystem::exists(path_))
        return 0;
    const int fd = ::open(path_.c_str(), O_RDWR);
    if (fd < 0)
        throw std::runtime_error("cannot read " + std::string(what) + " " + path_.string());
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const auto saved = errno;
        ::close(fd);
        throw std::runtime_error("cannot stat " + std::string(what) + ": " +
                                 std::strerror(saved));
    }
    const uint64_t file_size = static_cast<uint64_t>(st.st_size);
    uint64_t offset = 0;
    uint64_t durable = 0;
    while (offset + 4 <= file_size) {
        std::array<uint8_t, 4> length_bytes{};
        if (!pread_all(fd, length_bytes, offset))
            break;
        const uint32_t length = read_be32(length_bytes.data());
        if (length > max_read_frame)
            break;
        const uint64_t frame_size = frame_header + static_cast<uint64_t>(length);
        if (offset > std::numeric_limits<uint64_t>::max() - frame_size ||
            offset + frame_size > file_size)
            break;
        std::array<uint8_t, 12> nonce{};
        std::array<uint8_t, 16> tag{};
        Bytes ciphertext(length);
        if (!pread_all(fd, nonce, offset + 4) || !pread_all(fd, tag, offset + 16) ||
            !pread_all(fd, ciphertext, offset + 32))
            break;
        try {
            apply(aes_gcm_open(key_, nonce, tag, ciphertext, aad_));
        } catch (const std::exception& error) {
            Log::warn(std::string(what) + " recovered path=" + path_.string() +
                      " offset=" + std::to_string(offset) + " reason=" + error.what());
            break;
        }
        offset += frame_size;
        durable = offset;
        ++frames_;
    }
    if (durable != file_size) {
        if (::ftruncate(fd, static_cast<off_t>(durable)) != 0) {
            const auto error = errno;
            ::close(fd);
            throw std::runtime_error("cannot truncate " + std::string(what) + ": " +
                                     std::strerror(error));
        }
        fsync_checked(fd);
    }
    if (::close(fd) != 0)
        throw std::runtime_error("cannot close " + std::string(what));
    bytes_ = durable;
    return frames_;
}

namespace {
std::vector<uint8_t> seal_frame(std::span<const uint8_t, 32> key, std::span<const uint8_t> aad,
                                std::span<const uint8_t> plaintext, uint32_t max_frame,
                                const std::filesystem::path& path) {
    const auto sealed = aes_gcm_seal(key, plaintext, aad);
    if (sealed.ciphertext.size() > max_frame)
        throw std::runtime_error("journal frame too large: " + path.string());
    const auto length = static_cast<uint32_t>(sealed.ciphertext.size());
    std::vector<uint8_t> frame;
    frame.reserve(frame_header + sealed.ciphertext.size());
    frame.push_back(static_cast<uint8_t>(length >> 24U));
    frame.push_back(static_cast<uint8_t>(length >> 16U));
    frame.push_back(static_cast<uint8_t>(length >> 8U));
    frame.push_back(static_cast<uint8_t>(length));
    frame.insert(frame.end(), sealed.nonce.begin(), sealed.nonce.end());
    frame.insert(frame.end(), sealed.tag.begin(), sealed.tag.end());
    frame.insert(frame.end(), sealed.ciphertext.begin(), sealed.ciphertext.end());
    return frame;
}
} // namespace

void SealedJournal::append(std::span<const uint8_t> plaintext) {
    (void)append_unsynced(plaintext);
    sync();
}

uint64_t SealedJournal::append_unsynced(std::span<const uint8_t> plaintext) {
    const auto frame = seal_frame(key_, aad_, plaintext, max_frame_, path_);
    const auto offset = bytes_;
    write_all(fd(), frame);
    ++frames_;
    bytes_ += frame.size();
    return offset;
}

void SealedJournal::sync() {
    if (fd_ >= 0)
        fsync_checked(fd_);
}

Bytes SealedJournal::read_at(uint64_t offset) const {
    std::array<uint8_t, 4> length_bytes{};
    if (offset + frame_header > bytes_ || !pread_all(fd(), length_bytes, offset))
        throw std::runtime_error("no journal frame at offset " + std::to_string(offset));
    const uint32_t length = read_be32(length_bytes.data());
    if (length > max_frame_ || offset + frame_header + length > bytes_)
        throw std::runtime_error("journal frame at offset " + std::to_string(offset) +
                                 " runs past the end");
    std::array<uint8_t, 12> nonce{};
    std::array<uint8_t, 16> tag{};
    Bytes ciphertext(length);
    if (!pread_all(fd(), nonce, offset + 4) || !pread_all(fd(), tag, offset + 16) ||
        !pread_all(fd(), ciphertext, offset + 32))
        throw std::runtime_error("short journal frame at offset " + std::to_string(offset));
    return aes_gcm_open(key_, nonce, tag, ciphertext, aad_);
}

void SealedJournal::reset() {
    close_fd();
    durable_replace_file(path_, {});
    frames_ = 0;
    bytes_ = 0;
}

} // namespace macha
