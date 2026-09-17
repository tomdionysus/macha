// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "http.hpp"
#include "types.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace macha {

// gzip, for the text the API and the web client actually send.
//
// Media is deliberately out of scope. It is already compressed, it is served
// through an HttpBodySource rather than a byte buffer, and the reactor sends
// it straight from resident memory with no copy and no pool hop (see
// HttpServer's flush()). Putting a transform on that path would undo the
// thing it exists for, and mixing Content-Encoding with the Content-Range of
// a ranged response is a correctness trap besides. Everything here therefore
// operates on a complete in-memory body and nothing else.

// True when the client said it would take gzip. Honours an explicit
// `gzip;q=0` refusal and the `*` wildcard, so a client that cannot decode one
// can say so and be believed.
bool client_accepts_gzip(const HttpRequest& request);

// Whether a Content-Type is worth the CPU. Text, JSON, XML, SVG, JavaScript
// and the other structured text types are; images, fonts, wasm and media
// already carry their own compression and a second pass only spends cycles to
// make them very slightly larger.
bool compressible_content_type(std::string_view content_type);

// gzip-wrapped deflate at `level` (1..9). Returns nothing when the input does
// not get smaller, or when zlib refuses it: the caller then sends the body
// unchanged rather than paying to inflate it.
std::optional<Bytes> gzip_compress(std::span<const uint8_t> input, int level);

} // namespace macha
