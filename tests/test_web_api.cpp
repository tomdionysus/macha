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
