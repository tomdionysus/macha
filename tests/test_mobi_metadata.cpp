// SPDX-License-Identifier: GPL-3.0-or-later
// The MOBI metadata reader over PDB files built here: the MOBI header and
// EXTH fields, text encodings, the cover record, KF8 and combined files, and
// the refusals of malformed databases.
#include "catalogue/media_catalogue.hpp"
#include "catalogue/mobi_metadata.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <fstream>
#include <random>

using namespace macha;
using namespace macha::test_support;

namespace {

constexpr uint32_t mobi_header_length = 232;
constexpr uint32_t utf8 = 65001;
constexpr uint32_t cp1252 = 1252;
constexpr uint32_t none = 0xFFFFFFFF;

void put16(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void put32(Bytes& out, uint32_t value) {
    put16(out, value >> 16);
    put16(out, value & 0xFFFF);
}

void put_text(Bytes& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}

void set32(Bytes& out, size_t at, uint32_t value) {
    out[at] = static_cast<uint8_t>(value >> 24);
    out[at + 1] = static_cast<uint8_t>(value >> 16);
    out[at + 2] = static_cast<uint8_t>(value >> 8);
    out[at + 3] = static_cast<uint8_t>(value);
}

uint32_t get32(const Bytes& in, size_t at) {
    return (static_cast<uint32_t>(in[at]) << 24) | (static_cast<uint32_t>(in[at + 1]) << 16) |
           (static_cast<uint32_t>(in[at + 2]) << 8) | static_cast<uint32_t>(in[at + 3]);
}

Bytes text_bytes(std::string_view text) {
    return Bytes(text.begin(), text.end());
}

Bytes number(uint32_t value) {
    Bytes out;
    put32(out, value);
    return out;
}

struct Exth {
    uint32_t type;
    Bytes data;
};

Exth exth(uint32_t type, std::string_view text) {
    return {type, text_bytes(text)};
}

Exth exth_number(uint32_t type, uint32_t value) {
    return {type, number(value)};
}

struct Header {
    std::string full_name{"The Full Name"};
    uint32_t encoding{utf8};
    uint32_t version{6};
    uint32_t locale{9};
    uint16_t encryption{};
    uint32_t first_image{none};
    bool with_exth{true};
    std::vector<Exth> exth;
};

// A record 0: PalmDOC header, a 232-byte MOBI header, the EXTH block, the
// full name, padding to four bytes.
Bytes record0(const Header& header) {
    Bytes exth_block;
    if (header.with_exth) {
        Bytes records;
        for (const auto& entry : header.exth) {
            put32(records, entry.type);
            put32(records, static_cast<uint32_t>(entry.data.size() + 8));
            records.insert(records.end(), entry.data.begin(), entry.data.end());
        }
        put_text(exth_block, "EXTH");
        put32(exth_block, static_cast<uint32_t>(records.size() + 12));
        put32(exth_block, static_cast<uint32_t>(header.exth.size()));
        exth_block.insert(exth_block.end(), records.begin(), records.end());
        while (exth_block.size() % 4)
            exth_block.push_back(0);
    }

    Bytes out;
    put16(out, 2);
    put16(out, 0);
    put32(out, 12000);
    put16(out, 3);
    put16(out, 4096);
    put16(out, header.encryption);
    put16(out, 0);

    Bytes mobi(mobi_header_length, 0);
    std::copy_n("MOBI", 4, mobi.begin());
    set32(mobi, 4, mobi_header_length);
    set32(mobi, 8, 2);
    set32(mobi, 12, header.encoding);
    set32(mobi, 16, 0x12345678);
    set32(mobi, 20, header.version);
    const auto name_offset = static_cast<uint32_t>(16 + mobi_header_length + exth_block.size());
    set32(mobi, 68, name_offset);
    set32(mobi, 72, static_cast<uint32_t>(header.full_name.size()));
    set32(mobi, 76, header.locale);
    set32(mobi, 92, header.first_image);
    set32(mobi, 112, header.with_exth ? 0x50 : 0x10);
    out.insert(out.end(), mobi.begin(), mobi.end());
    out.insert(out.end(), exth_block.begin(), exth_block.end());
    put_text(out, header.full_name);
    out.push_back(0);
    out.push_back(0);
    while (out.size() % 4)
        out.push_back(0);
    return out;
}

// A BOOKMOBI database of the records in order.
Bytes pdb(const std::vector<Bytes>& records, std::string_view name = "Test_Book",
          std::string_view type = "BOOKMOBI") {
    Bytes out(32, 0);
    std::copy_n(name.begin(), std::min<size_t>(name.size(), 31), out.begin());
    put16(out, 0);
    put16(out, 0);
    for (int i = 0; i < 6; ++i)
        put32(out, 0);
    put_text(out, type);
    put32(out, 0);
    put32(out, 0);
    put16(out, static_cast<uint32_t>(records.size()));
    uint32_t offset = static_cast<uint32_t>(out.size() + records.size() * 8 + 2);
    for (size_t i = 0; i < records.size(); ++i) {
        put32(out, offset);
        put32(out, static_cast<uint32_t>(2 * i));
        offset += static_cast<uint32_t>(records[i].size());
    }
    put16(out, 0);
    for (const auto& record : records)
        out.insert(out.end(), record.begin(), record.end());
    return out;
}

Bytes jpeg_bytes(size_t n) {
    Bytes out{0xFF, 0xD8, 0xFF, 0xE0};
    while (out.size() < n)
        out.push_back(static_cast<uint8_t>(out.size()));
    return out;
}

Bytes png_bytes(size_t n) {
    Bytes out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    while (out.size() < n)
        out.push_back(static_cast<uint8_t>(out.size()));
    return out;
}

Bytes text_record(size_t i, size_t n = 64) {
    Bytes out;
    put_text(out, "text record " + std::to_string(i) + " ");
    while (out.size() < n)
        out.push_back('x');
    return out;
}

// Record 0, two text records, then the images: the first image is record 3.
Bytes book(Header header, const std::vector<Bytes>& images = {}) {
    if (!images.empty() && header.first_image == none)
        header.first_image = 3;
    std::vector<Bytes> records{record0(header), text_record(1), text_record(2)};
    records.insert(records.end(), images.begin(), images.end());
    return pdb(records);
}

MobiMetadata read_bytes(const Bytes& file, const MobiReadLimits& limits = {}) {
    const std::function<size_t(uint64_t, std::span<uint8_t>)> read =
        [&file](uint64_t offset, std::span<uint8_t> destination) -> size_t {
        if (offset >= file.size())
            return 0;
        const auto n = std::min(destination.size(), file.size() - static_cast<size_t>(offset));
        std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(offset), n, destination.begin());
        return n;
    };
    return read_mobi_metadata(file.size(), read, limits);
}

bool read_throws(const Bytes& file, const MobiReadLimits& limits = {}) {
    try {
        read_bytes(file, limits);
        return false;
    } catch (const MobiError&) {
        return true;
    }
}

size_t record_offset(const Bytes& file, size_t index) {
    return get32(file, 78 + index * 8);
}

} // namespace

