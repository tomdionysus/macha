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

} // namespace

MACHA_FAST_TEST("users", test_roles_are_capabilities_not_a_ladder) {
    // Every capability implies media_viewer and view_status, and nothing else
    // implies anything.
    auto importer = expand_roles({std::string(role_importer)});
    CHECK(importer.size() == 3);
    CHECK(std::count(importer.begin(), importer.end(), role_media_viewer) == 1);
    CHECK(std::count(importer.begin(), importer.end(), role_view_status) == 1);
    CHECK(std::count(importer.begin(), importer.end(), role_manager) == 0);
    CHECK(std::count(importer.begin(), importer.end(), role_manage_users) == 0);

    auto manager = expand_roles({std::string(role_manager)});
    CHECK(std::count(manager.begin(), manager.end(), role_importer) == 0);
    CHECK(std::count(manager.begin(), manager.end(), role_manage_users) == 0);

    auto manage_users = expand_roles({std::string(role_manage_users)});
    CHECK(std::count(manage_users.begin(), manage_users.end(), role_manager) == 0);

    auto viewer = expand_roles({std::string(role_media_viewer)});
    CHECK(viewer.size() == 2);
    CHECK(std::count(viewer.begin(), viewer.end(), role_view_status) == 1);

    // view_status implies nothing; alone it grants no media.
    auto status_only = expand_roles({std::string(role_view_status)});
    CHECK(status_only.size() == 1);
    CHECK(std::count(status_only.begin(), status_only.end(), role_view_status) == 1);
    CHECK(std::count(status_only.begin(), status_only.end(), role_media_viewer) == 0);

    // root holds every capability explicitly, not by implication.
    auto root = expand_roles(all_roles());
    CHECK(root.size() == 5);

    // Already-expanded input must not duplicate.
    CHECK(expand_roles(expand_roles(all_roles())).size() == 5);
}

MACHA_FAST_TEST("users", test_provider_requests_need_the_manager_role_even_to_read) {
    // A read of the management API is a viewer's, but a provider search makes
    // the node call out on the caller's say-so.
    HttpRequest request;
    request.method = "GET";
    request.path = "/api/v1/manage/unmatched";
    CHECK(Service::required_role(request) == role_media_viewer);
    request.path = "/api/v1/manage/providers/search";
    CHECK(Service::required_role(request) == role_manager);
    request.path = "/api/v1/manage/providers/artwork";
    CHECK(Service::required_role(request) == role_manager);
}

MACHA_FAST_TEST("users", test_user_merge_is_deterministic_and_commutative) {
    // Replicas that saw the same writes in any order land on the same record.
    const auto low = sample("u1", 1, false, 1);
    const auto high = sample("u1", 2, false, 1);
    {
        UserStore a(16), b(16);
        a.apply(low);
        a.apply(high);
        b.apply(high);
        b.apply(low);
        CHECK(a.table_hash() == b.table_hash());
        CHECK(a.find("u1")->version == 2);
    }
    // At equal version a tombstone wins over a concurrent edit.
    {
        UserStore a(16), b(16);
        const auto live = sample("u1", 5, false, 9);
        const auto dead = sample("u1", 5, true, 1);
        a.apply(live);
        a.apply(dead);
        b.apply(dead);
        b.apply(live);
        CHECK(a.table_hash() == b.table_hash());
        CHECK(!a.find("u1").has_value());
    }
    // Equal version, both live: the higher updated_by breaks the tie the same
    // way on both sides.
    {
        UserStore a(16), b(16);
        const auto one = sample("u1", 7, false, 1);
        const auto two = sample("u1", 7, false, 2);
        a.apply(one);
        a.apply(two);
        b.apply(two);
        b.apply(one);
        CHECK(a.table_hash() == b.table_hash());
        CHECK(a.find("u1")->updated_by == node_id(2));
    }
}

MACHA_FAST_TEST("users", test_user_table_never_evicts_to_make_room) {
    // Dropping a record would lock someone out or resurrect a deleted account,
    // so at capacity a new record is refused and every existing one kept.
    UserStore store(2);
    REQUIRE(store.apply(sample("u1", 1)));
    REQUIRE(store.apply(sample("u2", 1)));
    CHECK(!store.apply(sample("u3", 1)));
    CHECK(store.find("u1").has_value());
    CHECK(store.find("u2").has_value());
    CHECK(!store.find("u3").has_value());

    // An update to a record already present still applies at capacity.
    CHECK(store.apply(sample("u1", 2)));
    CHECK(store.find("u1")->version == 2);
}

