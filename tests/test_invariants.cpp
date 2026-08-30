// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"
#include "manage_api.hpp"
#include "status_api.hpp"
#include "json.hpp"

#if defined(__linux__)
#include <sys/syscall.h>
#include <sys/wait.h>
#endif

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

MACHA_TEST("invariants", test_convergence_demand_coalesces_burst_and_keeps_same_generation_event) {
    ConvergenceDemand demand;
    CHECK(!demand.pending());

    demand.request(10);
    auto first = demand.begin();
    REQUIRE(first.has_value());
    CHECK(first->generation == 10);

    // Model notices arriving while the convergence owner is gated in its
    // current run. They advance the high-water state without scheduling one
    // maintenance run per intermediate generation.
    for (uint64_t generation = 11; generation <= 210; ++generation)
        demand.request(generation);
    demand.request(210); // same-generation sibling/topology change

    auto during = demand.diagnostics();
    CHECK(during.events_received == 202);
    CHECK(during.runs_scheduled == 1);
    CHECK(during.latest_generation == 210);
    CHECK(demand.complete(*first));

    auto second = demand.begin();
    REQUIRE(second.has_value());
    CHECK(second->generation == 210);
    CHECK(!demand.complete(*second));

    auto settled = demand.diagnostics();
    CHECK(settled.runs_scheduled == 2);
    CHECK(settled.runs_completed == 2);
    CHECK(settled.completed_epoch == settled.requested_epoch);
    CHECK(!settled.scheduled);

    // Generation alone is not the identity: a sibling notice at the already
    // processed generation must still create a new edge.
    demand.request(210);
    auto sibling = demand.begin();
    REQUIRE(sibling.has_value());
    CHECK(sibling->epoch > second->epoch);
    CHECK(!demand.complete(*sibling));
    CHECK(demand.diagnostics().runs_scheduled == 3);
}
Config config_for(const std::filesystem::path& path, const std::filesystem::path& key,
                  uint16_t port, std::vector<Endpoint> bootstrap = {}) {
    return macha::test_support::config_for(path, key, port, std::move(bootstrap),
                                           macha::test_support::ConfigProfile::isolated);
}

#if defined(__linux__)
std::atomic_bool track_fsync{false};
std::atomic_uint64_t fsync_calls{0};
std::atomic_uint64_t syncfs_calls{0};
#endif


MACHA_TEST("invariants", test_manage_unmatched_rename_manual_catalogue_and_filesystem_binding) {
    TestService fixture("manage");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.hydration.enabled = false;
    config.catalogue.scanner.enabled = false;
    auto& service = fixture.start();

    auto& fs = service.filesystem();
    fs.mkdir("/Movies", 0755, getuid(), getgid());
    write_file(fs, "/Movies/unknown.mkv", pattern(4096, 91));
    const auto before = fs.getattr("/Movies/unknown.mkv");
    const auto media_id = file_media_id(before);

    auto& hints = service.catalogue_hints();
    const auto hint_id = hints.submit("/Movies/unknown.mkv", "scanner", media_id,
                                      CatalogueHintPriority::periodic_scan);
    auto claimed = hints.claim_next();
    REQUIRE(claimed.has_value());
    REQUIRE(claimed->id == hint_id);
    hints.mark_no_match(hint_id, "movies", media_id, "no metadata provider match");

    CatalogueScanner scanner(service.node(), fs, service.catalogue(), hints,
                             config.catalogue.scanner);
    ManageApi manage(service.node(), service.metadata_manager(), fs, service.catalogue(), hints, scanner);

    HttpRequest list;
    list.method = "GET";
    list.path = "/api/v1/manage/unmatched";
    auto listed = manage.handle(list);
    REQUIRE(listed.status == 200);
    auto listed_json = Json::parse(std::string(reinterpret_cast<const char*>(listed.body.data()), listed.body.size()));
    REQUIRE(listed_json.find("count") != nullptr);
    CHECK(listed_json.find("count")->asUInt64() == 1);

    HttpRequest rename;
    rename.method = "POST";
    rename.path = "/api/v1/manage/filesystem/rename";
    const std::string rename_body = R"({"path":"/Movies/unknown.mkv","destination":"/Movies/renamed.mkv"})";
    rename.body.assign(rename_body.begin(), rename_body.end());
    REQUIRE(manage.handle(rename).status == 200);
    CHECK(file_media_id(fs.getattr("/Movies/renamed.mkv")) == media_id);

    auto moved_hints = hints.list();
    REQUIRE(moved_hints.size() == 1);
    CHECK(moved_hints.front().path == "/Movies/renamed.mkv");
    CHECK(moved_hints.front().media_id == media_id);
    const auto moved_hint_id = moved_hints.front().id;

    HttpRequest manual;
    manual.method = "POST";
    manual.path = "/api/v1/manage/unmatched/" + moved_hint_id + "/manual";
    const std::string manual_body = R"({"kind":"movie","title":"Manually Identified","year":2026})";
    manual.body.assign(manual_body.begin(), manual_body.end());
    auto created = manage.handle(manual);
    REQUIRE(created.status == 201);
    auto created_json = Json::parse(std::string(reinterpret_cast<const char*>(created.body.data()), created.body.size()));
    const auto leaf_id = created_json.find("leaf_item_id")->asString();
    auto item = service.catalogue().get(leaf_id);
    REQUIRE(item.has_value());
    CHECK(item->title == "Manually Identified");
    CHECK(item->media_ids == std::vector<std::string>{media_id});
    CHECK(!hints.get(moved_hint_id).has_value());

    HttpRequest browse;
    browse.method = "GET";
    browse.path = "/api/v1/manage/filesystem";
    browse.query["path"] = "/Movies";
    auto browsed = manage.handle(browse);
    REQUIRE(browsed.status == 200);
    auto browsed_json = Json::parse(std::string(reinterpret_cast<const char*>(browsed.body.data()), browsed.body.size()));
    const auto& entries = browsed_json.find("entries")->asArray();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().find("path")->asString() == "/Movies/renamed.mkv");
    CHECK(entries.front().find("media_id")->asString() == media_id);
    REQUIRE(entries.front().find("catalogue_item_ids")->asArray().size() == 1);
    CHECK(entries.front().find("catalogue_item_ids")->asArray().front().asString() == leaf_id);

    // Generic Files-tab deletion also clears any durable match state for the path.
    const auto delete_hint = hints.submit("/Movies/renamed.mkv", "scanner", media_id,
                                          CatalogueHintPriority::periodic_scan);
    REQUIRE(hints.claim_next().has_value());
    hints.mark_no_match(delete_hint, "movies", media_id, "synthetic stale exception");
    HttpRequest remove;
    remove.method = "DELETE";
    remove.path = "/api/v1/manage/filesystem";
    remove.query["path"] = "/Movies/renamed.mkv";
    REQUIRE(manage.handle(remove).status == 204);
    CHECK(hints.list().empty());
}

