// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/session_api.hpp"
#include "test_backend_support.hpp"
#include "api/users_api.hpp"

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

NodeId node_id(uint8_t seed) {
    NodeId id{};
    id.bytes[0] = seed;
    return id;
}

UserRecord sample(std::string id, uint64_t version, bool tombstone = false, uint8_t by = 1) {
    UserRecord user;
    user.id = std::move(id);
    user.username = "sample";
    user.kdf = 1;
    user.roles = {std::string(role_media_viewer)};
    user.version = version;
    user.tombstone = tombstone;
    user.updated_by = node_id(by);
    user.created_unix_ms = user.updated_unix_ms = unix_ms();
    return user;
}

HttpRequest users_request(std::string method, std::string path,
                          std::optional<SessionIdentity> identity, std::string body = {}) {
    HttpRequest request;
    request.method = std::move(method);
    request.path = std::move(path);
    request.session = std::move(identity);
    request.body.assign(body.begin(), body.end());
    return request;
}

SessionIdentity admin_identity(std::string user_id = "admin-user") {
    return SessionIdentity{"session-id", Hash256{}, expand_roles(all_roles()), std::move(user_id)};
}

std::string json_body(const HttpResponse& response) {
    return std::string(response.body.begin(), response.body.end());
}

size_t count_of(const std::vector<std::string>& roles, std::string_view role) {
    return static_cast<size_t>(std::count(roles.begin(), roles.end(), role));
}

Json credentials(std::string_view username, std::string_view password) {
    Json::Object out;
    out["username"] = std::string(username);
    out["password"] = std::string(password);
    return Json(std::move(out));
}

HttpRequest login_request(std::string_view username, std::string_view password) {
    HttpRequest request;
    request.method = "POST";
    request.path = "/api/v1/session";
    Json::Object root{{"credentials", credentials(username, password)}};
    const auto text = Json(std::move(root)).dump();
    request.body.assign(text.begin(), text.end());
    return request;
}

} // namespace

// Roles are capabilities, not a ladder; implications are resolved when a
// session is minted; each route names the one capability it needs.
MACHA_FAST_TEST("users", test_role_policy) {
    // Every capability implies media_viewer and view_status, and nothing else
    // implies anything.
    struct Expansion {
        std::vector<std::string> granted;
        std::vector<std::pair<std::string_view, size_t>> holds;
        size_t size;
    };
    const std::vector<Expansion> expansions{
        {{std::string(role_importer)},
         {{role_media_viewer, 1}, {role_view_status, 1}, {role_manager, 0}, {role_manage_users, 0}},
         3},
        {{std::string(role_manager)}, {{role_importer, 0}, {role_manage_users, 0}}, 3},
        {{std::string(role_manage_users)}, {{role_manager, 0}}, 3},
        {{std::string(role_media_viewer)}, {{role_view_status, 1}}, 2},
        // view_status implies nothing; alone it grants no media.
        {{std::string(role_view_status)}, {{role_view_status, 1}, {role_media_viewer, 0}}, 1},
        // root holds every capability explicitly, not by implication.
        {all_roles(), {}, 5},
        // Already-expanded input must not duplicate.
        {expand_roles(all_roles()), {}, 5},
    };
    for (const auto& expansion : expansions) {
        const auto expanded = expand_roles(expansion.granted);
        CHECK(expanded.size() == expansion.size);
        for (const auto& [role, count] : expansion.holds)
            CHECK(count_of(expanded, role) == count);
    }

    // A record written before an implication existed still gets it at mint
    // time, with no widening beyond the documented implications.
    UserStore store(16);
    auto created =
        store.create("olduser", "a-long-enough-pw", {std::string(role_manager)}, node_id(1));
    REQUIRE(created.has_value());
    auto legacy = *created;
    legacy.roles = {std::string(role_manager), std::string(role_media_viewer)};
    legacy.version = created->version + 1;
    REQUIRE(store.apply(legacy));
    CHECK(!user_has_role(*store.find(created->id), role_view_status));
    const auto check = store.verify("olduser", "a-long-enough-pw");
    REQUIRE(check.ok);
    CHECK(count_of(check.roles, role_view_status) == 1);
    CHECK(count_of(check.roles, role_manager) == 1);
    CHECK(count_of(check.roles, role_manage_users) == 0);

    // Each route's capability. Status is a read for every capability; a
    // provider search makes the node call out on the caller's say-so.
    struct Route {
        const char* method;
        const char* path;
        std::string_view role;
    };
    const std::vector<Route> routes{
        {"GET", "/api/v1/session", ""},
        {"GET", "/api/v1/status", role_view_status},
        {"GET", "/api/v1/status/diagnostics", role_view_status},
        {"POST", "/api/v1/status/connectivity", role_manager},
        {"GET", "/api/v1/catalogue/items", role_media_viewer},
        {"GET", "/api/v1/manage/unmatched", role_media_viewer},
        {"GET", "/api/v1/manage/providers/search", role_manager},
        {"GET", "/api/v1/manage/providers/artwork", role_manager},
        {"GET",
         "/api/v1/manage/providers/musicbrainz/releases/0f9a7b22-3c3e-4f5e-9d1a-2b8e6f7c5d41/tracks",
         role_manager},
        {"GET", "/api/v1/users", role_manage_users},
        {"PATCH", "/api/v1/users/me", role_media_viewer},
    };
    for (const auto& route : routes) {
        HttpRequest request;
        request.method = route.method;
        request.path = route.path;
        CHECK(Service::required_role(request) == route.role);
    }
}