MACHA_FAST_TEST("mobi_metadata", test_title_from_full_name_without_exth) {
    Header header;
    header.with_exth = false;
    const auto metadata = read_bytes(book(header));
    CHECK(metadata.title == "The Full Name");
    CHECK(metadata.authors.empty());
    CHECK(!metadata.isbn13);
    CHECK(!metadata.publisher);
    CHECK(!metadata.description);
    CHECK(!metadata.date);
    CHECK(metadata.language == "en");
    CHECK(metadata.series.empty());
    CHECK(!metadata.cover);
    CHECK(!metadata.kf8);
}

MACHA_FAST_TEST("mobi_metadata", test_the_internal_database_name_is_no_title) {
    // No full name: the database's internal name is not the book's title.
    Header header;
    header.full_name = "";
    header.exth = {exth(100, "Someone")};
    CHECK(read_bytes(book(header)).title.empty());
    // An updated title that only repeats it ("PratWher") is none either.
    header.full_name = "Test_Book";
    header.exth = {exth(503, "Test_Book")};
    CHECK(read_bytes(book(header)).title.empty());
}

MACHA_FAST_TEST("mobi_metadata", test_updated_title_overrides_full_name) {
    Header header;
    header.exth = {exth(503, "  The Updated\n Title  ")};
    CHECK(read_bytes(book(header)).title == "The Updated Title");

    // An empty 503 leaves the full name.
    header.exth = {exth(503, "   ")};
    CHECK(read_bytes(book(header)).title == "The Full Name");
}

