// SPDX-License-Identifier: GPL-3.0-or-later
#include "json.hpp"
#include "test_backend_support.hpp"
#include "web_api.hpp"

#include <fstream>

using namespace macha;
using namespace macha::test_support;

namespace {

void write_file(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string body_of(const HttpResponse& response) {
    if (!response.stream) return std::string(response.body.begin(), response.body.end());
    std::string out(static_cast<size_t>(response.stream->size()), '\0');
    size_t filled = 0;
    while (filled < out.size()) {
        auto n = response.stream->read(filled,
                                       std::span<uint8_t>(reinterpret_cast<uint8_t*>(out.data()) + filled,
                                                          out.size() - filled));
        if (n == 0) break;
        filled += n;
    }
    out.resize(filled);
    return out;
}

HttpRequest get(std::string path) {
    HttpRequest request;
    request.method = "GET";
    request.path = std::move(path);
    return request;
}

WebConfig config_for(const std::filesystem::path& root) {
    WebConfig config;
    config.root = root;
    return config;
}

HttpRequest get_accepting_gzip(std::string path) {
    auto request = get(std::move(path));
    request.headers["accept-encoding"] = "gzip, deflate";
    return request;
}

// Big enough to be worth compressing and repetitive enough to compress well,
// which is what a real client bundle looks like to gzip.
std::string bundle(size_t repeats = 200) {
    std::string out = "// macha client bundle\n";
    for (size_t i = 0; i < repeats; ++i)
        out += "export function widget" + std::to_string(i) +
               "(state){ return render(state, 'widget', " + std::to_string(i) + "); }\n";
    return out;
}

std::string gzip_of(std::string_view text) {
    auto compressed = gzip_compress(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()), 6);
    if (!compressed) throw std::runtime_error("test fixture would not compress");
    return std::string(compressed->begin(), compressed->end());
}

} // namespace

MACHA_FAST_TEST("web_api", test_unknown_routes_reach_the_client_and_real_files_do_not) {
    // A single-page application owns its own routing: a deep link is a route
    // the client resolves once it has loaded, so the server must hand it the
    // index document rather than a 404 it can never recover from. A path that
    // does name a file is that file.
    TempDir t;
    const auto root = t.path() / "web";
    write_file(root / "index.html", "<!doctype html><title>macha</title>");
    write_file(root / "assets" / "app.js", "console.log('macha')");
    write_file(root / "assets" / "app.css", "body{margin:0}");
    WebApi web(config_for(root));
    REQUIRE(web.enabled());

    auto index = web.handle(get("/"));
    CHECK(index.status == 200);
    CHECK(index.content_type == "text/html; charset=utf-8");
    CHECK(body_of(index) == "<!doctype html><title>macha</title>");

    // The client's own routes, which exist only in the browser.
    for (const auto* route : {"/library", "/library/artist/anything", "/settings?tab=playback"}) {
        auto deep = web.handle(get(route));
        CHECK(deep.status == 200);
        CHECK(body_of(deep) == "<!doctype html><title>macha</title>");
    }

    auto script = web.handle(get("/assets/app.js"));
    CHECK(script.status == 200);
    CHECK(script.content_type == "text/javascript; charset=utf-8");
    CHECK(body_of(script) == "console.log('macha')");

    auto style = web.handle(get("/assets/app.css"));
    CHECK(style.status == 200);
    CHECK(style.content_type == "text/css; charset=utf-8");

    // The index document is revalidated on every load or a deploy stays
    // invisible; the assets it names may be held.
    CHECK(index.headers.at("Cache-Control") == "no-cache");
    CHECK(script.headers.at("Cache-Control") == "public, max-age=3600");

    // An unchanged asset is not sent twice.
    auto conditional = get("/assets/app.js");
    conditional.headers["if-none-match"] = script.headers.at("ETag");
    auto revalidated = web.handle(conditional);
    CHECK(revalidated.status == 304);
    CHECK(body_of(revalidated).empty());
}

MACHA_FAST_TEST("web_api", test_the_api_namespace_is_never_the_clients) {
    CHECK(WebApi::api_path("/api"));
    CHECK(WebApi::api_path("/api/"));
    CHECK(WebApi::api_path("/api/v1/status"));
    CHECK(WebApi::api_path("/api/v2/anything"));
    CHECK(!WebApi::api_path("/"));
    CHECK(!WebApi::api_path("/apiary"));
    CHECK(!WebApi::api_path("/library/api"));
}

