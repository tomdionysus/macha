// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace macha {

struct EpubCreator {
    std::string name;
    // MARC relator code when the file gives one ("aut", "edt", "ill", "trl"),
    // else empty.
    std::string role;
    std::string file_as;
    auto operator<=>(const EpubCreator&) const = default;
};

// A series the book belongs to.
struct EpubCollection {
    std::string name;
    std::optional<double> position;
    auto operator<=>(const EpubCollection&) const = default;
};

struct EpubCover {
    Bytes bytes;
    std::string mime_type;
};

struct EpubMetadata {
    std::string title;
    // Document order.
    std::vector<EpubCreator> creators;
    // Every dc:identifier text, raw, trimmed.
    std::vector<std::string> identifiers;
    // Normalised from any identifier that is an ISBN-10 or ISBN-13: hyphens
    // and spaces stripped, urn:isbn: and isbn: prefixes dropped,
    // opf:scheme="ISBN" honoured, ISBN-10 converted to 13 with the check
    // digit recomputed. A bad check digit is not an ISBN.
    std::optional<std::string> isbn13;
    // First dc:language, trimmed.
    std::optional<std::string> language;
    std::optional<std::string> publisher;
    // First dc:date without an opf:event, else the first dc:date; raw text.
    std::optional<std::string> date;
    // dc:description: entities decoded, tags stripped when it holds markup,
    // whitespace collapsed.
    std::optional<std::string> description;
    // From calibre:series (+ calibre:series_index) and from EPUB 3
    // belongs-to-collection with collection-type "series" (or no type) and
    // group-position; deduplicated by name.
    std::vector<EpubCollection> series;
    // The EPUB 3 manifest item with "cover-image" in its properties; else the
    // EPUB 2 <meta name="cover" content="<manifest id>"> item; else none.
    std::optional<EpubCover> cover;
};

struct EpubReadLimits {
    // Any one of container.xml or the OPF, uncompressed.
    size_t max_member_bytes{1024 * 1024};
    size_t max_cover_bytes{16 * 1024 * 1024};
    // Everything inflated by one call.
    size_t max_total_bytes{32 * 1024 * 1024};
};

// Anything that is not a readable EPUB: no zip signature, zip64, an encrypted
// or unsupported-method member, a truncated member, a member over the limit,
// no container.xml, no rootfile, no <package> or <metadata>.
class EpubError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// `read(offset, destination)` returns the bytes read at that offset, short
// at the end. The reader fetches only the end-of-central-directory record
// (searched within the last 66 KiB), the central directory, and the local
// headers and data of META-INF/container.xml, the OPF it names and the
// cover image. The cover is left empty rather than failing when it is
// missing, oversized or unreadable.
EpubMetadata read_epub_metadata(
    uint64_t size,
    const std::function<size_t(uint64_t offset, std::span<uint8_t> destination)>& read,
    const EpubReadLimits& limits = {});

// The same over a host file.
EpubMetadata read_epub_metadata(const std::filesystem::path& path,
                                const EpubReadLimits& limits = {});

// The ISBN-13 of an identifier text that is an ISBN-10 or ISBN-13, else none.
std::optional<std::string> normalise_isbn13(std::string_view text);

} // namespace macha
