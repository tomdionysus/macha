// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "http/http.hpp"
#include "types.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace macha {

// gzip for complete in-memory API and web-client bodies. Media is out of
// scope: already compressed, sent zero-copy from resident memory, and
// Content-Encoding on a ranged response is a correctness trap.

// True when the client accepts gzip, honouring an explicit `gzip;q=0` and the
// `*` wildcard.
bool client_accepts_gzip(const HttpRequest& request);

// Whether a Content-Type is worth the CPU: structured text yes; images, fonts,
// wasm and media are already compressed.
bool compressible_content_type(std::string_view content_type);

// gzip-wrapped deflate at `level` (1..9). Nothing when the output would not be
// smaller or zlib refuses; the caller then sends the body unchanged.
std::optional<Bytes> gzip_compress(std::span<const uint8_t> input, int level);

} // namespace macha