MACHA_TEST("invariants", test_manage_node_identity_association_reset) {
    TestService fixture("manage-identity-reset");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.hydration.enabled = false;
    config.catalogue.scanner.enabled = false;
    auto& service = fixture.start();
    REQUIRE(wait_until([&] {
        return service.metadata_manager().cluster_status().availability ==
               MetadataAvailability::writable;
    }));

    auto& fs = service.filesystem();
    auto& hints = service.catalogue_hints();
    CatalogueScanner scanner(service.node(), fs, service.catalogue(), hints,
                             config.catalogue.scanner);
    ManageApi manage(service.node(), service.metadata_manager(), fs, service.catalogue(), hints,
                     scanner);

    HttpRequest root;
    root.method = "GET";
    root.path = "/api/v1/manage";
    auto root_response = manage.handle(root);
    REQUIRE(root_response.status == 200);
    auto root_json = Json::parse(std::string(reinterpret_cast<const char*>(root_response.body.data()),
                                             root_response.body.size()));
    CHECK(root_json.find("api")->asString() == "manage");
    CHECK(root_json.find("actions")->find("identity_association_reset") != nullptr);
    CHECK(root_json.find("actions")->find("node_identity_association_reset") != nullptr);

    NodeInfo stale;
    stale.id = random_node_id();
    stale.host = "10.44.1.50";
    stale.port = 57401;
    stale.failure_domain = "test";
    stale.seen_unix_ms = unix_ms();
    service.node().membership().observe(stale, true);

    PersistedNodeStatus durable;
    durable.observed_unix_ms = unix_ms();
    durable.host = stale.host;
    durable.port = stale.port;
    durable.failure_domain = stale.failure_domain;
    service.metadata_manager().mutate_delta([&](MetadataSnapshot& snapshot, MetadataDelta& delta) {
        snapshot.node_status[stale.id] = durable;
        delta.upsert_node_status[stale.id] = durable;
    });

    HttpRequest reset;
    reset.method = "POST";
    reset.path = "/api/v1/manage/nodes/" + to_string(stale.id) +
                 "/identity-association/reset";
    const std::string reset_body =
        R"({"host":"10.44.1.50","port":57401,"reason":"test endpoint reassignment"})";
    reset.body.assign(reset_body.begin(), reset_body.end());
    auto reset_response = manage.handle(reset);
    REQUIRE(reset_response.status == 200);
    auto reset_json = Json::parse(std::string(reinterpret_cast<const char*>(reset_response.body.data()),
                                              reset_response.body.size()));
    const auto& reset_value = *reset_json.find("reset");
    CHECK(reset_value.find("stale_node_id")->asString() == to_string(stale.id));
    CHECK(reset_value.find("epoch")->asUInt64() == 1);
    CHECK(reset_value.find("scope")->asString() == "[10.44.1.50]:57401");

    const auto membership_after = service.node().membership().all();
    CHECK(std::none_of(membership_after.begin(), membership_after.end(),
                       [&](const NodeInfo& node) { return node.id == stale.id; }));

    // Re-gossiping the pre-reset stale association cannot resurrect it.
    service.node().membership().observe(stale, false);
    const auto membership_regossip = service.node().membership().all();
    CHECK(std::none_of(membership_regossip.begin(), membership_regossip.end(),
                       [&](const NodeInfo& node) { return node.id == stale.id; }));

    // A different authenticated NodeId may immediately own the same endpoint.
    auto replacement = stale;
    replacement.id = random_node_id();
    service.node().membership().observe(replacement, true);
    const auto membership_replacement = service.node().membership().all();
    CHECK(std::any_of(membership_replacement.begin(), membership_replacement.end(),
                      [&](const NodeInfo& node) { return node.id == replacement.id; }));

    const auto metadata = service.metadata_manager().snapshot();
    CHECK(metadata.node_status.contains(stale.id));
    const auto key = identity_reset_key(stale.host, stale.port);
    REQUIRE(metadata.identity_resets.contains(key));
    CHECK(metadata.identity_resets.at(key).stale_node_id == stale.id);
    CHECK(metadata.identity_resets.at(key).reason == "test endpoint reassignment");

    // The generic management action also supports clearing by IP alone when
    // the stale NodeId is no longer known. Port 0 is the durable wildcard for
    // every advertised endpoint on the host.
    auto second_port = replacement;
    second_port.id = random_node_id();
    second_port.port = 57402;
    service.node().membership().observe(second_port, true);
    auto unrelated = replacement;
    unrelated.id = random_node_id();
    unrelated.host = "10.44.1.51";
    service.node().membership().observe(unrelated, true);

    HttpRequest reset_ip;
    reset_ip.method = "POST";
    reset_ip.path = "/api/v1/manage/identity-associations/reset";
    const std::string reset_ip_body = R"({"host":"10.44.1.50","reason":"clear by ip"})";
    reset_ip.body.assign(reset_ip_body.begin(), reset_ip_body.end());
    auto reset_ip_response = manage.handle(reset_ip);
    REQUIRE(reset_ip_response.status == 200);
    auto reset_ip_json = Json::parse(std::string(
        reinterpret_cast<const char*>(reset_ip_response.body.data()), reset_ip_response.body.size()));
    const auto& reset_ip_value = *reset_ip_json.find("reset");
    CHECK(reset_ip_value.find("scope")->asString() == "[10.44.1.50]:*");
    CHECK(reset_ip_value.find("port")->isNull());
    CHECK(reset_ip_value.find("stale_node_id")->isNull());
    const auto reset_ip_at = reset_ip_value.find("reset_at_unix_ms")->asUInt64();

    const auto after_ip_reset = service.node().membership().all();
    CHECK(std::none_of(after_ip_reset.begin(), after_ip_reset.end(), [&](const NodeInfo& node) {
        return node.host == "10.44.1.50";
    }));
    CHECK(std::any_of(after_ip_reset.begin(), after_ip_reset.end(), [&](const NodeInfo& node) {
        return node.id == unrelated.id;
    }));

    // Pre-reset gossip cannot recreate an association for that IP, but direct
    // post-reset authentication can establish a replacement identity.
    replacement.seen_unix_ms = reset_ip_at ? reset_ip_at - 1 : 0;
    service.node().membership().observe(replacement, false);
    const auto after_stale_regossip = service.node().membership().all();
    CHECK(std::none_of(after_stale_regossip.begin(), after_stale_regossip.end(),
                       [&](const NodeInfo& node) { return node.id == replacement.id; }));
    auto fresh = replacement;
    fresh.id = random_node_id();
    fresh.seen_unix_ms = reset_ip_at + 1;
    service.node().membership().observe(fresh, true);
    const auto after_fresh_auth = service.node().membership().all();
    CHECK(std::any_of(after_fresh_auth.begin(), after_fresh_auth.end(),
                      [&](const NodeInfo& node) { return node.id == fresh.id; }));

    const auto metadata_after_ip = service.metadata_manager().snapshot();
    const auto ip_key = identity_reset_key("10.44.1.50", 0);
    REQUIRE(metadata_after_ip.identity_resets.contains(ip_key));
    CHECK(metadata_after_ip.identity_resets.at(ip_key).stale_node_id == NodeId{});
    CHECK(metadata_after_ip.identity_resets.at(ip_key).reason == "clear by ip");
}



MACHA_TEST("invariants", test_status_api_precedes_control_plane_startup) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("status-first");
    config.catalogue.api.enabled = true;
    config.catalogue.api.listen = "127.0.0.1";
    config.catalogue.api.port = free_port();

    TestGate control_gate;
    Service service(config, cluster.keys(), [&](std::string_view stage) {
        if (stage == "control-plane")
            control_gate.enter_and_wait();
    });
    std::jthread starter([&] { service.start(); });
    struct ReleaseGate {
        TestGate& gate;
        ~ReleaseGate() { gate.open(); }
    } release{control_gate};
    REQUIRE(control_gate.wait_for_entries(1));

    const auto response = raw_http_get(config.catalogue.api.port, "/api/v1/status");
    CHECK(response.find("HTTP/1.1 200") != std::string::npos);
    const auto body_at = response.find("\r\n\r\n");
    REQUIRE(body_at != std::string::npos);
    auto status = Json::parse(response.substr(body_at + 4));
    const auto* startup = status.find("startup");
    REQUIRE(startup != nullptr);
    CHECK(startup->find("phase")->asString() == "starting");
    CHECK(startup->find("api")->asString() == "ready");
    CHECK(startup->find("control_plane")->asString() == "starting");

    control_gate.open();
    starter.join();
    REQUIRE(wait_until([&] { return service.ready(); }, 10s));
}

MACHA_TEST("invariants", test_control_plane_and_status_api_are_online_while_backends_recover) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("recovering-service");
    config.catalogue.api.enabled = true;
    config.catalogue.api.listen = "127.0.0.1";
    config.catalogue.api.port = free_port();

    TestGate recovery_gate;
    Service service(config, cluster.keys(), [&](std::string_view stage) {
        if (stage == "data-storage" || stage == "control-storage")
            recovery_gate.enter_and_wait();
    });
    struct ReleaseGate {
        TestGate& gate;
        ~ReleaseGate() { gate.open(); }
    } release{recovery_gate};

    service.start();
    REQUIRE(recovery_gate.wait_for_entries(2));

    const auto status_response = raw_http_get(config.catalogue.api.port, "/api/v1/status");
    CHECK(status_response.find("HTTP/1.1 200") != std::string::npos);
    const auto body_at = status_response.find("\r\n\r\n");
    REQUIRE(body_at != std::string::npos);
    auto status = Json::parse(status_response.substr(body_at + 4));
    const auto* startup = status.find("startup");
    REQUIRE(startup != nullptr);
    CHECK(startup->find("phase")->asString() == "recovering");
    CHECK(startup->find("api")->asString() == "ready");
    CHECK(startup->find("control_plane")->asString() == "ready");
    CHECK(startup->find("data_storage")->asString() == "recovering");
    CHECK(startup->find("control_storage")->asString() == "recovering");
    CHECK(!service.ready());

    const auto ordinary = raw_http_get(config.catalogue.api.port, "/api/v1/catalogue/status");
    CHECK(ordinary.find("HTTP/1.1 503") != std::string::npos);
    CHECK(ordinary.find("service_recovering") != std::string::npos);

    recovery_gate.open();
    REQUIRE(wait_until([&] { return service.ready(); }, 10s));
    const auto ready_response = raw_http_get(config.catalogue.api.port, "/api/v1/status");
    const auto ready_body_at = ready_response.find("\r\n\r\n");
    REQUIRE(ready_body_at != std::string::npos);
    auto ready_status = Json::parse(ready_response.substr(ready_body_at + 4));
    CHECK(ready_status.find("startup")->find("phase")->asString() == "ready");
}