MACHA_FAST_TEST("mobi_metadata", test_authors_in_order_trimmed_empty_dropped) {
    Header header;
    header.exth = {exth(100, " Terry Pratchett "), exth(101, "Corgi"), exth(100, ""),
                   exth(100, "Neil Gaiman\n")};
    const auto metadata = read_bytes(book(header));
    REQUIRE(metadata.authors.size() == 2);
    CHECK(metadata.authors[0] == "Terry Pratchett");
    CHECK(metadata.authors[1] == "Neil Gaiman");
}

MACHA_FAST_TEST("mobi_metadata", test_isbn10_with_hyphens_normalised) {
    Header header;
    header.exth = {exth(104, "0-306-40615-2")};
    CHECK(read_bytes(book(header)).isbn13 == "9780306406157");

    // A bad check digit is dropped; a later good one is taken.
    header.exth = {exth(104, "0-306-40615-3")};
    CHECK(!read_bytes(book(header)).isbn13);
    header.exth = {exth(104, "0-306-40615-3"), exth(104, "978-3-16-148410-0")};
    CHECK(read_bytes(book(header)).isbn13 == "9783161484100");
}

MACHA_FAST_TEST("mobi_metadata", test_asin_used_only_when_it_is_an_isbn) {
    Header header;
    header.exth = {exth(113, "B000FC1PJI")};
    CHECK(!read_bytes(book(header)).isbn13);
    header.exth = {exth(113, "0306406152")};
    CHECK(read_bytes(book(header)).isbn13 == "9780306406157");
    // EXTH 104 wins over an ASIN.
    header.exth = {exth(113, "0306406152"), exth(104, "9783161484100")};
    CHECK(read_bytes(book(header)).isbn13 == "9783161484100");
}

MACHA_FAST_TEST("mobi_metadata", test_publisher_description_and_date) {
    Header header;
    header.exth = {
        exth(101, " Corgi "),
        exth(103, "<p>A tale of &quot;dragons&quot;</p>\n<p>&amp; the&nbsp;Watch&#x2026;</p>"),
        exth(106, " 1989-11-01T00:00:00+00:00 ")};
    const auto metadata = read_bytes(book(header));
    CHECK(metadata.publisher == "Corgi");
    CHECK(metadata.description == "A tale of \"dragons\" & the Watch\xE2\x80\xA6");
    CHECK(metadata.date == "1989-11-01T00:00:00+00:00");

    // Plain text keeps its angle brackets but has its entities decoded.
    header.exth = {exth(103, "1 < 2 &amp; 3 > 2")};
    CHECK(read_bytes(book(header)).description == "1 < 2 & 3 > 2");

    // Markup with no text is no description.
    header.exth = {exth(103, "<p> </p>")};
    CHECK(!read_bytes(book(header)).description);
}

MACHA_FAST_TEST("mobi_metadata", test_cp1252_text_becomes_utf8) {
    Header header;
    header.encoding = cp1252;
    header.full_name = "Caf\xE9";
    header.exth = {exth(100, "Ren\xE9 \x93Q\x94"), exth(101, "\x80 Books")};
    const auto metadata = read_bytes(book(header));
    CHECK(metadata.title == "Caf\xC3\xA9");
    REQUIRE(metadata.authors.size() == 1);
    CHECK(metadata.authors[0] == "Ren\xC3\xA9 \xE2\x80\x9CQ\xE2\x80\x9D");
    CHECK(metadata.publisher == "\xE2\x82\xAC Books");

    // UTF-8 text passes through.
    header.encoding = utf8;
    header.full_name = "Caf\xC3\xA9";
    header.exth = {};
    CHECK(read_bytes(book(header)).title == "Caf\xC3\xA9");
}

