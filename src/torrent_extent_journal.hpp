// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The record of which extents of a torrent's payload have already been
// published to the store, kept beside the payload in the job's staging
// directory. The torrent disk backend (plugin) appends to it the moment an
// extent's whole range has verified and been stored; the ingest (core) reads
// it when the download is finished and commits each file by naming its
// extents instead of copying the file's bytes a second time.
//
// Stage 2 of TODO/2026-09-23-torrent-disk-backend-plan.md. Nothing here is
// trusted beyond what the commit itself proves: the DATA retention barrier on
// the commit refuses a manifest naming objects the cluster does not hold, and
// the ingest then falls back to copying.

#include "metadata.hpp"
#include "types.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace macha {

class TorrentExtentJournal {
  public:
    struct Extent {
        uint64_t offset{};
        uint64_t length{};
        ObjectId id{};
    };
    struct File {
        uint64_t size{};
        std::map<uint64_t, Extent> extents; // by offset
    };

    static std::filesystem::path path_for(const std::filesystem::path& save_path);

    // Every file with at least one recorded extent, keyed by its path
    // relative to the save path. A torn final line (a crash mid-append) is
    // ignored; so is any line that does not parse.
    static std::map<std::string, File> load(const std::filesystem::path& save_path);

    // The manifest for a file of `size` bytes, if the recorded extents cover
    // [0, size) exactly, contiguously and without overlap; otherwise nothing.
    static std::optional<std::vector<ExtentRef>> manifest(const File&, uint64_t size);

    explicit TorrentExtentJournal(std::filesystem::path save_path);
    ~TorrentExtentJournal();
    TorrentExtentJournal(const TorrentExtentJournal&) = delete;
    TorrentExtentJournal& operator=(const TorrentExtentJournal&) = delete;

    // Appends and fsyncs one extent. Throws on I/O failure; a path containing
    // a newline or a tab cannot be recorded and is refused.
    void append(const std::string& relative_path, uint64_t file_size, const Extent&);

  private:
    std::filesystem::path path_;
    std::mutex mutex_;
    int fd_{-1};
};

} // namespace macha