MACHA_TEST("invariants", test_rpc_membership_is_online_while_local_state_recovers) {
    TestCluster cluster(ConfigProfile::isolated);
    auto recovering_config = cluster.node_config("recovering-node");
    auto peer_config = cluster.node_config("peer-node");

    TestGate recovery_gate;
    NodeRuntime recovering(recovering_config, cluster.keys(), [&](std::string_view stage) {
        if (stage == "data-storage" || stage == "control-storage")
            recovery_gate.enter_and_wait();
    });
    struct ReleaseGate {
        TestGate& gate;
        ~ReleaseGate() { gate.open(); }
    } release{recovery_gate};

    recovering.start();
    REQUIRE(recovery_gate.wait_for_entries(2));
    CHECK(recovering.readiness().control_plane_online);
    CHECK(!recovering.readiness().local_state_ready);

    NodeRuntime peer(peer_config, cluster.keys());
    peer.start();
    REQUIRE(peer.wait_local_state_ready(10s));

    const Endpoint recovering_endpoint{"127.0.0.1", recovering_config.port};
    const auto ping = peer.call(recovering_endpoint, MessageType::ping);
    CHECK(ping.message.type == MessageType::ok);
    const auto members = peer.call(recovering_endpoint, MessageType::members);
    CHECK(members.message.type == MessageType::members_reply);
    REQUIRE(wait_until([&] {
        const auto all = recovering.membership().all();
        return std::any_of(all.begin(), all.end(), [&](const NodeInfo& node) {
            return node.id == peer.node_id();
        });
    }));

    recovery_gate.open();
    REQUIRE(recovering.wait_local_state_ready(10s));
    peer.stop();
    recovering.stop();
}

MACHA_TEST("invariants", test_status_uses_membership_without_telemetry) {
    TestNode fixture("status-membership");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.hydration.enabled = false;
    config.catalogue.scanner.enabled = false;
    auto& node = fixture.start();
    auto& metadata = fixture.metadata();

    // Establish the coherent local metadata view without a Service maintenance
    // thread. This keeps the availability state deterministic for this test.
    const auto local_snapshot = metadata.snapshot();
    REQUIRE(local_snapshot.metadata_voters.empty());

    NodeInfo peer;
    peer.id = random_node_id();
    peer.host = "10.44.1.200";
    peer.port = 7437;
    peer.failure_domain = "test-lab";
    peer.capacity = 4ULL * 1024 * 1024 * 1024;
    peer.used = 1024ULL * 1024 * 1024;
    peer.metadata_generation = node.metadata_replica().generation();
    peer.seen_unix_ms = unix_ms();
    node.membership().observe(peer, true);

    // Deliberately do not create a telemetry observation for the peer. Cluster
    // membership alone must make it visible and online in Status. Replica-set
    // validation is convergence telemetry in 0.19; it must not demote write
    // capability while the configured durability floor is reachable.
    metadata.note_replica_validation(false, "test metadata reconciliation pending");
    ClusterStatusService status(node);
    status.attach_metadata(metadata);
    HttpRequest request;
    request.method = "GET";
    request.path = "/api/v1/status";
    auto response = status.handle(request);
    REQUIRE(response.status == 200);
    auto root = Json::parse(std::string(reinterpret_cast<const char*>(response.body.data()),
                                        response.body.size()));
    const auto* cluster = root.find("cluster");
    REQUIRE(cluster != nullptr);
    CHECK(cluster->find("nodes_known")->asUInt64() == 2);
    CHECK(cluster->find("nodes_online")->asUInt64() == 2);
    CHECK(cluster->find("metadata_availability")->asString() == "writable");
    CHECK(cluster->find("metadata_read_available")->asBool());
    CHECK(cluster->find("metadata_write_available")->asBool());
    CHECK(!cluster->find("metadata_replica_set_validated")->asBool());

    const auto* connectivity = root.find("connectivity");
    REQUIRE(connectivity != nullptr);
    const auto* upnp = connectivity->find("upnp");
    REQUIRE(upnp != nullptr);
    CHECK(!upnp->find("enabled")->asBool());
    CHECK(!upnp->find("mapping_active")->asBool());
    const auto* advertised = connectivity->find("advertised");
    REQUIRE(advertised != nullptr);
    const auto self = node.membership().self();
    CHECK(advertised->find("host")->asString() == self.host);
    CHECK(advertised->find("port")->asUInt64() == self.port);
    CHECK(advertised->find("source")->asString() == "configured");

    const auto* nodes = root.find("nodes");
    REQUIRE(nodes != nullptr);
    bool found = false;
    for (const auto& value : nodes->asArray()) {
        if (value.find("id")->asString() != to_string(peer.id))
            continue;
        found = true;
        CHECK(value.find("state")->asString() == "online");
        CHECK(value.find("telemetry_freshness")->asString() == "unavailable");
        CHECK(value.find("host")->asString() == peer.host);
        CHECK(value.find("port")->asUInt64() == peer.port);
        CHECK(value.find("storage")->find("capacity_bytes")->asUInt64() == peer.capacity);
    }
    CHECK(found);

    metadata.note_replica_validation(true);
    response = status.handle(request);
    REQUIRE(response.status == 200);
    root = Json::parse(std::string(reinterpret_cast<const char*>(response.body.data()),
                                   response.body.size()));
    cluster = root.find("cluster");
    REQUIRE(cluster != nullptr);
    CHECK(cluster->find("metadata_availability")->asString() == "writable");
    CHECK(cluster->find("metadata_write_available")->asBool());
}

MACHA_TEST("invariants", test_metadata_availability_logs_only_transitions) {
    TestNode fixture("metadata-availability-log");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    auto& node = fixture.start();
    (void)node;
    auto& metadata = fixture.metadata();
    REQUIRE(metadata.snapshot().metadata_voters.empty());

    auto capture = std::make_shared<ConcurrentCapturingLogger>(LogLevel::all);
    Log::set_logger(capture);

    metadata.note_replica_validation(false, "metadata reconciliation pending");
    CHECK(metadata.cluster_status().write_available);
    CHECK(!metadata.cluster_status().stable);
    metadata.note_replica_validation(false, "metadata reconciliation pending");
    metadata.note_replica_validation(true);
    CHECK(metadata.cluster_status().write_available);
    CHECK(metadata.cluster_status().stable);
    metadata.note_replica_validation(false, "metadata reconciliation pending");
    CHECK(metadata.cluster_status().write_available);
    CHECK(!metadata.cluster_status().stable);

    const auto records = capture->records();
    Log::set_logger(std::make_shared<ConsoleLogger>(LogLevel::info));

    size_t availability_logs = 0;
    bool saw_initial_writable = false;
    for (const auto& [level, message] : records) {
        if (message.find("metadata availability changed") == std::string::npos)
            continue;
        ++availability_logs;
        if (level == LogLevel::info &&
            message.find("state=writable previous=unavailable") != std::string::npos &&
            message.find("reconciliation pending") != std::string::npos)
            saw_initial_writable = true;
    }
    CHECK(availability_logs == 1);
    CHECK(saw_initial_writable);
}

MACHA_TEST("invariants", test_fuse_open_inode_identity_survives_external_replace_and_unlink) {
    TestNode fixture("node");
    auto& config = fixture.config();
    fixture.start();
    auto& fs = fixture.filesystem();

    const auto old_bytes = pattern(4096, 3);
    const auto new_bytes = pattern(4096, 4);
    write_file(fs, "/replace.bin", old_bytes);
    write_file(fs, "/unlink.bin", old_bytes);

    FuseFrontend frontend(fs, config.fuse);
    const auto replaced_handle = frontend.open("/replace.bin", true, false, false, false);
    const auto unlinked_handle = frontend.open("/unlink.bin", true, false, false, false);

    // Stand in for another node publishing a new namespace generation.
    fs.rename("/replace.bin", "/old-replace.bin");
    write_file(fs, "/replace.bin", new_bytes);
    fs.unlink("/unlink.bin");

    // Force adoption of the already-decoded newer namespace.
    REQUIRE(frontend.inode_for_path("/replace.bin").has_value());
    const auto stale_name = frontend.inode_for_path("/unlink.bin");

    // The descriptor opened before replacement still denotes the original file.
    CHECK(fuse_read(frontend, replaced_handle.inode, old_bytes.size()) == old_bytes);
    // POSIX unlink removes the pathname immediately even though the open inode lives on.
    CHECK(!stale_name.has_value());
    CHECK(fuse_read(frontend, unlinked_handle.inode, old_bytes.size()) == old_bytes);

    frontend.stop();
}