MACHA_FAST_TEST("mobi_metadata", test_locale_and_exth_language) {
    const std::vector<std::pair<uint32_t, std::string>> locales{
        {9, "en"},  {12, "fr"}, {7, "de"},  {10, "es"}, {16, "it"},
        {19, "nl"}, {22, "pt"}, {25, "ru"}, {17, "ja"}, {4, "zh"}};
    Header header;
    for (const auto& [locale, language] : locales) {
        header.locale = locale;
        CHECK(read_bytes(book(header)).language == language);
    }
    // A sublanguage above the low byte: en-GB is 0x809.
    header.locale = 0x809;
    CHECK(read_bytes(book(header)).language == "en");
    header.locale = 0;
    CHECK(!read_bytes(book(header)).language);
    header.locale = 99;
    CHECK(!read_bytes(book(header)).language);

    header.locale = 9;
    header.exth = {exth(524, " en-GB ")};
    CHECK(read_bytes(book(header)).language == "en-GB");
}

MACHA_FAST_TEST("mobi_metadata", test_cover_by_exth_201) {
    const auto cover = jpeg_bytes(500);
    const auto thumbnail = png_bytes(100);
    Header header;
    header.exth = {exth_number(201, 1), exth_number(202, 0)};
    const auto metadata = read_bytes(book(header, {thumbnail, cover}));
    REQUIRE(metadata.cover.has_value());
    CHECK(metadata.cover->mime_type == "image/jpeg");
    CHECK(metadata.cover->bytes == cover);
}

MACHA_FAST_TEST("mobi_metadata", test_cover_falls_back_to_thumbnail_202) {
    const auto thumbnail = png_bytes(100);
    Header header;
    header.exth = {exth_number(202, 1)};
    auto metadata = read_bytes(book(header, {jpeg_bytes(50), thumbnail}));
    REQUIRE(metadata.cover.has_value());
    CHECK(metadata.cover->mime_type == "image/png");
    CHECK(metadata.cover->bytes == thumbnail);

    // A 201 that is out of range falls back to the 202.
    header.exth = {exth_number(201, 40), exth_number(202, 1)};
    metadata = read_bytes(book(header, {jpeg_bytes(50), thumbnail}));
    REQUIRE(metadata.cover.has_value());
    CHECK(metadata.cover->bytes == thumbnail);
}

MACHA_FAST_TEST("mobi_metadata", test_cover_mime_by_magic) {
    Header header;
    header.exth = {exth_number(201, 0)};
    Bytes gif = text_bytes("GIF89a....");
    Bytes bmp = text_bytes("BM........");
    Bytes other = text_bytes("RIFF....WEBP");
    auto metadata = read_bytes(book(header, {gif}));
    REQUIRE(metadata.cover.has_value());
    CHECK(metadata.cover->mime_type == "image/gif");
    metadata = read_bytes(book(header, {bmp}));
    REQUIRE(metadata.cover.has_value());
    CHECK(metadata.cover->mime_type == "image/bmp");
    CHECK(!read_bytes(book(header, {other})).cover);
}

MACHA_FAST_TEST("mobi_metadata", test_cover_offset_out_of_range_skipped) {
    Header header;
    header.exth = {exth_number(201, 5)};
    auto metadata = read_bytes(book(header, {jpeg_bytes(50)}));
    CHECK(!metadata.cover);
    CHECK(metadata.title == "The Full Name");

    // Offset "none".
    header.exth = {exth_number(201, none)};
    CHECK(!read_bytes(book(header, {jpeg_bytes(50)})).cover);

    // A cover offset record too short to hold a number.
    header.exth = {Exth{201, Bytes{0, 0}}};
    CHECK(!read_bytes(book(header, {jpeg_bytes(50)})).cover);
}

MACHA_FAST_TEST("mobi_metadata", test_oversized_cover_skipped) {
    Header header;
    header.exth = {exth_number(201, 0)};
    const auto file = book(header, {jpeg_bytes(4096)});
    MobiReadLimits limits;
    limits.max_cover_bytes = 4095;
    const auto metadata = read_bytes(file, limits);
    CHECK(!metadata.cover);
    CHECK(metadata.title == "The Full Name");
    limits.max_cover_bytes = 4096;
    CHECK(read_bytes(file, limits).cover.has_value());
}

