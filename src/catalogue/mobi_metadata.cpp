// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalogue/mobi_metadata.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string_view>
#include <utility>

namespace macha {
namespace {

// Text helpers.

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && (is_space(value.front()) || value.front() == '\0'))
        value.remove_prefix(1);
    while (!value.empty() && (is_space(value.back()) || value.back() == '\0'))
        value.remove_suffix(1);
    return value;
}

// Runs of whitespace become one space; none at either end.
std::string collapse_whitespace(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    bool pending = false;
    for (char c : value) {
        if (is_space(c)) {
            pending = !out.empty();
            continue;
        }
        if (pending)
            out += ' ';
        pending = false;
        out += c;
    }
    return out;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

void append_utf8(std::string& out, uint32_t code_point) {
    if (code_point < 0x80) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        out += static_cast<char>(0xC0 | (code_point >> 6));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else if (code_point < 0x10000) {
        out += static_cast<char>(0xE0 | (code_point >> 12));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code_point >> 18));
        out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    }
}

// The five XML entities, &nbsp; and numeric character references; anything
// else stays verbatim.
std::string decode_entities(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    size_t p = 0;
    while (p < text.size()) {
        const char c = text[p];
        if (c != '&') {
            out += c;
            ++p;
            continue;
        }
        const auto semicolon = text.find(';', p + 1);
        if (semicolon == std::string_view::npos || semicolon - p > 12) {
            out += c;
            ++p;
            continue;
        }
        const auto body = text.substr(p + 1, semicolon - p - 1);
        bool decoded = true;
        if (body == "lt") {
            out += '<';
        } else if (body == "gt") {
            out += '>';
        } else if (body == "amp") {
            out += '&';
        } else if (body == "quot") {
            out += '"';
        } else if (body == "apos") {
            out += '\'';
        } else if (body == "nbsp") {
            out += ' ';
        } else if (body.size() > 1 && body[0] == '#') {
            uint64_t code_point = 0;
            bool numeric = true;
            if (body[1] == 'x' || body[1] == 'X') {
                const auto digits = body.substr(2);
                numeric = !digits.empty();
                for (char d : digits) {
                    const int v = hex_value(d);
                    if (v < 0) {
                        numeric = false;
                        break;
                    }
                    code_point = code_point * 16 + static_cast<uint64_t>(v);
                }
            } else {
                for (char d : body.substr(1)) {
                    if (d < '0' || d > '9') {
                        numeric = false;
                        break;
                    }
                    code_point = code_point * 10 + static_cast<uint64_t>(d - '0');
                }
            }
            const bool valid = numeric && code_point != 0 && code_point <= 0x10FFFF &&
                               !(code_point >= 0xD800 && code_point <= 0xDFFF);
            if (valid)
                append_utf8(out, static_cast<uint32_t>(code_point));
            else
                decoded = false;
        } else {
            decoded = false;
        }
        if (decoded) {
            p = semicolon + 1;
        } else {
            out += c;
            ++p;
        }
    }
    return out;
}

// A '<' that opens a tag: a name, a closing slash or a declaration follows,
// and a '>' ends it. "1 < 2" is not markup.
bool opens_tag(std::string_view text, size_t p) {
    if (p + 1 >= text.size())
        return false;
    const auto next = static_cast<unsigned char>(text[p + 1]);
    if (!std::isalpha(next) && next != '/' && next != '!')
        return false;
    return text.find('>', p + 1) != std::string_view::npos;
}

bool looks_like_markup(std::string_view text) {
    for (auto p = text.find('<'); p != std::string_view::npos; p = text.find('<', p + 1))
        if (opens_tag(text, p))
            return true;
    return false;
}

std::string strip_markup(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    size_t p = 0;
    while (p < text.size()) {
        if (text[p] != '<' || !opens_tag(text, p)) {
            out += text[p++];
            continue;
        }
        out += ' ';
        p = text.find('>', p + 1) + 1;
    }
    return out;
}

// CP1252 0x80-0x9F; the five unassigned bytes keep their Latin-1 code point.
constexpr uint16_t cp1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

std::string cp1252_to_utf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x80)
            out += c;
        else if (byte < 0xA0)
            append_utf8(out, cp1252_high[byte - 0x80]);
        else
            append_utf8(out, byte);
    }
    return out;
}

