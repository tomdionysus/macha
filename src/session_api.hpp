// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster.hpp"
#include "http.hpp"
#include "json.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace macha {

// Seam for future non-anonymous credential validation (real accounts, etc).
// Only AnonymousCredentialValidator exists today; a real validator is a
// drop-in replacement passed into SessionApi's constructor.
class CredentialValidator {
  public:
    virtual ~CredentialValidator() = default;
    virtual std::optional<std::vector<std::string>> validate(const Json& credentials) const = 0;
};

class AnonymousCredentialValidator final : public CredentialValidator {
  public:
    std::optional<std::vector<std::string>> validate(const Json& credentials) const override;
};

// REST surface for the cluster session/auth subsystem: mint, introspect, and
// revoke the caller's own bearer-token-backed session. This is the only
// endpoint reachable without an existing session (see capability_request).
class SessionApi {
    SessionManager& sessions_;
    NodeRuntime& node_;
    std::unique_ptr<CredentialValidator> validator_;

  public:
    explicit SessionApi(NodeRuntime& node,
                        std::unique_ptr<CredentialValidator> validator =
                            std::make_unique<AnonymousCredentialValidator>());
    HttpResponse handle(const HttpRequest&);
    static bool capability_request(const HttpRequest&);
};

} // namespace macha