MACHA_TEST("invariants", test_dirty_open_inode_never_writes_remote_replacement) {
    TestNode fixture("node");
    auto& config = fixture.config();
    fixture.start();
    auto& fs = fixture.filesystem();

    const auto original = pattern(4096, 21);
    const auto replacement = pattern(4096, 22);
    const auto dirty = pattern(2048, 23);
    write_file(fs, "/victim.bin", original);
    write_file(fs, "/incoming.bin", replacement);

    FuseFrontend frontend(fs, config.fuse);
    const auto old = frontend.open("/victim.bin", true, true, false, false);
    REQUIRE(frontend.write(old.inode, 0, dirty) == dirty.size());

    // Stand in for a second node atomically renaming a different inode over the
    // dirty pathname. The old open descriptor remains valid locally, but it no
    // longer owns /victim.bin and must never publish through that name.
    fs.rename("/incoming.bin", "/victim.bin");
    const auto current = frontend.inode_for_path("/victim.bin");
    REQUIRE(current.has_value());
    CHECK(*current != old.inode);
    CHECK(fuse_read(frontend, old.inode, dirty.size()) == dirty);

    frontend.release(old.inode, true);
    REQUIRE(frontend.wait_for_idle(5s));

    auto reader = fs.open_read("/victim.bin");
    Bytes actual(replacement.size());
    size_t offset = 0;
    while (offset < actual.size()) {
        const auto n = reader->read(offset, {actual.data() + offset, actual.size() - offset});
        REQUIRE(n > 0);
        offset += n;
    }
    CHECK(actual == replacement);

    frontend.stop();
}

MACHA_TEST("invariants", test_failed_catalogue_commit_never_deletes_live_filesystem_object) {
    TestNode fixture("node");
    fixture.prepare();
    auto& node = fixture.start();
    auto& store = fixture.store();
    auto& metadata = fixture.metadata();
    auto& fs = fixture.filesystem();
    CatalogueManager catalogue(node, store, metadata);

    const auto live_bytes = pattern(8192, 5);
    write_file(fs, "/live.bin", live_bytes);
    const auto live_entry = fs.getattr("/live.bin");
    REQUIRE(live_entry.extents.size() == 1);
    const auto live_id = live_entry.extents.front().id;
    REQUIRE(node.local_store().get(live_id).has_value());

    CatalogueItem item;
    item.id = "test:movie:failed-stage";
    item.kind = CatalogueKind::movie;
    item.title = "Failed stage must not delete live data";
    item.artwork.push_back({"poster", live_id, "application/octet-stream"});
    auto missing_bytes = pattern(7777, 6);
    const auto missing_id = object_id(missing_bytes);
    REQUIRE(missing_id != live_id);
    item.artwork.push_back({"backdrop", missing_id, "application/octet-stream"});

    bool failed = false;
    try {
        (void)catalogue.upsert(std::move(item));
    } catch (...) {
        failed = true;
    }
    REQUIRE(failed);

    // Catalogue rollback must never directly erase a hash that is still
    // reachable from namespace metadata; orphan collection belongs to GC.
    bool live_object_readable = false;
    try {
        auto data = node.local_store().get(live_id);
        live_object_readable = data && *data == live_bytes;
    } catch (...) {
        live_object_readable = false;
    }
    CHECK(live_object_readable);

}

MACHA_TEST("invariants", test_scanner_prune_is_fenced_to_scanned_namespace) {
    TestNode fixture("node");
    fixture.prepare();
    auto& node = fixture.start();
    auto& store = fixture.store();
    auto& metadata = fixture.metadata();
    auto& fs = fixture.filesystem();
    CatalogueManager catalogue(node, store, metadata);

    const auto old_bytes = pattern(4096, 31);
    const auto new_bytes = pattern(4096, 32);
    write_file(fs, "/media.bin", old_bytes);
    const auto old_entry = fs.getattr("/media.bin");
    const auto scanned = fs.local_snapshot_view();
    const auto scanned_signature = metadata_namespace_signature(*scanned.snapshot);
    const std::set<std::string> stale_active{file_media_id(old_entry)};

    fs.unlink("/media.bin");
    write_file(fs, "/media.bin", new_bytes);
    const auto new_entry = fs.getattr("/media.bin");
    const auto new_media_id = file_media_id(new_entry);
    REQUIRE(new_media_id != file_media_id(old_entry));

    CatalogueItem item;
    item.id = "test:movie:namespace-fence";
    item.kind = CatalogueKind::movie;
    item.title = "Namespace fence";
    item.external_ids["macha_scanner"] = "1";
    item.media_ids = {new_media_id};
    (void)catalogue.upsert(item);

    bool conflicted = false;
    try {
        catalogue.reconcile_scanner({}, stale_active, true, scanned_signature);
    } catch (const CatalogueConflict&) {
        conflicted = true;
    }
    CHECK(conflicted);
    auto after = catalogue.get(item.id);
    REQUIRE(after.has_value());
    CHECK(after->media_ids == std::vector<std::string>{new_media_id});

}

MACHA_FAST_TEST("invariants", test_scanner_does_not_prune_from_mixed_namespace_generations) {
    TestNode fixture("node");
    fixture.prepare();
    auto& node = fixture.start();
    auto& store = fixture.store();
    auto& metadata = fixture.metadata();
    auto& fs = fixture.filesystem();
    CatalogueManager catalogue(node, store, metadata);

    FsEntry dir;
    dir.type = EntryType::directory;
    dir.mode = 0755;
    dir.uid = getuid();
    dir.gid = getgid();
    dir.ctime_ns = dir.mtime_ns = wall_time_ns();
    FsEntry file;
    file.type = EntryType::file;
    file.mode = 0644;
    file.uid = getuid();
    file.gid = getgid();
    file.size = 1234;
    file.ctime_ns = file.mtime_ns = wall_time_ns();
    file.extents.push_back({0, file.size, object_id(pattern(1234, 7)), false});

    metadata.mutate([&](MetadataSnapshot& snapshot) {
        snapshot.entries["/Movies"] = dir;
        snapshot.entries["/Movies/A"] = dir;
        snapshot.entries["/Movies/B"] = dir;
        snapshot.entries["/Movies/A/live.mkv"] = file;
    });

    CatalogueItem item;
    item.id = "test:movie:mixed-generation";
    item.kind = CatalogueKind::movie;
    item.title = "Still live";
    item.external_ids["macha_scanner"] = "1";
    item.media_ids = {file_media_id(file)};
    (void)catalogue.upsert(item);

    // Capture the exact immutable generation a complete scanner pass enumerates.
    // The production scanner calls catalogue_snapshot_files() with this snapshot
    // and derives its destructive-reconciliation signature from the same object.
    const auto scanned = fs.local_snapshot_view();
    const auto scanned_signature = metadata_namespace_signature(*scanned.snapshot);
    REQUIRE(scanned.snapshot->entries.contains("/Movies/A/live.mkv"));
    CHECK(!scanned.snapshot->entries.contains("/Movies/B/live.mkv"));

    const auto discovered = catalogue_snapshot_files("/Movies", *scanned.snapshot);
    REQUIRE(discovered.size() == 1);
    CHECK(discovered.front().first == "/Movies/A/live.mkv");
    CHECK(file_media_id(discovered.front().second) == file_media_id(file));

    std::set<std::string> active;
    for (const auto& [_, entry] : discovered)
        active.insert(file_media_id(entry));

    // Reproduce the historical failure deterministically: the immutable object
    // moves after the scan snapshot has been captured. A snapshot-pinned scan
    // still sees that object's media id, so complete reconciliation must not
    // infer absence merely because the live path now belongs to a later
    // namespace generation.
    fs.rename("/Movies/A/live.mkv", "/Movies/B/live.mkv");
    const auto moved = fs.local_snapshot_view();
    CHECK(!moved.snapshot->entries.contains("/Movies/A/live.mkv"));
    REQUIRE(moved.snapshot->entries.contains("/Movies/B/live.mkv"));
    CHECK(metadata_namespace_signature(*moved.snapshot) != scanned_signature);

    // No catalogue mutation is required here: the immutable media id is still
    // active in the captured generation. In particular, a namespace conflict is
    // not itself required when reconciliation is a no-op.
    catalogue.reconcile_scanner({}, active, true, scanned_signature);
    auto after_move = catalogue.get(item.id);
    REQUIRE(after_move.has_value());
    CHECK(after_move->media_ids == std::vector<std::string>{file_media_id(file)});

    // Now make the stale scan genuinely destructive relative to current state.
    // A new immutable object replaces the moved file and becomes the catalogue
    // binding. Reusing the old scan's active set would prune that live binding,
    // so the old namespace signature must fence the commit.
    const Bytes replacement = pattern(1234, 19);
    fs.unlink("/Movies/B/live.mkv");
    write_file(fs, "/Movies/B/live.mkv", replacement);
    const auto replacement_entry = fs.getattr("/Movies/B/live.mkv");
    const auto replacement_media_id = file_media_id(replacement_entry);
    REQUIRE(replacement_media_id != file_media_id(file));

    auto current_item = catalogue.get(item.id);
    REQUIRE(current_item.has_value());
    current_item->media_ids = {replacement_media_id};
    (void)catalogue.upsert(*current_item, current_item->revision);

    bool conflicted = false;
    try {
        catalogue.reconcile_scanner({}, active, true, scanned_signature);
    } catch (const CatalogueConflict&) {
        conflicted = true;
    }
    CHECK(conflicted);
    auto after_replace = catalogue.get(item.id);
    REQUIRE(after_replace.has_value());
    CHECK(after_replace->media_ids == std::vector<std::string>{replacement_media_id});
}