MACHA_FAST_TEST("users", test_user_tombstone_is_retained_and_blocks_resurrection) {
    UserStore store(16);
    auto created = store.create("alice", "correct horse", {std::string(role_media_viewer)}, node_id(1));
    REQUIRE(created.has_value());
    const auto generation_before = created->credential_generation;

    auto removed = store.remove(created->id, node_id(1));
    REQUIRE(removed.has_value());
    CHECK(removed->tombstone);
    // Deleting bumps the credential generation, retiring live sessions too.
    CHECK(removed->credential_generation > generation_before);
    CHECK(!store.find(created->id).has_value());
    CHECK(store.tombstones() == 1);
    CHECK(store.all().size() == 1);

    // A stale replica re-offering the pre-deletion record must not bring the
    // account back.
    auto stale = *created;
    CHECK(!store.apply(stale));
    CHECK(!store.find(created->id).has_value());
    CHECK(!store.verify("alice", "correct horse").ok);
}

MACHA_FAST_TEST("users", test_password_verification_and_parameters) {
    UserStore store(16);
    auto created = store.create("Alice", "correct horse", {std::string(role_manage_users)}, node_id(1));
    REQUIRE(created.has_value());
    // Usernames are matched case-insensitively; the stored form is normalized.
    CHECK(created->username == "alice");

    auto ok = store.verify("alice", "correct horse");
    CHECK(ok.ok);
    CHECK(ok.user_id == created->id);
    CHECK(std::count(ok.roles.begin(), ok.roles.end(), role_manage_users) == 1);

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
    CHECK(!store.create("alice", "another", {std::string(role_media_viewer)}, node_id(1)).has_value());
}

MACHA_FAST_TEST("users", test_password_change_bumps_credential_generation) {
    UserStore store(16);
    auto created = store.create("bob", "first", {std::string(role_media_viewer)}, node_id(1));
    REQUIRE(created.has_value());

    auto changed = store.update(created->id, "second", std::nullopt, node_id(1));
    REQUIRE(changed.has_value());
    CHECK(changed->credential_generation == created->credential_generation + 1);
    CHECK(!store.verify("bob", "first").ok);
    CHECK(store.verify("bob", "second").ok);

    // A roles-only change invalidates sessions too, since a session carries the
    // roles it was minted with.
    auto reroled = store.update(created->id, "",
                                std::vector<std::string>{std::string(role_manage_users)},
                                node_id(1));
    REQUIRE(reroled.has_value());
    CHECK(reroled->credential_generation == changed->credential_generation + 1);
    CHECK(user_has_role(*reroled, role_manage_users));
}

