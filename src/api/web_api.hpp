// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "http/http.hpp"

#include <filesystem>

namespace macha {

// Serves the web client from a directory at the server's root. A path naming
// a file there is served as that file; anything else gets the client's index
// document, so single-page-app deep links reach the client's router.
//
// Never serves /api: everything under it stays the server's, 404s included, so
// a client asking for a missing endpoint gets JSON, not HTML.
class WebApi {
    WebConfig config_;
    HttpCompressionConfig compression_;

  public:
    // Compression settings come from catalogue.api: one HTTP server answers the
    // API and the client under the same rules. Assets are negotiated here, not by
    // the generic compressor, because the entity tag and encoding are chosen together.
    explicit WebApi(WebConfig config, HttpCompressionConfig compression = {});

    // Whether this node serves a web client at all (`web.root` set); without one,
    // non-API paths 404.
    bool enabled() const noexcept;

    // Whether a path belongs to the server rather than the client. The whole /api
    // prefix is reserved, so a future API version is never swallowed by the client.
    static bool api_path(std::string_view path) noexcept;

    HttpResponse handle(const HttpRequest& request) const;
};

} // namespace macha
