// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalogue/epub_metadata.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <utility>

#include <zlib.h>

namespace macha {
namespace {

// Text helpers.

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && is_space(value.front()))
        value.remove_prefix(1);
    while (!value.empty() && is_space(value.back()))
        value.remove_suffix(1);
    return value;
}

std::string lowercase(std::string_view value) {
    std::string out(value);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
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

std::string_view local_name(std::string_view qualified) {
    const auto colon = qualified.rfind(':');
    return colon == std::string_view::npos ? qualified : qualified.substr(colon + 1);
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

// The five predefined entities and numeric character references; anything
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

std::string percent_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t p = 0; p < text.size(); ++p) {
        if (text[p] == '%' && p + 2 < text.size()) {
            const int high = hex_value(text[p + 1]);
            const int low = hex_value(text[p + 2]);
            if (high >= 0 && low >= 0) {
                out += static_cast<char>(high * 16 + low);
                p += 2;
                continue;
            }
        }
        out += text[p];
    }
    return out;
}

// A decimal number as a series position: sign, digits, optional fraction.
// Locale-independent.
std::optional<double> parse_position(std::string_view text) {
    text = trim(text);
    if (text.empty())
        return std::nullopt;
    size_t p = 0;
    bool negative = false;
    if (text[p] == '+' || text[p] == '-') {
        negative = text[p] == '-';
        ++p;
    }
    double value = 0;
    size_t digits = 0;
    for (; p < text.size() && text[p] >= '0' && text[p] <= '9'; ++p, ++digits)
        value = value * 10 + (text[p] - '0');
    if (p < text.size() && text[p] == '.') {
        ++p;
        double scale = 0.1;
        for (; p < text.size() && text[p] >= '0' && text[p] <= '9'; ++p, ++digits) {
            value += (text[p] - '0') * scale;
            scale /= 10;
        }
    }
    if (digits == 0 || p != text.size())
        return std::nullopt;
    return negative ? -value : value;
}

// XML: a tolerant element reader for the OPF and container.xml. Names are
// local names; text nodes are unnamed children holding decoded text.

struct XmlAttribute {
    std::string name;
    std::string value;
};

struct XmlElement {
    std::string name;
    std::vector<XmlAttribute> attributes;
    std::string text;
    std::vector<XmlElement> children;

    const std::string* find_attribute(std::string_view local) const {
        for (const auto& attribute : attributes)
            if (attribute.name == local)
                return &attribute.value;
        return nullptr;
    }

    std::string_view attribute(std::string_view local) const {
        const auto* value = find_attribute(local);
        return value ? std::string_view(*value) : std::string_view{};
    }

    const XmlElement* child(std::string_view local) const {
        for (const auto& element : children)
            if (element.name == local)
                return &element;
        return nullptr;
    }
};

constexpr size_t xml_max_depth = 256;