// The replicated table's merge: deterministic and commutative, never evicting,
// and a deletion that a stale replica cannot undo.
MACHA_FAST_TEST("users", test_user_table_merge_rules) {
    struct Order {
        const char* what;
        UserRecord first;
        UserRecord second;
        bool present;
        std::optional<uint64_t> version;
        std::optional<NodeId> updated_by;
    };
    const std::vector<Order> orders{
        {"the higher version wins", sample("u1", 1), sample("u1", 2), true, 2, std::nullopt},
        {"at equal version a tombstone wins over a concurrent edit", sample("u1", 5, false, 9),
         sample("u1", 5, true, 1), false, std::nullopt, std::nullopt},
        {"equal live versions: the higher updated_by wins", sample("u1", 7, false, 1),
         sample("u1", 7, false, 2), true, 7, node_id(2)},
    };
    for (const auto& order : orders) {
        std::cerr << "row: " << order.what << "\n";
        UserStore a(16), b(16);
        a.apply(order.first);
        a.apply(order.second);
        b.apply(order.second);
        b.apply(order.first);
        CHECK(a.table_hash() == b.table_hash());
        const auto found = a.find("u1");
        CHECK(found.has_value() == order.present);
        if (found && order.version)
            CHECK(found->version == *order.version);
        if (found && order.updated_by)
            CHECK(found->updated_by == *order.updated_by);
    }

    // At capacity a new record is refused and every existing one kept;
    // dropping one would lock someone out or resurrect a deletion.
    {
        UserStore store(2);
        REQUIRE(store.apply(sample("u1", 1)));
        REQUIRE(store.apply(sample("u2", 1)));
        CHECK(!store.apply(sample("u3", 1)));
        CHECK(store.find("u1").has_value());
        CHECK(store.find("u2").has_value());
        CHECK(!store.find("u3").has_value());
        // An update to a record already present still applies.
        CHECK(store.apply(sample("u1", 2)));
        CHECK(store.find("u1")->version == 2);
    }

    // A deletion is a retained tombstone that retires sessions and blocks a
    // stale replica's pre-deletion record.
    {
        UserStore store(16);
        auto created =
            store.create("alice", "correct horse", {std::string(role_media_viewer)}, node_id(1));
        REQUIRE(created.has_value());
        auto removed = store.remove(created->id, node_id(1));
        REQUIRE(removed.has_value());
        CHECK(removed->tombstone);
        CHECK(removed->credential_generation > created->credential_generation);
        CHECK(!store.find(created->id).has_value());
        CHECK(store.tombstones() == 1);
        CHECK(store.all().size() == 1);
        CHECK(!store.apply(*created));
        CHECK(!store.find(created->id).has_value());
        CHECK(!store.verify("alice", "correct horse").ok);
    }
}