MACHA_FAST_TEST("mobi_metadata", test_no_first_image_means_no_cover) {
    Header header;
    header.exth = {exth_number(201, 0)};
    // The image is there but the header says there are none.
    const auto file = pdb({record0(header), text_record(1), jpeg_bytes(64)});
    const auto metadata = read_bytes(file);
    CHECK(!metadata.cover);
    CHECK(metadata.title == "The Full Name");
}

MACHA_FAST_TEST("mobi_metadata", test_drm_encrypted_still_yields_metadata) {
    Header header;
    header.encryption = 2;
    header.exth = {exth(100, "Author"), exth(503, "Locked"), exth_number(201, 0)};
    const auto metadata = read_bytes(book(header, {jpeg_bytes(80)}));
    CHECK(metadata.title == "Locked");
    REQUIRE(metadata.authors.size() == 1);
    CHECK(metadata.authors[0] == "Author");
    CHECK(metadata.cover.has_value());
}

MACHA_FAST_TEST("mobi_metadata", test_kf8_version_8) {
    Header header;
    header.version = 8;
    header.exth = {exth(503, "KF8 Book")};
    const auto metadata = read_bytes(book(header));
    CHECK(metadata.kf8);
    CHECK(metadata.title == "KF8 Book");

    header.version = 6;
    CHECK(!read_bytes(book(header)).kf8);
}

MACHA_FAST_TEST("mobi_metadata", test_combined_kf7_kf8_file) {
    // 0 KF7 record 0, 1-2 text, 3 image, 4 BOUNDARY, 5 KF8 record 0, 6 text.
    Header kf7;
    kf7.first_image = 3;
    kf7.exth = {exth(503, "Combined"), exth(100, "Author"), exth_number(121, 4),
                exth_number(201, 0)};
    Header kf8;
    kf8.version = 8;
    kf8.full_name = "KF8 Name";
    kf8.exth = {exth(503, "KF8 Title")};
    const auto cover = jpeg_bytes(100);
    const auto file = pdb({record0(kf7), text_record(1), text_record(2), cover,
                           text_bytes("BOUNDARY"), record0(kf8), text_record(6)});
    const auto metadata = read_bytes(file);
    CHECK(metadata.kf8);
    CHECK(metadata.title == "Combined");
    REQUIRE(metadata.authors.size() == 1);
    REQUIRE(metadata.cover.has_value());
    CHECK(metadata.cover->bytes == cover);

    // EXTH 121 naming a record that is not BOUNDARY is not a combined file.
    kf7.exth = {exth(503, "Combined"), exth_number(121, 2)};
    const auto plain = pdb({record0(kf7), text_record(1), text_record(2), cover,
                            text_bytes("BOUNDARY"), record0(kf8), text_record(6)});
    CHECK(!read_bytes(plain).kf8);

    // A boundary index past the records.
    kf7.exth = {exth(503, "Combined"), exth_number(121, 40)};
    CHECK(!read_bytes(pdb({record0(kf7), text_record(1)})).kf8);
}

MACHA_FAST_TEST("mobi_metadata", test_combined_file_without_kf7_title_reads_kf8_header) {
    // 0 KF7 record 0, 1 text, 2 BOUNDARY, 3 KF8 record 0, 4 text, 5 image.
    Header kf7;
    kf7.full_name = "";
    kf7.exth = {exth_number(121, 2)};
    Header kf8;
    kf8.version = 8;
    kf8.full_name = "From KF8";
    // Image indices in the KF8 header count from its record 0.
    kf8.first_image = 2;
    kf8.locale = 12;
    kf8.exth = {exth(100, "KF8 Author"), exth_number(201, 0)};
    const auto cover = png_bytes(64);
    const auto file = pdb({record0(kf7), text_record(1), text_bytes("BOUNDARY"), record0(kf8),
                           text_record(4), cover});
    const auto metadata = read_bytes(file);
    CHECK(metadata.kf8);
    CHECK(metadata.title == "From KF8");
    REQUIRE(metadata.authors.size() == 1);
    CHECK(metadata.authors[0] == "KF8 Author");
    CHECK(metadata.language == "fr");
    REQUIRE(metadata.cover.has_value());
    CHECK(metadata.cover->bytes == cover);
}