std::string_view as_text(std::span<const uint8_t> bytes) {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void append_text(std::vector<XmlElement>& stack, std::string text) {
    if (text.empty())
        return;
    XmlElement node;
    node.text = std::move(text);
    stack.back().children.push_back(std::move(node));
}

// Parses the tag at `p` (just past '<'), pushing the element; returns the
// position after the tag and whether it was self-closing.
std::pair<size_t, bool> parse_open_tag(std::string_view s, size_t p,
                                       std::vector<XmlElement>& stack) {
    const size_t n = s.size();
    size_t name_end = p;
    while (name_end < n && !is_space(s[name_end]) && s[name_end] != '>' && s[name_end] != '/')
        ++name_end;
    XmlElement element;
    element.name = std::string(local_name(s.substr(p, name_end - p)));
    p = name_end;
    bool self_closing = false;
    while (true) {
        while (p < n && is_space(s[p]))
            ++p;
        if (p >= n)
            break;
        if (s[p] == '>') {
            ++p;
            break;
        }
        if (s[p] == '/') {
            ++p;
            if (p < n && s[p] == '>')
                ++p;
            self_closing = true;
            break;
        }
        size_t attribute_end = p;
        while (attribute_end < n && !is_space(s[attribute_end]) && s[attribute_end] != '=' &&
               s[attribute_end] != '>' && s[attribute_end] != '/')
            ++attribute_end;
        if (attribute_end == p) {
            // A stray quote or '=': skip it.
            ++p;
            continue;
        }
        XmlAttribute attribute;
        attribute.name = std::string(local_name(s.substr(p, attribute_end - p)));
        p = attribute_end;
        while (p < n && is_space(s[p]))
            ++p;
        if (p < n && s[p] == '=') {
            ++p;
            while (p < n && is_space(s[p]))
                ++p;
            if (p < n && (s[p] == '"' || s[p] == '\'')) {
                const auto close = s.find(s[p], p + 1);
                if (close == std::string_view::npos) {
                    attribute.value = decode_entities(s.substr(p + 1));
                    p = n;
                } else {
                    attribute.value = decode_entities(s.substr(p + 1, close - p - 1));
                    p = close + 1;
                }
            } else {
                size_t value_end = p;
                while (value_end < n && !is_space(s[value_end]) && s[value_end] != '>')
                    ++value_end;
                attribute.value = decode_entities(s.substr(p, value_end - p));
                p = value_end;
            }
        }
        element.attributes.push_back(std::move(attribute));
    }
    stack.push_back(std::move(element));
    return {p, self_closing};
}

void pop_element(std::vector<XmlElement>& stack) {
    if (stack.size() < 2)
        return;
    XmlElement element = std::move(stack.back());
    stack.pop_back();
    stack.back().children.push_back(std::move(element));
}

// The document: its children are the top-level elements. Never reads past
// the buffer and always advances; malformed input yields a partial tree.
XmlElement parse_xml(std::string_view s) {
    if (s.starts_with("\xEF\xBB\xBF"))
        s.remove_prefix(3);
    const size_t n = s.size();
    std::vector<XmlElement> stack;
    stack.emplace_back();
    size_t p = 0;
    while (p < n) {
        if (s[p] != '<') {
            auto next = s.find('<', p);
            if (next == std::string_view::npos)
                next = n;
            append_text(stack, decode_entities(s.substr(p, next - p)));
            p = next;
            continue;
        }
        const auto rest = s.substr(p);
        if (rest.starts_with("<!--")) {
            const auto end = s.find("-->", p + 4);
            p = end == std::string_view::npos ? n : end + 3;
        } else if (rest.starts_with("<![CDATA[")) {
            const auto end = s.find("]]>", p + 9);
            if (end == std::string_view::npos) {
                append_text(stack, std::string(s.substr(p + 9)));
                p = n;
            } else {
                append_text(stack, std::string(s.substr(p + 9, end - p - 9)));
                p = end + 3;
            }
        } else if (rest.starts_with("<?")) {
            const auto end = s.find("?>", p + 2);
            p = end == std::string_view::npos ? n : end + 2;
        } else if (rest.starts_with("<!")) {
            // DOCTYPE, possibly with an internal subset in brackets.
            int depth = 0;
            p += 2;
            while (p < n) {
                const char c = s[p++];
                if (c == '[')
                    ++depth;
                else if (c == ']')
                    --depth;
                else if (c == '>' && depth <= 0)
                    break;
            }
        } else if (rest.starts_with("</")) {
            const auto end = s.find('>', p + 2);
            p = end == std::string_view::npos ? n : end + 1;
            pop_element(stack);
        } else if (p + 1 < n && !is_space(s[p + 1]) && s[p + 1] != '>' && s[p + 1] != '/') {
            if (stack.size() > xml_max_depth)
                throw EpubError("xml nested too deeply");
            const auto [next, self_closing] = parse_open_tag(s, p + 1, stack);
            p = next;
            if (self_closing)
                pop_element(stack);
        } else {
            // A bare '<': text.
            append_text(stack, "<");
            ++p;
        }
    }
    while (stack.size() > 1)
        pop_element(stack);
    return std::move(stack.front());
}

// Every text node under `element`, in document order.
void gather_text(const XmlElement& element, std::string& out) {
    for (const auto& child : element.children) {
        if (child.name.empty())
            out += child.text;
        else
            gather_text(child, out);
    }
}

std::string all_text(const XmlElement& element) {
    std::string out;
    gather_text(element, out);
    return out;
}

// The first element named `local` anywhere under `element`, depth first.
const XmlElement* find_element(const XmlElement& element, std::string_view local) {
    for (const auto& child : element.children) {
        if (child.name.empty())
            continue;
        if (child.name == local)
            return &child;
        if (const auto* found = find_element(child, local))
            return found;
    }
    return nullptr;
}

void collect_elements(const XmlElement& element, std::string_view local,
                      std::vector<const XmlElement*>& out) {
    for (const auto& child : element.children) {
        if (child.name.empty())
            continue;
        if (child.name == local)
            out.push_back(&child);
        collect_elements(child, local, out);
    }
}

// Zip: the end-of-central-directory record, the central directory, and one
// member at a time through its local header.

using ReadFunction = std::function<size_t(uint64_t, std::span<uint8_t>)>;

struct Source {
    uint64_t size;
    const ReadFunction& read;
};

constexpr uint32_t eocd_signature = 0x06054b50;
constexpr uint32_t eocd64_locator_signature = 0x07064b50;
constexpr uint32_t central_signature = 0x02014b50;
constexpr uint32_t local_signature = 0x04034b50;
constexpr size_t eocd_fixed_size = 22;
constexpr size_t central_fixed_size = 46;
constexpr size_t local_fixed_size = 30;
constexpr size_t max_comment = 65535;

uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

Bytes fetch(const Source& source, uint64_t offset, size_t length) {
    if (offset > source.size || length > source.size - offset)
        throw EpubError("truncated zip");
    Bytes out(length);
    size_t got = 0;
    while (got < length) {
        const auto n = source.read(offset + got, std::span<uint8_t>(out).subspan(got));
        if (n == 0 || n > length - got)
            throw EpubError("truncated zip");
        got += n;
    }
    return out;
}

struct ZipMember {
    uint16_t flags{};
    uint16_t method{};
    uint32_t compressed{};
    uint32_t uncompressed{};
    uint32_t local_offset{};
};

using ZipDirectory = std::map<std::string, ZipMember>;

// Backslashes become slashes; leading "./" and "/" go.
std::string normalise_member_name(std::string_view name) {
    std::string out(name);
    std::replace(out.begin(), out.end(), '\\', '/');
    size_t p = 0;
    while (true) {
        if (out.compare(p, 2, "./") == 0)
            p += 2;
        else if (p < out.size() && out[p] == '/')
            ++p;
        else
            break;
    }
    return out.substr(p);
}

ZipDirectory read_central_directory(const Source& source, const EpubReadLimits& limits) {
    if (source.size < eocd_fixed_size)
        throw EpubError("not a zip");
    const auto tail_length =
        static_cast<size_t>(std::min<uint64_t>(source.size, eocd_fixed_size + max_comment));
    const auto tail_offset = source.size - tail_length;
    const auto tail = fetch(source, tail_offset, tail_length);

    size_t at = tail_length - eocd_fixed_size;
    while (true) {
        if (le32(tail.data() + at) == eocd_signature)
            break;
        if (at == 0)
            throw EpubError("not a zip");
        --at;
    }
    const auto* eocd = tail.data() + at;
    const auto eocd_offset = tail_offset + at;
    if (le16(eocd + 4) != 0 || le16(eocd + 6) != 0)
        throw EpubError("multi-disk zip");
    const uint32_t entries = le16(eocd + 10);
    const auto directory_size = le32(eocd + 12);
    const auto directory_offset = le32(eocd + 16);
    if (entries == 0xFFFF || directory_size == 0xFFFFFFFF || directory_offset == 0xFFFFFFFF)
        throw EpubError("zip64 unsupported");
    if (eocd_offset >= 20) {
        uint32_t before = 0;
        if (at >= 20)
            before = le32(tail.data() + at - 20);
        else
            before = le32(fetch(source, eocd_offset - 20, 4).data());
        if (before == eocd64_locator_signature)
            throw EpubError("zip64 unsupported");
    }
    if (static_cast<uint64_t>(directory_offset) + directory_size > eocd_offset)
        throw EpubError("central directory outside the file");
    if (directory_size > limits.max_total_bytes)
        throw EpubError("central directory over the limit");

    const auto directory = fetch(source, directory_offset, directory_size);
    ZipDirectory members;
    size_t p = 0;
    for (uint32_t i = 0; i < entries; ++i) {
        if (p + central_fixed_size > directory.size())
            throw EpubError("truncated central directory");
        const auto* entry = directory.data() + p;
        if (le32(entry) != central_signature)
            throw EpubError("corrupt central directory");
        ZipMember member;
        member.flags = le16(entry + 8);
        member.method = le16(entry + 10);
        member.compressed = le32(entry + 20);
        member.uncompressed = le32(entry + 24);
        const size_t name_length = le16(entry + 28);
        const size_t extra_length = le16(entry + 30);
        const size_t comment_length = le16(entry + 32);
        member.local_offset = le32(entry + 42);
        if (p + central_fixed_size + name_length > directory.size())
            throw EpubError("truncated central directory");
        if (member.compressed == 0xFFFFFFFF || member.uncompressed == 0xFFFFFFFF ||
            member.local_offset == 0xFFFFFFFF)
            throw EpubError("zip64 unsupported");
        const auto name = as_text(
            std::span<const uint8_t>(directory).subspan(p + central_fixed_size, name_length));
        members.emplace(normalise_member_name(name), member);
        p += central_fixed_size + name_length + extra_length + comment_length;
    }
    return members;
}

Bytes inflate_raw(std::span<const uint8_t> input, size_t expected) {
    if (expected == 0)
        return {};
    if (input.size() > std::numeric_limits<uInt>::max() ||
        expected > std::numeric_limits<uInt>::max())
        throw EpubError("member too large to inflate");
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        throw EpubError("inflate init failed");
    Bytes out(expected);
    stream.next_in = const_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(expected);
    const int result = inflate(&stream, Z_FINISH);
    const auto produced = static_cast<size_t>(stream.total_out);
    inflateEnd(&stream);
    if (result != Z_STREAM_END || produced != expected)
        throw EpubError("corrupt deflate member");
    return out;
}

// The member's bytes, checked against `limit` and the call's remaining
// `budget` before anything is read.
Bytes read_member(const Source& source, const ZipMember& member, size_t limit, size_t& budget) {
    if (member.flags & 1)
        throw EpubError("encrypted member");
    if (member.method != 0 && member.method != 8)
        throw EpubError("unsupported compression method");
    if (member.uncompressed > limit)
        throw EpubError("member over the limit");
    if (member.uncompressed > budget)
        throw EpubError("read over the total limit");
    if (member.method == 0 && member.compressed != member.uncompressed)
        throw EpubError("stored member sizes disagree");
    budget -= member.uncompressed;

    const auto header = fetch(source, member.local_offset, local_fixed_size);
    if (le32(header.data()) != local_signature)
        throw EpubError("corrupt local header");
    const size_t name_length = le16(header.data() + 26);
    const size_t extra_length = le16(header.data() + 28);
    const uint64_t data_offset =
        static_cast<uint64_t>(member.local_offset) + local_fixed_size + name_length + extra_length;
    auto data = fetch(source, data_offset, member.compressed);
    if (member.method == 0)
        return data;
    return inflate_raw(data, member.uncompressed);
}

// OPF.

std::string directory_of(std::string_view path) {
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string{} : std::string(path.substr(0, slash + 1));
}

// `href` resolved against `base` (a directory ending in '/' or empty), with
// "." and ".." segments folded.
std::string resolve_path(std::string_view base, std::string_view href) {
    std::string joined;
    if (href.starts_with('/'))
        joined = std::string(href.substr(1));
    else
        joined = std::string(base) + std::string(href);
    std::vector<std::string> segments;
    size_t p = 0;
    while (p <= joined.size()) {
        auto slash = joined.find('/', p);
        if (slash == std::string::npos)
            slash = joined.size();
        const auto segment = joined.substr(p, slash - p);
        if (segment == "..") {
            if (!segments.empty())
                segments.pop_back();
        } else if (!segment.empty() && segment != ".") {
            segments.push_back(segment);
        }
        p = slash + 1;
    }
    std::string out;
    for (const auto& segment : segments) {
        if (!out.empty())
            out += '/';
        out += segment;
    }
    return out;
}

std::string mime_from_extension(std::string_view path) {
    const auto dot = path.rfind('.');
    const auto extension =
        dot == std::string_view::npos ? std::string{} : lowercase(path.substr(dot + 1));
    if (extension == "jpg" || extension == "jpeg")
        return "image/jpeg";
    if (extension == "png")
        return "image/png";
    if (extension == "gif")
        return "image/gif";
    if (extension == "webp")
        return "image/webp";
    if (extension == "svg")
        return "image/svg+xml";
    return "application/octet-stream";
}

std::string rootfile_path(const XmlElement& container) {
    std::vector<const XmlElement*> rootfiles;
    collect_elements(container, "rootfile", rootfiles);
    const XmlElement* chosen = nullptr;
    for (const auto* rootfile : rootfiles) {
        if (trim(rootfile->attribute("full-path")).empty())
            continue;
        if (rootfile->attribute("media-type") == "application/oebps-package+xml") {
            chosen = rootfile;
            break;
        }
        if (!chosen)
            chosen = rootfile;
    }
    if (!chosen)
        throw EpubError("no rootfile in container.xml");
    return normalise_member_name(trim(chosen->attribute("full-path")));
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

// Markup inside a description's text: tags out, the entities they escaped
// decoded.
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
    return decode_entities(out);
}

struct CoverReference {
    std::string path;
    std::string mime_type;
};

struct Refinement {
    std::string target;
    std::string property;
    std::string scheme;
    std::string text;
};

struct CollectionDraft {
    std::string id;
    std::string name;
    std::string type;
    std::optional<double> position;
};

void add_series(std::vector<EpubCollection>& series, const CollectionDraft& draft) {
    if (draft.name.empty())
        return;
    for (auto& existing : series) {
        if (existing.name != draft.name)
            continue;
        if (!existing.position && draft.position)
            existing.position = draft.position;
        return;
    }
    series.push_back({draft.name, draft.position});
}

struct ParsedOpf {
    EpubMetadata metadata;
    std::optional<CoverReference> cover;
};

ParsedOpf parse_opf(const XmlElement& document, std::string_view opf_directory) {
    const auto* package = find_element(document, "package");
    if (!package)
        throw EpubError("no <package> in the OPF");
    const auto* metadata = package->child("metadata");
    if (!metadata)
        metadata = find_element(*package, "metadata");
    if (!metadata)
        throw EpubError("no <metadata> in the OPF");

    ParsedOpf parsed;
    auto& out = parsed.metadata;
    std::map<std::string, size_t> creator_ids;
    std::vector<Refinement> refinements;
    std::vector<CollectionDraft> collections;
    std::vector<std::pair<std::string, std::string>> identifier_schemes;
    std::optional<std::string> calibre_series;
    std::optional<double> calibre_index;
    std::optional<std::string> cover_id;
    std::optional<std::string> first_date;

    for (const auto& element : metadata->children) {
        const auto& name = element.name;
        if (name.empty())
            continue;
        if (name == "title") {
            if (out.title.empty())
                out.title = collapse_whitespace(all_text(element));
        } else if (name == "creator") {
            EpubCreator creator;
            creator.name = collapse_whitespace(all_text(element));
            creator.role = std::string(trim(element.attribute("role")));
            creator.file_as = collapse_whitespace(element.attribute("file-as"));
            const auto id = trim(element.attribute("id"));
            if (!id.empty())
                creator_ids.emplace(std::string(id), out.creators.size());
            out.creators.push_back(std::move(creator));
        } else if (name == "identifier") {
            const auto raw = all_text(element);
            const auto text = trim(raw);
            if (text.empty())
                continue;
            out.identifiers.emplace_back(text);
            identifier_schemes.emplace_back(std::string(text),
                                            lowercase(trim(element.attribute("scheme"))));
        } else if (name == "language") {
            if (!out.language) {
                const auto raw = all_text(element);
                const auto text = trim(raw);
                if (!text.empty())
                    out.language = std::string(text);
            }
        } else if (name == "publisher") {
            if (!out.publisher) {
                auto text = collapse_whitespace(all_text(element));
                if (!text.empty())
                    out.publisher = std::move(text);
            }
        } else if (name == "date") {
            const auto raw = all_text(element);
            const auto text = trim(raw);
            if (text.empty())
                continue;
            if (!first_date)
                first_date = std::string(text);
            if (!out.date && !element.find_attribute("event"))
                out.date = std::string(text);
        } else if (name == "description") {
            if (out.description)
                continue;
            auto text = collapse_whitespace(all_text(element));
            if (looks_like_markup(text))
                text = collapse_whitespace(strip_markup(text));
            if (!text.empty())
                out.description = std::move(text);
        } else if (name == "meta") {
            const auto meta_name = trim(element.attribute("name"));
            const auto property = trim(element.attribute("property"));
            if (!meta_name.empty()) {
                const auto content = element.attribute("content");
                if (meta_name == "calibre:series")
                    calibre_series = collapse_whitespace(content);
                else if (meta_name == "calibre:series_index")
                    calibre_index = parse_position(content);
                else if (meta_name == "cover")
                    cover_id = std::string(trim(content));
                continue;
            }
            if (property.empty())
                continue;
            auto refines = trim(element.attribute("refines"));
            if (refines.starts_with('#'))
                refines.remove_prefix(1);
            if (!refines.empty()) {
                refinements.push_back({std::string(refines), std::string(property),
                                       lowercase(trim(element.attribute("scheme"))),
                                       collapse_whitespace(all_text(element))});
            } else if (property == "belongs-to-collection") {
                CollectionDraft draft;
                draft.id = std::string(trim(element.attribute("id")));
                draft.name = collapse_whitespace(all_text(element));
                collections.push_back(std::move(draft));
            }
        }
    }

    if (!out.date)
        out.date = first_date;

    for (const auto& refinement : refinements) {
        if (auto creator = creator_ids.find(refinement.target); creator != creator_ids.end()) {
            auto& target = out.creators[creator->second];
            if (refinement.property == "role" &&
                (refinement.scheme.empty() || refinement.scheme == "marc:relators"))
                target.role = refinement.text;
            else if (refinement.property == "file-as")
                target.file_as = refinement.text;
            continue;
        }
        for (auto& collection : collections) {
            if (collection.id.empty() || collection.id != refinement.target)
                continue;
            if (refinement.property == "collection-type")
                collection.type = lowercase(refinement.text);
            else if (refinement.property == "group-position")
                collection.position = parse_position(refinement.text);
        }
    }

    if (calibre_series) {
        CollectionDraft draft;
        draft.name = *calibre_series;
        draft.position = calibre_index;
        add_series(out.series, draft);
    }
    for (const auto& collection : collections)
        if (collection.type.empty() || collection.type == "series")
            add_series(out.series, collection);

    // ISBN: an identifier declared as one first, then any that has the shape.
    for (const auto& [text, scheme] : identifier_schemes) {
        if (scheme != "isbn")
            continue;
        if (auto isbn = normalise_isbn13(text)) {
            out.isbn13 = std::move(isbn);
            break;
        }
    }
    if (!out.isbn13) {
        for (const auto& [text, scheme] : identifier_schemes) {
            if (auto isbn = normalise_isbn13(text)) {
                out.isbn13 = std::move(isbn);
                break;
            }
        }
    }

    // Cover: an EPUB 3 cover-image item, else the EPUB 2 meta's item by id,
    // else by href.
    if (const auto* manifest = find_element(*package, "manifest")) {
        const XmlElement* cover_item = nullptr;
        const XmlElement* by_id = nullptr;
        const XmlElement* by_href = nullptr;
        for (const auto& item : manifest->children) {
            if (item.name != "item")
                continue;
            const auto properties = item.attribute("properties");
            size_t p = 0;
            while (p < properties.size() && !cover_item) {
                while (p < properties.size() && is_space(properties[p]))
                    ++p;
                size_t end = p;
                while (end < properties.size() && !is_space(properties[end]))
                    ++end;
                if (properties.substr(p, end - p) == "cover-image")
                    cover_item = &item;
                p = end;
            }
            if (cover_item)
                break;
            if (cover_id && !by_id && trim(item.attribute("id")) == *cover_id)
                by_id = &item;
            if (cover_id && !by_href && trim(item.attribute("href")) == *cover_id)
                by_href = &item;
        }
        if (!cover_item)
            cover_item = by_id ? by_id : by_href;
        if (cover_item) {
            auto href = std::string(trim(cover_item->attribute("href")));
            if (const auto hash = href.find('#'); hash != std::string::npos)
                href.resize(hash);
            if (!href.empty()) {
                CoverReference reference;
                reference.path = resolve_path(opf_directory, percent_decode(href));
                reference.mime_type = std::string(trim(cover_item->attribute("media-type")));
                if (reference.mime_type.empty())
                    reference.mime_type = mime_from_extension(reference.path);
                parsed.cover = std::move(reference);
            }
        }
    }
    return parsed;
}

} // namespace