// Passwords: case-insensitive usernames, self-describing KDF parameters, a
// credential generation that every credential or role change bumps, and an
// anonymous account that can hold no password.
MACHA_FAST_TEST("users", test_user_credentials) {
    UserStore store(16);
    auto created =
        store.create("Alice", "correct horse", {std::string(role_manage_users)}, node_id(1));
    REQUIRE(created.has_value());
    CHECK(created->username == "alice");

    auto ok = store.verify("alice", "correct horse");
    CHECK(ok.ok);
    CHECK(ok.user_id == created->id);
    CHECK(count_of(ok.roles, role_manage_users) == 1);
    CHECK(store.verify("ALICE", "correct horse").ok);
    CHECK(!store.verify("alice", "wrong horse").ok);
    CHECK(!store.verify("nobody", "correct horse").ok);
    CHECK(!store.verify("alice", "").ok);

    // The record carries its KDF parameters, so changing defaults cannot
    // invalidate an existing password.
    CHECK(created->kdf == 1);
    CHECK(created->kdf_n > 0);
    CHECK(created->kdf_r > 0);
    CHECK(created->kdf_p > 0);

    // A duplicate username is refused rather than shadowing the first.
    CHECK(!store.create("alice", "another", {std::string(role_media_viewer)}, node_id(1))
               .has_value());

    // A password change bumps the generation; so does a roles-only change,
    // since a session carries the roles it was minted with.
    auto changed = store.update(created->id, "second", std::nullopt, node_id(1));
    REQUIRE(changed.has_value());
    CHECK(changed->credential_generation == created->credential_generation + 1);
    CHECK(!store.verify("alice", "correct horse").ok);
    CHECK(store.verify("alice", "second").ok);
    // It keeps manage_users: the last holder may not lose it.
    auto reroled = store.update(
        created->id, "",
        std::vector<std::string>{std::string(role_manage_users), std::string(role_importer)},
        node_id(1));
    REQUIRE(reroled.has_value());
    CHECK(reroled->credential_generation == changed->credential_generation + 1);
    CHECK(user_has_role(*reroled, role_importer));

    // Anonymous has no credential at all, and the store refuses it one, so no
    // caller can route around the rule; its roles stay editable.
    auto anonymous = store.create_without_password(
        anonymous_username, {std::string(role_media_viewer)}, node_id(1));
    REQUIRE(anonymous.has_value());
    CHECK(anonymous->kdf == 0);
    CHECK(!store.verify(anonymous_username, "").ok);
    CHECK(!store.verify(anonymous_username, "unused-password").ok);
    CHECK(!store.update(anonymous->id, "a-long-enough-password", std::nullopt, node_id(1))
               .has_value());
    CHECK(store.update(anonymous->id, "",
                       std::vector<std::string>{std::string(role_importer)}, node_id(1))
              .has_value());
}