// PDB and MOBI.

using ReadFunction = std::function<size_t(uint64_t, std::span<uint8_t>)>;

struct Source {
    uint64_t size;
    const ReadFunction& read;
};

constexpr size_t pdb_header_size = 78;
constexpr size_t palmdoc_header_size = 16;
constexpr uint32_t text_encoding_cp1252 = 1252;
constexpr uint32_t no_index = 0xFFFFFFFF;
constexpr uint32_t exth_present_flag = 0x40;

enum ExthType : uint32_t {
    exth_author = 100,
    exth_publisher = 101,
    exth_description = 103,
    exth_isbn = 104,
    exth_date = 106,
    exth_asin = 113,
    exth_kf8_boundary = 121,
    exth_cover_offset = 201,
    exth_thumbnail_offset = 202,
    exth_updated_title = 503,
    exth_language = 524,
};

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

std::string_view as_text(std::span<const uint8_t> bytes) {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

Bytes fetch(const Source& source, uint64_t offset, size_t length) {
    if (offset > source.size || length > source.size - offset)
        throw MobiError("truncated mobi");
    Bytes out(length);
    size_t got = 0;
    while (got < length) {
        const auto n = source.read(offset + got, std::span<uint8_t>(out).subspan(got));
        if (n == 0 || n > length - got)
            throw MobiError("truncated mobi");
        got += n;
    }
    return out;
}

struct Record {
    uint64_t offset;
    uint64_t length;
};

struct Database {
    std::string name;
    std::vector<Record> records;
};

Database read_database(const Source& source) {
    if (source.size < pdb_header_size)
        throw MobiError("not a pdb");
    const auto header = fetch(source, 0, pdb_header_size);
    if (as_text(std::span<const uint8_t>(header).subspan(60, 8)) != "BOOKMOBI")
        throw MobiError("not a mobi");
    const size_t count = be16(header.data() + 76);
    if (count == 0)
        throw MobiError("no records");
    const uint64_t table_end = pdb_header_size + static_cast<uint64_t>(count) * 8;
    if (table_end > source.size)
        throw MobiError("truncated record table");
    const auto table = fetch(source, pdb_header_size, count * 8);

    Database database;
    const auto name = as_text(std::span<const uint8_t>(header).first(32));
    database.name = std::string(name.substr(0, std::min(name.find('\0'), name.size())));
    database.records.resize(count);
    uint64_t previous = table_end;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t offset = be32(table.data() + i * 8);
        if (offset < previous)
            throw MobiError("record offsets out of order");
        if (offset > source.size)
            throw MobiError("record outside the file");
        database.records[i].offset = offset;
        previous = offset;
    }
    for (size_t i = 0; i < count; ++i) {
        const auto end = i + 1 < count ? database.records[i + 1].offset : source.size;
        database.records[i].length = end - database.records[i].offset;
    }
    return database;
}

struct ExthRecord {
    uint32_t type;
    std::string data;
};

// One record 0: the PalmDOC header, the MOBI header and its EXTH block.
struct MobiHeader {
    bool cp1252{};
    uint32_t version{};
    uint32_t locale{};
    // Absolute record index, or no_index.
    uint32_t first_image{no_index};
    std::string full_name;
    std::vector<ExthRecord> exth;

    const ExthRecord* find(uint32_t type) const {
        for (const auto& record : exth)
            if (record.type == type)
                return &record;
        return nullptr;
    }

    std::optional<uint32_t> number(uint32_t type) const {
        const auto* record = find(type);
        if (!record || record->data.size() < 4)
            return std::nullopt;
        return be32(reinterpret_cast<const uint8_t*>(record->data.data()));
    }
};