MACHA_TEST("invariants", test_replica_repair_does_not_count_corrupt_remote_as_healthy) {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(t.path() / "n1", keyfile, p1);
    auto c2 = config_for(t.path() / "n2", keyfile, p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 2;
    c1.min_write_replicas = c2.min_write_replicas = 2;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 1;
    c1.storage_packing = c2.storage_packing = StoragePackingConfig{0, 0};

    NodeRuntime n1(c1, keys);
    NodeRuntime n2(c2, keys);
    n1.start();
    n2.start();
    REQUIRE(wait_until([&] {
        return n1.membership().active().size() >= 2 && n2.membership().active().size() >= 2;
    }));

    DistributedStore distributed(n1);
    const auto bytes = pattern(128 * 1024, 9);
    const auto id = object_id(bytes);
    REQUIRE(distributed.put(id, bytes));
    REQUIRE(wait_until([&] { return n2.local_store().has(id); }));
    corrupt_object(c2.storage_backends.front().path, id);

    std::vector<ObjectId> live{id};
    std::vector<ObjectId> universal{id};
    for (int i = 0; i < 4; ++i)
        distributed.repair_once(8ULL * 1024 * 1024, &live, &universal);

    bool repaired = false;
    try {
        auto data = n2.local_store().get(id);
        repaired = data && *data == bytes;
    } catch (...) {
        repaired = false;
    }
    CHECK(repaired);

    n2.stop();
    n1.stop();
}

MACHA_TEST("invariants", test_rebalance_never_deletes_last_valid_copy_for_corrupt_preferred_copy) {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto a = t.path() / "a";
    const auto b = t.path() / "b";
    std::filesystem::create_directories(a);
    std::filesystem::create_directories(b);
    const auto bytes = pattern(96 * 1024, 10);
    const auto id = object_id(bytes);

    const auto pool_state = t.path() / "pool-state";
    const auto pool_node = random_node_id();
    const std::vector<StorageBackendConfig> backends{
        {a, 64ULL * 1024 * 1024}, {b, 64ULL * 1024 * 1024}};

    // Establish the 0.18 backend identity/format boundary while the stores are
    // empty, then seed the corruption scenario through loose LocalStore objects.
    // Reopening the pool must therefore exercise valid 0.18 media, not bypass
    // genesis protection with an unversioned pre-populated directory.
    {
        StoragePool initialise(pool_state, pool_node, backends, keys.storage);
        REQUIRE(wait_until([&] { return initialise.online_backends() == 2; }));
    }
    {
        LocalStore sa(a, 64ULL * 1024 * 1024, keys.storage);
        LocalStore sb(b, 64ULL * 1024 * 1024, keys.storage);
        REQUIRE(wait_until([&] { return sa.scan_complete() && sb.scan_complete(); }));
        REQUIRE(sa.put(id, bytes));
        REQUIRE(sb.put(id, bytes));
    }

    StoragePool pool(pool_state, pool_node, backends, keys.storage);
    REQUIRE(wait_until([&] { return pool.online_backends() == 2; }));

    // pool.put() reaffirms/touches only the deterministic preferred backend.
    const auto a_before = std::filesystem::last_write_time(object_path(a, id));
    std::this_thread::sleep_for(20ms);
    REQUIRE(pool.put(id, bytes));
    const auto a_after = std::filesystem::last_write_time(object_path(a, id));
    const auto preferred = a_after != a_before ? a : b;
    const auto secondary = preferred == a ? b : a;
    REQUIRE(std::filesystem::exists(object_path(secondary, id)));

    corrupt_object(preferred, id);
    for (int i = 0; i < 8; ++i) {
        auto result = pool.rebalance_step(8ULL * 1024 * 1024, 8);
        if (result.complete) break;
    }

    // Rebalance may converge back to one physical copy, but it must not remove
    // the last previously-valid copy until the repaired preferred replica has
    // itself passed strong validation.
    CHECK(std::filesystem::exists(object_path(secondary, id)) || pool.valid(id));
    bool readable = false;
    try {
        auto data = pool.get(id);
        readable = data && *data == bytes;
    } catch (...) {
        readable = false;
    }
    CHECK(readable);
}

MACHA_TEST("invariants", test_rpc_pre_auth_admission_is_bounded) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto port = free_port();
    NodeInfo server_info{random_node_id(), "127.0.0.1", "test", port};
    RpcServer server("127.0.0.1", port, keys, server_info,
                     [](const NodeInfo&, FrameType, const RpcMessage&) {
                         return RpcMessage{MessageType::ok, {}};
                     },
                     [](const NodeInfo&) {});
    server.start();

    const auto thread_count = [] {
        size_t count = 0;
        for (const auto& ignored : std::filesystem::directory_iterator("/proc/self/task")) {
            (void)ignored;
            ++count;
        }
        return count;
    };
    const auto before = thread_count();
    std::vector<int> sockets;
    for (int i = 0; i < 32; ++i) sockets.push_back(connect_idle(port));
    std::this_thread::sleep_for(100ms);
    const auto after = thread_count();

    // Unauthenticated sockets must consume bounded resources; one blocking
    // jthread per pre-auth peer is the failure this regression exposes.
    CHECK(after <= before + 8);

    for (auto fd : sockets) close(fd);
    server.stop();
#else
    std::cout << "[ARCH-REGRESSION] pre-auth admission check requires /proc/self/task; skipped\n";
#endif
}

MACHA_TEST("invariants", test_authoritative_deferred_generation_batches_stable_storage_barriers) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    auto domain = std::make_shared<DurabilityDomain>(1, root, 20ms);
    LocalStore store(root, 64ULL * 1024 * 1024, keys.storage,
                     LocalStoreMode::authoritative, domain);
    REQUIRE(wait_until([&] { return store.scan_complete(); }));

    const auto strict_a = pattern(256 * 1024, 31);
    const auto strict_b = pattern(256 * 1024, 32);
    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    REQUIRE(store.put(object_id(strict_a), strict_a));
    REQUIRE(store.put(object_id(strict_b), strict_b));
    track_fsync = false;

    // Strict puts now use exactly the same write/rename/generation path as WAL-
    // backed publication. The first authoritative mutation marks accounting
    // DIRTY once; each strict caller requests physical durability through its
    // domain generation. There are no per-object or directory fsyncs.
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 1);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 2);

    const auto deferred_a = pattern(256 * 1024, 33);
    const auto deferred_b = pattern(256 * 1024, 34);
    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    const auto ga = store.put_deferred(object_id(deferred_a), deferred_a);
    const auto gb = store.put_deferred(object_id(deferred_b), deferred_b);
    REQUIRE(ga.has_value());
    REQUIRE(gb.has_value());
    REQUIRE(*gb > *ga);
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 0);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 0);

    store.durability_barrier(*gb);
    track_fsync = false;
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 0);
    CHECK(store.get(object_id(deferred_a)) == std::optional<Bytes>{deferred_a});
    CHECK(store.get(object_id(deferred_b)) == std::optional<Bytes>{deferred_b});