std::optional<std::string> normalise_isbn13(std::string_view text) {
    const auto value = trim(text);
    const auto lower = lowercase(value);
    size_t p = 0;
    if (lower.compare(0, 4, "urn:") == 0)
        p = 4;
    if (lower.compare(p, 4, "isbn") == 0) {
        p += 4;
        if (lower.compare(p, 3, "-10") == 0 || lower.compare(p, 3, "-13") == 0)
            p += 3;
        while (p < lower.size() && (lower[p] == ':' || lower[p] == ' '))
            ++p;
    }

    std::string digits;
    for (; p < value.size(); ++p) {
        const char c = value[p];
        if (c == '-' || c == ' ')
            continue;
        if (c >= '0' && c <= '9')
            digits += c;
        else if (c == 'x' || c == 'X')
            digits += 'X';
        else
            return std::nullopt;
    }

    if (digits.size() == 10) {
        int sum = 0;
        for (size_t i = 0; i < 10; ++i) {
            const char c = digits[i];
            int v = 0;
            if (c == 'X') {
                if (i != 9)
                    return std::nullopt;
                v = 10;
            } else {
                v = c - '0';
            }
            sum += static_cast<int>(10 - i) * v;
        }
        if (sum % 11 != 0)
            return std::nullopt;
        std::string isbn13 = "978" + digits.substr(0, 9);
        int check_sum = 0;
        for (size_t i = 0; i < 12; ++i)
            check_sum += (isbn13[i] - '0') * (i % 2 ? 3 : 1);
        isbn13 += static_cast<char>('0' + (10 - check_sum % 10) % 10);
        return isbn13;
    }

    if (digits.size() == 13) {
        if (digits.find('X') != std::string::npos)
            return std::nullopt;
        if (!(digits.starts_with("978") || digits.starts_with("979")))
            return std::nullopt;
        int sum = 0;
        for (size_t i = 0; i < 13; ++i)
            sum += (digits[i] - '0') * (i % 2 ? 3 : 1);
        if (sum % 10 != 0)
            return std::nullopt;
        return digits;
    }
    return std::nullopt;
}

