// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Which extents of a torrent's payload are published, kept in the job's
// staging directory. The disk backend appends as each extent is verified and
// stored; the ingest reads it to commit files by naming extents, not copying.
// Trusted no further than the commit proves: the retention barrier refuses a
// manifest naming objects the cluster lacks, and the ingest then copies.

#include "contract/thread_safety.hpp"
#include "metadata/metadata.hpp"
#include "types.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
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

    // Every file with a recorded extent, by path relative to the save path.
    // A torn final line or unparsable line is ignored.
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
    // Held across the journal file's write and fsync; guards no state, it
    // keeps appended lines whole.
    IoMutex mutex_;
    int fd_{-1}; // opened at construction, fixed after

};

} // namespace macha