MACHA_FAST_TEST("mobi_metadata", test_not_bookmobi_throws) {
    Header header;
    const std::vector<Bytes> records{record0(header), text_record(1)};
    CHECK(read_throws(pdb(records, "Doc", "TEXtREAd")));
    CHECK(read_throws(pdb(records, "Doc", "BOOKMOBX")));
    CHECK(!read_throws(pdb(records, "Doc", "BOOKMOBI")));
    CHECK(read_throws(Bytes{}));
    CHECK(read_throws(Bytes(77, 0)));
}

MACHA_FAST_TEST("mobi_metadata", test_truncated_record_table_throws) {
    Header header;
    const auto file = book(header);
    // Cut inside the record table.
    CHECK(read_throws(Bytes(file.begin(), file.begin() + 78 + 12)));
    // A count of records the file cannot hold.
    auto inflated = file;
    inflated[76] = 0xFF;
    inflated[77] = 0xFF;
    CHECK(read_throws(inflated));
    // No records.
    auto empty = file;
    empty[76] = 0;
    empty[77] = 0;
    CHECK(read_throws(empty));
}

MACHA_FAST_TEST("mobi_metadata", test_record_offsets_out_of_order_throw) {
    Header header;
    const auto file = book(header);
    auto swapped = file;
    const auto first = record_offset(file, 1);
    const auto second = record_offset(file, 2);
    set32(swapped, 78 + 8, second);
    set32(swapped, 78 + 16, first);
    CHECK(read_throws(swapped));

    // An offset past the end of the file.
    auto outside = file;
    set32(outside, 78 + 16, static_cast<uint32_t>(file.size() + 1));
    CHECK(read_throws(outside));

    // Record 0 overlapping the record table.
    auto overlapping = file;
    set32(overlapping, 78, 40);
    CHECK(read_throws(overlapping));
}

MACHA_FAST_TEST("mobi_metadata", test_exth_record_past_block_throws) {
    Header header;
    header.exth = {exth(100, "Author"), exth(101, "Publisher")};
    const auto file = book(header);
    const auto exth_at = record_offset(file, 0) + 16 + mobi_header_length;
    REQUIRE(get32(file, exth_at) == get32(text_bytes("EXTH"), 0));

    auto long_record = file;
    set32(long_record, exth_at + 12 + 4, 0x1000);
    CHECK(read_throws(long_record));

    // A record shorter than its own header.
    auto short_record = file;
    set32(short_record, exth_at + 12 + 4, 4);
    CHECK(read_throws(short_record));

    // More records counted than the block holds.
    auto counted = file;
    set32(counted, exth_at + 8, 3);
    CHECK(read_throws(counted));

    // A block longer than record 0.
    auto block = file;
    set32(block, exth_at + 4, 0x100000);
    CHECK(read_throws(block));

    // The EXTH flag with no EXTH magic.
    auto magic = file;
    magic[exth_at] = 'X';
    CHECK(read_throws(magic));
}

MACHA_FAST_TEST("mobi_metadata", test_malformed_mobi_header_throws) {
    Header header;
    const auto file = book(header);
    const auto r0 = record_offset(file, 0);

    auto magic = file;
    magic[r0 + 16] = 'X';
    CHECK(read_throws(magic));

    auto length = file;
    set32(length, r0 + 16 + 4, 0x10000);
    CHECK(read_throws(length));

    auto name = file;
    set32(name, r0 + 16 + 72, 0x10000);
    CHECK(read_throws(name));
}

MACHA_FAST_TEST("mobi_metadata", test_record0_over_the_header_limit_throws) {
    Header header;
    const auto file = book(header);
    MobiReadLimits limits;
    limits.max_header_bytes = 64;
    CHECK(read_throws(file, limits));
    CHECK(!read_throws(file));
}