#else
    std::cout << "[ARCH-REGRESSION] syncfs generation check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_catalogue_artwork_batch_defers_durability_until_barrier) {
    TestNode fixture("catalogue-artwork-batch");
    fixture.config().maintenance.interval = std::chrono::hours(1);
    fixture.start();
    REQUIRE(wait_until([&] { return fixture.node().local_store().online_backends() == 1; }));

    CatalogueManager catalogue(fixture.node(), fixture.store(), fixture.metadata());
    DistributedStore::DurabilityBatch batch;
    const auto a = pattern(64 * 1024, 201);
    const auto b = pattern(64 * 1024, 202);
    const auto art_a = catalogue.stage_artwork_deferred("poster", "image/jpeg", a, batch);
    const auto art_b = catalogue.stage_artwork_deferred("backdrop", "image/jpeg", b, batch);

    REQUIRE(batch.requirements.size() == 2);
    for (const auto& requirement : batch.requirements) {
        REQUIRE(requirement.replicas.size() == 1);
        const auto& replica = requirement.replicas.front();
        REQUIRE(replica.id == fixture.node().node_id());
        const StoragePool::DurabilityToken token{
            replica.domain, replica.generation, replica.backend_instance};
        CHECK(!fixture.node().local_store().durability_covered(token));
    }

    REQUIRE(catalogue.artwork_durability_barrier(batch));
    for (const auto& requirement : batch.requirements) {
        const auto& replica = requirement.replicas.front();
        const StoragePool::DurabilityToken token{
            replica.domain, replica.generation, replica.backend_instance};
        CHECK(fixture.node().local_store().durability_covered(token));
    }
    CHECK(fixture.node().local_store().has(art_a.id));
    CHECK(fixture.node().local_store().has(art_b.id));
}

MACHA_TEST("invariants", test_accounting_dirty_marker_is_process_session_scoped) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    auto domain = std::make_shared<DurabilityDomain>(1, root, 10ms);
    LocalStore store(root, 64ULL * 1024 * 1024, keys.storage,
                     LocalStoreMode::authoritative, domain);
    REQUIRE(wait_until([&] { return store.scan_complete(); }));

    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    const auto a = pattern(128 * 1024, 44);
    const auto b = pattern(128 * 1024, 45);
    const auto c = pattern(128 * 1024, 46);
    const auto ga = store.put_deferred(object_id(a), a);
    const auto gb = store.put_deferred(object_id(b), b);
    REQUIRE(ga.has_value());
    REQUIRE(gb.has_value());
    store.durability_barrier(*gb);
    const auto gc = store.put_deferred(object_id(c), c);
    REQUIRE(gc.has_value());
    store.durability_barrier(*gc);
    track_fsync = false;

    // Capacity accounting is derived state. It is made crash-detectably DIRTY
    // once before the first mutation and is not toggled CLEAN/DIRTY around each
    // publication generation. Clean teardown checkpoints it after a final
    // domain barrier outside this measured scope.
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 1);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 2);
#else
    std::cout << "[ARCH-REGRESSION] accounting session check is Linux-only; skipped\n";
#endif
}


MACHA_TEST("invariants", test_unclean_accounting_recovery_establishes_durable_baseline) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    const auto a = pattern(128 * 1024, 140);
    const auto b = pattern(128 * 1024, 141);
    uint64_t clean_used = 0;
    {
        LocalStore store(root, 64ULL * 1024 * 1024, keys.storage);
        REQUIRE(wait_until([&] { return store.scan_complete(); }));
        REQUIRE(store.put(object_id(a), a));
        clean_used = store.used();
    }

    const auto child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        try {
            LocalStore store(root, 64ULL * 1024 * 1024, keys.storage);
            if (!store.scan_complete())
                _exit(20);
            const auto generation = store.put_deferred(object_id(b), b);
            if (!generation.has_value())
                _exit(21);
            // Deliberately bypass LocalStore destruction/clean accounting. This
            // models an unclean process exit while the OS itself keeps running.
            _exit(0);
        } catch (...) {
            _exit(22);
        }
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);

    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    {
        auto domain = std::make_shared<DurabilityDomain>(1, root, 10ms);
        LocalStore recovered(root, 64ULL * 1024 * 1024, keys.storage,
                             LocalStoreMode::authoritative, domain);
        REQUIRE(wait_until([&] { return recovered.scan_complete(); }, 5s));
        CHECK(recovered.used() > clean_used);
        REQUIRE(recovered.get(object_id(b)).has_value());
    }
    track_fsync = false;

    // DIRTY accounting forces a tree reconciliation and one physical baseline
    // before generation-zero existing objects can be used as durability proof.
    CHECK(syncfs_calls.load(std::memory_order_relaxed) >= 1);
#else
    std::cout << "[ARCH-REGRESSION] unclean accounting baseline check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_restart_durable_reaffirmation_requires_no_new_barrier) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    const auto bytes = pattern(128 * 1024, 142);
    const auto id = object_id(bytes);
    {
        LocalStore store(root, 64ULL * 1024 * 1024, keys.storage);
        REQUIRE(wait_until([&] { return store.scan_complete(); }));
        REQUIRE(store.put(id, bytes));
    }

    auto domain = std::make_shared<DurabilityDomain>(1, root, 100ms);
    LocalStore reopened(root, 64ULL * 1024 * 1024, keys.storage,
                        LocalStoreMode::authoritative, domain);
    REQUIRE(reopened.scan_complete());

    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    const auto generation = reopened.put_deferred(id, bytes);
    REQUIRE(generation.has_value());
    CHECK(*generation == 0);
    reopened.durability_barrier(*generation);
    track_fsync = false;
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 0);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 0);
#else
    std::cout << "[ARCH-REGRESSION] restart reaffirmation check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_authoritative_delete_is_lazy_durability) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    const auto bytes = pattern(128 * 1024, 143);
    const auto id = object_id(bytes);
    {
        LocalStore store(root, 64ULL * 1024 * 1024, keys.storage);
        REQUIRE(wait_until([&] { return store.scan_complete(); }));
        REQUIRE(store.put(id, bytes));
    }

    auto domain = std::make_shared<DurabilityDomain>(1, root, 100ms);
    LocalStore reopened(root, 64ULL * 1024 * 1024, keys.storage,
                        LocalStoreMode::authoritative, domain);
    REQUIRE(reopened.scan_complete());
    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    REQUIRE(reopened.remove(id));
    track_fsync = false;

    // The first mutation dirties derived accounting once. The unlink itself is
    // intentionally not a synchronous durability event: losing it in a crash
    // leaks unreachable garbage, never published media.
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 1);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 0);
#else
    std::cout << "[ARCH-REGRESSION] lazy deletion check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_local_store_barrier_remembers_complete_physical_cut) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    auto domain = std::make_shared<DurabilityDomain>(1, root, 10ms);
    LocalStore store(root, 64ULL * 1024 * 1024, keys.storage,
                     LocalStoreMode::authoritative, domain);
    REQUIRE(wait_until([&] { return store.scan_complete(); }));

    const auto a = pattern(256 * 1024, 47);
    const auto b = pattern(256 * 1024, 48);
    const auto ga = store.put_deferred(object_id(a), a);
    const auto gb = store.put_deferred(object_id(b), b);
    REQUIRE(ga.has_value());
    REQUIRE(gb.has_value());
    REQUIRE(*gb > *ga);

    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    store.durability_barrier(*ga);
    track_fsync = false;
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 0);
    CHECK(store.durable_generation() >= *gb);

    const auto c = pattern(256 * 1024, 49);
    const auto gc = store.put_deferred(object_id(c), c);
    REQUIRE(gc.has_value());
    REQUIRE(*gc > *gb);

    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    store.durability_barrier(*gb);
    track_fsync = false;
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 0);
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 0);

    syncfs_calls = 0;
    track_fsync = true;
    store.durability_barrier(*gc);
    track_fsync = false;
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
#else
    std::cout << "[ARCH-REGRESSION] local generation cut check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_durability_domain_group_commits_independent_publications) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    auto domain = std::make_shared<DurabilityDomain>(1, root, 120ms);
    LocalStore store(root, 64ULL * 1024 * 1024, keys.storage,
                     LocalStoreMode::authoritative, domain);
    REQUIRE(wait_until([&] { return store.scan_complete(); }));

    const auto a = pattern(256 * 1024, 50);
    const auto ga = store.put_deferred(object_id(a), a);
    REQUIRE(ga.has_value());

    syncfs_calls = 0;
    track_fsync = true;
    auto first = std::async(std::launch::async, [&] { store.durability_barrier(*ga); });
    std::this_thread::sleep_for(30ms);

    const auto b = pattern(256 * 1024, 51);
    const auto gb = store.put_deferred(object_id(b), b);
    REQUIRE(gb.has_value());
    auto second = std::async(std::launch::async, [&] { store.durability_barrier(*gb); });
    std::this_thread::sleep_for(30ms);

    const auto c = pattern(256 * 1024, 52);
    const auto gc = store.put_deferred(object_id(c), c);
    REQUIRE(gc.has_value());
    auto third = std::async(std::launch::async, [&] { store.durability_barrier(*gc); });

    first.get();
    second.get();
    third.get();
    track_fsync = false;

    // Three independent publication completions entered the same physical
    // domain inside one scheduling window. Exactly one filesystem barrier must
    // satisfy all three tickets.
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    CHECK(domain->durable_generation() >= *gc);
#else
    std::cout << "[ARCH-REGRESSION] durability group-commit check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_storage_pool_tokens_name_physical_domain_and_backend_incarnation) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto disk_a = t.path() / "disk-a";
    const auto disk_b = t.path() / "disk-b";
    std::filesystem::create_directories(disk_a);
    std::filesystem::create_directories(disk_b);
    StoragePool pool(t.path() / "state", random_node_id(),
                     {{disk_a, 64ULL * 1024 * 1024}, {disk_b, 64ULL * 1024 * 1024}},
                     keys.storage, 100ms);
    REQUIRE(wait_until([&] { return pool.online_backends() == 2; }));

    std::optional<StoragePool::DurabilityToken> first;
    std::optional<StoragePool::DurabilityToken> second;
    for (uint8_t seed = 60; seed < 120 && !second; ++seed) {
        const auto bytes = pattern(128 * 1024, seed);
        const auto token = pool.put_deferred(object_id(bytes), bytes);
        REQUIRE(token.has_value());
        if (!first)
            first = token;
        else if (token->backend_instance != first->backend_instance)
            second = token;
    }
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->domain == second->domain); // both configured paths are on TempDir's filesystem
    CHECK(first->backend_instance != second->backend_instance);

    syncfs_calls = 0;
    track_fsync = true;
    auto a = std::async(std::launch::async, [&] { pool.durability_barrier(*first); });
    auto b = std::async(std::launch::async, [&] { pool.durability_barrier(*second); });
    a.get();
    b.get();
    track_fsync = false;
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    CHECK(pool.durability_covered(*first));
    CHECK(pool.durability_covered(*second));