EpubMetadata read_epub_metadata(uint64_t size,
                                const std::function<size_t(uint64_t, std::span<uint8_t>)>& read,
                                const EpubReadLimits& limits) {
    const Source source{size, read};
    const auto members = read_central_directory(source, limits);
    size_t budget = limits.max_total_bytes;

    const auto container_member = members.find("META-INF/container.xml");
    if (container_member == members.end())
        throw EpubError("no META-INF/container.xml");
    const auto container =
        read_member(source, container_member->second, limits.max_member_bytes, budget);
    const auto opf_path = rootfile_path(parse_xml(as_text(container)));

    const auto opf_member = members.find(opf_path);
    if (opf_member == members.end())
        throw EpubError("rootfile not in the zip");
    const auto opf = read_member(source, opf_member->second, limits.max_member_bytes, budget);
    auto parsed = parse_opf(parse_xml(as_text(opf)), directory_of(opf_path));

    if (parsed.cover) {
        const auto cover_member = members.find(parsed.cover->path);
        if (cover_member != members.end()) {
            try {
                EpubCover cover;
                cover.bytes =
                    read_member(source, cover_member->second, limits.max_cover_bytes, budget);
                cover.mime_type = std::move(parsed.cover->mime_type);
                parsed.metadata.cover = std::move(cover);
                // NOLINTNEXTLINE(bugprone-empty-catch): a bad cover does not make a bad book
            } catch (const EpubError&) {
            }
        }
    }
    return std::move(parsed.metadata);
}

EpubMetadata read_opf_metadata(std::string_view opf) {
    return std::move(parse_opf(parse_xml(opf), {}).metadata);
}

EpubMetadata read_epub_metadata(const std::filesystem::path& path, const EpubReadLimits& limits) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw EpubError("cannot open " + path.string());
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
        throw EpubError("cannot size " + path.string());
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
    return read_epub_metadata(size, read, limits);
}

} // namespace macha
