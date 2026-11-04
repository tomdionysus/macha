// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue/epub_metadata.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace macha {

// Strings are UTF-8 whatever the file's text encoding (UTF-8 or CP1252).
struct MobiMetadata {
    // EXTH 503 (updated title), else the MOBI header's full name, else the
    // PDB name.
    std::string title;
    // Every EXTH 100, in order, trimmed; empty ones dropped.
    std::vector<std::string> authors;
    // EXTH 104 (ISBN) normalised by normalise_isbn13, else an EXTH 113 (ASIN)
    // that is an ISBN.
    std::optional<std::string> isbn13;
    // EXTH 101.
    std::optional<std::string> publisher;
    // EXTH 103: tags stripped when it holds markup, entities decoded,
    // whitespace collapsed.
    std::optional<std::string> description;
    // EXTH 106, trimmed.
    std::optional<std::string> date;
    // EXTH 524, else the MOBI header locale for the common languages.
    std::optional<std::string> language;
    // Always empty: MOBI carries no series.
    std::vector<EpubCollection> series;
    // The image record at first image index + EXTH 201 (cover offset), else +
    // EXTH 202 (thumbnail offset); a JPEG, PNG, GIF or BMP by its magic bytes.
    std::optional<EpubCover> cover;
    // MOBI version 8, or a combined KF7 + KF8 file (EXTH 121 naming a
    // BOUNDARY record).
    bool kf8{};
};

struct MobiReadLimits {
    // Record 0 (PalmDOC, MOBI and EXTH headers and the full name).
    size_t max_header_bytes{1024 * 1024};
    size_t max_cover_bytes{16 * 1024 * 1024};
};

// Anything that is not a readable MOBI: not a BOOKMOBI database, a truncated
// or disordered record table, no MOBI header, a header, full name or EXTH
// record running past record 0, record 0 over the limit.
class MobiError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// `read(offset, destination)` returns the bytes read at that offset, short
// at the end. The reader fetches only the PDB header and record table, record
// 0 in one read, the start of a KF8 boundary record and the record after it,
// and the cover image record; never the text records. DRM-encrypted books
// still yield their metadata. The cover is left empty rather than failing
// when it is missing, oversized, unreadable or not a known image.
MobiMetadata read_mobi_metadata(
    uint64_t size,
    const std::function<size_t(uint64_t offset, std::span<uint8_t> destination)>& read,
    const MobiReadLimits& limits = {});

// The same over a host file.
MobiMetadata read_mobi_metadata(const std::filesystem::path& path,
                                const MobiReadLimits& limits = {});

} // namespace macha