// `base` is the database index of this record 0: image indices in a KF8
// header count from it.
MobiHeader parse_header(std::span<const uint8_t> record, uint32_t base) {
    if (record.size() < palmdoc_header_size + 8)
        throw MobiError("record 0 too short");
    const auto* mobi = record.data() + palmdoc_header_size;
    if (as_text(record.subspan(palmdoc_header_size, 4)) != "MOBI")
        throw MobiError("no mobi header");
    const uint64_t header_length = be32(mobi + 4);
    if (header_length < 8 || palmdoc_header_size + header_length > record.size())
        throw MobiError("mobi header runs past record 0");
    const auto field = [&](size_t offset) -> std::optional<uint32_t> {
        if (offset + 4 > header_length)
            return std::nullopt;
        return be32(mobi + offset);
    };

    MobiHeader header;
    header.cp1252 = field(12).value_or(0) == text_encoding_cp1252;
    header.version = field(20).value_or(0);
    header.locale = field(76).value_or(0);
    if (const auto first = field(92); first && *first != no_index) {
        const uint64_t absolute = static_cast<uint64_t>(*first) + base;
        if (absolute < no_index)
            header.first_image = static_cast<uint32_t>(absolute);
    }
    const auto convert = [&](std::string_view text) {
        return header.cp1252 ? cp1252_to_utf8(text) : std::string(text);
    };

    const auto name_offset = field(68);
    const auto name_length = field(72);
    if (name_offset && name_length) {
        if (static_cast<uint64_t>(*name_offset) + *name_length > record.size())
            throw MobiError("full name runs past record 0");
        header.full_name = convert(as_text(record.subspan(*name_offset, *name_length)));
    }

    if ((field(112).value_or(0) & exth_present_flag) == 0)
        return header;
    const uint64_t exth_start = palmdoc_header_size + header_length;
    if (exth_start + 12 > record.size())
        throw MobiError("exth runs past record 0");
    const auto* exth = record.data() + exth_start;
    if (as_text(record.subspan(exth_start, 4)) != "EXTH")
        throw MobiError("no exth header");
    const uint64_t exth_length = be32(exth + 4);
    const uint32_t count = be32(exth + 8);
    if (exth_length < 12 || exth_start + exth_length > record.size())
        throw MobiError("exth runs past record 0");
    uint64_t p = 12;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 8 > exth_length)
            throw MobiError("exth record runs past the block");
        const uint32_t type = be32(exth + p);
        const uint64_t length = be32(exth + p + 4);
        if (length < 8 || p + length > exth_length)
            throw MobiError("exth record runs past the block");
        header.exth.push_back(
            {type, std::string(as_text(record.subspan(exth_start + p + 8, length - 8)))});
        p += length;
    }
    // Strings follow the text encoding; numeric records are read raw.
    for (auto& entry : header.exth) {
        if (entry.type == exth_kf8_boundary || entry.type == exth_cover_offset ||
            entry.type == exth_thumbnail_offset)
            continue;
        entry.data = convert(entry.data);
    }
    return header;
}

Bytes read_record(const Source& source, const Record& record, size_t limit) {
    if (record.length > limit)
        throw MobiError("record over the limit");
    return fetch(source, record.offset, static_cast<size_t>(record.length));
}

std::optional<std::string> locale_language(uint32_t locale) {
    switch (locale & 0xFF) {
    case 4: return "zh";
    case 7: return "de";
    case 9: return "en";
    case 10: return "es";
    case 12: return "fr";
    case 16: return "it";
    case 17: return "ja";
    case 19: return "nl";
    case 22: return "pt";
    case 25: return "ru";
    default: return std::nullopt;
    }
}

std::optional<std::string> image_mime(std::span<const uint8_t> bytes) {
    const auto text = as_text(bytes);
    if (text.starts_with("\xFF\xD8\xFF"))
        return "image/jpeg";
    if (text.starts_with("\x89PNG\r\n\x1A\n"))
        return "image/png";
    if (text.starts_with("GIF87a") || text.starts_with("GIF89a"))
        return "image/gif";
    if (text.starts_with("BM"))
        return "image/bmp";
    return std::nullopt;
}

