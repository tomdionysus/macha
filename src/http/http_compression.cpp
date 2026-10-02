// SPDX-License-Identifier: GPL-3.0-or-later
#include "http/http_compression.hpp"

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

// Quality of one Accept-Encoding entry; absent means 1. Only zero (a refusal)
// matters.
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
        // "0", "0.", "0.000" are refusals. Parsed by hand to avoid locale-sensitive
        // float conversion.
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

        // An explicit gzip entry decides, whatever a wildcard says.
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
    // Media type only; parameters say nothing about compressibility.
    auto semicolon = content_type.find(';');
    const auto type = trim(content_type.substr(0, semicolon));
    if (type.empty())
        return false;

    if (type.size() > 5 && equal_ignoring_case(type.substr(0, 5), "text/"))
        return true;

    // Structured syntax suffixes (application/vnd.x+json) rather than a table of
    // names.
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
    // windowBits 15 + 16 selects the gzip wrapper Content-Encoding: gzip names.
    constexpr int gzip_window_bits = 15 + 16;
    constexpr int default_memory_level = 8;
    if (deflateInit2(&stream, std::clamp(level, 1, 9), Z_DEFLATED, gzip_window_bits,
                     default_memory_level, Z_DEFAULT_STRATEGY) != Z_OK)
        return std::nullopt;

    // deflateBound is the worst case, so one buffer and one pass suffice.
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

    // Incompressible input gains nothing over a gzip header; send the original.
    if (produced >= input.size())
        return std::nullopt;
    out.resize(produced);
    return out;
}

} // namespace macha
