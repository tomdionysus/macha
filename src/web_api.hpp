// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "http.hpp"

#include <filesystem>

namespace macha {

// Serves the web client from a directory at the server's root. A path that
// names a file under that directory is served as that file; anything else is
// answered with the client's index document, because a single-page
// application owns its own routing and a deep link must reach it rather than
// the server's 404.
//
// The API namespace is never served from here. Everything under /api stays
// the server's, 404s included: a client asking for an endpoint that does not
// exist must be told so in JSON, not handed an HTML page that its parser will
// choke on. That isolation is the whole reason this is a separate handler
// rather than a fallback bolted onto the API's own dispatch.
class WebApi {
    WebConfig config_;

  public:
    explicit WebApi(WebConfig config);

    // Whether this node is configured to serve a web client at all. A node
    // with no `web.root` serves none, and non-API paths keep their old 404.
    bool enabled() const noexcept;

    // Whether a path belongs to the server rather than to the client. The
    // whole /api prefix is reserved, not just /api/v1, so a future version of
    // the API cannot be silently swallowed by the client's fallback.
    static bool api_path(std::string_view path) noexcept;

    HttpResponse handle(const HttpRequest& request) const;
};

} // namespace macha
