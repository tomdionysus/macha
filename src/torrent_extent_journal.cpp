// SPDX-License-Identifier: GPL-3.0-or-later
#include "torrent_extent_journal.hpp"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

namespace macha {
namespace {

// One line per extent, fields tab-separated, path last:
//   v1 <id hex> <offset> <length> <file size> <relative path>\n
constexpr std::string_view line_version = "v1";

bool parse_u64(std::string_view text, uint64_t& out) {
    if (text.empty()) return false;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && end == text.data() + text.size();
}

} // namespace

std::filesystem::path TorrentExtentJournal::path_for(const std::filesystem::path& save_path) {
    return save_path / ".macha-extents";
}

std::map<std::string, TorrentExtentJournal::File>
TorrentExtentJournal::load(const std::filesystem::path& save_path) {
    std::map<std::string, File> out;
    std::ifstream in(path_for(save_path), std::ios::binary);
    if (!in) return out;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t start = 0;
    while (start < content.size()) {
        const auto end = content.find('\n', start);
        if (end == std::string::npos) break; // torn final line
        const std::string_view line(content.data() + start, end - start);
        start = end + 1;
        std::string_view fields[6];
        size_t field = 0, from = 0;
        for (; field < 5; ++field) {
            const auto tab = line.find('\t', from);
            if (tab == std::string_view::npos) break;
            fields[field] = line.substr(from, tab - from);
            from = tab + 1;
        }
        if (field != 5 || fields[0] != line_version) continue;
        fields[5] = line.substr(from);
        const auto id_bytes = unhex(std::string(fields[1]));
        Extent extent;
        uint64_t file_size = 0;
        if (!id_bytes || id_bytes->size() != extent.id.bytes.size() ||
            !parse_u64(fields[2], extent.offset) || !parse_u64(fields[3], extent.length) ||
            !parse_u64(fields[4], file_size) || fields[5].empty() || extent.length == 0)
            continue;
        std::memcpy(extent.id.bytes.data(), id_bytes->data(), extent.id.bytes.size());
        auto& file = out[std::string(fields[5])];
        // A file's recorded size is the size the torrent says it has; a line
        // that disagrees belongs to some other torrent layout and is ignored.
        if (!file.extents.empty() && file.size != file_size) continue;
        file.size = file_size;
        file.extents[extent.offset] = extent;
    }
    return out;
}

std::optional<std::vector<ExtentRef>> TorrentExtentJournal::manifest(const File& file,
                                                                     uint64_t size) {
    if (file.size != size) return std::nullopt;
    std::vector<ExtentRef> out;
    uint64_t next = 0;
    for (const auto& [offset, extent] : file.extents) {
        if (offset != next) return std::nullopt;
        out.push_back(ExtentRef{extent.offset, extent.length, extent.id, false});
        next += extent.length;
    }
    if (next != size) return std::nullopt;
    return out;
}

TorrentExtentJournal::TorrentExtentJournal(std::filesystem::path save_path)
    : path_(path_for(save_path)) {
    std::filesystem::create_directories(save_path);
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd_ < 0)
        throw std::runtime_error("cannot open torrent extent journal " + path_.string() + ": " +
                                 std::strerror(errno));
}

TorrentExtentJournal::~TorrentExtentJournal() {
    if (fd_ >= 0) ::close(fd_);
}

void TorrentExtentJournal::append(const std::string& relative_path, uint64_t file_size,
                                  const Extent& extent) {
    if (relative_path.empty() || relative_path.find_first_of("\t\n") != std::string::npos)
        throw std::invalid_argument("torrent path cannot be recorded in the extent journal");
    std::ostringstream line;
    line << line_version << '\t' << to_string(extent.id) << '\t' << extent.offset << '\t'
         << extent.length << '\t' << file_size << '\t' << relative_path << '\n';
    const auto text = line.str();
    std::lock_guard lock(mutex_);
    size_t done = 0;
    while (done < text.size()) {
        const auto wrote = ::write(fd_, text.data() + done, text.size() - done);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0)
            throw std::runtime_error("torrent extent journal write failed: " +
                                     std::string(std::strerror(errno)));
        done += static_cast<size_t>(wrote);
    }
    while (::fsync(fd_) != 0) {
        if (errno != EINTR)
            throw std::runtime_error("torrent extent journal fsync failed: " +
                                     std::string(std::strerror(errno)));
    }
}

} // namespace macha