#else
    std::cout << "[ARCH-REGRESSION] storage-domain token check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_immediate_reaffirmation_flushes_provisional_generation) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto root = t.path() / "objects";
    auto domain = std::make_shared<DurabilityDomain>(1, root, 50ms);
    LocalStore store(root, 64ULL * 1024 * 1024, keys.storage,
                     LocalStoreMode::authoritative, domain);
    REQUIRE(wait_until([&] { return store.scan_complete(); }));

    const auto bytes = pattern(256 * 1024, 35);
    const auto id = object_id(bytes);
    REQUIRE(store.put_deferred(id, bytes).has_value());

    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    REQUIRE(store.put(id, bytes));
    track_fsync = false;

    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 0);
#else
    std::cout << "[ARCH-REGRESSION] provisional reaffirmation check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_publication_generation_barrier_precedes_metadata_commit) {
#if defined(__linux__)
    TestNode fixture("publication-generation");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    fixture.start();
    auto& fs = fixture.filesystem();
    fs.create_file("/generation.bin", 0644, getuid(), getgid());

    const auto bytes = pattern(config.extent_size * 8, 37);
    auto writer = fs.open_write("/generation.bin", true, false,
                                WriteDurability::publication_generation);
    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    REQUIRE(writer->write(0, bytes) == bytes.size());
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 0);
    writer->commit();
    track_fsync = false;

    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    auto reader = fs.open_read("/generation.bin");
    Bytes actual(bytes.size());
    size_t done = 0;
    while (done < actual.size()) {
        const auto count = reader->read(done, {actual.data() + done, actual.size() - done});
        REQUIRE(count > 0);
        done += count;
    }
    CHECK(actual == bytes);
#else
    std::cout << "[ARCH-REGRESSION] publication generation syncfs check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_persistent_cache_is_explicitly_ephemeral) {
#if defined(__linux__)
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    PersistentBlockCache cache({t.path() / "cache", 2, true}, keys.storage);

    const auto a = pattern(64 * 1024, 41);
    const auto b = pattern(64 * 1024, 42);
    const auto c = pattern(64 * 1024, 43);
    fsync_calls = 0;
    syncfs_calls = 0;
    track_fsync = true;
    REQUIRE(cache.put(object_id(a), a));
    REQUIRE(cache.put(object_id(b), b));
    REQUIRE(cache.put(object_id(c), c));
    track_fsync = false;

    CHECK(cache.blocks() == 2);
    CHECK(fsync_calls.load(std::memory_order_relaxed) == 0);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 0);
#else
    std::cout << "[ARCH-REGRESSION] cache fsync interception check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_control_plane_hint_admission_is_storage_durable) {
#if defined(__linux__)
    TempDir t;
    CatalogueHintQueue hints(t.path() / "state");
    fsync_calls = 0;
    track_fsync = true;
    (void)hints.submit("/Movies/Durable.mkv", "manual", "manual:1",
                       CatalogueHintPriority::manual_rescan);
    track_fsync = false;
    CHECK(fsync_calls.load() >= 2);
#else
    std::cout << "[ARCH-REGRESSION] fsync interception check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_deferred_object_barrier_rejects_stale_process_epoch) {
    TestCluster cluster(ConfigProfile::isolated);
    auto server_config = cluster.node_config("durability-epoch-server");
    auto client_config = cluster.node_config("durability-epoch-client");
    server_config.replication = client_config.replication = 1;
    server_config.metadata_min_write_replicas = client_config.metadata_min_write_replicas = 1;
    NodeRuntime server(server_config, cluster.keys());
    NodeRuntime client(client_config, cluster.keys());
    server.start();
    client.start();
    REQUIRE(server.wait_local_state_ready(10s));
    REQUIRE(client.wait_local_state_ready(10s));

    const auto bytes = pattern(64 * 1024, 46);
    const auto id = object_id(bytes);
    Writer request;
    request.fixed(id.bytes);
    request.bytes(bytes);
    const Endpoint endpoint{"127.0.0.1", server_config.port};
    auto placed = client.call(endpoint, MessageType::put_object_deferred, request.data(),
                              FrameType::read_ahead);
    REQUIRE(placed.message.type == MessageType::ok);
    Reader placed_reply(placed.message.payload);
    NodeId acknowledged_epoch{placed_reply.fixed<16>()};
    const auto domain = placed_reply.u64();
    const auto generation = placed_reply.u64();
    const auto backend_instance = placed_reply.u64();
    placed_reply.finish();
    CHECK(acknowledged_epoch == server.durability_epoch());

    auto barrier_payload = [&](const NodeId& epoch) {
        Writer writer;
        writer.fixed(epoch.bytes);
        writer.u64(domain);
        writer.u64(generation);
        writer.u64(backend_instance);
        return writer.take();
    };

    auto wrong_epoch = random_node_id();
    while (wrong_epoch == acknowledged_epoch)
        wrong_epoch = random_node_id();
    auto stale = barrier_payload(wrong_epoch);
    auto rejected = client.call(endpoint, MessageType::object_durability_barrier, stale,
                                FrameType::read_ahead);
    CHECK(rejected.message.type == MessageType::error);

    auto current = barrier_payload(acknowledged_epoch);
    auto durable = client.call(endpoint, MessageType::object_durability_barrier, current,
                               FrameType::read_ahead);
    CHECK(durable.message.type == MessageType::ok);
    client.stop();
    server.stop();
}