MACHA_FAST_TEST("web_api", test_a_request_cannot_climb_out_of_the_web_root) {
    // The fallback is what makes traversal interesting: a refused path must
    // not fall through to something else on disk, and must not become a 404
    // that tells an attacker whether a file exists.
    TempDir t;
    const auto root = t.path() / "web";
    write_file(root / "index.html", "INDEX");
    write_file(t.path() / "secret.txt", "SECRET");
    write_file(root / ".env", "TOKEN=SECRET");
    WebApi web(config_for(root));

    for (const auto* path : {"/../secret.txt", "/assets/../../secret.txt", "/./../secret.txt",
                             "/%2e%2e/secret.txt", "/.env", "/.git/config"}) {
        auto response = web.handle(get(path));
        CHECK(response.status == 200);
        CHECK(body_of(response) == "INDEX");
    }
}

MACHA_FAST_TEST("web_api", test_a_web_client_is_read_only_and_optional) {
    TempDir t;
    const auto root = t.path() / "web";
    write_file(root / "index.html", "INDEX");
    WebApi web(config_for(root));

    HttpRequest post;
    post.method = "POST";
    post.path = "/library";
    auto refused = web.handle(post);
    CHECK(refused.status == 405);

    HttpRequest head;
    head.method = "HEAD";
    head.path = "/library";
    CHECK(web.handle(head).status == 200);

    // A node that serves no client keeps the behaviour every node had before
    // this existed: non-API paths are simply not found.
    WebApi none{WebConfig{}};
    CHECK(!none.enabled());
    CHECK(none.handle(get("/library")).status == 404);

    // A node configured for a client whose files are not there yet says so,
    // rather than pretending the route does not exist.
    WebConfig missing;
    missing.root = t.path() / "not-deployed-yet";
    WebApi absent(missing);
    REQUIRE(absent.enabled());
    auto unavailable = absent.handle(get("/library"));
    CHECK(unavailable.status == 503);
    auto parsed = Json::parse(std::string(unavailable.body.begin(), unavailable.body.end()));
    CHECK(parsed.find("error")->find("code")->asString() == "web_client_unavailable");

    // The API namespace is refused here even if something routes it wrongly.
    CHECK(web.handle(get("/api/v1/anything")).status == 404);
}

MACHA_FAST_TEST("web_api", test_a_client_asset_is_gzipped_only_for_a_client_that_takes_one) {
    // The reason this exists: a first page load pulls the whole bundle, and
    // over a WAN link that is the user-visible cost of opening the client.
    TempDir t;
    const auto root = t.path() / "web";
    const auto script = bundle();
    write_file(root / "index.html", "<!doctype html><title>macha</title>");
    write_file(root / "app.js", script);
    write_file(root / "logo.png", std::string(4096, '\x89'));
    WebApi web(config_for(root));

    auto compressed = web.handle(get_accepting_gzip("/app.js"));
    CHECK(compressed.status == 200);
    CHECK(compressed.content_type == "text/javascript; charset=utf-8");
    CHECK(compressed.headers.at("Content-Encoding") == "gzip");
    CHECK(compressed.headers.at("Vary") == "Accept-Encoding");
    // What the browser ends up with must be the file, byte for byte.
    CHECK(gunzip(body_of(compressed)) == script);
    CHECK(body_of(compressed).size() < script.size() / 2);

    // The same asset, for a client that said nothing about encodings.
    auto plain = web.handle(get("/app.js"));
    CHECK(plain.status == 200);
    CHECK(!plain.headers.contains("Content-Encoding"));
    CHECK(body_of(plain) == script);
    // Still stated, so a shared cache keys the two representations apart
    // rather than handing this body to the next client that asks for gzip.
    CHECK(plain.headers.at("Vary") == "Accept-Encoding");

    // A PNG is already compressed; a second pass would only spend CPU.
    auto image = web.handle(get_accepting_gzip("/logo.png"));
    CHECK(image.status == 200);
    CHECK(!image.headers.contains("Content-Encoding"));
    CHECK(!image.headers.contains("Vary"));
}