MACHA_FAST_TEST("mobi_metadata", test_reads_only_headers_and_cover) {
    Header header;
    header.exth = {exth(503, "Title"), exth_number(201, 0)};
    const auto cover = jpeg_bytes(1000);
    std::vector<Bytes> records{record0(header)};
    for (size_t i = 1; i <= 8; ++i)
        records.push_back(text_record(i, 64 * 1024));
    header.first_image = static_cast<uint32_t>(records.size());
    records[0] = record0(header);
    records.push_back(cover);
    const auto file = pdb(records);
    size_t total_read = 0;
    const std::function<size_t(uint64_t, std::span<uint8_t>)> read =
        [&](uint64_t offset, std::span<uint8_t> destination) -> size_t {
        if (offset >= file.size())
            return 0;
        const auto n = std::min(destination.size(), file.size() - static_cast<size_t>(offset));
        std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(offset), n, destination.begin());
        total_read += n;
        return n;
    };
    const auto metadata = read_mobi_metadata(file.size(), read);
    CHECK(metadata.title == "Title");
    REQUIRE(metadata.cover.has_value());
    CHECK(total_read == 78 + records.size() * 8 + records[0].size() + cover.size());
}

MACHA_FAST_TEST("mobi_metadata", test_host_file_overload) {
    TempDir dir;
    const auto path = dir.path() / "book.azw3";
    Header header;
    header.version = 8;
    header.exth = {exth(503, "On Disk"), exth(104, "0-306-40615-2"), exth_number(201, 0)};
    const auto cover = jpeg_bytes(2048);
    const auto file = book(header, {cover});
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(file.data()),
                  static_cast<std::streamsize>(file.size()));
    }
    const auto metadata = read_mobi_metadata(path);
    CHECK(metadata.title == "On Disk");
    CHECK(metadata.isbn13 == "9780306406157");
    CHECK(metadata.kf8);
    REQUIRE(metadata.cover.has_value());
    CHECK(metadata.cover->bytes == cover);

    bool threw = false;
    try {
        read_mobi_metadata(dir.path() / "missing.mobi");
    } catch (const MobiError&) {
        threw = true;
    }
    CHECK(threw);
}

MACHA_FAST_TEST("mobi_metadata", test_truncation_and_flip_fuzz_never_crashes) {
    std::mt19937 rng(20261104);
    Header kf7;
    kf7.first_image = 3;
    kf7.encoding = cp1252;
    kf7.exth = {exth(100, "Author \xE9"),   exth(101, "Publisher"), exth(103, "<p>Desc &amp;</p>"),
                exth(104, "0-306-40615-2"), exth(106, "2020"),      exth(503, "Title"),
                exth_number(121, 4),        exth_number(201, 0),    exth_number(202, 0)};
    Header kf8;
    kf8.version = 8;
    kf8.exth = {exth(503, "KF8")};
    const auto whole = pdb({record0(kf7), text_record(1), text_record(2), jpeg_bytes(256),
                            text_bytes("BOUNDARY"), record0(kf8), text_record(6)});
    size_t returned = 0;
    size_t threw = 0;
    const auto outcome = [&](const Bytes& file) {
        try {
            read_bytes(file);
            ++returned;
        } catch (const MobiError&) {
            ++threw;
        }
    };
    for (int i = 0; i < 300; ++i) {
        const auto length = std::uniform_int_distribution<size_t>(0, whole.size())(rng);
        outcome(Bytes(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(length)));
    }
    // Flips anywhere, then flips confined to the headers where they matter.
    const auto r0 = record_offset(whole, 0);
    const auto header_end = r0 + record0(kf7).size();
    for (int i = 0; i < 300; ++i) {
        auto mutated = whole;
        const int flips = 1 + static_cast<int>(rng() % 6);
        for (int f = 0; f < flips; ++f) {
            const auto at = i % 2 ? rng() % mutated.size() : rng() % header_end;
            mutated[at] = static_cast<uint8_t>(rng());
        }
        outcome(mutated);
    }
    CHECK(returned + threw == 600);
    CHECK(returned > 0);
    CHECK(threw > 0);
}

