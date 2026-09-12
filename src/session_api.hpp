// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster.hpp"
#include "http.hpp"
#include "json.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace macha {

struct ValidatedCredentials {
    std::vector<std::string> roles;
    std::string user_id; // empty for an anonymous session
    uint64_t credential_generation{};
};

enum class CredentialOutcome : uint8_t {
    ok = 0,
    unsupported,  // not a credential shape this validator understands -> 400
    rejected,     // wrong username/password -> 401
    disabled,     // anonymous requested but not permitted here -> 403
    rate_limited, // too many password checks in flight, or locked out -> 429
};

struct CredentialResult {
    CredentialOutcome outcome{CredentialOutcome::unsupported};
    ValidatedCredentials credentials;
};

class CredentialValidator {
  public:
    virtual ~CredentialValidator() = default;
    virtual CredentialResult validate(const Json& credentials) const = 0;
};

// Empty credentials mint an anonymous session carrying
// SessionConfig::anonymous_roles; a {username, password} pair is checked
// against the node's own replica of the cluster user table. Both paths are
// local: no RPC, no metadata, no catalogue -- a node that is temporarily alone
// still authenticates every user it knows about.
class PasswordCredentialValidator final : public CredentialValidator {
    const UserStore& users_;
    SessionConfig config_;

    // Local brakes on an unauthenticated endpoint that runs a deliberately
    // expensive KDF. Neither is cluster state: they protect this node's CPU,
    // and replicating them would be both pointless and a channel of its own.
    mutable std::mutex mutex_;
    mutable size_t in_flight_{};
    struct Failures {
        size_t count{};
        Clock::time_point until{};
    };
    mutable std::map<std::string, Failures, std::less<>> failures_;

    bool begin_check(const std::string& username) const;
    void end_check(const std::string& username, bool success) const;

  public:
    PasswordCredentialValidator(const UserStore&, SessionConfig);
    CredentialResult validate(const Json& credentials) const override;
};

// REST surface for the cluster session/auth subsystem: mint, introspect, and
// revoke the caller's own bearer-token-backed session. This is the only
// endpoint reachable without an existing session (see capability_request).
class SessionApi {
    SessionManager& sessions_;
    NodeRuntime& node_;
    std::unique_ptr<CredentialValidator> validator_;

  public:
    explicit SessionApi(NodeRuntime& node, std::unique_ptr<CredentialValidator> validator = {});
    HttpResponse handle(const HttpRequest&);
    static bool capability_request(const HttpRequest&);
};

} // namespace macha
