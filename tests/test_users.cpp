// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_api.hpp"
#include "test_backend_support.hpp"
#include "users_api.hpp"

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
    // Two implications, both downward: every capability can read media and see
    // cluster health. Nothing else implies anything. Importing torrents must
    // not carry the right to delete the catalogue, and managing the catalogue
    // must not carry the right to hand out accounts.
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

    // view_status is the weakest capability: implied by everything, implying
    // nothing. Granting it alone must not hand out media.
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

MACHA_FAST_TEST("users", test_user_merge_is_deterministic_and_commutative) {
    // Two replicas that saw the same writes in a different order must land on
    // the same record, or a partition heals into a flap rather than a value.
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
    // At equal version a tombstone wins: a deletion must not be undone by a
    // concurrent edit on the other side of a partition.
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
    // Unlike the session cache, dropping a record here either locks someone
    // out or resurrects a deleted account. At capacity the new record is
    // refused and every existing one is kept.
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
    // Deleting bumps the credential generation too: a tombstone alone would
    // stop new logins while leaving live sessions working.
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

    // The record carries the parameters it was written with, so raising the
    // defaults later cannot invalidate an existing password.
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

    // A roles-only change invalidates sessions too: a session carries the
    // roles it was minted with, so a demotion that left them alive would not
    // take effect until they expired -- up to the whole TTL.
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
        // Write-through: no explicit persist() call, and no idle window to
        // wait for -- a credential must be durable when the call returns.
    }
    REQUIRE(std::filesystem::exists(path));
    {
        std::ifstream input(path, std::ios::binary);
        const std::string raw((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
        // Password hashes replicate to an offsite node; the file must not be
        // readable plaintext on a stolen disk.
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
    // The wrong key must not silently produce an empty-but-usable table that
    // then replicates over the real one.
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
    // A pre-0.38 peer must keep merging anonymous sessions across a rolling
    // upgrade, so a payload gains the new magic only when it actually carries
    // something the old decoder has no field for.
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
    NodeRuntime node(config, cluster.keys());
    auto anonymous = node.users().create_without_password(
        anonymous_username, {std::string(role_media_viewer)}, node.node_id());
    REQUIRE(anonymous.has_value());

    PasswordCredentialValidator validator(node.users(), config.session);
    auto minted = validator.validate(Json(Json::Object{}));
    REQUIRE(minted.outcome == CredentialOutcome::ok);
    // An anonymous session is an ordinary bound session, so it is retired by
    // the same credential_generation check as anyone else's.
    CHECK(minted.credentials.user_id == anonymous->id);
    CHECK(minted.credentials.credential_generation == anonymous->credential_generation);
    CHECK(std::count(minted.credentials.roles.begin(), minted.credentials.roles.end(),
                     role_media_viewer) == 1);

    // Changing what an unauthenticated visitor may do is an ordinary PATCH of
    // an ordinary account, and takes effect on the next mint rather than on
    // restart. This is the only control over what a television can reach.
    auto widened = node.users().update(
        anonymous->id, "", std::vector<std::string>{std::string(role_importer)}, node.node_id());
    REQUIRE(widened.has_value());
    auto after = validator.validate(Json(Json::Object{}));
    REQUIRE(after.outcome == CredentialOutcome::ok);
    CHECK(std::count(after.credentials.roles.begin(), after.credentials.roles.end(),
                     role_importer) == 1);
    CHECK(after.credentials.credential_generation != minted.credentials.credential_generation);

    // allow_anonymous: false is the whole point of the switch -- browsing
    // without an account stops, and the refusal says why rather than 401-ing
    // as if a token were merely missing.
    auto closed = config.session;
    closed.allow_anonymous = false;
    PasswordCredentialValidator strict(node.users(), closed);
    CHECK(strict.validate(Json(Json::Object{})).outcome == CredentialOutcome::disabled);
}

// Anonymous access switched off and anonymous granted nothing are different
// states, and a cluster may legitimately be in either. Reporting the second as
// the first told a client to show a login form when the truthful answer was
// that it already had a session and this cluster gives visitors no
// capabilities.
MACHA_FAST_TEST("users", test_anonymous_with_no_roles_still_mints_a_powerless_session) {
    TestCluster cluster;
    auto config = cluster.node_config("n1");
    config.session.allow_anonymous = true;
    NodeRuntime node(config, cluster.keys());
    auto anonymous =
        node.users().create_without_password(anonymous_username, {}, node.node_id());
    REQUIRE(anonymous.has_value());
    CHECK(anonymous->roles.empty());

    PasswordCredentialValidator validator(node.users(), config.session);
    auto minted = validator.validate(Json(Json::Object{}));
    REQUIRE(minted.outcome == CredentialOutcome::ok);
    CHECK(minted.credentials.roles.empty());
    CHECK(minted.credentials.user_id == anonymous->id);

    // Switching anonymous access off is still a different answer, and it is
    // the only thing that produces one.
    auto closed = config.session;
    closed.allow_anonymous = false;
    PasswordCredentialValidator strict(node.users(), closed);
    CHECK(strict.validate(Json(Json::Object{})).outcome == CredentialOutcome::disabled);
}

// `session.allow_anonymous: false` guards the no-credentials path only. An
// anonymous account that could be logged into would therefore be a second door
// beside the switch -- and the session it handed back would be an ordinary
// bound one that outlives the switch being turned off.
MACHA_FAST_TEST("users", test_anonymous_has_no_password_and_cannot_be_given_one) {
    TestCluster cluster;
    auto config = cluster.node_config("n1");
    config.session.allow_anonymous = false;
    NodeRuntime node(config, cluster.keys());
    UsersApi api(node);
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

    // The store refuses to install one, so no caller -- API, CLI or a future
    // one -- can route around the rule.
    CHECK(!node.users()
               .update(anonymous->id, "a-long-enough-password", std::nullopt, node.node_id())
               .has_value());
    // Roles remain ordinary, which is the whole control over what a visitor
    // may do.
    REQUIRE(node.users()
                .update(anonymous->id, "",
                        std::vector<std::string>{std::string(role_media_viewer)}, node.node_id())
                .has_value());

    // Before 0.38.4 this was reachable by any holder of an anonymous session:
    // /api/v1/users/me needs only media_viewer, and a self PATCH carrying a
    // password set the anonymous account's credential and handed back a token.
    SessionIdentity visitor{"s", Hash256{}, {std::string(role_media_viewer)}, anonymous->id};
    auto refused = api.handle(
        users_request("PATCH", "/api/v1/users/me", visitor, R"({"password":"a-long-enough-pw"})"));
    CHECK(refused.status == 409);
    CHECK(json_body(refused).find("no_password") != std::string::npos);
    auto refused_by_id =
        api.handle(users_request("PATCH", "/api/v1/users/" + anonymous->id, admin_identity(),
                                 R"({"password":"a-long-enough-pw"})"));
    CHECK(refused_by_id.status == 409);

    // And the client is told, so it does not draw a field the server refuses.
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
    // root holds every capability; anonymous can only read.
    // No recovery key is issued, and root carries no envelope.
    CHECK(genesis->recovery_key.empty());
    CHECK(!genesis->root.recovery.present());
    CHECK(genesis->root.roles.size() == 5);
    // media_viewer as granted, plus the view_status it implies.
    CHECK(genesis->anonymous.roles.size() == 2);
    CHECK(user_has_role(genesis->anonymous, role_media_viewer));
    CHECK(user_has_role(genesis->anonymous, role_view_status));
    CHECK(store.verify(root_username, genesis->password).ok);
    // Anonymous is created with no credential at all rather than a random
    // password nobody is told: there is nothing to leak, nothing to guess, and
    // nothing that could become a way past allow_anonymous.
    const std::array<uint8_t, 32> no_hash{};
    const std::array<uint8_t, 16> no_salt{};
    CHECK(genesis->anonymous.kdf == 0);
    CHECK(genesis->anonymous.password_hash.bytes == no_hash);
    CHECK(genesis->anonymous.salt == no_salt);

    // The generated password has to survive being read off a screen and
    // retyped, so no vowels and none of the characters that look alike.
    CHECK(genesis->password.size() >= 20);
    for (const char c : genesis->password)
        CHECK(std::string("aeiou015lIOS").find(c) == std::string::npos);

    // The password file names the way back, and it is macha-users rather
    // than a recovery key -- nothing must point an operator at a key that was
    // never issued.
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

    // Once only: a second call on a table that already holds anything must not
    // mint a second root with a new password and full privileges.
    CHECK(!create_initial_accounts(store, keys, state, node_id(1)).has_value());

    // Root cannot be removed at all: the store refuses to leave the cluster
    // with nobody holding manage_users.
    CHECK(!store.remove(genesis->root.id, node_id(1)).has_value());
    CHECK(store.find(genesis->root.id).has_value());

    // And not even once an account is gone: all() counts tombstones precisely
    // so that a deletion cannot cause the next restart to recreate anything.
    REQUIRE(store.remove(genesis->anonymous.id, node_id(1)).has_value());
    CHECK(!create_initial_accounts(store, keys, state, node_id(1)).has_value());
}

MACHA_FAST_TEST("users", test_root_and_anonymous_cannot_be_removed_or_recreated) {
    TestCluster cluster;
    NodeRuntime node(cluster.node_config("n1"), cluster.keys());
    UsersApi api(node);
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

    // The client is told which fields it may offer, so it never has to test a
    // username against a hardcoded list of its own.
    auto listed = api.handle(users_request("GET", "/api/v1/users/" + genesis->anonymous.id,
                                           admin_identity()));
    REQUIRE(listed.status == 200);
    auto body = Json::parse(json_body(listed));
    const auto* may = body.find("mutable");
    REQUIRE(may != nullptr);
    CHECK(!may->find("rename")->asBool());
    CHECK(!may->find("delete")->asBool());
    // Anonymous's roles are the only control over what an unauthenticated
    // television can reach, so they must stay editable -- while its password,
    // which does not exist, must not be offered.
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
    NodeRuntime node(config, cluster.keys());
    auto created =
        node.users().create("dave", "hunter2", {std::string(role_manager)}, node.node_id());
    REQUIRE(created.has_value());

    SessionApi api(node);
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
    NodeRuntime node(config, cluster.keys());
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
    // scrypt is deliberately expensive; an unauthenticated endpoint that runs
    // it has to stop answering before it becomes the DoS.
    CHECK(attempt("right") == CredentialOutcome::rate_limited);
    std::this_thread::sleep_for(260ms);
    CHECK(attempt("right") == CredentialOutcome::ok);
}

MACHA_FAST_TEST("users", test_users_api_requires_admin_and_hides_hashes) {
    TestCluster cluster;
    NodeRuntime node(cluster.node_config("n1"), cluster.keys());
    UsersApi api(node);

    const auto create_body =
        R"({"username":"frank","password":"long-enough-pw","roles":["manager"]})";
    auto created =
        api.handle(users_request("POST", "/api/v1/users", admin_identity(), create_body));
    REQUIRE(created.status == 201);
    auto body = Json::parse(json_body(created));
    const auto id = body.find("id")->asString();
    // A credential must have no read path at all, or an account that can manage
    // users becomes an offline-cracking dump.
    CHECK(json_body(created).find("long-enough-pw") == std::string::npos);
    CHECK(json_body(created).find("salt") == std::string::npos);
    CHECK(json_body(created).find("password_hash") == std::string::npos);
    // manager plus the media_viewer and view_status every role implies.
    CHECK(body.find("roles")->asArray().size() == 3);
    // The record carries its LWW counter for optimistic concurrency.
    CHECK(body.find("version")->asUInt64() == 1);
    // An ordinary account may be renamed and deleted; the client is told so
    // rather than working it out from the name.
    CHECK(body.find("mutable")->find("delete")->asBool());
    CHECK(body.find("mutable")->find("set_roles")->asBool());

    auto listed = api.handle(users_request("GET", "/api/v1/users", admin_identity()));
    REQUIRE(listed.status == 200);
    CHECK(json_body(listed).find("frank") != std::string::npos);
    CHECK(json_body(listed).find("password_hash") == std::string::npos);

    // An unknown role is refused rather than stored as an unenforceable string.
    auto bogus = api.handle(
        users_request("POST", "/api/v1/users", admin_identity(),
                      R"({"username":"x","password":"long-enough-pw","roles":["wizard"]})"));
    CHECK(bogus.status == 400);

    // Password policy is enforced, and says which field was wrong so the
    // message can land under the right input.
    auto weak = api.handle(users_request("POST", "/api/v1/users", admin_identity(),
                                         R"({"username":"y","password":"short"})"));
    CHECK(weak.status == 400);
    CHECK(json_body(weak).find("password_rejected") != std::string::npos);

    // A duplicate username says so specifically.
    auto duplicate =
        api.handle(users_request("POST", "/api/v1/users", admin_identity(), create_body));
    CHECK(duplicate.status == 409);
    CHECK(json_body(duplicate).find("username_taken") != std::string::npos);

    // Changing your own password must not be a privilege-escalation route.
    SessionIdentity viewer{"s", Hash256{}, {std::string(role_media_viewer)}, id};
    auto escalate = api.handle(
        users_request("PATCH", "/api/v1/users/me", viewer, R"({"roles":["manage_users"]})"));
    CHECK(escalate.status == 403);

    // Changing your own password hands back a fresh session, so a person is
    // not logged out by their own change.
    auto changed = api.handle(
        users_request("PATCH", "/api/v1/users/me", viewer, R"({"password":"another-long-pw"})"));
    REQUIRE(changed.status == 200);
    auto changed_body = Json::parse(json_body(changed));
    REQUIRE(changed_body.find("token") != nullptr);
    CHECK(node.sessions().validate(changed_body.find("token")->asString()).has_value());
    CHECK(node.users().verify("frank", "another-long-pw").ok);

    // The last account that can manage users can be neither removed nor
    // demoted: either would replicate perfectly and leave nobody able to undo
    // it. The refusal has its own code so a client can say why.
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
    NodeRuntime node(cluster.node_config("n1"), cluster.keys());
    UsersApi api(node);
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

    NodeRuntime n1(c1, cluster.keys());
    NodeRuntime n2(c2, cluster.keys());
    n1.start();
    n2.start();
    REQUIRE(wait_until([&] {
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2;
    }));

    auto created = n1.users().create("grace", "pw", {std::string(role_manage_users)}, n1.node_id());
    REQUIRE(created.has_value());
    n1.propagate_users();

    REQUIRE(wait_until([&] { return n2.users().find(created->id).has_value(); }, 5s));
    // The point of replicating the table rather than only the session: n2 can
    // authenticate this person itself, without reaching the node that first
    // knew about them.
    auto check = n2.users().verify("grace", "pw");
    CHECK(check.ok);
    CHECK(check.user_id == created->id);
    CHECK(n1.users().table_hash() == n2.users().table_hash());

    // A password change invalidates sessions minted against the old one, on
    // every node, by replicating one record.
    SessionApi api2(n2);
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
    // The session record itself is untouched; it is the generation mismatch
    // against the replicated user that retires it.
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
    // The gossip backstop rides the telemetry tick; make it quick enough to
    // observe convergence without a push.
    c1.telemetry_interval = c2.telemetry_interval = 250ms;

    NodeRuntime n1(c1, cluster.keys());
    n1.start();

    // Created and deleted entirely while n2 is down, so n2 never sees the live
    // record -- only the tombstone can reach it.
    auto created = n1.users().create("heidi", "pw", {std::string(role_media_viewer)}, n1.node_id());
    REQUIRE(created.has_value());
    REQUIRE(n1.users().remove(created->id, n1.node_id()).has_value());

    NodeRuntime n2(c2, cluster.keys());
    n2.start();
    REQUIRE(wait_until([&] {
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2;
    }));

    // No explicit propagate_users() here: this is the periodic backstop, which
    // sends the whole table precisely so a node that missed a window converges
    // rather than staying stale forever.
    REQUIRE(wait_until([&] { return n2.users().tombstones() == 1; }, 10s));
    CHECK(!n2.users().find(created->id).has_value());
    CHECK(!n2.users().verify("heidi", "pw").ok);
    CHECK(n1.users().table_hash() == n2.users().table_hash());

    n2.stop();
    n1.stop();
}

MACHA_TEST("users", test_login_does_not_wait_on_an_unreachable_peer) {
    // The regression this whole change exists for: propagate_session used to
    // call() every peer membership still called active, each to
    // control_no_progress_deadline (30 s). One unreachable-but-not-yet-dead
    // peer therefore stalled every login by that long -- which is exactly the
    // situation (degraded cluster, peers unreachable) in which you need to log
    // in. Nothing in the request path may wait on a peer.
    TestCluster cluster;
    const auto p1 = free_port();
    const auto dead = free_port(); // nothing ever listens here
    auto c1 = cluster.node_config("n1", p1, {{"127.0.0.1", dead}});
    c1.replication = 1;
    c1.metadata_min_write_replicas = 1;

    NodeRuntime n1(c1, cluster.keys());
    n1.start();
    REQUIRE(n1.users()
                .create("ivan", "pw", {std::string(role_manage_users)}, n1.node_id())
                .has_value());

    SessionApi api(n1);
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
    // Nothing issues a recovery key: genesis does not, and no route accepts
    // one (see test_no_recovery_route_is_exposed). The machinery is kept for a
    // deployment model that does not exist yet -- one where the operator
    // cannot get a shell on a node -- so it is tested rather than left to rot.
    //
    // The property under test is the one that made the design worth keeping:
    // holding the cluster key must NOT yield the recovery key.
    TestCluster cluster;
    const auto& keys = cluster.keys();

    auto issued = issue_recovery_key(keys);
    CHECK(issued.key.size() == 64);
    CHECK(issued.envelope.present());
    CHECK(recovery_key_matches(issued.envelope, keys, issued.key));

    // Nothing in the stored envelope is the key, or a hash of it. The only way
    // to produce a matching key from the envelope is to break X25519.
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

    // Each issue is independent: re-issuing invalidates the previous key,
    // which is what makes a lost one replaceable.
    CHECK(recovery_key_matches(other.envelope, keys, other.key));
    CHECK(!recovery_key_matches(other.envelope, keys, issued.key));

    // An envelope sealed against a different cluster key must not verify, even
    // with the key that sealed it -- it no longer unlocks this cluster. This
    // is what the plaintext comparison catches that the AEAD tag does not.
    ClusterKeys foreign = keys;
    foreign.master.front() = static_cast<uint8_t>(foreign.master.front() ^ 0xFF);
    CHECK(!recovery_key_matches(issued.envelope, foreign, issued.key));
}

MACHA_FAST_TEST("users", test_the_last_user_manager_cannot_be_demoted_or_removed) {
    // The invariant: it must never be possible to reach a cluster where no
    // account can administer accounts. Not a property of root -- root's roles
    // are ordinary -- but of the role, so it moves as the role moves.
    TestCluster cluster;
    NodeRuntime node(cluster.node_config("n1"), cluster.keys());
    TempDir dir;
    auto genesis = create_initial_accounts(node.users(), cluster.keys(), dir.path(), node.node_id());
    REQUIRE(genesis.has_value());
    UsersApi api(node);

    const auto root_id = genesis->root.id;
    CHECK(node.users().sole_user_manager(root_id));

    // Demoting the only holder is refused, with a code a client can show.
    auto demoted = api.handle(users_request("PATCH", "/api/v1/users/" + root_id, admin_identity(),
                                            R"({"roles":["media_viewer"]})"));
    CHECK(demoted.status == 409);
    CHECK(json_body(demoted).find("last_user_manager") != std::string::npos);
    CHECK(user_has_role(*node.users().find(root_id), role_manage_users));

    // But its roles are not frozen: adding to them is fine, and the client is
    // told precisely which role is pinned rather than being handed a blanket
    // "roles are read-only".
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
    // The upgrade lockout: an existing cluster has bootstrap peers, so no node
    // treats itself as founding one, so nothing creates accounts -- and with
    // no anonymous account the session mint refuses, which means every route
    // refuses. Status must say so, because the symptom (403 everywhere) points
    // nowhere near the cause.
    TestCluster cluster;
    auto config = cluster.node_config("upgraded");
    config.bootstrap.push_back(Endpoint{"127.0.0.1", free_port()});
    NodeRuntime node(config, cluster.keys());
    CHECK(node.users().all().empty());

    PasswordCredentialValidator validator(node.users(), config.session);
    CHECK(validator.validate(Json(Json::Object{})).outcome == CredentialOutcome::disabled);

    // macha-users init is the documented way out, and it produces exactly what
    // a founding node would have produced.
    TempDir dir;
    auto created = create_initial_accounts(node.users(), cluster.keys(), dir.path(),
                                           node.node_id());
    REQUIRE(created.has_value());
    CHECK(created->root.username == root_username);
    CHECK(created->anonymous.username == anonymous_username);
    CHECK(created->root.roles.size() == 5);

    // And now anonymous access works again, without a restart.
    CHECK(validator.validate(Json(Json::Object{})).outcome == CredentialOutcome::ok);

    // Running it twice is refused rather than minting a second root.
    CHECK(!create_initial_accounts(node.users(), cluster.keys(), dir.path(),
                                   node.node_id()).has_value());
}

// Implications are resolved when a session is minted, not only when a record is
// written. An account created before view_status existed holds roles that never
// mention it, and must still see cluster health -- otherwise upgrading takes the
// diagnostic screen away from every existing account until someone edits them
// all, which is the worst possible moment to lose it.
MACHA_FAST_TEST("users", test_role_implications_reach_accounts_written_before_them) {
    TestCluster cluster;
    NodeRuntime node(cluster.node_config("n1"), cluster.keys());
    auto created = node.users().create("olduser", "a-long-enough-pw",
                                       {std::string(role_manager)}, node.node_id());
    REQUIRE(created.has_value());

    // Rewrite the record the way a pre-0.38.5 node would have stored it: the
    // granted role plus the media_viewer of the day, and no view_status.
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

// Cluster health is a capability like any other. A session the cluster granted
// nothing must not be shown the node roster, capacities and diagnostics -- and
// an operator who wants that public says so by granting view_status, rather
// than by the route having no gate at all.
MACHA_TEST("users", test_status_needs_view_status_and_health_needs_nothing) {
    TestService fixture("status-role");
    fixture.config().catalogue.api.enabled = true;
    fixture.config().catalogue.api.port = free_port();
    auto& service = fixture.start();
    const auto port = fixture.config().catalogue.api.port;

    const auto token_for = [&](std::vector<std::string> roles) {
        auto minted = service.node().sessions().create(expand_roles(roles));
        REQUIRE(minted.has_value());
        return std::map<std::string, std::string>{
            {"Authorization", "Bearer " + minted->bearer_token}};
    };

    // What a roles-less anonymous session is in a registered-users-only
    // deployment: a real session that may do nothing.
    auto refused = raw_http_get(port, "/api/v1/status", token_for({}));
    CHECK(refused.find("403") != std::string::npos);
    CHECK(refused.find("view_status") != std::string::npos);

    // Granting it alone is enough for health, and grants nothing else.
    auto allowed = raw_http_get(port, "/api/v1/status",
                                token_for({std::string(role_view_status)}));
    CHECK(allowed.find("200") != std::string::npos);
    auto media_refused = raw_http_get(port, "/api/v1/catalogue/items",
                                      token_for({std::string(role_view_status)}));
    CHECK(media_refused.find("403") != std::string::npos);

    // And every other capability implies it, so nobody who could see status
    // before loses it.
    auto importer = raw_http_get(port, "/api/v1/status",
                                 token_for({std::string(role_importer)}));
    CHECK(importer.find("200") != std::string::npos);

    // Liveness is a separate route with no token at all, because the things
    // that ask it -- a load balancer, an uptime monitor, a client choosing an
    // endpoint -- have no session and should not need one. It says whether this
    // node is serving and nothing else: no version, no node id, no topology.
    auto health = raw_http_get(port, "/api/v1/health");
    CHECK(health.find("200") != std::string::npos);
    CHECK(health.find("\"status\":\"ok\"") != std::string::npos);
    CHECK(health.find("node") == std::string::npos);
    CHECK(health.find("capacity") == std::string::npos);
    CHECK(health.find("version") == std::string::npos);
}

MACHA_TEST("users", test_a_peer_that_joins_after_the_announcement_converges) {
    // Observed live on 2026-09-12 during the 0.38.0 rollout. Gossip announces
    // on change, and marks a peer told when broadcast_best_effort() reports it
    // QUEUED a frame -- which is not the same as the peer having received and
    // applied it. A node that is still starting has no usable inbound route,
    // so it is marked told while receiving nothing, and since neither the
    // table nor the membership set changes afterwards it waits for ever. The
    // upgraded node sat refusing every request with anonymous_disabled until
    // the sender happened to restart.
    //
    // What this test actually covers is the late-JOINER half: n2 does not
    // exist when the announcement is made, and converges because its arrival
    // changes n1's peer set. That is a real path and it is worth holding, but
    // it is NOT the path that failed live -- there, gbni-2 was already a known
    // member, so its restart changed nothing n1 could see and only the
    // periodic re-announce could have saved it.
    //
    // The re-announce itself is not covered here: at a 30 s interval a unit
    // test would have to wait that long, and the interval is a constant rather
    // than config. It was verified against the real cluster instead, by
    // restarting a converged node and watching it refill without touching the
    // sender. If this is ever made configurable, assert it here properly.
    TestCluster cluster;
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = cluster.node_config("n1", p1, {{"127.0.0.1", p2}});
    auto c2 = cluster.node_config("n2", p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c1.heartbeat = c2.heartbeat = 20ms;
    c1.telemetry_interval = c2.telemetry_interval = 250ms;

    NodeRuntime n1(c1, cluster.keys());
    n1.start();
    auto created = n1.users().create("late", "long-enough-pw", {std::string(role_media_viewer)},
                                     n1.node_id());
    REQUIRE(created.has_value());
    // Announce now, while n2 does not exist at all: n1 records having told
    // every peer it knows about, which is none.
    n1.propagate_users();

    NodeRuntime n2(c2, cluster.keys());
    n2.start();
    REQUIRE(wait_until([&] {
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2;
    }));

    // Deliberately no further mutation and no explicit propagate: only the
    // periodic re-announce can deliver this.
    REQUIRE(wait_until([&] { return n2.users().find(created->id).has_value(); }, 60s));
    CHECK(n2.users().verify("late", "long-enough-pw").ok);
    CHECK(n1.users().table_hash() == n2.users().table_hash());

    n2.stop();
    n1.stop();
}