MACHA_FAST_TEST("users", test_user_table_persist_reload_is_sealed) {
    TempDir dir;
    const auto path = dir.path() / "users" / "users.bin";
    std::array<uint8_t, 32> key{};
    key.fill(7);
    std::string id;
    {
        UserStore store(16, path, key);
        auto created = store.create("carol", "secret", {std::string(role_manage_users)}, node_id(1));
        REQUIRE(created.has_value());
        id = created->id;
        // Write-through: durable when the call returns, with no persist() call.
    }
    REQUIRE(std::filesystem::exists(path));
    {
        std::ifstream input(path, std::ios::binary);
        const std::string raw((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
        // The file is encrypted: no plaintext on a stolen disk.
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
    // The wrong key must not yield an empty-but-usable table that would
    // replicate over the real one.
    {
        std::array<uint8_t, 32> other{};
        other.fill(9);
        UserStore reloaded(16, path, other);
        CHECK(reloaded.all().empty());
        // The unreadable file is kept for inspection rather than overwritten.
        bool kept = false;
        for (const auto& entry : std::filesystem::directory_iterator(path.parent_path()))
            if (entry.path().string().find(".corrupt.") != std::string::npos)
                kept = true;
        CHECK(kept);
    }
}

MACHA_FAST_TEST("users", test_user_codec_round_trip) {
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
}

MACHA_FAST_TEST("users", test_session_wire_stays_v1_until_a_user_is_present) {
    // A payload uses the v2 magic only when it carries a field v1 cannot hold.
    AuthSession anonymous;
    anonymous.id = "s1";
    anonymous.token_hash = sha256(Bytes{1});
    anonymous.roles = {std::string(role_media_viewer)};
    anonymous.expires_unix_ms = unix_ms() + 60000;
    anonymous.version = 1;

    const auto v1 = encode_sessions({anonymous});
    CHECK(std::string(v1.begin(), v1.begin() + 8) == "MACHSES1");

    auto bound = anonymous;
    bound.id = "s2";
    bound.token_hash = sha256(Bytes{2});
    bound.user_id = "u1";
    bound.credential_generation = 4;

    const auto v2 = encode_sessions({bound});
    CHECK(std::string(v2.begin(), v2.begin() + 8) == "MACHSES2");

    // Both decode, and v1 decodes into a record with no user identity.
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

MACHA_FAST_TEST("users", test_anonymous_is_an_ordinary_account) {
    TestCluster cluster;
    auto config = cluster.node_config("n1");
    config.session.allow_anonymous = true;
    BareNode node(config, cluster.keys());
    auto anonymous = node.users().create_without_password(
        anonymous_username, {std::string(role_media_viewer)}, node.node_id());
    REQUIRE(anonymous.has_value());

    PasswordCredentialValidator validator(node.users(), config.session);
    auto minted = validator.validate(Json(Json::Object{}));
    REQUIRE(minted.outcome == CredentialOutcome::ok);
    // An anonymous session is an ordinary bound session, retired by the same
    // credential_generation check.
    CHECK(minted.credentials.user_id == anonymous->id);
    CHECK(minted.credentials.credential_generation == anonymous->credential_generation);
    CHECK(std::count(minted.credentials.roles.begin(), minted.credentials.roles.end(),
                     role_media_viewer) == 1);

    // A visitor's capabilities are the anonymous account's roles: an ordinary
    // update, effective on the next mint.
    auto widened = node.users().update(
        anonymous->id, "", std::vector<std::string>{std::string(role_importer)}, node.node_id());
    REQUIRE(widened.has_value());
    auto after = validator.validate(Json(Json::Object{}));
    REQUIRE(after.outcome == CredentialOutcome::ok);
    CHECK(std::count(after.credentials.roles.begin(), after.credentials.roles.end(),
                     role_importer) == 1);
    CHECK(after.credentials.credential_generation != minted.credentials.credential_generation);

    // allow_anonymous: false stops browsing without an account, with a
    // refusal that says why rather than a bare 401.
    auto closed = config.session;
    closed.allow_anonymous = false;
    PasswordCredentialValidator strict(node.users(), closed);
    CHECK(strict.validate(Json(Json::Object{})).outcome == CredentialOutcome::disabled);
}

// Anonymous granted nothing still mints a session; only switching anonymous
// access off refuses.
MACHA_FAST_TEST("users", test_anonymous_with_no_roles_still_mints_a_powerless_session) {
    TestCluster cluster;
    auto config = cluster.node_config("n1");
    config.session.allow_anonymous = true;
    BareNode node(config, cluster.keys());
    auto anonymous =
        node.users().create_without_password(anonymous_username, {}, node.node_id());
    REQUIRE(anonymous.has_value());
    CHECK(anonymous->roles.empty());

    PasswordCredentialValidator validator(node.users(), config.session);
    auto minted = validator.validate(Json(Json::Object{}));
    REQUIRE(minted.outcome == CredentialOutcome::ok);
    CHECK(minted.credentials.roles.empty());
    CHECK(minted.credentials.user_id == anonymous->id);

    // Switching anonymous access off is the only thing that refuses.
    auto closed = config.session;
    closed.allow_anonymous = false;
    PasswordCredentialValidator strict(node.users(), closed);
    CHECK(strict.validate(Json(Json::Object{})).outcome == CredentialOutcome::disabled);
}

// allow_anonymous guards only the no-credentials path, so a password on the
// anonymous account would be a second door beside the switch.
MACHA_FAST_TEST("users", test_anonymous_has_no_password_and_cannot_be_given_one) {
    TestCluster cluster;
    auto config = cluster.node_config("n1");
    config.session.allow_anonymous = false;
    BareNode node(config, cluster.keys());
    UsersApi api(node, node.accounts());
    auto anonymous = node.users().create_without_password(
        anonymous_username, {std::string(role_media_viewer)}, node.node_id());
    REQUIRE(anonymous.has_value());
    CHECK(anonymous->kdf == 0);

    // No password reaches this account, including the empty one.
    PasswordCredentialValidator validator(node.users(), config.session);
    for (const auto* attempt : {"", "unused-password", "anonymous"}) {
        Json::Object credentials;
        credentials["username"] = std::string(anonymous_username);
        credentials["password"] = std::string(attempt);
        CHECK(validator.validate(Json(credentials)).outcome == CredentialOutcome::rejected);
    }
    CHECK(!node.users().verify(anonymous_username, "unused-password").ok);

    // The store itself refuses one, so no caller can route around the rule.
    CHECK(!node.users()
               .update(anonymous->id, "a-long-enough-password", std::nullopt, node.node_id())
               .has_value());
    // Roles remain editable.
    REQUIRE(node.users()
                .update(anonymous->id, "",
                        std::vector<std::string>{std::string(role_media_viewer)}, node.node_id())
                .has_value());

    // A self PATCH from an anonymous session carrying a password is refused.
    SessionIdentity visitor{"s", Hash256{}, {std::string(role_media_viewer)}, anonymous->id};
    auto refused = api.handle(
        users_request("PATCH", "/api/v1/users/me", visitor, R"({"password":"a-long-enough-pw"})"));
    CHECK(refused.status == 409);
    CHECK(json_body(refused).find("no_password") != std::string::npos);
    auto refused_by_id =
        api.handle(users_request("PATCH", "/api/v1/users/" + anonymous->id, admin_identity(),
                                 R"({"password":"a-long-enough-pw"})"));
    CHECK(refused_by_id.status == 409);

    // The client is told, so it does not offer the field.
    auto listed = api.handle(
        users_request("GET", "/api/v1/users/" + anonymous->id, admin_identity()));
    REQUIRE(listed.status == 200);
    const auto body = Json::parse(json_body(listed));
    CHECK(!body.find("mutable")->find("set_password")->asBool());
}

MACHA_FAST_TEST("users", test_genesis_creates_root_and_anonymous_once) {
    TempDir dir;
    const auto state = dir.path() / "state";
    TestCluster cluster;
    const auto& keys = cluster.keys();
    UserStore store(16, state / "users" / "users.bin", {});

    auto genesis = create_initial_accounts(store, keys, state, node_id(1));
    REQUIRE(genesis.has_value());
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
    // Anonymous has no credential at all: nothing to leak or guess.
    const std::array<uint8_t, 32> no_hash{};
    const std::array<uint8_t, 16> no_salt{};
    CHECK(genesis->anonymous.kdf == 0);
    CHECK(genesis->anonymous.password_hash.bytes == no_hash);
    CHECK(genesis->anonymous.salt == no_salt);

    // The password is retypable: no vowels and no look-alike characters.
    CHECK(genesis->password.size() >= 20);
    for (const char c : genesis->password)
        CHECK(std::string("aeiou015lIOS").find(c) == std::string::npos);

    // The password file names macha-users as the way back, not a recovery key.
    {
        std::ifstream input(genesis->path);
        const std::string text((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
        CHECK(text.find("macha-users") != std::string::npos);
        CHECK(text.find("recovery key") == std::string::npos);
    }

    // The operator's only copy, readable by nobody else.
    REQUIRE(std::filesystem::exists(genesis->path));
    const auto mode = std::filesystem::status(genesis->path).permissions();
    CHECK((mode & std::filesystem::perms::group_all) == std::filesystem::perms::none);
    CHECK((mode & std::filesystem::perms::others_all) == std::filesystem::perms::none);
    {
        std::ifstream input(genesis->path);
        const std::string text((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
        CHECK(text.find(genesis->password) != std::string::npos);
    }

    // Once only: a non-empty table gets no second root.
    CHECK(!create_initial_accounts(store, keys, state, node_id(1)).has_value());

    // Root, the sole manage_users holder, cannot be removed.
    CHECK(!store.remove(genesis->root.id, node_id(1)).has_value());
    CHECK(store.find(genesis->root.id).has_value());

    // Tombstones count as accounts, so a deletion never re-enables creation.
    REQUIRE(store.remove(genesis->anonymous.id, node_id(1)).has_value());
    CHECK(!create_initial_accounts(store, keys, state, node_id(1)).has_value());
}

MACHA_FAST_TEST("users", test_root_and_anonymous_cannot_be_removed_or_recreated) {
    TestCluster cluster;
    BareNode node(cluster.node_config("n1"), cluster.keys());
    UsersApi api(node, node.accounts());
    TempDir dir;
    auto genesis = create_initial_accounts(node.users(), cluster.keys(), dir.path(), node.node_id());
    REQUIRE(genesis.has_value());

    for (const auto& id : {genesis->root.id, genesis->anonymous.id}) {
        auto refused = api.handle(users_request("DELETE", "/api/v1/users/" + id,
                                                admin_identity("someone-else")));
        CHECK(refused.status == 409);
        CHECK(json_body(refused).find("reserved_user") != std::string::npos);
        CHECK(node.users().find(id).has_value());
    }

    // The client is told which fields it may offer.
    auto listed = api.handle(users_request("GET", "/api/v1/users/" + genesis->anonymous.id,
                                           admin_identity()));
    REQUIRE(listed.status == 200);
    auto body = Json::parse(json_body(listed));
    const auto* may = body.find("mutable");
    REQUIRE(may != nullptr);
    CHECK(!may->find("rename")->asBool());
    CHECK(!may->find("delete")->asBool());
    // Anonymous's roles stay editable; its password is not offered.
    CHECK(may->find("set_roles")->asBool());
    CHECK(!may->find("set_password")->asBool());

    // Neither name can be taken by a new account.
    for (const auto* name : {"root", "ROOT", "anonymous"}) {
        auto taken = api.handle(
            users_request("POST", "/api/v1/users", admin_identity(),
                          std::string(R"({"username":")") + name +
                              R"(","password":"longenough"})"));
        CHECK(taken.status == 409);
        CHECK(json_body(taken).find("reserved_username") != std::string::npos);
    }
}

MACHA_FAST_TEST("users", test_password_login_is_local_and_mints_a_bound_session) {
    TestCluster cluster;
    auto config = cluster.node_config("n1");
    BareNode node(config, cluster.keys());
    auto created =
        node.users().create("dave", "hunter2", {std::string(role_manager)}, node.node_id());
    REQUIRE(created.has_value());

    SessionApi api(node, node.accounts());
    HttpRequest request;
    request.method = "POST";
    request.path = "/api/v1/session";
    Json::Object credentials{{"username", Json(std::string("dave"))},
                             {"password", Json(std::string("hunter2"))}};
    Json::Object root{{"credentials", Json(std::move(credentials))}};
    const auto text = Json(std::move(root)).dump();
    request.body.assign(text.begin(), text.end());

    // The node has never started and has no peers: login must still work.
    auto response = api.handle(request);
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

    // Wrong password and unknown user are the same answer.
    Json::Object bad{{"username", Json(std::string("dave"))},
                     {"password", Json(std::string("wrong"))}};
    Json::Object bad_root{{"credentials", Json(std::move(bad))}};
    const auto bad_text = Json(std::move(bad_root)).dump();
    request.body.assign(bad_text.begin(), bad_text.end());
    auto rejected = api.handle(request);
    CHECK(rejected.status == 401);
    CHECK(json_body(rejected).find("invalid_credentials") != std::string::npos);
}

MACHA_FAST_TEST("users", test_failed_logins_lock_out_then_recover) {
    TestCluster cluster;
    auto config = cluster.node_config("n1");
    config.session.failed_login_attempts = 2;
    config.session.failed_login_lockout = 200ms;
    BareNode node(config, cluster.keys());
    REQUIRE(node.users()
                .create("erin", "right", {std::string(role_media_viewer)}, node.node_id())
                .has_value());

    PasswordCredentialValidator validator(node.users(), config.session);
    auto attempt = [&](const char* password) {
        Json::Object credentials{{"username", Json(std::string("erin"))},
                                 {"password", Json(std::string(password))}};
        return validator.validate(Json(std::move(credentials))).outcome;
    };

    CHECK(attempt("wrong") == CredentialOutcome::rejected);
    CHECK(attempt("wrong") == CredentialOutcome::rejected);
    // scrypt is expensive, so the unauthenticated endpoint is rate limited.
    CHECK(attempt("right") == CredentialOutcome::rate_limited);
    std::this_thread::sleep_for(260ms);
    CHECK(attempt("right") == CredentialOutcome::ok);
}

MACHA_FAST_TEST("users", test_users_api_requires_admin_and_hides_hashes) {
    TestCluster cluster;
    BareNode node(cluster.node_config("n1"), cluster.keys());
    UsersApi api(node, node.accounts());

    const auto create_body =
        R"({"username":"frank","password":"long-enough-pw","roles":["manager"]})";
    auto created =
        api.handle(users_request("POST", "/api/v1/users", admin_identity(), create_body));
    REQUIRE(created.status == 201);
    auto body = Json::parse(json_body(created));
    const auto id = body.find("id")->asString();
    // Credentials have no read path at all.
    CHECK(json_body(created).find("long-enough-pw") == std::string::npos);
    CHECK(json_body(created).find("salt") == std::string::npos);
    CHECK(json_body(created).find("password_hash") == std::string::npos);
    // manager plus the media_viewer and view_status every role implies.
    CHECK(body.find("roles")->asArray().size() == 3);
    // The record carries its LWW counter for optimistic concurrency.
    CHECK(body.find("version")->asUInt64() == 1);
    // An ordinary account may be deleted and re-roled; the client is told so.
    CHECK(body.find("mutable")->find("delete")->asBool());
    CHECK(body.find("mutable")->find("set_roles")->asBool());

    auto listed = api.handle(users_request("GET", "/api/v1/users", admin_identity()));
    REQUIRE(listed.status == 200);
    CHECK(json_body(listed).find("frank") != std::string::npos);
    CHECK(json_body(listed).find("password_hash") == std::string::npos);
    // Every collection in this API answers under "items".
    auto listed_body = Json::parse(json_body(listed));
    REQUIRE(listed_body.find("items") != nullptr);
    CHECK(listed_body.find("items")->isArray());

    // An unknown role is refused rather than stored as an unenforceable string.
    auto bogus = api.handle(
        users_request("POST", "/api/v1/users", admin_identity(),
                      R"({"username":"x","password":"long-enough-pw","roles":["wizard"]})"));
    CHECK(bogus.status == 400);

    // Password policy is enforced and names the offending field.
    auto weak = api.handle(users_request("POST", "/api/v1/users", admin_identity(),
                                         R"({"username":"y","password":"short"})"));
    CHECK(weak.status == 400);
    CHECK(json_body(weak).find("password_rejected") != std::string::npos);

    // A duplicate username says so specifically.
    auto duplicate =
        api.handle(users_request("POST", "/api/v1/users", admin_identity(), create_body));
    CHECK(duplicate.status == 409);
    CHECK(json_body(duplicate).find("username_taken") != std::string::npos);

    // A self PATCH cannot escalate privileges.
    SessionIdentity viewer{"s", Hash256{}, {std::string(role_media_viewer)}, id};
    auto escalate = api.handle(
        users_request("PATCH", "/api/v1/users/me", viewer, R"({"roles":["manage_users"]})"));
    CHECK(escalate.status == 403);

    // Changing your own password hands back a fresh session.
    auto changed = api.handle(
        users_request("PATCH", "/api/v1/users/me", viewer, R"({"password":"another-long-pw"})"));
    REQUIRE(changed.status == 200);
    auto changed_body = Json::parse(json_body(changed));
    REQUIRE(changed_body.find("token") != nullptr);
    CHECK(node.sessions().validate(changed_body.find("token")->asString()).has_value());
    CHECK(node.users().verify("frank", "another-long-pw").ok);

    // The last account that can manage users can be neither removed nor
    // demoted; the refusal has its own code.
    auto manager =
        api.handle(users_request("POST", "/api/v1/users", admin_identity(),
                                 R"({"username":"gail","password":"long-enough-pw",)"
                                 R"("roles":["manage_users"]})"));
    REQUIRE(manager.status == 201);
    const auto manager_id = Json::parse(json_body(manager)).find("id")->asString();
    CHECK(!Json::parse(json_body(manager)).find("mutable")->find("delete")->asBool());

    auto removed =
        api.handle(users_request("DELETE", "/api/v1/users/" + manager_id, admin_identity()));
    CHECK(removed.status == 409);
    CHECK(json_body(removed).find("last_user_manager") != std::string::npos);

    auto demoted = api.handle(users_request("PATCH", "/api/v1/users/" + manager_id,
                                            admin_identity(), R"({"roles":["media_viewer"]})"));
    CHECK(demoted.status == 409);
    CHECK(json_body(demoted).find("last_user_manager") != std::string::npos);
}

MACHA_FAST_TEST("users", test_anonymous_session_has_no_account) {
    TestCluster cluster;
    BareNode node(cluster.node_config("n1"), cluster.keys());
    UsersApi api(node, node.accounts());
    SessionIdentity anonymous{"s", Hash256{}, {std::string(role_media_viewer)}, {}};
    auto response = api.handle(users_request("GET", "/api/v1/users/me", anonymous));
    CHECK(response.status == 404);
    CHECK(json_body(response).find("no_account") != std::string::npos);
}

MACHA_TEST("users", test_users_replicate_and_login_works_on_the_other_node) {
    TestCluster cluster;
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = cluster.node_config("n1", p1, {{"127.0.0.1", p2}});
    auto c2 = cluster.node_config("n2", p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c1.heartbeat = c2.heartbeat = 20ms;

    BareNode n1(c1, cluster.keys());
    BareNode n2(c2, cluster.keys());
    n1.start();
    n2.start();
    REQUIRE(wait_until([&] {
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2;
    }));

    auto created = n1.users().create("grace", "pw", {std::string(role_manage_users)}, n1.node_id());
    REQUIRE(created.has_value());
    n1.propagate_users();

    REQUIRE(wait_until([&] { return n2.users().find(created->id).has_value(); }, 5s));
    // n2 authenticates the user itself, without reaching n1.
    auto check = n2.users().verify("grace", "pw");
    CHECK(check.ok);
    CHECK(check.user_id == created->id);
    CHECK(n1.users().table_hash() == n2.users().table_hash());

    // A password change invalidates sessions minted against the old one, on
    // every node, by replicating one record.
    SessionApi api2(n2, n2.accounts());
    auto minted = n2.sessions().create(check.roles, check.user_id, check.credential_generation);
    REQUIRE(minted.has_value());
    REQUIRE(n2.users().find(check.user_id)->credential_generation ==
            minted->session.credential_generation);

    auto changed = n1.users().update(created->id, "pw2", std::nullopt, n1.node_id());
    REQUIRE(changed.has_value());
    n1.propagate_users();
    REQUIRE(wait_until(
        [&] {
            auto seen = n2.users().find(created->id);
            return seen && seen->credential_generation == changed->credential_generation;
        },
        5s));
    // The session record is untouched; the generation mismatch retires it.
    CHECK(n2.sessions().validate(minted->bearer_token).has_value());
    CHECK(n2.users().find(check.user_id)->credential_generation !=
          minted->session.credential_generation);

    n2.stop();
    n1.stop();
}

MACHA_TEST("users", test_a_node_that_was_down_learns_a_deletion_not_a_resurrection) {
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

    // Created and deleted while n2 is down: only the tombstone can reach it.
    auto created = n1.users().create("heidi", "pw", {std::string(role_media_viewer)}, n1.node_id());
    REQUIRE(created.has_value());
    REQUIRE(n1.users().remove(created->id, n1.node_id()).has_value());

    BareNode n2(c2, cluster.keys());
    n2.start();
    REQUIRE(wait_until([&] {
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2;
    }));

    // No explicit propagate_users(): the periodic whole-table backstop delivers it.
    REQUIRE(wait_until([&] { return n2.users().tombstones() == 1; }, 10s));
    CHECK(!n2.users().find(created->id).has_value());
    CHECK(!n2.users().verify("heidi", "pw").ok);
    CHECK(n1.users().table_hash() == n2.users().table_hash());

    n2.stop();
    n1.stop();
}

MACHA_TEST("users", test_login_does_not_wait_on_an_unreachable_peer) {
    // Nothing in the login path waits on a peer, even one not yet declared dead.
    TestCluster cluster;
    const auto p1 = free_port();
    const auto dead = free_port(); // nothing ever listens here
    auto c1 = cluster.node_config("n1", p1, {{"127.0.0.1", dead}});
    c1.replication = 1;
    c1.metadata_min_write_replicas = 1;

    BareNode n1(c1, cluster.keys());
    n1.start();
    REQUIRE(n1.users()
                .create("ivan", "pw", {std::string(role_manage_users)}, n1.node_id())
                .has_value());

    SessionApi api(n1, n1.accounts());
    HttpRequest request;
    request.method = "POST";
    request.path = "/api/v1/session";
    Json::Object credentials{{"username", Json(std::string("ivan"))},
                             {"password", Json(std::string("pw"))}};
    Json::Object root{{"credentials", Json(std::move(credentials))}};
    const auto text = Json(std::move(root)).dump();
    request.body.assign(text.begin(), text.end());

    const auto started = Clock::now();
    auto response = api.handle(request);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    REQUIRE(response.status == 201);
    CHECK(elapsed < 2s);

    auto body = Json::parse(json_body(response));
    CHECK(n1.sessions().validate(body.find("token")->asString()).has_value());

    n1.stop();
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

MACHA_FAST_TEST("users", test_the_last_user_manager_cannot_be_demoted_or_removed) {
    // Some account always holds manage_users: the protection follows the role, not root.
    TestCluster cluster;
    BareNode node(cluster.node_config("n1"), cluster.keys());
    TempDir dir;
    auto genesis = create_initial_accounts(node.users(), cluster.keys(), dir.path(), node.node_id());
    REQUIRE(genesis.has_value());
    UsersApi api(node, node.accounts());

    const auto root_id = genesis->root.id;
    CHECK(node.users().sole_user_manager(root_id));

    // Demoting the only holder is refused, with a code a client can show.
    auto demoted = api.handle(users_request("PATCH", "/api/v1/users/" + root_id, admin_identity(),
                                            R"({"roles":["media_viewer"]})"));
    CHECK(demoted.status == 409);
    CHECK(json_body(demoted).find("last_user_manager") != std::string::npos);
    CHECK(user_has_role(*node.users().find(root_id), role_manage_users));

    // Its roles can still be added to, and the client is told which role is pinned.
    auto widened = api.handle(
        users_request("PATCH", "/api/v1/users/" + root_id, admin_identity(),
                      R"({"roles":["manage_users","media_viewer"]})"));
    CHECK(widened.status == 200);
    const auto widened_body = Json::parse(json_body(widened));
    const auto* may = widened_body.find("mutable");
    REQUIRE(may != nullptr);
    CHECK(may->find("set_roles")->asBool());
    REQUIRE(may->find("required_roles")->asArray().size() == 1);
    CHECK(may->find("required_roles")->asArray().front().asString() == role_manage_users);
    CHECK(may->find("required_roles_reason")->asString() == "last_user_manager");

    // Once a second holder exists, the first is free -- including root.
    auto second = api.handle(
        users_request("POST", "/api/v1/users", admin_identity(),
                      R"({"username":"deputy","password":"long-enough-pw",)"
                      R"("roles":["manage_users"]})"));
    REQUIRE(second.status == 201);
    CHECK(!node.users().sole_user_manager(root_id));
    auto now_ok = api.handle(users_request("PATCH", "/api/v1/users/" + root_id, admin_identity(),
                                           R"({"roles":["media_viewer"]})"));
    CHECK(now_ok.status == 200);
    CHECK(!user_has_role(*node.users().find(root_id), role_manage_users));

    // And the deputy, now the only holder, inherits the protection.
    const auto deputy_id = Json::parse(json_body(second)).find("id")->asString();
    CHECK(node.users().sole_user_manager(deputy_id));
    auto deputy_demoted =
        api.handle(users_request("PATCH", "/api/v1/users/" + deputy_id, admin_identity(),
                                 R"({"roles":["media_viewer"]})"));
    CHECK(deputy_demoted.status == 409);
    auto deputy_removed =
        api.handle(users_request("DELETE", "/api/v1/users/" + deputy_id, admin_identity()));
    CHECK(deputy_removed.status == 409);
    CHECK(json_body(deputy_removed).find("last_user_manager") != std::string::npos);
}

MACHA_FAST_TEST("users", test_an_upgraded_cluster_announces_that_it_has_no_accounts) {
    // A cluster with bootstrap peers creates no accounts, so with no anonymous
    // account every route refuses; Status must say why.
    TestCluster cluster;
    auto config = cluster.node_config("upgraded");
    config.bootstrap.push_back(Endpoint{"127.0.0.1", free_port()});
    BareNode node(config, cluster.keys());
    CHECK(node.users().all().empty());

    PasswordCredentialValidator validator(node.users(), config.session);
    CHECK(validator.validate(Json(Json::Object{})).outcome == CredentialOutcome::disabled);

    // macha-users init produces exactly what a founding node would have.
    TempDir dir;
    auto created = create_initial_accounts(node.users(), cluster.keys(), dir.path(),
                                           node.node_id());
    REQUIRE(created.has_value());
    CHECK(created->root.username == root_username);
    CHECK(created->anonymous.username == anonymous_username);
    CHECK(created->root.roles.size() == 5);

    // Anonymous access then works without a restart.
    CHECK(validator.validate(Json(Json::Object{})).outcome == CredentialOutcome::ok);

    // Running it twice is refused rather than minting a second root.
    CHECK(!create_initial_accounts(node.users(), cluster.keys(), dir.path(),
                                   node.node_id()).has_value());
}

// Implications are resolved when a session is minted, not only when a record
// is written, so stored roles lacking an implied role still get it.
MACHA_FAST_TEST("users", test_role_implications_reach_accounts_written_before_them) {
    TestCluster cluster;
    BareNode node(cluster.node_config("n1"), cluster.keys());
    auto created = node.users().create("olduser", "a-long-enough-pw",
                                       {std::string(role_manager)}, node.node_id());
    REQUIRE(created.has_value());

    // A stored record with the granted role and media_viewer but no view_status.
    auto legacy = *created;
    legacy.roles = {std::string(role_manager), std::string(role_media_viewer)};
    legacy.version = created->version + 1;
    REQUIRE(node.users().apply(legacy));
    auto stored = node.users().find(created->id);
    REQUIRE(stored.has_value());
    CHECK(!user_has_role(*stored, role_view_status));

    auto check = node.users().verify("olduser", "a-long-enough-pw");
    REQUIRE(check.ok);
    CHECK(std::count(check.roles.begin(), check.roles.end(), role_view_status) == 1);
    CHECK(std::count(check.roles.begin(), check.roles.end(), role_manager) == 1);
    // Still no widening beyond the documented implications.
    CHECK(std::count(check.roles.begin(), check.roles.end(), role_manage_users) == 0);
}

// /api/v1/status requires view_status; /api/v1/health requires no session.
MACHA_TEST("users", test_status_needs_view_status_and_health_needs_nothing) {
    TestService fixture("status-role");
    fixture.config().catalogue.api.enabled = true;
    fixture.config().catalogue.api.port = free_port();
    auto& service = fixture.start();
    const auto port = fixture.config().catalogue.api.port;

    const auto token_for = [&](std::vector<std::string> roles) {
        auto minted = service.accounts().sessions().create(expand_roles(roles));
        REQUIRE(minted.has_value());
        return std::map<std::string, std::string>{
            {"Authorization", "Bearer " + minted->bearer_token}};
    };

    // A session with no roles may do nothing.
    auto refused = raw_http_get(port, "/api/v1/status", token_for({}));
    CHECK(refused.find("403") != std::string::npos);
    CHECK(refused.find("view_status") != std::string::npos);

    // view_status alone is enough for status, and grants no media.
    auto allowed = raw_http_get(port, "/api/v1/status",
                                token_for({std::string(role_view_status)}));
    CHECK(allowed.find("200") != std::string::npos);
    auto media_refused = raw_http_get(port, "/api/v1/catalogue/items",
                                      token_for({std::string(role_view_status)}));
    CHECK(media_refused.find("403") != std::string::npos);

    // Every other capability implies it.
    auto importer = raw_http_get(port, "/api/v1/status",
                                 token_for({std::string(role_importer)}));
    CHECK(importer.find("200") != std::string::npos);

    // Health needs no token: status, service and version, but nothing of the
    // cluster's shape.
    auto health = raw_http_get(port, "/api/v1/health");
    CHECK(health.find("200") != std::string::npos);
    CHECK(health.find("\"status\":\"ok\"") != std::string::npos);
    CHECK(health.find("\"service\":\"macha\"") != std::string::npos);
    CHECK(health.find("\"version\":\"") != std::string::npos);
    CHECK(health.find("node") == std::string::npos);
    CHECK(health.find("capacity") == std::string::npos);
}

MACHA_TEST("users", test_a_peer_that_joins_after_the_announcement_converges) {
    // A node that does not exist when the users table is announced still
    // converges once it joins. The 30 s periodic re-announce (a constant, not
    // config) is not exercised here.
    TestCluster cluster;
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = cluster.node_config("n1", p1, {{"127.0.0.1", p2}});
    auto c2 = cluster.node_config("n2", p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c1.heartbeat = c2.heartbeat = 20ms;
    c1.telemetry_interval = c2.telemetry_interval = 250ms;

    BareNode n1(c1, cluster.keys());
    n1.start();
    auto created = n1.users().create("late", "long-enough-pw", {std::string(role_media_viewer)},
                                     n1.node_id());
    REQUIRE(created.has_value());
    // Announce while n2 does not exist: n1 has no peer to tell.
    n1.propagate_users();

    BareNode n2(c2, cluster.keys());
    n2.start();
    REQUIRE(wait_until([&] {
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2;
    }));

    // No further mutation and no explicit propagate.
    REQUIRE(wait_until([&] { return n2.users().find(created->id).has_value(); }, 60s));
    CHECK(n2.users().verify("late", "long-enough-pw").ok);
    CHECK(n1.users().table_hash() == n2.users().table_hash());

    n2.stop();
    n1.stop();
}
