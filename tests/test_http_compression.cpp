// SPDX-License-Identifier: GPL-3.0-or-later
// Content negotiation and the gzip codec, without a socket.
#include "test_backend_support.hpp"

using namespace macha;
using namespace macha::test_support;

namespace {

HttpRequest with_accept_encoding(std::string value) {
    HttpRequest request;
    request.method = "GET";
    request.path = "/api/v1/catalogue";
    // Header keys arrive lowercased from the server's parser.
    request.headers["accept-encoding"] = std::move(value);
    return request;
}

} // namespace

MACHA_FAST_TEST("http_compression", test_gzip_is_offered_only_when_the_client_actually_takes_it) {
    CHECK(client_accepts_gzip(with_accept_encoding("gzip")));
    CHECK(client_accepts_gzip(with_accept_encoding("gzip, deflate, br")));
    CHECK(client_accepts_gzip(with_accept_encoding("deflate, gzip;q=1.0, *;q=0.5")));
    CHECK(client_accepts_gzip(with_accept_encoding("GZIP")));
    CHECK(client_accepts_gzip(with_accept_encoding("x-gzip")));
    CHECK(client_accepts_gzip(with_accept_encoding("  gzip  ")));

    // An explicit gzip refusal wins over any wildcard in the same header.
    CHECK(!client_accepts_gzip(with_accept_encoding("gzip;q=0")));
    CHECK(!client_accepts_gzip(with_accept_encoding("gzip;q=0.0")));
    CHECK(!client_accepts_gzip(with_accept_encoding("gzip;q=0.000")));
    CHECK(!client_accepts_gzip(with_accept_encoding("*, gzip;q=0")));
    CHECK(!client_accepts_gzip(with_accept_encoding("gzip;q=0, *")));

    // Only gzip is on offer; other encodings are not an acceptance.
    CHECK(!client_accepts_gzip(with_accept_encoding("deflate")));
    CHECK(!client_accepts_gzip(with_accept_encoding("br")));
    CHECK(!client_accepts_gzip(with_accept_encoding("identity")));
    CHECK(!client_accepts_gzip(with_accept_encoding("")));
    CHECK(!client_accepts_gzip(with_accept_encoding("*;q=0")));

    // A wildcard with no opinion about gzip is an acceptance.
    CHECK(client_accepts_gzip(with_accept_encoding("*")));
    CHECK(client_accepts_gzip(with_accept_encoding("identity;q=0.5, *")));

    // No header is not an acceptance: clients that never send one get plain bytes.
    HttpRequest bare;
    bare.method = "GET";
    bare.path = "/api/v1/catalogue";
    CHECK(!client_accepts_gzip(bare));
}

MACHA_FAST_TEST("http_compression", test_only_types_that_gain_from_compression_are_offered_it) {
    CHECK(compressible_content_type("application/json; charset=utf-8"));
    CHECK(compressible_content_type("application/json"));
    CHECK(compressible_content_type("text/html; charset=utf-8"));
    CHECK(compressible_content_type("text/plain"));
    CHECK(compressible_content_type("text/javascript; charset=utf-8"));
    CHECK(compressible_content_type("text/css; charset=utf-8"));
    CHECK(compressible_content_type("application/xml"));
    CHECK(compressible_content_type("image/svg+xml"));
    CHECK(compressible_content_type("application/manifest+json"));
    // Any type with a +json structured syntax suffix is compressible.
    CHECK(compressible_content_type("application/vnd.macha.thing+json"));

    // Already compressed: a second pass would only make these larger.
    CHECK(!compressible_content_type("video/mp4"));
    CHECK(!compressible_content_type("audio/mpeg"));
    CHECK(!compressible_content_type("image/png"));
    CHECK(!compressible_content_type("image/jpeg"));
    CHECK(!compressible_content_type("image/webp"));
    CHECK(!compressible_content_type("font/woff2"));
    CHECK(!compressible_content_type("application/wasm"));
    CHECK(!compressible_content_type("application/octet-stream"));
    CHECK(!compressible_content_type(""));
}

MACHA_FAST_TEST("http_compression", test_gzip_round_trips_and_refuses_to_make_anything_larger) {
    // Repetitive JSON, the shape of a catalogue listing.
    std::string json = "{\"items\":[";
    for (int i = 0; i < 500; ++i) {
        if (i)
            json += ',';
        json += "{\"id\":\"macha:" + std::to_string(i) +
                "\",\"kind\":\"movie\",\"title\":\"A Title\",\"year\":1999}";
    }
    json += "]}";

    const std::span<const uint8_t> input(reinterpret_cast<const uint8_t*>(json.data()),
                                         json.size());
    auto compressed = gzip_compress(input, 6);
    REQUIRE(compressed.has_value());
    CHECK(compressed->size() < json.size() / 4);
    CHECK(gunzip(std::string(compressed->begin(), compressed->end())) == json);

    // Every level produces a readable stream.
    for (int level : {1, 6, 9}) {
        auto at_level = gzip_compress(input, level);
        REQUIRE(at_level.has_value());
        CHECK(gunzip(std::string(at_level->begin(), at_level->end())) == json);
    }

    // Incompressible input yields nothing rather than a larger result.
    auto noise = pattern(4096);
    std::vector<uint8_t> random_bytes(noise.begin(), noise.end());
    std::mt19937 rng(12345);
    for (auto& byte : random_bytes)
        byte = static_cast<uint8_t>(rng());
    CHECK(!gzip_compress(random_bytes, 6).has_value());

    CHECK(!gzip_compress({}, 6).has_value());
}