// What reaches the disk and the wire: the sealed table, the user codec, and
// a session payload that stays v1 until it carries a user.
MACHA_FAST_TEST("users", test_user_and_session_encodings) {
    TempDir dir;
    const auto path = dir.path() / "users" / "users.bin";
    std::array<uint8_t, 32> key{};
    key.fill(7);
    std::string id;
    {
        UserStore store(16, path, key);
        auto created =
            store.create("carol", "secret", {std::string(role_manage_users)}, node_id(1));
        REQUIRE(created.has_value());
        id = created->id;
        // Write-through: durable when the call returns, with no persist() call.
    }
    REQUIRE(std::filesystem::exists(path));
    {
        std::ifstream input(path, std::ios::binary);
        const std::string raw((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
        // Encrypted: no plaintext on a stolen disk.
        CHECK(raw.find("carol") == std::string::npos);
        CHECK(raw.find("MACHUSR1") == std::string::npos);
    }
    {
        UserStore reloaded(16, path, key);
        auto found = reloaded.find(id);
        REQUIRE(found.has_value());
        CHECK(found->username == "carol");
        CHECK(reloaded.verify("carol", "secret").ok);
    }
    // The wrong key yields no empty-but-usable table that would replicate over
    // the real one, and the unreadable file is kept for inspection.
    {
        std::array<uint8_t, 32> other{};
        other.fill(9);
        UserStore reloaded(16, path, other);
        CHECK(reloaded.all().empty());
        bool kept = false;
        for (const auto& entry : std::filesystem::directory_iterator(path.parent_path()))
            if (entry.path().string().find(".corrupt.") != std::string::npos)
                kept = true;
        CHECK(kept);
    }

    std::vector<UserRecord> values{sample("u1", 3), sample("u2", 4, true, 2)};
    values[0].salt.fill(0xAB);
    values[0].password_hash = sha256(Bytes{9, 9, 9});
    values[0].kdf_n = 1u << 15;
    values[0].kdf_r = 8;
    values[0].kdf_p = 1;
    values[0].roles = expand_roles({std::string(role_manage_users)});
    auto decoded = decode_users(encode_users(values));
    REQUIRE(decoded.size() == 2);
    CHECK(decoded[0].id == "u1");
    CHECK(decoded[0].salt == values[0].salt);
    CHECK(decoded[0].password_hash == values[0].password_hash);
    CHECK(decoded[0].roles == values[0].roles);
    CHECK(decoded[0].kdf_n == values[0].kdf_n);
    CHECK(decoded[1].tombstone);
    CHECK(decoded[1].updated_by == node_id(2));

    AuthSession anonymous;
    anonymous.id = "s1";
    anonymous.token_hash = sha256(Bytes{1});
    anonymous.roles = {std::string(role_media_viewer)};
    anonymous.expires_unix_ms = unix_ms() + 60000;
    anonymous.version = 1;
    auto bound = anonymous;
    bound.id = "s2";
    bound.token_hash = sha256(Bytes{2});
    bound.user_id = "u1";
    bound.credential_generation = 4;

    const auto v1 = encode_sessions({anonymous});
    CHECK(std::string(v1.begin(), v1.begin() + 8) == "MACHSES1");
    const auto v2 = encode_sessions({bound});
    CHECK(std::string(v2.begin(), v2.begin() + 8) == "MACHSES2");
    auto from_v1 = decode_sessions(v1);
    REQUIRE(from_v1.size() == 1);
    CHECK(from_v1[0].user_id.empty());
    CHECK(from_v1[0].credential_generation == 0);
    auto from_v2 = decode_sessions(v2);
    REQUIRE(from_v2.size() == 1);
    CHECK(from_v2[0].user_id == "u1");
    CHECK(from_v2[0].credential_generation == 4);
    // A mixed payload has to be v2; the anonymous record still round-trips.
    auto mixed = decode_sessions(encode_sessions({anonymous, bound}));
    REQUIRE(mixed.size() == 2);
    CHECK(mixed[0].user_id.empty());
    CHECK(mixed[1].user_id == "u1");
}

// The first accounts: root and anonymous, created once, with root's password
// only in a 0600 file. A table with no accounts refuses even anonymous access
// until macha-users init creates them, with no restart.
MACHA_FAST_TEST("users", test_initial_accounts) {
    TempDir dir;
    const auto state = dir.path() / "state";
    TestCluster cluster;
    const auto& keys = cluster.keys();
    UserStore store(16, state / "users" / "users.bin", {});
    SessionConfig session;
    session.allow_anonymous = true;
    PasswordCredentialValidator validator(store, session);
    CHECK(validator.validate(Json(Json::Object{})).outcome == CredentialOutcome::disabled);

    auto genesis = create_initial_accounts(store, keys, state, node_id(1));
    REQUIRE(genesis.has_value());
    CHECK(validator.validate(Json(Json::Object{})).outcome == CredentialOutcome::ok);
    CHECK(genesis->root.username == root_username);
    CHECK(genesis->anonymous.username == anonymous_username);
    // No recovery key is issued, and root carries no envelope.
    CHECK(genesis->recovery_key.empty());
    CHECK(!genesis->root.recovery.present());
    CHECK(genesis->root.roles.size() == 5);
    // media_viewer as granted, plus the view_status it implies.
    CHECK(genesis->anonymous.roles.size() == 2);
    CHECK(user_has_role(genesis->anonymous, role_media_viewer));
    CHECK(user_has_role(genesis->anonymous, role_view_status));
    CHECK(store.verify(root_username, genesis->password).ok);
    const std::array<uint8_t, 32> no_hash{};
    const std::array<uint8_t, 16> no_salt{};
    CHECK(genesis->anonymous.kdf == 0);
    CHECK(genesis->anonymous.password_hash.bytes == no_hash);
    CHECK(genesis->anonymous.salt == no_salt);

    // Retypable: no vowels and no look-alike characters.
    CHECK(genesis->password.size() >= 20);
    for (const char c : genesis->password)
        CHECK(std::string("aeiou015lIOS").find(c) == std::string::npos);

    // The operator's only copy, readable by nobody else, naming macha-users as
    // the way back rather than a recovery key.
    REQUIRE(std::filesystem::exists(genesis->path));
    const auto mode = std::filesystem::status(genesis->path).permissions();
    CHECK((mode & std::filesystem::perms::group_all) == std::filesystem::perms::none);
    CHECK((mode & std::filesystem::perms::others_all) == std::filesystem::perms::none);
    {
        std::ifstream input(genesis->path);
        const std::string text((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
        CHECK(text.find(genesis->password) != std::string::npos);
        CHECK(text.find("macha-users") != std::string::npos);
        CHECK(text.find("recovery key") == std::string::npos);
    }

    // Once only: a non-empty table gets no second root.
    CHECK(!create_initial_accounts(store, keys, state, node_id(1)).has_value());
    // Root, the sole manage_users holder, cannot be removed.
    CHECK(!store.remove(genesis->root.id, node_id(1)).has_value());
    CHECK(store.find(genesis->root.id).has_value());
    // Tombstones count as accounts, so a deletion never re-enables creation.
    REQUIRE(store.remove(genesis->anonymous.id, node_id(1)).has_value());
    CHECK(!create_initial_accounts(store, keys, state, node_id(1)).has_value());
    UserStore deleted_only(16);
    auto gone = deleted_only.create("gone", "long-enough-pw", {std::string(role_media_viewer)},
                                    node_id(1));
    REQUIRE(gone.has_value());
    REQUIRE(deleted_only.remove(gone->id, node_id(1)).has_value());
    CHECK(!create_initial_accounts(deleted_only, keys, dir.path() / "deleted-only", node_id(1))
               .has_value());
}

// What each credential mints: anonymous is an ordinary account whose roles
// a visitor gets, a password never reaches it, and failed passwords lock out
// for a while.
MACHA_FAST_TEST("users", test_password_validator_outcomes) {
    UserStore store(16);
    SessionConfig open;
    open.allow_anonymous = true;
    auto closed = open;
    closed.allow_anonymous = false;
    PasswordCredentialValidator validator(store, open);
    PasswordCredentialValidator strict(store, closed);

    auto anonymous = store.create_without_password(
        anonymous_username, {std::string(role_media_viewer)}, node_id(1));
    REQUIRE(anonymous.has_value());

    // An anonymous session is an ordinary bound session, retired by the same
    // credential_generation check.
    auto minted = validator.validate(Json(Json::Object{}));
    REQUIRE(minted.outcome == CredentialOutcome::ok);
    CHECK(minted.credentials.user_id == anonymous->id);
    CHECK(minted.credentials.credential_generation == anonymous->credential_generation);
    CHECK(count_of(minted.credentials.roles, role_media_viewer) == 1);

    // A visitor's capabilities are the account's roles: an ordinary update,
    // effective on the next mint.
    REQUIRE(store.update(anonymous->id, "",
                         std::vector<std::string>{std::string(role_importer)}, node_id(1))
                .has_value());
    auto widened = validator.validate(Json(Json::Object{}));
    REQUIRE(widened.outcome == CredentialOutcome::ok);
    CHECK(count_of(widened.credentials.roles, role_importer) == 1);
    CHECK(widened.credentials.credential_generation != minted.credentials.credential_generation);

    // Granted nothing, it still mints a powerless session.
    REQUIRE(store.update(anonymous->id, "", std::vector<std::string>{}, node_id(1)).has_value());
    auto powerless = validator.validate(Json(Json::Object{}));
    REQUIRE(powerless.outcome == CredentialOutcome::ok);
    CHECK(powerless.credentials.roles.empty());
    CHECK(powerless.credentials.user_id == anonymous->id);

    // Switching anonymous access off is the only thing that refuses it.
    CHECK(strict.validate(Json(Json::Object{})).outcome == CredentialOutcome::disabled);

    // No password reaches the anonymous account, including the empty one:
    // allow_anonymous guards the no-credentials path alone.
    for (const auto* attempt : {"", "unused-password", "anonymous"})
        CHECK(strict.validate(credentials(anonymous_username, attempt)).outcome ==
              CredentialOutcome::rejected);

    // scrypt is expensive, so failures lock the username out for a while.
    auto limited_config = open;
    limited_config.failed_login_attempts = 2;
    limited_config.failed_login_lockout = 200ms;
    PasswordCredentialValidator limited(store, limited_config);
    REQUIRE(store.create("erin", "right", {std::string(role_media_viewer)}, node_id(1))
                .has_value());
    CHECK(limited.validate(credentials("erin", "wrong")).outcome == CredentialOutcome::rejected);
    CHECK(limited.validate(credentials("erin", "wrong")).outcome == CredentialOutcome::rejected);
    CHECK(limited.validate(credentials("erin", "right")).outcome ==
          CredentialOutcome::rate_limited);
    std::this_thread::sleep_for(260ms);
    CHECK(limited.validate(credentials("erin", "right")).outcome == CredentialOutcome::ok);
}

// Login is local: a node that never started and has no peers mints a bound
// session; a wrong password and an unknown user are the same answer.
MACHA_FAST_TEST("users", test_password_login_mints_a_bound_session) {
    TestCluster cluster;
    BareNode node(cluster.node_config("n1"), cluster.keys());
    auto created =
        node.users().create("dave", "hunter2", {std::string(role_manager)}, node.node_id());
    REQUIRE(created.has_value());

    SessionApi api(node, node.accounts());
    auto response = api.handle(login_request("dave", "hunter2"));
    REQUIRE(response.status == 201);
    auto body = Json::parse(json_body(response));
    CHECK(body.find("user_id")->asString() == created->id);
    auto validated = node.sessions().validate(body.find("token")->asString());
    REQUIRE(validated.has_value());
    CHECK(validated->user_id == created->id);
    CHECK(validated->credential_generation == created->credential_generation);
    CHECK(session_has_role(*validated, role_manager));
    CHECK(session_has_role(*validated, role_media_viewer));
    CHECK(!session_has_role(*validated, role_manage_users));

    for (const auto& [username, password] :
         {std::pair{"dave", "wrong"}, std::pair{"nobody", "hunter2"}}) {
        auto rejected = api.handle(login_request(username, password));
        CHECK(rejected.status == 401);
        CHECK(json_body(rejected).find("invalid_credentials") != std::string::npos);
    }
}

// The users API: admin-only, hashes never readable, policy refusals with
// their own codes, reserved accounts, the last user manager, and the
// mutability it tells the client.
MACHA_FAST_TEST("users", test_users_api) {
    TestCluster cluster;
    BareNode node(cluster.node_config("n1"), cluster.keys());
    TempDir dir;
    auto genesis =
        create_initial_accounts(node.users(), cluster.keys(), dir.path(), node.node_id());
    REQUIRE(genesis.has_value());
    UsersApi api(node, node.accounts());
    const auto parse = [](const HttpResponse& response) { return Json::parse(json_body(response)); };

    const auto create_body =
        R"({"username":"frank","password":"long-enough-pw","roles":["manager"]})";
    auto created =
        api.handle(users_request("POST", "/api/v1/users", admin_identity(), create_body));
    REQUIRE(created.status == 201);
    auto body = parse(created);
    const auto id = body.find("id")->asString();
    // Credentials have no read path at all.
    CHECK(json_body(created).find("long-enough-pw") == std::string::npos);
    CHECK(json_body(created).find("salt") == std::string::npos);
    CHECK(json_body(created).find("password_hash") == std::string::npos);
    // manager plus the media_viewer and view_status every role implies.
    CHECK(body.find("roles")->asArray().size() == 3);
    // The record carries its LWW counter for optimistic concurrency.
    CHECK(body.find("version")->asUInt64() == 1);
    CHECK(body.find("mutable")->find("delete")->asBool());
    CHECK(body.find("mutable")->find("set_roles")->asBool());

    auto listed = api.handle(users_request("GET", "/api/v1/users", admin_identity()));
    REQUIRE(listed.status == 200);
    CHECK(json_body(listed).find("frank") != std::string::npos);
    CHECK(json_body(listed).find("password_hash") == std::string::npos);
    // Every collection in this API answers under "items".
    REQUIRE(parse(listed).find("items") != nullptr);
    CHECK(parse(listed).find("items")->isArray());

    // Refusals, each with its own status and code.
    struct Refusal {
        const char* method;
        std::string path;
        SessionIdentity identity;
        std::string body;
        int status;
        const char* code;
    };
    const SessionIdentity viewer{"s", Hash256{}, {std::string(role_media_viewer)}, id};
    const SessionIdentity visitor{
        "s", Hash256{}, {std::string(role_media_viewer)}, genesis->anonymous.id};
    const SessionIdentity no_account{"s", Hash256{}, {std::string(role_media_viewer)}, {}};
    const std::vector<Refusal> refusals{
        {"POST", "/api/v1/users", admin_identity(),
         R"({"username":"x","password":"long-enough-pw","roles":["wizard"]})", 400, nullptr},
        {"POST", "/api/v1/users", admin_identity(), R"({"username":"y","password":"short"})", 400,
         "password_rejected"},
        {"POST", "/api/v1/users", admin_identity(), create_body, 409, "username_taken"},
        {"POST", "/api/v1/users", admin_identity(),
         R"({"username":"root","password":"longenough"})", 409, "reserved_username"},
        {"POST", "/api/v1/users", admin_identity(),
         R"({"username":"ROOT","password":"longenough"})", 409, "reserved_username"},
        {"POST", "/api/v1/users", admin_identity(),
         R"({"username":"anonymous","password":"longenough"})", 409, "reserved_username"},
        // A self PATCH cannot escalate privileges.
        {"PATCH", "/api/v1/users/me", viewer, R"({"roles":["manage_users"]})", 403, nullptr},
        // Anonymous takes no password, from itself or an admin.
        {"PATCH", "/api/v1/users/me", visitor, R"({"password":"a-long-enough-pw"})", 409,
         "no_password"},
        {"PATCH", "/api/v1/users/" + genesis->anonymous.id, admin_identity(),
         R"({"password":"a-long-enough-pw"})", 409, nullptr},
        // Root and anonymous cannot be removed.
        {"DELETE", "/api/v1/users/" + genesis->root.id, admin_identity("someone-else"), "", 409,
         "reserved_user"},
        {"DELETE", "/api/v1/users/" + genesis->anonymous.id, admin_identity("someone-else"), "",
         409, "reserved_user"},
        // Root is the only manage_users holder: not demotable.
        {"PATCH", "/api/v1/users/" + genesis->root.id, admin_identity(),
         R"({"roles":["media_viewer"]})", 409, "last_user_manager"},
        // An anonymous session has no account to show.
        {"GET", "/api/v1/users/me", no_account, "", 404, "no_account"},
    };
    for (const auto& refusal : refusals) {
        auto response = api.handle(
            users_request(refusal.method, refusal.path, refusal.identity, refusal.body));
        CHECK(response.status == refusal.status);
        if (refusal.code)
            CHECK(json_body(response).find(refusal.code) != std::string::npos);
    }
    CHECK(node.users().find(genesis->root.id).has_value());
    CHECK(node.users().find(genesis->anonymous.id).has_value());
    CHECK(user_has_role(*node.users().find(genesis->root.id), role_manage_users));

    // Changing your own password hands back a fresh session.
    auto changed = api.handle(
        users_request("PATCH", "/api/v1/users/me", viewer, R"({"password":"another-long-pw"})"));
    REQUIRE(changed.status == 200);
    REQUIRE(parse(changed).find("token") != nullptr);
    CHECK(node.sessions().validate(parse(changed).find("token")->asString()).has_value());
    CHECK(node.users().verify("frank", "another-long-pw").ok);

    // The client is told which fields it may offer: anonymous keeps editable
    // roles, but no rename, delete or password.
    auto anonymous =
        api.handle(users_request("GET", "/api/v1/users/" + genesis->anonymous.id, admin_identity()));
    REQUIRE(anonymous.status == 200);
    const auto anonymous_body = parse(anonymous);
    const auto* may = anonymous_body.find("mutable");
    REQUIRE(may != nullptr);
    CHECK(!may->find("rename")->asBool());
    CHECK(!may->find("delete")->asBool());
    CHECK(may->find("set_roles")->asBool());
    CHECK(!may->find("set_password")->asBool());

    // The sole manager's roles can be added to; the pinned role is named.
    const auto root_id = genesis->root.id;
    CHECK(node.users().sole_user_manager(root_id));
    auto widened = api.handle(users_request("PATCH", "/api/v1/users/" + root_id, admin_identity(),
                                            R"({"roles":["manage_users","media_viewer"]})"));
    CHECK(widened.status == 200);
    const auto widened_body = parse(widened);
    const auto* pinned = widened_body.find("mutable");
    REQUIRE(pinned != nullptr);
    CHECK(pinned->find("set_roles")->asBool());
    REQUIRE(pinned->find("required_roles")->asArray().size() == 1);
    CHECK(pinned->find("required_roles")->asArray().front().asString() == role_manage_users);
    CHECK(pinned->find("required_roles_reason")->asString() == "last_user_manager");

    // The protection follows the role, not root: a second holder frees root,
    // and the deputy, once alone, inherits it.
    auto deputy = api.handle(users_request("POST", "/api/v1/users", admin_identity(),
                                           R"({"username":"deputy","password":"long-enough-pw",)"
                                           R"("roles":["manage_users"]})"));
    REQUIRE(deputy.status == 201);
    CHECK(!node.users().sole_user_manager(root_id));
    auto demoted = api.handle(users_request("PATCH", "/api/v1/users/" + root_id, admin_identity(),
                                            R"({"roles":["media_viewer"]})"));
    CHECK(demoted.status == 200);
    CHECK(!user_has_role(*node.users().find(root_id), role_manage_users));
    const auto deputy_id = parse(deputy).find("id")->asString();
    CHECK(node.users().sole_user_manager(deputy_id));
    auto deputy_demoted = api.handle(users_request("PATCH", "/api/v1/users/" + deputy_id,
                                                   admin_identity(), R"({"roles":["media_viewer"]})"));
    CHECK(deputy_demoted.status == 409);
    auto deputy_removed =
        api.handle(users_request("DELETE", "/api/v1/users/" + deputy_id, admin_identity()));
    CHECK(deputy_removed.status == 409);
    CHECK(json_body(deputy_removed).find("last_user_manager") != std::string::npos);
    auto deputy_view =
        api.handle(users_request("GET", "/api/v1/users/" + deputy_id, admin_identity()));
    CHECK(!parse(deputy_view).find("mutable")->find("delete")->asBool());
}

MACHA_FAST_TEST("users", test_recovery_key_machinery_is_dormant_but_sound) {
    // Nothing issues or accepts a recovery key, but the machinery is tested:
    // holding the cluster key must not yield the recovery key.
    TestCluster cluster;
    const auto& keys = cluster.keys();

    auto issued = issue_recovery_key(keys);
    CHECK(issued.key.size() == 64);
    CHECK(issued.envelope.present());
    CHECK(recovery_key_matches(issued.envelope, keys, issued.key));

    // The stored envelope holds neither the key nor a hash of it.
    const auto raw = unhex(issued.key);
    REQUIRE(raw.has_value());
    const auto envelope_bytes = Bytes(issued.envelope.ephemeral_public.begin(),
                                      issued.envelope.ephemeral_public.end());
    CHECK(std::search(envelope_bytes.begin(), envelope_bytes.end(), raw->begin(), raw->end()) ==
          envelope_bytes.end());
    const auto ciphertext = Bytes(issued.envelope.ciphertext.begin(),
                                  issued.envelope.ciphertext.end());
    CHECK(std::search(ciphertext.begin(), ciphertext.end(), raw->begin(), raw->end()) ==
          ciphertext.end());
    // Nor is the cluster key sitting in the envelope in the clear.
    CHECK(std::search(ciphertext.begin(), ciphertext.end(), keys.master.begin(),
                      keys.master.end()) == ciphertext.end());

    // A different key, a malformed key, and an empty key all fail closed.
    auto other = issue_recovery_key(keys);
    CHECK(!recovery_key_matches(issued.envelope, keys, other.key));
    CHECK(!recovery_key_matches(issued.envelope, keys, ""));
    CHECK(!recovery_key_matches(issued.envelope, keys, "not-hex"));
    CHECK(!recovery_key_matches(issued.envelope, keys, std::string(64, 'z')));
    // Right length, wrong value.
    CHECK(!recovery_key_matches(issued.envelope, keys, std::string(64, '0')));

    // Re-issuing invalidates the previous key.
    CHECK(recovery_key_matches(other.envelope, keys, other.key));
    CHECK(!recovery_key_matches(other.envelope, keys, issued.key));

    // An envelope sealed under a different cluster key does not verify, even
    // with its own key: the plaintext comparison catches what the AEAD tag does not.
    ClusterKeys foreign = keys;
    foreign.master.front() = static_cast<uint8_t>(foreign.master.front() ^ 0xFF);
    CHECK(!recovery_key_matches(issued.envelope, foreign, issued.key));
}

// Kept integrated: convergence is the gossip between two real nodes, across a
// node that was down while the table changed.
//
// n1 runs alone with n2 configured but absent: login waits on no peer, and
// what it creates and deletes meanwhile reaches n2 when it joins, with no
// further mutation and no explicit announcement; a deletion arrives as a
// deletion, not a resurrection. Then a password change on n1 retires a
// session minted on n2.
MACHA_TEST("users", test_accounts_converge_across_nodes) {
    TestCluster cluster;
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = cluster.node_config("n1", p1, {{"127.0.0.1", p2}});
    auto c2 = cluster.node_config("n2", p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c1.heartbeat = c2.heartbeat = 20ms;
    // The gossip backstop rides the telemetry tick.
    c1.telemetry_interval = c2.telemetry_interval = 250ms;

    BareNode n1(c1, cluster.keys());
    n1.start();
    auto created = n1.users().create("grace", "long-enough-pw",
                                     {std::string(role_manage_users)}, n1.node_id());
    REQUIRE(created.has_value());
    // Announced while n2 does not exist: n1 has no peer to tell.
    n1.propagate_users();
    auto deleted =
        n1.users().create("heidi", "pw", {std::string(role_media_viewer)}, n1.node_id());
    REQUIRE(deleted.has_value());
    REQUIRE(n1.users().remove(deleted->id, n1.node_id()).has_value());

    // Nothing in the login path waits on a peer, even one not yet declared dead.
    SessionApi api(n1, n1.accounts());
    const auto started = Clock::now();
    auto login = api.handle(login_request("grace", "long-enough-pw"));
    CHECK(Clock::now() - started < 2s);
    REQUIRE(login.status == 201);
    CHECK(n1.sessions().validate(Json::parse(json_body(login)).find("token")->asString())
              .has_value());

    BareNode n2(c2, cluster.keys());
    n2.start();
    REQUIRE(wait_until([&] {
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2;
    }));
    REQUIRE(wait_until([&] {
        return n2.users().find(created->id).has_value() && n2.users().tombstones() == 1;
    }, 10s));
    // n2 authenticates the user itself, without reaching n1.
    auto check = n2.users().verify("grace", "long-enough-pw");
    CHECK(check.ok);
    CHECK(check.user_id == created->id);
    CHECK(!n2.users().find(deleted->id).has_value());
    CHECK(!n2.users().verify("heidi", "pw").ok);
    CHECK(n1.users().table_hash() == n2.users().table_hash());

    // A password change invalidates sessions minted against the old one, on
    // every node, by replicating one record.
    auto minted = n2.sessions().create(check.roles, check.user_id, check.credential_generation);
    REQUIRE(minted.has_value());
    auto changed = n1.users().update(created->id, "another-long-pw", std::nullopt, n1.node_id());
    REQUIRE(changed.has_value());
    n1.propagate_users();
    REQUIRE(wait_until([&] {
        auto seen = n2.users().find(created->id);
        return seen && seen->credential_generation == changed->credential_generation;
    }));
    // The session record is untouched; the generation mismatch retires it.
    CHECK(n2.sessions().validate(minted->bearer_token).has_value());
    CHECK(n2.users().find(check.user_id)->credential_generation !=
          minted->session.credential_generation);

    n2.stop();
    n1.stop();
}