MACHA_FAST_TEST("web_api", test_a_precompressed_sibling_is_served_rather_than_compressed_again) {
    // A build that emits app.js.gz has already paid for the compression. The
    // server must spend nothing per request to use it.
    TempDir t;
    const auto root = t.path() / "web";
    const auto script = bundle();
    // Deliberately not the gzip of app.js: if the sibling is what gets sent,
    // this is what comes back, and nothing else could produce it.
    const auto sibling_contents = bundle(7) + "// served from the sibling\n";
    write_file(root / "index.html", "<!doctype html><title>macha</title>");
    write_file(root / "app.js", script);
    write_file(root / "app.js.gz", gzip_of(sibling_contents));
    WebApi web(config_for(root));

    auto served = web.handle(get_accepting_gzip("/app.js"));
    CHECK(served.status == 200);
    CHECK(served.headers.at("Content-Encoding") == "gzip");
    // The Content-Type is the asset's, not the archive's.
    CHECK(served.content_type == "text/javascript; charset=utf-8");
    CHECK(gunzip(body_of(served)) == sibling_contents);

    // A client that cannot take gzip is still served the real asset.
    auto plain = web.handle(get("/app.js"));
    CHECK(plain.status == 200);
    CHECK(!plain.headers.contains("Content-Encoding"));
    CHECK(body_of(plain) == script);

    // The sibling is never reachable as a resource in its own right under a
    // type that would make a browser try to execute it.
    auto direct = web.handle(get("/app.js.gz"));
    CHECK(direct.status == 200);
    CHECK(direct.content_type == "application/octet-stream");
}

MACHA_FAST_TEST("web_api", test_the_gzip_and_identity_representations_never_share_an_entity_tag) {
    // The trap this closes: one tag for two different bodies lets a cache --
    // or the browser's own store -- answer a client with a representation it
    // cannot read, and makes a 304 a lie.
    TempDir t;
    const auto root = t.path() / "web";
    write_file(root / "index.html", "<!doctype html><title>macha</title>");
    write_file(root / "app.js", bundle());
    WebApi web(config_for(root));

    const auto compressed = web.handle(get_accepting_gzip("/app.js"));
    const auto plain = web.handle(get("/app.js"));
    const auto gzip_tag = compressed.headers.at("ETag");
    const auto identity_tag = plain.headers.at("ETag");
    CHECK(gzip_tag != identity_tag);

    // Each representation revalidates against its own tag.
    auto revalidate_gzip = get_accepting_gzip("/app.js");
    revalidate_gzip.headers["if-none-match"] = gzip_tag;
    CHECK(web.handle(revalidate_gzip).status == 304);

    auto revalidate_plain = get("/app.js");
    revalidate_plain.headers["if-none-match"] = identity_tag;
    CHECK(web.handle(revalidate_plain).status == 304);

    // And never against the other's: a client holding the identity body must
    // be sent the gzip one in full rather than told it is unchanged.
    auto crossed = get_accepting_gzip("/app.js");
    crossed.headers["if-none-match"] = identity_tag;
    auto crossed_response = web.handle(crossed);
    CHECK(crossed_response.status == 200);
    CHECK(crossed_response.headers.at("Content-Encoding") == "gzip");

    auto crossed_back = get("/app.js");
    crossed_back.headers["if-none-match"] = gzip_tag;
    auto crossed_back_response = web.handle(crossed_back);
    CHECK(crossed_back_response.status == 200);
    CHECK(!crossed_back_response.headers.contains("Content-Encoding"));
}

MACHA_FAST_TEST("web_api", test_compression_is_configurable_and_off_means_off) {
    // A node behind a proxy that already compresses has no reason to pay for
    // it twice, so this is a supported deployment rather than a degraded one.
    TempDir t;
    const auto root = t.path() / "web";
    const auto script = bundle();
    write_file(root / "index.html", "<!doctype html><title>macha</title>");
    write_file(root / "app.js", script);
    write_file(root / "app.js.gz", gzip_of(script));

    HttpCompressionConfig off;
    off.enabled = false;
    WebApi disabled(config_for(root), off);
    auto untouched = disabled.handle(get_accepting_gzip("/app.js"));
    CHECK(untouched.status == 200);
    CHECK(!untouched.headers.contains("Content-Encoding"));
    // Nothing varies when nothing is negotiated.
    CHECK(!untouched.headers.contains("Vary"));
    CHECK(body_of(untouched) == script);

    // The floor is honoured: below it there is nothing to win and gzip's own
    // header is a real fraction of the body.
    HttpCompressionConfig floored;
    floored.min_bytes = script.size() + 1;
    WebApi high_floor(config_for(root), floored);
    auto small = high_floor.handle(get_accepting_gzip("/index.html"));
    CHECK(small.status == 200);
    CHECK(!small.headers.contains("Content-Encoding"));

    // An asset past the on-demand ceiling is streamed as it is rather than
    // read whole into memory once per request -- but a precompressed sibling
    // is still free, so it is still preferred.
    HttpCompressionConfig capped;
    capped.max_asset_bytes = 16;
    WebApi tight(config_for(root), capped);
    auto sibling = tight.handle(get_accepting_gzip("/app.js"));
    CHECK(sibling.status == 200);
    CHECK(sibling.headers.at("Content-Encoding") == "gzip");
    CHECK(gunzip(body_of(sibling)) == script);
}