MACHA_FAST_TEST("mobi_metadata", test_book_credits_titles_and_dates_read_as_a_reader_would) {
    using Names = std::vector<std::string>;
    CHECK((split_authors("Pratchett, Terry") == Names{"Terry Pratchett"}));
    CHECK((split_authors("Le Guin, Ursula") == Names{"Ursula Le Guin"}));
    CHECK((split_authors("Terry Pratchett, Stephen Briggs, Tina Hannan") ==
           Names{"Terry Pratchett", "Stephen Briggs", "Tina Hannan"}));
    CHECK((split_authors("Ian Stewart, Jack Cohen") == Names{"Ian Stewart", "Jack Cohen"}));
    CHECK((split_authors("Pratchett, Terry; Baxter, Stephen") ==
           Names{"Terry Pratchett", "Stephen Baxter"}));
    CHECK((split_authors("Pratchett, Terry, Baxter, Stephen") ==
           Names{"Terry Pratchett", "Stephen Baxter"}));
    CHECK((split_authors("Terry Pratchett & Neil Gaiman") ==
           Names{"Terry Pratchett", "Neil Gaiman"}));
    CHECK((split_authors("Tolkien, J.R.R.") == Names{"J.R.R. Tolkien"}));
    CHECK((split_authors("Jean-Luc Picard") == Names{"Jean-Luc Picard"}));
    CHECK(split_authors("Unknown").empty());
    CHECK((split_authors("Unknown & Terry Pratchett") == Names{"Terry Pratchett"}));

    auto [steam, steam_series] =
        book_title_series("Raising Steam: (Discworld novel 40) (Discworld Novels)");
    CHECK(steam == "Raising Steam");
    REQUIRE(steam_series.size() == 1);
    CHECK(steam_series.front().name == "Discworld");
    CHECK(steam_series.front().index == std::optional<double>{40});
    auto [gods, gods_series] = book_title_series("Small Gods: Discworld Novel, A");
    CHECK(gods == "Small Gods");
    REQUIRE(gods_series.size() == 1);
    CHECK(gods_series.front().name == "Discworld");
    CHECK(!gods_series.front().index);
    auto [mort, mort_series] = book_title_series("Mort: A Discworld Novel");
    CHECK(mort == "Mort");
    CHECK(mort_series.size() == 1);
    auto [plain, plain_series] =
        book_title_series("The Science of Discworld 1 - University of Warwick");
    CHECK(plain == "The Science of Discworld 1 - University of Warwick");
    CHECK(plain_series.empty());
    auto [frank, frank_series] = book_title_series("Frankenstein: A Novel");
    CHECK(frank == "Frankenstein");
    CHECK(frank_series.empty());
    auto [novel, novel_series] = book_title_series("A Novel");
    CHECK(novel == "A Novel");
    CHECK(novel_series.empty());

    auto [spider, spider_series] = book_title_series("Spider-Man: Blue");
    CHECK(spider == "Spider-Man: Blue");
    CHECK(spider_series.empty());

    auto disc = openlibrary_series("Discworld novels ; 4");
    REQUIRE(disc.has_value());
    CHECK(disc->name == "Discworld");
    CHECK(disc->index == std::optional<double>{4});
    REQUIRE(openlibrary_series("Discworld series").has_value());
    CHECK(!openlibrary_series("Discworld series")->index);
    CHECK(!openlibrary_series("Corgi books").has_value());
    CHECK(!openlibrary_series("Penguin classics").has_value());

    CHECK(book_date("2005-09-12T23:00:00+00:00") == "2005-09-13");
    CHECK(book_date("2013-11-07T06:00:00+00:00") == "2013-11-07");
    CHECK(book_date("1998-12-31T23:00:00+00:00") == "1999-01-01");
    CHECK(book_date("2005-09-13T01:00:00+02:00") == "2005-09-13");
    CHECK(book_date("2005-09-13T00:30:00Z") == "2005-09-13");
    CHECK(book_date("2005-09-13") == "2005-09-13");
    CHECK(book_date(" 2005 ") == "2005");
}