std::optional<EpubCover> read_cover(const Source& source, const Database& database,
                                    const MobiHeader& header, const MobiReadLimits& limits) {
    if (header.first_image == no_index)
        return std::nullopt;
    for (const uint32_t type : {exth_cover_offset, exth_thumbnail_offset}) {
        const auto offset = header.number(type);
        if (!offset || *offset == no_index)
            continue;
        const uint64_t index = static_cast<uint64_t>(header.first_image) + *offset;
        if (index == 0 || index >= database.records.size())
            continue;
        const auto& record = database.records[index];
        if (record.length == 0 || record.length > limits.max_cover_bytes)
            continue;
        try {
            auto bytes = read_record(source, record, limits.max_cover_bytes);
            auto mime = image_mime(bytes);
            if (!mime)
                continue;
            return EpubCover{std::move(bytes), std::move(*mime)};
        } catch (const MobiError&) {
            // A bad cover does not make a bad book.
        }
    }
    return std::nullopt;
}

std::string header_title(const MobiHeader& header) {
    if (const auto* updated = header.find(exth_updated_title)) {
        auto title = collapse_whitespace(trim(updated->data));
        if (!title.empty())
            return title;
    }
    return collapse_whitespace(trim(header.full_name));
}

std::optional<std::string> first_text(const MobiHeader& header, uint32_t type) {
    for (const auto& record : header.exth) {
        if (record.type != type)
            continue;
        const auto text = trim(record.data);
        if (!text.empty())
            return std::string(text);
    }
    return std::nullopt;
}

} // namespace

MobiMetadata read_mobi_metadata(uint64_t size,
                                const std::function<size_t(uint64_t, std::span<uint8_t>)>& read,
                                const MobiReadLimits& limits) {
    const Source source{size, read};
    const auto database = read_database(source);
    const auto record0 = read_record(source, database.records[0], limits.max_header_bytes);
    auto header = parse_header(record0, 0);

    MobiMetadata out;
    out.kf8 = header.version == 8;

    // A combined KF7 + KF8 file: the record after the BOUNDARY record is a
    // KF8 record 0, read only when the first header has no title.
    if (const auto boundary = header.number(exth_kf8_boundary);
        boundary && *boundary > 0 && static_cast<uint64_t>(*boundary) + 1 < database.records.size()) {
        const auto& marker = database.records[*boundary];
        if (marker.length >= 8 && as_text(fetch(source, marker.offset, 8)) == "BOUNDARY") {
            out.kf8 = true;
            if (header_title(header).empty()) {
                const auto kf8_record0 = read_record(source, database.records[*boundary + 1],
                                                     limits.max_header_bytes);
                header = parse_header(kf8_record0, *boundary + 1);
            }
        }
    }

    out.title = header_title(header);
    if (out.title.empty())
        out.title = collapse_whitespace(trim(database.name));

    for (const auto& record : header.exth) {
        if (record.type != exth_author)
            continue;
        const auto author = trim(record.data);
        if (!author.empty())
            out.authors.emplace_back(author);
    }

    for (const uint32_t type : {exth_isbn, exth_asin}) {
        for (const auto& record : header.exth) {
            if (record.type != type)
                continue;
            if (auto isbn = normalise_isbn13(record.data)) {
                out.isbn13 = std::move(isbn);
                break;
            }
        }
        if (out.isbn13)
            break;
    }

    out.publisher = first_text(header, exth_publisher);
    out.date = first_text(header, exth_date);
    if (auto description = first_text(header, exth_description)) {
        auto text = collapse_whitespace(*description);
        text = collapse_whitespace(looks_like_markup(text) ? decode_entities(strip_markup(text))
                                                           : decode_entities(text));
        if (!text.empty())
            out.description = std::move(text);
    }
    out.language = first_text(header, exth_language);
    if (!out.language)
        out.language = locale_language(header.locale);

    out.cover = read_cover(source, database, header, limits);
    return out;
}

MobiMetadata read_mobi_metadata(const std::filesystem::path& path, const MobiReadLimits& limits) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw MobiError("cannot open " + path.string());
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
        throw MobiError("cannot size " + path.string());
    const std::function<size_t(uint64_t, std::span<uint8_t>)> read =
        [&file](uint64_t offset, std::span<uint8_t> destination) -> size_t {
        file.clear();
        file.seekg(static_cast<std::streamoff>(offset));
        if (!file)
            return 0;
        file.read(reinterpret_cast<char*>(destination.data()),
                  static_cast<std::streamsize>(destination.size()));
        return static_cast<size_t>(file.gcount());
    };
    return read_mobi_metadata(size, read, limits);
}

} // namespace macha
