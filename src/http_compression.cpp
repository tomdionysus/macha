// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_compression.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <string>

#include <zlib.h>

namespace macha {
namespace {

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

bool equal_ignoring_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

// The quality of one Accept-Encoding entry. Absent means 1 (accepted); the
// only value that matters to us is whether it is zero, which is how a client
// refuses an encoding it would otherwise be offered.
bool quality_is_zero(std::string_view parameters) {
    while (!parameters.empty()) {
        auto semicolon = parameters.find(';');
        auto parameter = trim(parameters.substr(0, semicolon));
        parameters = semicolon == std::string_view::npos ? std::string_view{}
                                                         : parameters.substr(semicolon + 1);
        auto equals = parameter.find('=');
        if (equals == std::string_view::npos)
            continue;
        if (!equal_ignoring_case(trim(parameter.substr(0, equals)), "q"))
            continue;
        const auto value = trim(parameter.substr(equals + 1));
        // "0", "0.", "0.0", "0.000" are all a refusal; anything else is not.
        // Parsed by hand rather than with a locale-sensitive float conversion.
        if (value.empty() || value.front() != '0')
            return false;
        return value.find_first_of("123456789") == std::string_view::npos;
    }
    return false;
}

} // namespace

bool client_accepts_gzip(const HttpRequest& request) {
    auto header = request.headers.find("accept-encoding");
    if (header == request.headers.end())
        return false;

    std::string_view remaining = header->second;
    bool wildcard = false;
    bool wildcard_refused = false;
    while (!remaining.empty()) {
        auto comma = remaining.find(',');
        auto entry = trim(remaining.substr(0, comma));
        remaining =
            comma == std::string_view::npos ? std::string_view{} : remaining.substr(comma + 1);
        if (entry.empty())
            continue;

        auto semicolon = entry.find(';');
        const auto token = trim(entry.substr(0, semicolon));
        const auto parameters =
            semicolon == std::string_view::npos ? std::string_view{} : entry.substr(semicolon + 1);
        const bool refused = quality_is_zero(parameters);

        // An explicit entry for gzip is the answer, either way: a client that
        // names it and then refuses it must not be handed one because of a
        // wildcard elsewhere in the same header.
        if (equal_ignoring_case(token, "gzip") || equal_ignoring_case(token, "x-gzip"))
            return !refused;
        if (token == "*") {
            wildcard = true;
            wildcard_refused = refused;
        }
    }
    return wildcard && !wildcard_refused;
}

bool compressible_content_type(std::string_view content_type) {
    // Compare the media type only: the parameters ("; charset=utf-8") say
    // nothing about whether the bytes compress.
    auto semicolon = content_type.find(';');
    const auto type = trim(content_type.substr(0, semicolon));
    if (type.empty())
        return false;

    // Everything under text/ is text, whatever the subtype happens to be.
    if (type.size() > 5 && equal_ignoring_case(type.substr(0, 5), "text/"))
        return true;

    // Structured syntax suffixes: application/vnd.whatever+json is JSON, and
    // a table of concrete names would never keep up with them.
    if (type.ends_with("+json") || type.ends_with("+xml") || type.ends_with("+text"))
        return true;

    static constexpr std::array<std::string_view, 9> types{{
        "application/json",
        "application/javascript",
        "application/xml",
        "application/xhtml+xml",
        "application/manifest+json",
        "application/x-ndjson",
        "application/rss+xml",
        "application/atom+xml",
        "image/svg+xml",
    }};
    for (const auto& candidate : types)
        if (equal_ignoring_case(type, candidate))
            return true;
    return false;
}

std::optional<Bytes> gzip_compress(std::span<const uint8_t> input, int level) {
    if (input.empty())
        return std::nullopt;
    if (input.size() > std::numeric_limits<uInt>::max())
        return std::nullopt;

    z_stream stream{};
    // windowBits 15 + 16 selects a gzip wrapper rather than a zlib one, which
    // is what Content-Encoding: gzip names on the wire.
    constexpr int gzip_window_bits = 15 + 16;
    constexpr int default_memory_level = 8;
    if (deflateInit2(&stream, std::clamp(level, 1, 9), Z_DEFLATED, gzip_window_bits,
                     default_memory_level, Z_DEFAULT_STRATEGY) != Z_OK)
        return std::nullopt;

    // deflateBound is the worst case for this input, so one buffer and one
    // pass is enough and the output is never reallocated mid-compression.
    Bytes out(deflateBound(&stream, static_cast<uLong>(input.size())));
    stream.next_in = const_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());

    const int result = deflate(&stream, Z_FINISH);
    const auto produced = static_cast<size_t>(stream.total_out);
    deflateEnd(&stream);
    if (result != Z_STREAM_END)
        return std::nullopt;

    // Incompressible input (already-compressed bytes that slipped past the
    // content-type check, or a pathological small body) costs a gzip header
    // and gains nothing. Say so and let the caller send the original.
    if (produced >= input.size())
        return std::nullopt;
    out.resize(produced);
    return out;
}

} // namespace macha