MACHA_TEST("invariants", test_rpc_durability_barrier_group_commits_independent_publications) {
#if defined(__linux__)
    TestCluster cluster(ConfigProfile::isolated);
    auto server_config = cluster.node_config("durability-group-rpc-server");
    auto client_config = cluster.node_config("durability-group-rpc-client");
    server_config.replication = client_config.replication = 1;
    server_config.metadata_min_write_replicas = client_config.metadata_min_write_replicas = 1;
    NodeRuntime server(server_config, cluster.keys());
    NodeRuntime client(client_config, cluster.keys());
    server.start();
    client.start();
    REQUIRE(server.wait_local_state_ready(10s));
    REQUIRE(client.wait_local_state_ready(10s));
    const Endpoint endpoint{"127.0.0.1", server_config.port};

    struct RemoteToken {
        NodeId epoch{};
        uint64_t domain{};
        uint64_t generation{};
        uint64_t backend_instance{};
    };
    auto defer = [&](uint8_t seed) {
        const auto bytes = pattern(64 * 1024, seed);
        Writer request;
        request.fixed(object_id(bytes).bytes);
        request.bytes(bytes);
        auto reply = client.call(endpoint, MessageType::put_object_deferred, request.data(),
                               FrameType::read_ahead);
        REQUIRE(reply.message.type == MessageType::ok);
        Reader reader(reply.message.payload);
        RemoteToken token;
        token.epoch.bytes = reader.fixed<16>();
        token.domain = reader.u64();
        token.generation = reader.u64();
        token.backend_instance = reader.u64();
        reader.finish();
        return token;
    };
    auto barrier = [&](const RemoteToken& token) {
        Writer request;
        request.fixed(token.epoch.bytes);
        request.u64(token.domain);
        request.u64(token.generation);
        request.u64(token.backend_instance);
        return client.call(endpoint, MessageType::object_durability_barrier, request.data(),
                         FrameType::read_ahead);
    };

    const auto a = defer(53);
    syncfs_calls = 0;
    track_fsync = true;
    auto first = std::async(std::launch::async, [&] { return barrier(a); });
    std::this_thread::sleep_for(75ms);
    const auto b = defer(54);
    auto second = std::async(std::launch::async, [&] { return barrier(b); });
    std::this_thread::sleep_for(75ms);
    const auto c = defer(55);
    auto third = std::async(std::launch::async, [&] { return barrier(c); });

    REQUIRE(first.get().message.type == MessageType::ok);
    REQUIRE(second.get().message.type == MessageType::ok);
    REQUIRE(third.get().message.type == MessageType::ok);
    track_fsync = false;

    CHECK(a.epoch == b.epoch);
    CHECK(b.epoch == c.epoch);
    CHECK(a.domain == b.domain);
    CHECK(b.domain == c.domain);
    CHECK(a.backend_instance == b.backend_instance);
    CHECK(b.backend_instance == c.backend_instance);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    client.stop();
    server.stop();
#else
    std::cout << "[ARCH-REGRESSION] RPC group-commit check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_rpc_durability_barrier_reuses_already_covered_generation) {
#if defined(__linux__)
    TestCluster cluster(ConfigProfile::isolated);
    auto server_config = cluster.node_config("durability-generation-rpc-server");
    auto client_config = cluster.node_config("durability-generation-rpc-client");
    server_config.replication = client_config.replication = 1;
    server_config.metadata_min_write_replicas = client_config.metadata_min_write_replicas = 1;
    NodeRuntime server(server_config, cluster.keys());
    NodeRuntime client(client_config, cluster.keys());
    server.start();
    client.start();
    REQUIRE(server.wait_local_state_ready(10s));
    REQUIRE(client.wait_local_state_ready(10s));
    const Endpoint endpoint{"127.0.0.1", server_config.port};

    struct RemoteToken {
        NodeId epoch{};
        uint64_t domain{};
        uint64_t generation{};
        uint64_t backend_instance{};
    };
    auto defer = [&](uint8_t seed) {
        const auto bytes = pattern(64 * 1024, seed);
        Writer request;
        request.fixed(object_id(bytes).bytes);
        request.bytes(bytes);
        auto reply = client.call(endpoint, MessageType::put_object_deferred, request.data(),
                               FrameType::read_ahead);
        REQUIRE(reply.message.type == MessageType::ok);
        Reader reader(reply.message.payload);
        RemoteToken token;
        token.epoch.bytes = reader.fixed<16>();
        token.domain = reader.u64();
        token.generation = reader.u64();
        token.backend_instance = reader.u64();
        reader.finish();
        return token;
    };
    auto barrier = [&](const RemoteToken& token) {
        Writer request;
        request.fixed(token.epoch.bytes);
        request.u64(token.domain);
        request.u64(token.generation);
        request.u64(token.backend_instance);
        return client.call(endpoint, MessageType::object_durability_barrier, request.data(),
                         FrameType::read_ahead);
    };

    const auto a = defer(56);
    const auto b = defer(57);
    REQUIRE(a.epoch == b.epoch);
    REQUIRE(a.domain == b.domain);
    REQUIRE(a.backend_instance == b.backend_instance);
    REQUIRE(b.generation > a.generation);

    syncfs_calls = 0;
    track_fsync = true;
    auto first = barrier(a);
    track_fsync = false;
    REQUIRE(first.message.type == MessageType::ok);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);

    // The physical cut for a covered b as well. Make the domain dirty again
    // with c; waiting for b must not flush this newer generation.
    const auto c = defer(58);
    REQUIRE(c.epoch == a.epoch);
    REQUIRE(c.domain == a.domain);
    REQUIRE(c.backend_instance == a.backend_instance);
    REQUIRE(c.generation > b.generation);

    syncfs_calls = 0;
    track_fsync = true;
    auto covered = barrier(b);
    track_fsync = false;
    REQUIRE(covered.message.type == MessageType::ok);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 0);

    syncfs_calls = 0;
    track_fsync = true;
    auto latest = barrier(c);
    track_fsync = false;
    REQUIRE(latest.message.type == MessageType::ok);
    CHECK(syncfs_calls.load(std::memory_order_relaxed) == 1);
    client.stop();
    server.stop();
#else
    std::cout << "[ARCH-REGRESSION] RPC generation reuse check is Linux-only; skipped\n";
#endif
}

MACHA_TEST("invariants", test_authenticated_receiver_enforces_transport_lane) {
    TempDir t;
    const auto keyfile = t.path() / "cluster.key";
    write_key(keyfile);
    const auto keys = load_cluster_keys(keyfile);
    const auto port = free_port();
    NodeInfo server_info{random_node_id(), "127.0.0.1", "server", port};
    std::atomic_bool object_dispatched{false};
    RpcServer server("127.0.0.1", port, keys, server_info,
                     [&](const NodeInfo&, FrameType, const RpcMessage& message) {
                         if (message.type == MessageType::get_object)
                             object_dispatched = true;
                         return RpcMessage{MessageType::ok, {}};
                     },
                     [](const NodeInfo&) {});
    server.start();

    int fd = connect_idle(port);
    NodeInfo client_info{random_node_id(), "127.0.0.1", "client", free_port()};
    SecureChannel channel(fd, keys, client_info, 64 * 1024);
    (void)channel.client_handshake(TransportLane::control);
    channel.send_fragment(1, FrameType::foreground, MessageType::get_object, true, true, {});
    std::this_thread::sleep_for(100ms);

    // Object traffic on a negotiated CONTROL channel must be rejected before
    // dispatch; sender-side lane selection alone is not protocol enforcement.
    CHECK(!object_dispatched.load());

    channel.shutdown();
    server.stop();
}

MACHA_TEST("invariants", test_http_slow_client_cannot_pin_worker_indefinitely) {
    CatalogueApiConfig config;
    config.enabled = true;
    config.listen = "127.0.0.1";
    config.port = free_port();
    config.workers = 1;
    config.max_queued_connections = 4;
    config.client_io_timeout = 100ms;

    HttpServer server(config, [](const HttpRequest&) {
        return http_json(200, "{\"ok\":true}");
    });
    server.start();
    REQUIRE(wait_until([&] { return server.bound_port() == config.port; }, 2s));

    const int slow = connect_idle(config.port);
    const std::string partial = "GET /slow HTTP/1.1\r\nHost: localhost\r\n";
    REQUIRE(::send(slow, partial.data(), partial.size(), 0) ==
            static_cast<ssize_t>(partial.size()));

    // Queue a complete request behind the sole worker. It must run after the
    // incomplete client exceeds its bounded socket-I/O occupancy.
    const int fast = connect_idle(config.port);
    const std::string request = "GET /ok HTTP/1.1\r\nHost: localhost\r\n\r\n";
    REQUIRE(::send(fast, request.data(), request.size(), 0) ==
            static_cast<ssize_t>(request.size()));
    timeval timeout{1, 0};
    REQUIRE(setsockopt(fast, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    std::string response;
    char buffer[1024];
    while (true) {
        const auto n = ::recv(fast, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        response.append(buffer, static_cast<size_t>(n));
    }
    CHECK(response.find("HTTP/1.1 200 OK") != std::string::npos);

    ::close(slow);
    ::close(fast);
    server.stop();
}

MACHA_TEST("invariants", test_catalogue_gc_liveness_fails_closed_when_current_root_unavailable) {
    TestNode fixture("node");
    fixture.prepare();
    auto& node = fixture.start();
    auto& store = fixture.store();
    auto& metadata = fixture.metadata();
    CatalogueManager catalogue(node, store, metadata);
    catalogue.repair_once(); // establish a coherent empty cached catalogue

    const auto missing_root = object_id(pattern(32123, 11));
    REQUIRE(!node.local_store().has(missing_root));
    metadata.mutate([&](MetadataSnapshot& snapshot) { snapshot.catalogue_root = missing_root; });

    const auto maintenance = catalogue.maintenance_objects();
    // Even when the current immutable catalogue cannot yet be fetched/decoded,
    // its metadata-referenced root is unconditionally live and GC must fail closed.
    CHECK(maintenance.control_live.contains(missing_root));
    CHECK(!maintenance.complete);

}

} // namespace

#if defined(__linux__)
extern "C" int fsync(int fd) {
    if (track_fsync.load(std::memory_order_relaxed))
        fsync_calls.fetch_add(1, std::memory_order_relaxed);
    return static_cast<int>(::syscall(SYS_fsync, fd));
}

extern "C" int syncfs(int fd) {
    if (track_fsync.load(std::memory_order_relaxed))
        syncfs_calls.fetch_add(1, std::memory_order_relaxed);
    return static_cast<int>(::syscall(SYS_syncfs, fd));
}
#endif
