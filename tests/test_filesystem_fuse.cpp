// SPDX-License-Identifier: GPL-3.0-or-later
#include "fuse_journal.hpp"
#include "test_backend_support.hpp"
#include <fcntl.h>

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

MACHA_TEST("filesystem_fuse", test_status_exposes_filesystem_and_convergence_counters) {
    TestService fixture("status-operational-counters", ConfigProfile::isolated);
    auto& config = fixture.config();
    config.catalogue.api.enabled = true;
    config.catalogue.api.listen = "127.0.0.1";
    config.catalogue.api.port = free_port();
    config.fuse.publication_quiet = 0ms;
    auto& service = fixture.start();

    auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
    service.attach_fuse_frontend(frontend);
    frontend->mkdir("/status-counter", 0755, getuid(), getgid());
    REQUIRE(frontend->wait_for_idle(10s));
    REQUIRE(wait_until([&] {
        const auto current = service.metadata_convergence_diagnostics();
        return !current.scheduled && current.runs_scheduled == current.runs_completed;
    }));

    const auto response = raw_http_get(config.catalogue.api.port, "/api/v1/status");
    CHECK(response.find("HTTP/1.1 200") != std::string::npos);
    const auto body_at = response.find("\r\n\r\n");
    REQUIRE(body_at != std::string::npos);
    const auto root = Json::parse(response.substr(body_at + 4));
    const auto* diagnostics = root.find("diagnostics");
    REQUIRE(diagnostics != nullptr);

    const auto* filesystem = diagnostics->find("filesystem");
    REQUIRE(filesystem != nullptr);
    CHECK(filesystem->find("available")->asBool());
    CHECK(filesystem->find("namespace_operations_admitted")->asUInt64() == 1);
    CHECK(filesystem->find("namespace_publication_batches")->asUInt64() == 1);
    CHECK(filesystem->find("namespace_operations_batched")->asUInt64() == 1);
    CHECK(filesystem->find("namespace_operations_published")->asUInt64() == 1);
    CHECK(filesystem->find("namespace_operations_confirmed")->asUInt64() == 1);
    // A new live inode durably appends its descriptor and operation before
    // returning, followed by published and done. Recovery frontends report
    // only the latter two because both admission records predate restart.
    CHECK(filesystem->find("journal_append_batches")->asUInt64() == 4);
    CHECK(filesystem->find("journal_records_appended")->asUInt64() == 4);
    CHECK(filesystem->find("journal_durability_barriers")->asUInt64() == 4);
    CHECK(filesystem->find("spool_bytes")->asUInt64() == 0);
    CHECK(filesystem->find("spool_limit_bytes")->asUInt64() ==
          config.fuse.max_spool_bytes);
    REQUIRE(filesystem->find("spool_publish_rate_bytes_per_second") != nullptr);
    REQUIRE(filesystem->find("spool_publish_rate_window_bytes") != nullptr);
    REQUIRE(filesystem->find("spool_publish_rate_window_ms") != nullptr);
    REQUIRE(filesystem->find("spool_throttle_waits") != nullptr);
    REQUIRE(filesystem->find("spool_throttle_wait_ms") != nullptr);
    REQUIRE(filesystem->find("data_publication_requests") != nullptr);
    REQUIRE(filesystem->find("data_publication_coalesced_queued") != nullptr);
    REQUIRE(filesystem->find("data_publication_coalesced_running") != nullptr);
    REQUIRE(filesystem->find("data_publication_coalesced_unconfirmed") != nullptr);
    REQUIRE(filesystem->find("data_publications_started") != nullptr);
    REQUIRE(filesystem->find("data_publications_completed") != nullptr);
    REQUIRE(filesystem->find("data_publication_peak_active") != nullptr);
    REQUIRE(filesystem->find("data_publication_quanta") != nullptr);
    REQUIRE(filesystem->find("data_publication_yields") != nullptr);
    REQUIRE(filesystem->find("data_publication_peak_inflight_bytes") != nullptr);
    REQUIRE(filesystem->find("data_publication_pipeline_limit_bytes") != nullptr);
    REQUIRE(filesystem->find("data_publication_peak_pipeline_extents") != nullptr);
    CHECK(filesystem->find("data_publication_pipeline_limit_bytes")->asUInt64() ==
          2 * config.extent_size);
    REQUIRE(filesystem->find("data_closed_priority_selections") != nullptr);
    REQUIRE(filesystem->find("data_publication_bytes_read") != nullptr);
    REQUIRE(filesystem->find("data_publication_bytes_committed") != nullptr);
    REQUIRE(filesystem->find("data_publication_bytes_confirmed") != nullptr);

    const auto* convergence = diagnostics->find("convergence");
    REQUIRE(convergence != nullptr);
    CHECK(convergence->find("available")->asBool());
    CHECK(convergence->find("events_received")->asUInt64() >= 1);
    CHECK(convergence->find("runs_scheduled")->asUInt64() >= 1);
    CHECK(convergence->find("runs_completed")->asUInt64() ==
          convergence->find("runs_scheduled")->asUInt64());
    CHECK(convergence->find("requested_epoch")->asUInt64() ==
          convergence->find("completed_epoch")->asUInt64());
    CHECK(convergence->find("latest_generation")->asUInt64() ==
          service.node().known_metadata_generation());
    CHECK(!convergence->find("scheduled")->asBool());

    service.attach_fuse_frontend({});
    const auto detached_response = raw_http_get(config.catalogue.api.port, "/api/v1/status");
    const auto detached_body_at = detached_response.find("\r\n\r\n");
    REQUIRE(detached_body_at != std::string::npos);
    const auto detached = Json::parse(detached_response.substr(detached_body_at + 4));
    CHECK(!detached.find("diagnostics")->find("filesystem")->find("available")->asBool());
    frontend->stop();
}

MACHA_TEST("filesystem_fuse", test_open_write_metadata_merge) {
    TestService fixture("single-write");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;

    auto& service = fixture.start();

    service.filesystem().create_file("/copy.mkv", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/copy.mkv", true);
    auto input = pattern(3 * config.extent_size + 12345);
    size_t offset = 0;
    while (offset < input.size()) {
        size_t n = std::min<size_t>(4096, input.size() - offset);
        REQUIRE(writer->write(offset, {input.data() + offset, n}) == n);
        offset += n;
    }

    // macOS copyfile/cp can apply mode/ownership/timestamps through the still-open
    // file descriptor before FUSE flush/release publishes the data manifest.
    // These are metadata-only changes and must not invalidate the writer.
    const int64_t preserved_mtime = 1700000000123456789LL;
    service.filesystem().chmod("/copy.mkv", 0600);
    service.filesystem().chown("/copy.mkv", getuid(), getgid(), true, true);
    service.filesystem().utimens("/copy.mkv", preserved_mtime);
    writer->commit();

    auto entry = service.filesystem().getattr("/copy.mkv");
    CHECK(entry.size == input.size());
    CHECK(entry.mode == 0600);
    CHECK(entry.uid == static_cast<uint32_t>(getuid()));
    CHECK(entry.gid == static_cast<uint32_t>(getgid()));
    CHECK(entry.mtime_ns == preserved_mtime);

    auto reader = service.filesystem().open_read("/copy.mkv");
    Bytes output(input.size());
    size_t got = 0;
    while (got < output.size()) {
        auto n = reader->read(got, {output.data() + got, output.size() - got});
        REQUIRE(n > 0);
        got += n;
    }
    CHECK(output == input);

    // A real concurrent content update is still a conflict.
    service.filesystem().create_file("/conflict.bin", 0644, getuid(), getgid());
    auto first = service.filesystem().open_write("/conflict.bin", true);
    auto second = service.filesystem().open_write("/conflict.bin", true);
    auto a = pattern(8192);
    auto b = pattern(8193);
    REQUIRE(first->write(0, a) == a.size());
    REQUIRE(second->write(0, b) == b.size());
    first->commit();
    bool conflicted = false;
    try {
        second->commit();
    } catch (const FsError& e) {
        conflicted = e.code() == EAGAIN;
    }
    CHECK(conflicted);
}

MACHA_TEST("filesystem_fuse", test_publication_extent_pipeline_is_bounded_and_atomic) {
    TestService fixture("publication-extent-pipeline");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    auto& service = fixture.start();

    service.filesystem().create_file("/pipeline.bin", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write(
        "/pipeline.bin", true, false, WriteDurability::publication_generation,
        2 * config.extent_size);
    const auto input = pattern(4 * config.extent_size);
    for (size_t offset = 0; offset < input.size(); offset += config.extent_size) {
        REQUIRE(writer->write(offset,
                              {input.data() + offset, config.extent_size}) ==
                config.extent_size);
    }

    // Enqueueing a third extent must retire the oldest one first. Completed
    // provisional objects are intentionally not namespace-visible.
    auto staged = writer->diagnostics();
    CHECK(staged.peak_pending_extent_puts == 2);
    CHECK(staged.pending_extent_puts == 2);
    CHECK(staged.new_extent_puts == 2);
    CHECK(service.filesystem().getattr("/pipeline.bin").size == 0);

    // A fairness/viewer boundary drains the bounded admitted set while leaving
    // the complete-file metadata transaction uncommitted.
    writer->drain_staging();
    staged = writer->diagnostics();
    CHECK(staged.pending_extent_puts == 0);
    CHECK(staged.new_extent_puts == 4);
    CHECK(service.filesystem().getattr("/pipeline.bin").size == 0);

    writer->commit();
    CHECK(service.filesystem().getattr("/pipeline.bin").size == input.size());
    auto reader = service.filesystem().open_read("/pipeline.bin");
    Bytes output(input.size());
    REQUIRE(reader->read(0, output) == output.size());
    CHECK(output == input);
}

MACHA_TEST("filesystem_fuse", test_publication_extent_pipeline_failure_stays_invisible) {
    TestService fixture("publication-extent-pipeline-failure");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    config.storage_backends.front().limit = 2 * config.extent_size;
    auto& service = fixture.start();

    service.filesystem().create_file("/pipeline-failure.bin", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write(
        "/pipeline-failure.bin", true, false, WriteDurability::publication_generation,
        2 * config.extent_size);
    const auto input = pattern(4 * config.extent_size);
    bool failed = false;
    try {
        for (size_t offset = 0; offset < input.size(); offset += config.extent_size)
            writer->write(offset, {input.data() + offset, config.extent_size});
        writer->drain_staging();
    } catch (const std::exception&) {
        failed = true;
    }
    CHECK(failed);
    writer.reset();

    // Successful provisional extents from the abandoned generation are not a
    // partial file. The durable spool caller remains free to replay the whole
    // generation after storage becomes available.
    CHECK(service.filesystem().getattr("/pipeline-failure.bin").size == 0);
}

MACHA_TEST("filesystem_fuse", test_fresh_and_resumed_write_exactness) {
    TestService fixture("write-exactness");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;

    auto& service = fixture.start();

    const auto read_exact = [&](const std::string& path, size_t size) {
        auto reader = service.filesystem().open_read(path);
        Bytes output(size);
        size_t offset = 0;
        while (offset < output.size()) {
            const auto n = reader->read(
                offset, {output.data() + offset, std::min<size_t>(131071, output.size() - offset)});
            REQUIRE(n > 0);
            offset += n;
        }
        return output;
    };

    // Fresh rsync-style sequential write: varied FUSE-sized chunks cross many
    // extent boundaries and the final extent is deliberately partial.
    auto fresh = pattern(5 * config.extent_size + 123457);
    service.filesystem().create_file("/fresh.bin", 0644, getuid(), getgid());
    auto fresh_writer = service.filesystem().open_write("/fresh.bin", true);
    size_t offset = 0;
    while (offset < fresh.size()) {
        const auto n = std::min<size_t>(65537, fresh.size() - offset);
        REQUIRE(fresh_writer->write(offset, {fresh.data() + offset, n}) == n);
        offset += n;
    }
    fresh_writer->commit();
    CHECK(read_exact("/fresh.bin", fresh.size()) == fresh);

    // --append/--append-verify style resume: an existing committed prefix is
    // reopened without truncation and writing resumes exactly at EOF.  This is
    // the case that can otherwise retain a bad prefix or corrupt rematerialised
    // data without being noticed until rsync's final verification pass.
    auto resumed = pattern(6 * config.extent_size + 654321);
    const size_t prefix = 2 * config.extent_size + 77777;
    service.filesystem().create_file("/resumed.bin", 0644, getuid(), getgid());
    auto prefix_writer = service.filesystem().open_write("/resumed.bin", true);
    REQUIRE(prefix_writer->write(0, {resumed.data(), prefix}) == prefix);
    prefix_writer->commit();
    prefix_writer.reset();

    const auto prefix_entry = service.filesystem().getattr("/resumed.bin");
    REQUIRE(prefix_entry.extents.size() == 3);
    const auto first_full = prefix_entry.extents[0];
    const auto second_full = prefix_entry.extents[1];

    auto resumed_writer = service.filesystem().open_write("/resumed.bin", false);
    offset = prefix;
    while (offset < resumed.size()) {
        const auto n = std::min<size_t>(98317, resumed.size() - offset);
        REQUIRE(resumed_writer->write(offset, {resumed.data() + offset, n}) == n);
        offset += n;
    }
    resumed_writer->commit();
    const auto resume_diag = resumed_writer->diagnostics();
    CHECK(resume_diag.sequential);
    CHECK(!resume_diag.temp_open);
    CHECK(resume_diag.append_tail_fetches == 1);
    CHECK(resume_diag.materialize_source_reads == 0);
    CHECK(resume_diag.rebuild_reused_extents == 0);
    CHECK(resume_diag.rebuild_put_extents == 0);

    const auto resumed_entry = service.filesystem().getattr("/resumed.bin");
    REQUIRE(resumed_entry.extents.size() >= 2);
    CHECK(resumed_entry.extents[0] == first_full);
    CHECK(resumed_entry.extents[1] == second_full);
    CHECK(read_exact("/resumed.bin", resumed.size()) == resumed);

    // Extent-aligned resume is even cheaper: no old object is fetched at all.
    auto aligned = pattern(5 * config.extent_size + 333);
    service.filesystem().create_file("/aligned.bin", 0644, getuid(), getgid());
    auto aligned_prefix = service.filesystem().open_write("/aligned.bin", true);
    REQUIRE(aligned_prefix->write(0, {aligned.data(), 3 * config.extent_size}) ==
            3 * config.extent_size);
    aligned_prefix->commit();
    aligned_prefix.reset();
    const auto aligned_before = service.filesystem().getattr("/aligned.bin");
    REQUIRE(aligned_before.extents.size() == 3);

    auto aligned_writer = service.filesystem().open_write("/aligned.bin", false);
    offset = 3 * config.extent_size;
    while (offset < aligned.size()) {
        const auto n = std::min<size_t>(77777, aligned.size() - offset);
        REQUIRE(aligned_writer->write(offset, {aligned.data() + offset, n}) == n);
        offset += n;
    }
    aligned_writer->commit();
    const auto aligned_diag = aligned_writer->diagnostics();
    CHECK(aligned_diag.sequential);
    CHECK(!aligned_diag.temp_open);
    CHECK(aligned_diag.append_tail_fetches == 0);
    CHECK(aligned_diag.materialize_source_reads == 0);
    CHECK(aligned_diag.rebuild_put_extents == 0);
    const auto aligned_after = service.filesystem().getattr("/aligned.bin");
    REQUIRE(aligned_after.extents.size() >= aligned_before.extents.size());
    for (size_t i = 0; i < aligned_before.extents.size(); ++i)
        CHECK(aligned_after.extents[i] == aligned_before.extents[i]);
    CHECK(read_exact("/aligned.bin", aligned.size()) == aligned);

    // A FUSE flush does not close the handle.  Appending again after a commit
    // must lazily reopen only the newly committed partial tail, not materialise
    // the complete file.
    auto more = pattern(777);
    const auto aligned_old_size = aligned.size();
    aligned.insert(aligned.end(), more.begin(), more.end());
    REQUIRE(aligned_writer->write(aligned_old_size, more) == more.size());
    aligned_writer->commit();
    const auto aligned_again_diag = aligned_writer->diagnostics();
    CHECK(!aligned_again_diag.temp_open);
    CHECK(aligned_again_diag.append_tail_fetches == 1);
    CHECK(aligned_again_diag.materialize_source_reads == 0);
    CHECK(aligned_again_diag.rebuild_put_extents == 0);
    CHECK(read_exact("/aligned.bin", aligned.size()) == aligned);

    // Arbitrary overwrite still uses staging, but unchanged extents are reused
    // rather than re-put. Change one byte in a four-extent file and assert only
    // the touched extent is newly stored at rebuild time.
    auto random_write = pattern(4 * config.extent_size);
    service.filesystem().create_file("/random.bin", 0644, getuid(), getgid());
    auto random_seed = service.filesystem().open_write("/random.bin", true);
    REQUIRE(random_seed->write(0, random_write) == random_write.size());
    random_seed->commit();
    random_seed.reset();
    auto random_writer = service.filesystem().open_write("/random.bin", false);
    const uint64_t changed_offset = config.extent_size + 1234;
    const uint8_t changed = static_cast<uint8_t>(random_write[changed_offset] ^ 0x5a);
    random_write[changed_offset] = changed;
    REQUIRE(random_writer->write(changed_offset, {&changed, 1}) == 1);
    random_writer->commit();
    const auto random_diag = random_writer->diagnostics();
    CHECK(random_diag.temp_open);
    CHECK(random_diag.materialize_source_reads == 4);
    CHECK(random_diag.rebuild_reused_extents == 3);
    CHECK(random_diag.rebuild_put_extents == 1);
    CHECK(read_exact("/random.bin", random_write.size()) == random_write);
}

MACHA_TEST("filesystem_fuse", test_active_write_size_visibility) {
    TestService fixture("active-size");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;

    auto& service = fixture.start();

    service.filesystem().create_file("/.active.tmp", 0600, getuid(), getgid());
    CHECK(!service.filesystem().active_write_size("/.active.tmp").has_value());

    auto writer = service.filesystem().open_write("/.active.tmp", true);
    auto input = pattern(2 * 1024 * 1024 + 12345);
    REQUIRE(writer->write(0, input) == input.size());

    // Authoritative metadata remains uncommitted until close, but FUSE must be
    // able to project the live writer size to the kernel while the handle is open.
    CHECK(service.filesystem().getattr("/.active.tmp").size == 0);
    auto active = service.filesystem().active_write_size("/.active.tmp");
    REQUIRE(active.has_value());
    CHECK(*active == input.size());

    writer->truncate(65536);
    active = service.filesystem().active_write_size("/.active.tmp");
    REQUIRE(active.has_value());
    CHECK(*active == 65536);

    service.filesystem().rename("/.active.tmp", "/active.bin");
    CHECK(!service.filesystem().active_write_size("/.active.tmp").has_value());
    active = service.filesystem().active_write_size("/active.bin");
    REQUIRE(active.has_value());
    CHECK(*active == 65536);

    writer->commit();
    CHECK(service.filesystem().getattr("/active.bin").size == 65536);
    writer.reset();
    CHECK(!service.filesystem().active_write_size("/active.bin").has_value());
}

MACHA_TEST("filesystem_fuse", test_open_write_survives_rename) {
    TestService fixture("single-rename");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;

    auto& service = fixture.start();

    service.filesystem().create_file("/.upload.tmp", 0600, getuid(), getgid());
    auto writer = service.filesystem().open_write("/.upload.tmp", true);
    auto input = pattern(3 * config.extent_size + 12345);
    size_t offset = 0;
    while (offset < input.size()) {
        size_t n = std::min<size_t>(128 * 1024, input.size() - offset);
        REQUIRE(writer->write(offset, {input.data() + offset, n}) == n);
        offset += n;
    }

    // rsync writes a temporary file, renames it to the destination while the
    // descriptor is still open, then flushes/closes that same descriptor.
    service.filesystem().rename("/.upload.tmp", "/movie.mkv");
    writer->commit();

    bool old_missing = false;
    try {
        (void)service.filesystem().getattr("/.upload.tmp");
    } catch (const FsError& e) {
        old_missing = e.code() == ENOENT;
    }
    CHECK(old_missing);

    auto entry = service.filesystem().getattr("/movie.mkv");
    CHECK(entry.size == input.size());

    auto reader = service.filesystem().open_read("/movie.mkv");
    Bytes output(input.size());
    size_t got = 0;
    while (got < output.size()) {
        auto n = reader->read(got, {output.data() + got, output.size() - got});
        REQUIRE(n > 0);
        got += n;
    }
    CHECK(output == input);
}

MACHA_TEST("filesystem_fuse", test_local_snapshot_view_is_local_before_cluster_forms) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "genesis-n1", cluster.keyfile(), p1);
    auto c2 = config_for(cluster.path() / "genesis-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 2;

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    REQUIRE(s1.node().wait_local_state_ready(5s));

    // This is deliberately available before the configured metadata write
    // floor can form. It reflects only the local replica and must not enter the
    // authoritative MetadataManager path (which would throw MetadataNotReady
    // and perform discovery/history work).
    CHECK(!s1.metadata_manager().available_snapshot_view().has_value());
    const auto local_record = s1.node().metadata_replica().current();
    const auto first_local = s1.filesystem().local_snapshot_view();
    CHECK(first_local.generation == local_record.generation);
    CHECK(first_local.hash == local_record.hash);
    CHECK(first_local.snapshot->entries.contains("/"));
    const auto second_local = s1.filesystem().local_snapshot_view();
    CHECK(second_local.snapshot == first_local.snapshot);
    CHECK(!s1.metadata_manager().available_snapshot_view().has_value());

    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));

    REQUIRE(wait_until([&] {
        return s1.filesystem().local_snapshot_view().generation > local_record.generation;
    }));
    REQUIRE(wait_until([&] {
        return s2.filesystem().local_snapshot_view().generation > local_record.generation;
    }));

    auto f1 = std::make_shared<FuseFrontend>(s1.filesystem(), c1.fuse);
    auto f2 = std::make_shared<FuseFrontend>(s2.filesystem(), c2.fuse);
    CHECK(s1.node().metadata_replica().current().generation > 1);
    CHECK(s2.node().metadata_replica().current().generation > 1);

    f1->stop();
    f2->stop();
    f1.reset();
    f2.reset();
    s2.stop();
    s1.stop();
}

MACHA_TEST("filesystem_fuse", test_disconnected_maintenance_sleeps_until_peer_event) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "event-maint-n1", cluster.keyfile(), p1);
    auto c2 =
        config_for(cluster.path() / "event-maint-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_min_write_replicas = c2.metadata_min_write_replicas = 2;
    c1.heartbeat = c2.heartbeat = 50ms;
    c1.dead_after = c2.dead_after = 500ms;
    c1.maintenance.no_progress_backoff = c2.maintenance.no_progress_backoff = 30s;
    c1.catalogue.scanner.enabled = c2.catalogue.scanner.enabled = false;
    c1.catalogue.api.enabled = c2.catalogue.api.enabled = false;

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();
    REQUIRE(s1.node().wait_local_state_ready(5s));
    (void)s1.filesystem();

    // Allow the initial event and one bounded repair slice to settle. With no
    // peer and no new input, the scheduler must remain parked rather than
    // rediscovering the same unavailable write floor on an interval.
    REQUIRE(wait_until(
        [&] {
            const auto before = s1.maintenance_wakeups();
            std::this_thread::sleep_for(300ms);
            return s1.maintenance_wakeups() == before;
        },
        3s));
    const auto parked = s1.maintenance_wakeups();
    std::this_thread::sleep_for(1500ms);
    CHECK(s1.maintenance_wakeups() == parked);

    // A peer/membership event must bypass the outstanding retry deadline and
    // immediately form the metadata floor.
    s2.start();
    REQUIRE(wait_until(
        [&] {
            return s1.node().metadata_replica().current().generation > 1 &&
                   s2.node().metadata_replica().current().generation > 1;
        },
        5s));

    // Identical membership exchanges are heartbeats, not maintenance events.
    // Once the event-triggered convergence and quiet follow-up have completed,
    // rapid connected heartbeats must leave both maintenance workers parked.
    REQUIRE(wait_until(
        [&] {
            const auto before1 = s1.maintenance_wakeups();
            const auto before2 = s2.maintenance_wakeups();
            std::this_thread::sleep_for(300ms);
            return s1.maintenance_wakeups() == before1 && s2.maintenance_wakeups() == before2;
        },
        5s));
    const auto connected_parked1 = s1.maintenance_wakeups();
    const auto connected_parked2 = s2.maintenance_wakeups();
    std::this_thread::sleep_for(500ms);
    CHECK(s1.maintenance_wakeups() == connected_parked1);
    CHECK(s2.maintenance_wakeups() == connected_parked2);

    s2.stop();
    s1.stop();
}

MACHA_TEST("filesystem_fuse", test_coalesced_delete_burst_wakes_at_exact_garbage_grace) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("coalesced-garbage-grace");
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.maintenance.garbage_grace = 750ms;
    config.maintenance.foreground_quiet = 10ms;
    config.maintenance.no_progress_backoff = 500ms;

    TestGate repair_gate;
    std::atomic_bool gate_repair{};
    std::atomic_bool gate_once{};
    Service service(config, cluster.keys(), {}, [&](std::string_view stage) {
        if (stage == "metadata-repair-begin" && gate_repair.load(std::memory_order_acquire) &&
            !gate_once.exchange(true, std::memory_order_acq_rel)) {
            repair_gate.enter_and_wait();
        }
    });
    struct GateOpener {
        TestGate& gate;
        ~GateOpener() {
            gate.open();
        }
    } open_on_exit{repair_gate};

    service.start();
    auto& fs = service.filesystem();
    std::vector<ObjectId> retired_ids;
    for (size_t index = 0; index < 3; ++index) {
        const auto path = "/garbage-grace-" + std::to_string(index);
        write_file(fs, path, pattern(64 * 1024 + index, static_cast<uint8_t>(index + 7)));
        const auto entry = fs.getattr(path);
        REQUIRE(entry.extents.size() == 1);
        retired_ids.push_back(entry.extents.front().id);
        REQUIRE(service.node().local_store().has(retired_ids.back()));
    }
    REQUIRE(wait_until(
        [&] {
            const auto diagnostics = service.metadata_convergence_diagnostics();
            return !diagnostics.scheduled &&
                   diagnostics.runs_scheduled == diagnostics.runs_completed;
        },
        5s));

    const auto before = service.metadata_convergence_diagnostics();
    gate_repair.store(true, std::memory_order_release);
    fs.unlink("/garbage-grace-0");
    REQUIRE(repair_gate.wait_for_entries(1, 5s));
    fs.unlink("/garbage-grace-1");
    fs.unlink("/garbage-grace-2");

    auto snapshot = service.metadata_manager().snapshot();
    int64_t latest_retirement{};
    for (const auto& id : retired_ids) {
        auto found = std::find_if(snapshot.garbage.begin(), snapshot.garbage.end(),
                                  [&](const GarbageRef& garbage) { return garbage.id == id; });
        REQUIRE(found != snapshot.garbage.end());
        latest_retirement = std::max(latest_retirement, found->retired_at_ns);
        CHECK(service.node().local_store().has(id));
    }

    repair_gate.open();
    const auto grace_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config.maintenance.garbage_grace)
            .count();
    const auto before_deadline_ns = latest_retirement + grace_ns - wall_time_ns();
    if (before_deadline_ns > 100'000'000)
        std::this_thread::sleep_for(std::chrono::nanoseconds(before_deadline_ns - 50'000'000));
    CHECK(service.node().local_store().has(retired_ids.back()));
    const auto before_grace = service.metadata_convergence_diagnostics();
    CHECK(before_grace.runs_scheduled == before.runs_scheduled + 2);
    CHECK(before_grace.runs_completed == before.runs_completed + 2);

    REQUIRE(wait_until(
        [&] {
            return std::none_of(retired_ids.begin(), retired_ids.end(), [&](const ObjectId& id) {
                return service.node().local_store().has(id);
            });
        },
        5s));
    REQUIRE(wait_until(
        [&] {
            const auto current = service.metadata_manager().snapshot();
            return std::none_of(current.garbage.begin(), current.garbage.end(),
                                [&](const GarbageRef& garbage) {
                                    return std::find(retired_ids.begin(), retired_ids.end(),
                                                     garbage.id) != retired_ids.end();
                                });
        },
        5s));

    REQUIRE(wait_until(
        [&] {
            const auto diagnostics = service.metadata_convergence_diagnostics();
            return !diagnostics.scheduled &&
                   diagnostics.runs_scheduled == diagnostics.runs_completed &&
                   diagnostics.runs_completed >= before.runs_completed + 3;
        },
        5s));

    const auto after = service.metadata_convergence_diagnostics();
    CHECK(after.runs_scheduled >= before.runs_scheduled + 3);
    CHECK(after.runs_scheduled <= before.runs_scheduled + 5);
    CHECK(after.runs_completed == after.runs_scheduled);
    CHECK(!after.scheduled);
    service.stop();
}

MACHA_TEST("filesystem_fuse", test_fuse_frontend_ordering_merging_and_cache) {
    TestService fixture("fuse-ordering");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    config.cache.path = fixture.path() / "cache";
    config.cache.max_blocks = 64;
    config.fuse.commit_workers = 2;
    config.fuse.read_ahead_extents = 2;
    config.fuse.write_through_cache = true;

    auto& service = fixture.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        auto handle = frontend->create("/.rsync.tmp", 0600, getuid(), getgid(), true, true, false);
        const auto inode = handle.inode;
        REQUIRE(inode != 0);

        Bytes expected(3 * config.extent_size + 8192, 0);
        auto first = pattern(config.extent_size + 32768);
        REQUIRE(frontend->write(inode, 0, first) == first.size());
        std::copy(first.begin(), first.end(), expected.begin());

        // Adjacent and overlapping writes are retained in exact byte order but
        // expose one coalesced dirty range to the frontend scheduler.
        auto adjacent = pattern(config.extent_size);
        REQUIRE(frontend->write(inode, first.size(), adjacent) == adjacent.size());
        std::copy(adjacent.begin(), adjacent.end(),
                  expected.begin() + static_cast<ptrdiff_t>(first.size()));
        auto patch = pattern(131072);
        const uint64_t patch_offset = config.extent_size - 65536;
        for (auto& byte : patch)
            byte ^= 0xa5;
        REQUIRE(frontend->write(inode, patch_offset, patch) == patch.size());
        std::copy(patch.begin(), patch.end(),
                  expected.begin() + static_cast<ptrdiff_t>(patch_offset));

        auto ranges = frontend->dirty_ranges(inode);
        REQUIRE(ranges.size() == 1);
        CHECK(ranges.front().offset == 0);
        CHECK(ranges.front().length == first.size() + adjacent.size());

        // Queue publication more than once. It is legal for the first commit to
        // complete very quickly on a one-node test cluster, but pending work may
        // never be double-counted and no duplicate bytes may result.
        frontend->flush(inode);
        frontend->flush(inode);
        auto during = frontend->status();
        CHECK(during.pending_data <= 1);
        CHECK(during.active_data <= config.fuse.commit_workers);

        // Rename twice while retaining the same open file description, then
        // continue writing through that inode. No path lookup participates in
        // the subsequent write/close sequence.
        frontend->rename("/.rsync.tmp", "/.stage.tmp");
        REQUIRE(frontend->inode_for_path("/.stage.tmp") == inode);
        CHECK(frontend->path_for_inode(inode) == "/.stage.tmp");
        frontend->rename("/.stage.tmp", "/movie.bin");
        REQUIRE(frontend->inode_for_path("/movie.bin") == inode);
        CHECK(!frontend->inode_for_path("/.rsync.tmp").has_value());
        CHECK(!frontend->inode_for_path("/.stage.tmp").has_value());

        auto tail = pattern(8192);
        for (auto& byte : tail)
            byte ^= 0x3c;
        const uint64_t tail_offset = 3 * config.extent_size;
        REQUIRE(frontend->write(inode, tail_offset, tail) == tail.size());
        std::copy(tail.begin(), tail.end(), expected.begin() + static_cast<ptrdiff_t>(tail_offset));
        frontend->release(inode, true);
        REQUIRE(frontend->wait_for_idle(10s));

        // Write publication is interactive I/O, not playback. If the backend
        // writer marks its own replay chunks as foreground, the publication
        // quiet policy self-throttles by one full quiet interval per chunk.
        CHECK(service.filesystem().foreground_idle_for() >= 1h);
        CHECK(frontend->status().pending_data == 0);
        auto entry = service.filesystem().getattr("/movie.bin");
        CHECK(entry.size == expected.size());
        auto reader = service.filesystem().open_read("/movie.bin");
        Bytes actual(expected.size());
        size_t offset = 0;
        while (offset < actual.size()) {
            auto n = reader->read(offset, {actual.data() + offset, actual.size() - offset});
            REQUIRE(n > 0);
            offset += n;
        }
        CHECK(actual == expected);

        // FUSE write-back publication uses the ordinary extent writer and, when
        // requested, also promotes each immutable extent into Macha's existing
        // persistent block cache rather than maintaining a second FUSE cache.
        REQUIRE(!entry.extents.empty());
        for (const auto& extent : entry.extents)
            if (!extent.hole)
                CHECK(service.node().block_cache().has(extent.id));
    }
}

MACHA_TEST("filesystem_fuse", test_fuse_completed_publication_unlinks_retired_spool) {
    TestService fixture("fuse-spool-retire-unlink");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 0ms;

    auto& service = fixture.start();
    auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
    auto handle =
        frontend->create("/retire-spool.bin", 0600, getuid(), getgid(), true, true, false);
    const auto payload = pattern(2 * 1024 * 1024 + 17, 71);
    REQUIRE(frontend->write(handle.inode, 0, payload) == payload.size());
    frontend->release(handle.inode, true);
    REQUIRE(frontend->wait_for_idle(10s));

    const auto spool_dir = config.fuse.spool_path.value_or(config.state_path / "fuse-spool");
    const auto spool = spool_dir / ("inode-" + std::to_string(handle.inode) + ".spool");
    CHECK(!std::filesystem::exists(spool));
    frontend->stop();
}

MACHA_TEST("filesystem_fuse", test_fuse_spool_capacity_backpressures_until_publication) {
    TestService fixture("fuse-spool-backpressure");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 0ms;
    config.fuse.max_spool_bytes = 384 * 1024;
    config.fuse.spool_reserve_free = 0;

    auto& service = fixture.start();
    auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
    auto handle =
        frontend->create("/bounded-spool.bin", 0600, getuid(), getgid(), true, true, false);
    const auto first = pattern(128 * 1024, 41);
    REQUIRE(frontend->write(handle.inode, 0, first) == first.size());

    auto second_write = std::async(std::launch::async, [&] {
        const auto second = pattern(300 * 1024, 42);
        return frontend->write(handle.inode, first.size(), second);
    });
    // The second write cannot fit, but saturation is backpressure rather than
    // ENOSPC. Pressure starts publication of the already-durable prefix and the
    // writer wakes only after that progress creates capacity.
    CHECK(second_write.wait_for(10ms) == std::future_status::timeout);
    REQUIRE(second_write.wait_for(10s) == std::future_status::ready);
    CHECK(second_write.get() == 300 * 1024);

    const auto pressure = frontend->status();
    CHECK(pressure.spool_limit_bytes == config.fuse.max_spool_bytes);
    CHECK(pressure.spool_bytes <= pressure.spool_limit_bytes);
    CHECK(pressure.spool_throttle_waits >= 1);
    CHECK(pressure.spool_publish_rate_bytes_per_second > 0);
    CHECK(pressure.spool_publish_rate_window_bytes >= first.size());
    CHECK(pressure.spool_publish_rate_window_ms > 0);

    const auto spool_dir = config.fuse.spool_path.value_or(config.state_path / "fuse-spool");
    const auto spool = spool_dir / ("inode-" + std::to_string(handle.inode) + ".spool");
    REQUIRE(std::filesystem::exists(spool));
    CHECK(std::filesystem::file_size(spool) <= config.fuse.max_spool_bytes);
    frontend->release(handle.inode, true);
    REQUIRE(frontend->wait_for_idle(10s));
    frontend->stop();
}

MACHA_TEST("filesystem_fuse", test_fuse_spool_stalled_publisher_blocks_without_enospc) {
    TestService fixture("fuse-spool-stalled-backpressure");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 30s;
    config.fuse.max_spool_bytes = 384 * 1024;
    config.fuse.spool_reserve_free = 0;

    auto& service = fixture.start();
    auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
    auto handle = frontend->create("/stalled-spool.bin", 0600, getuid(), getgid(), true, true,
                                   false);
    frontend->note_viewer_activity();
    const auto first = pattern(256 * 1024, 51);
    REQUIRE(frontend->write(handle.inode, 0, first) == first.size());

    auto blocked = std::async(std::launch::async, [&] {
        const auto second = pattern(256 * 1024, 52);
        try {
            (void)frontend->write(handle.inode, first.size(), second);
            return 0;
        } catch (const FsError& error) {
            return error.code();
        }
    });
    CHECK(blocked.wait_for(150ms) == std::future_status::timeout);
    const auto pressure = frontend->status();
    CHECK(pressure.spool_bytes <= pressure.spool_limit_bytes);
    CHECK(pressure.spool_throttle_waits >= 1);

    // Shutdown is a real wake event for blocked admissions. A permanently
    // stalled publisher does not busy-poll and does not manufacture ENOSPC;
    // stopping the mount cancels the waiting request explicitly.
    frontend->stop();
    REQUIRE(blocked.wait_for(2s) == std::future_status::ready);
    CHECK(blocked.get() == EINTR);
}

MACHA_TEST("filesystem_fuse", test_fuse_operation_journal_admission_is_bounded_while_busy) {
    TestService fixture("fuse-journal-budget");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 30s;
    config.fuse.max_operation_journal_bytes = 12 * 1024;

    auto& service = fixture.start();
    auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);

    // Keep one durable DATA operation outstanding so unrelated namespace work
    // cannot take the normal "pending == 0" journal reset fast path.
    auto hold = frontend->create("/journal-hold.bin", 0600, getuid(), getgid(), true, true, false);
    const auto payload = pattern(64 * 1024, 91);
    REQUIRE(frontend->write(hold.inode, 0, payload) == payload.size());
    frontend->release(hold.inode, true); // local durability only; publication remains quiet

    const auto journal = config.fuse.operation_journal_path.value_or(
        config.state_path / "fuse-spool" / "operations.log");
    REQUIRE(std::filesystem::exists(journal));

    bool refused = false;
    for (size_t i = 0; i < 256 && !refused; ++i) {
        try {
            frontend->mkdir("/journal-budget-" + std::to_string(i), 0700, getuid(), getgid());
        } catch (const FsError& error) {
            if (error.code() == ENOSPC)
                refused = true;
            else
                throw;
        }
    }
    REQUIRE(refused);

    // Completion records for work admitted just before the ceiling are allowed
    // to drain beyond the admission threshold. Rejected *new* work must not keep
    // extending the WAL indefinitely.
    const auto bounded_size = std::filesystem::file_size(journal);
    for (size_t i = 0; i < 8; ++i) {
        bool rejected_again = false;
        try {
            frontend->mkdir("/journal-refused-" + std::to_string(i), 0700, getuid(), getgid());
        } catch (const FsError& error) {
            rejected_again = error.code() == ENOSPC;
        }
        CHECK(rejected_again);
    }
    CHECK(std::filesystem::file_size(journal) == bounded_size);
    frontend->stop();
}

MACHA_TEST("filesystem_fuse", test_fuse_orphan_quarantine_is_byte_bounded_on_recovery) {
    TestService fixture("fuse-orphan-budget");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.max_orphan_bytes = 1024;

    // Establish/version the service state before introducing deliberately
    // unreferenced recovery artifacts.  Pre-populating state_path before
    // Service startup correctly trips the legacy/unversioned-state guard.
    auto& service = fixture.start();
    const auto spool_dir = config.state_path / "fuse-spool";
    std::filesystem::create_directories(spool_dir);
    const auto older = spool_dir / "inode-900.spool.orphan.1";
    const auto newer = spool_dir / "inode-901.spool.orphan.2";
    {
        std::ofstream out(older, std::ios::binary | std::ios::trunc);
        out << std::string(800, 'a');
    }
    {
        std::ofstream out(newer, std::ios::binary | std::ios::trunc);
        out << std::string(800, 'b');
    }
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(older, now - 2h);
    std::filesystem::last_write_time(newer, now - 1h);

    auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);

    uint64_t orphan_bytes = 0;
    size_t orphan_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(spool_dir)) {
        if (!entry.is_regular_file() ||
            entry.path().filename().string().find(".orphan.") == std::string::npos)
            continue;
        orphan_bytes += entry.file_size();
        ++orphan_files;
    }
    CHECK(orphan_bytes <= config.fuse.max_orphan_bytes);
    CHECK(orphan_files == 1);
    CHECK(!std::filesystem::exists(older));
    CHECK(std::filesystem::exists(newer));
    frontend->stop();
}

MACHA_TEST("filesystem_fuse", test_fuse_publication_yields_to_playback) {
    TestService fixture("fuse-playback-yield");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 500ms;
    config.fuse.publication_quantum_bytes = config.extent_size;
    config.fuse.publication_inflight_bytes = config.extent_size;
    config.fuse.publication_pipeline_bytes = config.extent_size;

    auto& service = fixture.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        auto handle =
            frontend->create("/playback-yield.bin", 0600, getuid(), getgid(), true, true, false);
        const auto inode = handle.inode;
        REQUIRE(inode != 0);
        REQUIRE(frontend->wait_for_idle(5s));

        auto payload = pattern(8 * config.extent_size);
        REQUIRE(frontend->write(inode, 0, payload) == payload.size());
        frontend->release(inode, true);
        REQUIRE(wait_until([&] { return frontend->status().data_publication_yields >= 1; }, 5s));

        // This is the public hook used by the real FUSE adapter before open/read,
        // not a direct test-only mutation of DistributedStore's clock. The
        // already-running bounded quantum may finish, then the resumable cursor
        // must remain parked for the rest of the viewer quiet window.
        frontend->note_viewer_activity(1);
        REQUIRE(wait_until([&] { return frontend->status().active_data == 0; }, 2s));
        const auto paused_quanta = frontend->status().data_publication_quanta;
        std::this_thread::sleep_for(100ms);
        const auto during = frontend->status();
        CHECK(during.data_publication_quanta == paused_quanta);
        CHECK(during.pending_data + during.active_data >= 1);
        CHECK(service.filesystem().getattr("/playback-yield.bin").size == 0);

        REQUIRE(frontend->wait_for_idle(10s));
        CHECK(service.filesystem().getattr("/playback-yield.bin").size == payload.size());
    }
}

MACHA_TEST("filesystem_fuse", test_fuse_open_loaders_use_available_publication_workers) {
    TestService fixture("fuse-open-loader-concurrency");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.commit_workers = 4;
    // This legacy setting previously collapsed every continuously open loader
    // workload to one publisher. It must no longer classify writers as viewers.
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 500ms;

    auto& service = fixture.start();
    auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
    constexpr size_t files = 4;
    std::vector<FuseOpenHandle> handles;
    handles.reserve(files);
    for (size_t i = 0; i < files; ++i)
        handles.push_back(frontend->create("/loader-" + std::to_string(i) + ".bin", 0644,
                                           getuid(), getgid(), false, true, false));
    REQUIRE(frontend->wait_for_idle(10s));

    const auto payload = pattern(8 * config.extent_size, 37);
    for (const auto& handle : handles)
        REQUIRE(frontend->write(handle.inode, 0, payload) == payload.size());
    REQUIRE(wait_until([&] { return frontend->status().durability_writes == files; }, 10s));

    // Hold the real viewer gate while all four durable inodes are queued. This
    // makes the runnable set deterministic without adding a scheduler test hook.
    service.filesystem().store().foreground_activity(1);
    for (const auto& handle : handles) {
        frontend->flush(handle.inode);
        frontend->flush(handle.inode); // repeated demand must coalesce
    }
    REQUIRE(frontend->status().pending_data >= files);
    REQUIRE(frontend->wait_for_idle(30s));

    const auto status = frontend->status();
    CHECK(status.data_publications_started == files);
    CHECK(status.data_publications_completed == files);
    CHECK(status.data_publication_peak_active >= 2);
    CHECK(status.data_publication_peak_active <= config.fuse.commit_workers);
    CHECK(status.data_publication_coalesced_queued >= files);
    CHECK(status.data_publication_bytes_read == files * payload.size());
    CHECK(status.data_publication_bytes_committed == files * payload.size());
    CHECK(status.data_publication_bytes_confirmed == files * payload.size());

    for (const auto& handle : handles)
        frontend->release(handle.inode, true);
    frontend->stop();
}

MACHA_TEST("filesystem_fuse", test_fuse_closed_file_is_selected_ahead_of_open_loader) {
    TestService fixture("fuse-closed-file-priority");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 500ms;

    auto& service = fixture.start();
    auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
    auto open_large = frontend->create("/open-large.bin", 0644, getuid(), getgid(), false, true,
                                       false);
    auto closed_small = frontend->create("/closed-small.bin", 0644, getuid(), getgid(), false,
                                         true, false);
    REQUIRE(frontend->wait_for_idle(10s));

    const auto large = pattern(8 * config.extent_size, 51);
    const auto small = pattern(64 * 1024, 52);
    REQUIRE(frontend->write(open_large.inode, 0, large) == large.size());
    REQUIRE(frontend->write(closed_small.inode, 0, small) == small.size());
    REQUIRE(wait_until([&] { return frontend->status().durability_writes == 2; }, 10s));

    service.filesystem().store().foreground_activity(1);
    frontend->flush(open_large.inode); // queued first, but remains open
    frontend->release(closed_small.inode, true); // queued second and closed
    REQUIRE(frontend->wait_for_idle(30s));

    const auto status = frontend->status();
    CHECK(status.data_closed_priority_selections >= 1);
    CHECK(service.filesystem().getattr("/closed-small.bin").size == small.size());
    CHECK(service.filesystem().getattr("/open-large.bin").size == large.size());
    frontend->release(open_large.inode, true);
    frontend->stop();
}

MACHA_TEST("filesystem_fuse", test_fuse_publication_quanta_are_fair_and_byte_bounded) {
    TestService fixture("fuse-publication-quanta");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.commit_workers = 4;
    config.fuse.publication_quiet = 80ms;
    config.fuse.publication_quantum_bytes = config.extent_size;
    // Although four workers exist, only one logical quantum may be admitted.
    config.fuse.publication_inflight_bytes = config.fuse.publication_quantum_bytes;
    config.fuse.publication_pipeline_bytes = config.extent_size;

    auto& service = fixture.start();
    auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
    auto large_handle =
        frontend->create("/quantum-large.bin", 0644, getuid(), getgid(), false, true, false);
    auto small_handle =
        frontend->create("/quantum-small.bin", 0644, getuid(), getgid(), false, true, false);
    REQUIRE(frontend->wait_for_idle(10s));

    const auto large = pattern(8 * config.extent_size, 61);
    const auto small = pattern(64 * 1024, 62);
    REQUIRE(frontend->write(large_handle.inode, 0, large) == large.size());
    REQUIRE(frontend->write(small_handle.inode, 0, small) == small.size());
    REQUIRE(wait_until([&] { return frontend->status().durability_writes == 2; }, 10s));

    // Queue in this order behind the real viewer gate. The large generation is
    // selected first, but must return to the tail after one quantum; the small
    // generation can then become atomically visible before the large one.
    service.filesystem().store().foreground_activity(1);
    frontend->release(large_handle.inode, true);
    frontend->release(small_handle.inode, true);
    REQUIRE(frontend->status().pending_data >= 2);

    bool observed_small_first = false;
    REQUIRE(wait_until(
        [&] {
            const auto small_entry = service.filesystem().getattr("/quantum-small.bin");
            const auto large_entry = service.filesystem().getattr("/quantum-large.bin");
            if (small_entry.size == small.size() && large_entry.size == 0)
                observed_small_first = true;
            return observed_small_first;
        },
        10s));
    CHECK(observed_small_first);
    REQUIRE(frontend->wait_for_idle(30s));

    const auto status = frontend->status();
    CHECK(status.data_publications_started == 2);
    CHECK(status.data_publications_completed == 2);
    CHECK(status.data_publication_yields >= large.size() /
                                                  config.fuse.publication_quantum_bytes -
                                              1);
    CHECK(status.data_publication_quanta > status.data_publications_completed);
    CHECK(status.data_publication_peak_active == 1);
    CHECK(status.data_publication_peak_inflight_bytes ==
          config.fuse.publication_inflight_bytes);
    // Cursor preservation is important: yielding must not reread or restage a
    // prefix merely to provide fairness.
    CHECK(status.data_publication_bytes_read == large.size() + small.size());
    CHECK(service.filesystem().getattr("/quantum-large.bin").size == large.size());
    frontend->stop();
}

MACHA_HEAVY_TEST("filesystem_fuse", test_fuse_durable_journal_recovers_namespace_and_data) {
    TestService fixture("fuse-journal-recovery");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 30s;
    config.fuse.spool_path = fixture.path() / "external-fuse-spool";
    config.fuse.operation_journal_path =
        fixture.path() / "external-fuse-journal" / "operations.log";

    auto& service = fixture.start();
    uint64_t inode = 0;
    auto payload = pattern(384 * 1024 + 17);
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);

        // Hold asynchronous convergence behind a viewer-critical quiet window.
        // The namespace and bytes below are nevertheless successful FUSE
        // operations and therefore must be reconstructable from local state.
        service.filesystem().store().foreground_activity(1);
        frontend->mkdir("/TV", 0755, getuid(), getgid());
        frontend->mkdir("/TV/Buffy", 0755, getuid(), getgid());
        auto handle =
            frontend->create("/TV/Buffy/S07E01.mp4", 0644, getuid(), getgid(), true, true, false);
        inode = handle.inode;
        REQUIRE(frontend->write(inode, 0, payload) == payload.size());
        frontend->release(inode, true);

        CHECK(frontend->inode_for_path("/TV").has_value());
        CHECK(frontend->inode_for_path("/TV/Buffy/S07E01.mp4") == inode);
        auto before = frontend->getattr("/TV/Buffy/S07E01.mp4");
        CHECK(before.size == payload.size());

        // Simulate the frontend process boundary while distributed publication
        // is still blocked. stop() must not need to publish the accepted work.
        frontend->stop();
    }

    auto replay_config = config.fuse;
    replay_config.publication_quiet = 0ms;
    {
        auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), replay_config);
        REQUIRE(recovered->inode_for_path("/TV").has_value());
        REQUIRE(recovered->inode_for_path("/TV/Buffy").has_value());
        auto recovered_inode = recovered->inode_for_path("/TV/Buffy/S07E01.mp4");
        REQUIRE(recovered_inode.has_value());
        CHECK(recovered->getattr("/TV/Buffy/S07E01.mp4").size == payload.size());

        Bytes local(payload.size());
        REQUIRE(recovered->read(*recovered_inode, 0, local) == local.size());
        CHECK(local == payload);

        REQUIRE(recovered->wait_for_idle(15s));
        auto committed = service.filesystem().getattr("/TV/Buffy/S07E01.mp4");
        CHECK(committed.size == payload.size());
        auto reader = service.filesystem().open_read("/TV/Buffy/S07E01.mp4");
        Bytes actual(payload.size());
        size_t offset = 0;
        while (offset < actual.size()) {
            auto n = reader->read(offset, {actual.data() + offset, actual.size() - offset});
            REQUIRE(n > 0);
            offset += n;
        }
        CHECK(actual == payload);

        const auto journal = *config.fuse.operation_journal_path;
        REQUIRE(std::filesystem::exists(journal));
        CHECK(std::filesystem::file_size(journal) == 8);
        CHECK(std::filesystem::exists(*config.fuse.spool_path));
        CHECK(!std::filesystem::exists(config.state_path / "fuse-spool"));
    }
}

MACHA_HEAVY_TEST("filesystem_fuse", test_fuse_durable_journal_recovers_ordered_mutations) {
    TestService fixture("fuse-journal-ordering");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 30s;

    auto& service = fixture.start();

    const auto initial = pattern(192 * 1024 + 31);
    const auto tail = pattern(24 * 1024 + 7);
    Bytes expected(initial.begin(), initial.begin() + 64 * 1024);
    expected.resize(96 * 1024, 0);
    expected.insert(expected.end(), tail.begin(), tail.end());

    constexpr std::string_view old_path = "/TV/Buffy/S07E01.mp4";
    constexpr std::string_view new_dir = "/TV/Buffy The Vampire Slayer";
    constexpr std::string_view new_path = "/TV/Buffy The Vampire Slayer/S07E01.mp4";
    constexpr std::string_view removed_path = "/TV/Buffy The Vampire Slayer/S07E02.mp4";

    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);

        frontend->mkdir("/TV", 0755, getuid(), getgid());
        frontend->mkdir("/TV/Buffy", 0755, getuid(), getgid());
        auto first = frontend->create(old_path, 0644, getuid(), getgid(), true, true, false);
        REQUIRE(frontend->write(first.inode, 0, initial) == initial.size());
        frontend->truncate(first.inode, 64 * 1024);
        REQUIRE(frontend->write(first.inode, 96 * 1024, tail) == tail.size());
        frontend->release(first.inode, true);

        frontend->rename("/TV/Buffy", new_dir);
        auto removed = frontend->create(removed_path, 0644, getuid(), getgid(), true, true, false);
        auto removed_bytes = pattern(32 * 1024 + 3);
        REQUIRE(frontend->write(removed.inode, 0, removed_bytes) == removed_bytes.size());
        frontend->release(removed.inode, true);
        frontend->unlink(removed_path);

        // Root metadata uses inode 1 and therefore exercises recovery of the
        // one stable inode which is never allocated from next_inode.
        frontend->chmod("/", 0700);

        CHECK(!frontend->inode_for_path(old_path).has_value());
        REQUIRE(frontend->inode_for_path(new_path).has_value());
        CHECK(!frontend->inode_for_path(removed_path).has_value());
        CHECK(frontend->getattr(new_path).size == expected.size());
        CHECK(frontend->getattr("/").mode == 0700);
        frontend->stop();
    }

    // First recovery remains publication-blocked: these assertions are about
    // reconstruction from committed metadata plus the durable local journal,
    // not about work which happened to converge quickly in the background.
    {
        auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        CHECK(!recovered->inode_for_path(old_path).has_value());
        auto recovered_inode = recovered->inode_for_path(new_path);
        REQUIRE(recovered_inode.has_value());
        CHECK(!recovered->inode_for_path(removed_path).has_value());
        CHECK(recovered->getattr(new_path).size == expected.size());
        CHECK(recovered->getattr("/").mode == 0700);

        Bytes local(expected.size());
        REQUIRE(recovered->read(*recovered_inode, 0, local) == local.size());
        CHECK(local == expected);
        recovered->stop();
    }

    // A second restart removes the artificial quiet window and verifies that
    // the recovered operation order can converge to the ordinary filesystem.
    auto replay_config = config.fuse;
    replay_config.publication_quiet = 0ms;
    {
        auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), replay_config);
        REQUIRE(recovered->wait_for_idle(20s));

        bool old_missing = false;
        try {
            (void)service.filesystem().getattr(std::string(old_path));
        } catch (const FsError& e) {
            old_missing = e.code() == ENOENT;
        }
        CHECK(old_missing);

        bool removed_missing = false;
        try {
            (void)service.filesystem().getattr(std::string(removed_path));
        } catch (const FsError& e) {
            removed_missing = e.code() == ENOENT;
        }
        CHECK(removed_missing);

        auto committed = service.filesystem().getattr(std::string(new_path));
        CHECK(committed.size == expected.size());
        CHECK(service.filesystem().getattr("/").mode == 0700);

        auto reader = service.filesystem().open_read(std::string(new_path));
        Bytes actual(expected.size());
        size_t offset = 0;
        while (offset < actual.size()) {
            auto n = reader->read(offset, {actual.data() + offset, actual.size() - offset});
            REQUIRE(n > 0);
            offset += n;
        }
        CHECK(actual == expected);
    }
}

MACHA_TEST("filesystem_fuse", test_fuse_recovery_batches_namespace_publication_and_markers) {
    TestService fixture("fuse-namespace-publication-amplification");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 30s;

    auto& service = fixture.start();
    constexpr size_t operations = 8;

    // Admit an ordered durable namespace backlog while publication is held
    // behind the foreground quiet boundary, then cross a frontend restart so
    // the ordinary recovery path owns the entire backlog.
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);
        for (size_t i = 0; i < operations; ++i)
            frontend->mkdir("/pending-" + std::to_string(i), 0755, getuid(), getgid());
        CHECK(frontend->status().namespace_operations_admitted == operations);
        frontend->stop();
    }

    auto replay = config.fuse;
    replay.publication_quiet = 0ms;
    replay.namespace_batch_operations = 3;
    const auto generation_before = service.filesystem().local_committed_metadata_generation();
    auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), replay);
    REQUIRE(recovered->wait_for_idle(20s));
    const auto status = recovered->status();
    constexpr size_t expected_batches = (operations + 3 - 1) / 3;

    CHECK(status.namespace_operations_recovered == operations);
    CHECK(status.namespace_publication_attempts == expected_batches);
    CHECK(status.namespace_publication_batches == expected_batches);
    CHECK(status.namespace_operations_batched == operations);
    CHECK(status.namespace_operations_published == operations);
    CHECK(status.namespace_operations_confirmed == operations);
    CHECK(service.filesystem().local_committed_metadata_generation() ==
          generation_before + expected_batches);
    // Each publication durably groups all individual published markers, then
    // all individual done markers. The journal format remains replay-compatible.
    CHECK(status.journal_append_batches == expected_batches * 2);
    CHECK(status.journal_records_appended == operations * 2);
    CHECK(status.journal_durability_barriers == expected_batches * 2);
}

MACHA_TEST("filesystem_fuse", test_fuse_recovery_thousand_operations_have_bounded_publications) {
    TestService fixture("fuse-namespace-thousand-batch");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.publication_quiet = 30s;
    auto& service = fixture.start();
    constexpr size_t operations = 1000;
    constexpr size_t batch_limit = 256;

    std::vector<FilesystemNamespaceMutation> creates;
    creates.reserve(operations);
    for (size_t i = 0; i < operations; ++i) {
        FilesystemNamespaceMutation op;
        op.kind = FilesystemNamespaceMutation::Kind::create;
        op.from = "/bulk-" + std::to_string(i);
        op.mode = 0644;
        op.uid = getuid();
        op.gid = getgid();
        creates.push_back(std::move(op));
    }
    CHECK(service.filesystem().apply_namespace_batch(creates).applied == operations);

    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);
        for (size_t i = 0; i < operations; ++i)
            frontend->unlink("/bulk-" + std::to_string(i));
        CHECK(frontend->status().namespace_operations_admitted == operations);
        frontend->stop();
    }

    auto replay = config.fuse;
    replay.publication_quiet = 0ms;
    replay.namespace_batch_operations = batch_limit;
    const auto generation_before = service.filesystem().local_committed_metadata_generation();
    auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), replay);
    REQUIRE(recovered->wait_for_idle(30s));
    const auto status = recovered->status();
    constexpr size_t expected_batches = (operations + batch_limit - 1) / batch_limit;

    CHECK(status.namespace_operations_recovered == operations);
    CHECK(status.namespace_publication_attempts == expected_batches);
    CHECK(status.namespace_publication_batches == expected_batches);
    CHECK(status.namespace_operations_batched == operations);
    CHECK(status.namespace_operations_published == operations);
    CHECK(status.namespace_operations_confirmed == operations);
    CHECK(status.journal_append_batches == expected_batches * 2);
    CHECK(status.journal_records_appended == operations * 2);
    CHECK(status.journal_durability_barriers == expected_batches * 2);
    CHECK(service.filesystem().local_committed_metadata_generation() ==
          generation_before + expected_batches);
}

MACHA_TEST("filesystem_fuse", test_fuse_namespace_batch_encoded_size_limit_is_hard) {
    TestService fixture("fuse-namespace-byte-limit");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.publication_quiet = 30s;
    auto& service = fixture.start();
    constexpr size_t operations = 4;

    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);
        for (size_t i = 0; i < operations; ++i)
            frontend->mkdir("/byte-limited-" + std::to_string(i), 0755, getuid(), getgid());
        frontend->stop();
    }

    auto replay = config.fuse;
    replay.publication_quiet = 0ms;
    replay.namespace_batch_bytes = 1;
    auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), replay);
    REQUIRE(recovered->wait_for_idle(20s));
    const auto status = recovered->status();

    // One oversized operation is allowed to make progress, but no second
    // operation may join it once the encoded-byte bound is exceeded.
    CHECK(status.namespace_publication_batches == operations);
    CHECK(status.namespace_operations_batched == operations);
    CHECK(status.journal_append_batches == operations * 2);
}

MACHA_TEST("filesystem_fuse", test_fuse_recovery_batches_unlinks_then_parent_rmdir) {
    TestService fixture("fuse-namespace-delete-batch");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.publication_quiet = 30s;
    auto& service = fixture.start();

    service.filesystem().mkdir("/doomed", 0755, getuid(), getgid());
    service.filesystem().create_file("/doomed/one", 0644, getuid(), getgid());
    service.filesystem().create_file("/doomed/two", 0644, getuid(), getgid());

    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);
        frontend->unlink("/doomed/one");
        frontend->unlink("/doomed/two");
        frontend->rmdir("/doomed");
        frontend->stop();
    }

    auto replay = config.fuse;
    replay.publication_quiet = 0ms;
    const auto generation_before = service.filesystem().local_committed_metadata_generation();
    auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), replay);
    REQUIRE(recovered->wait_for_idle(20s));
    const auto status = recovered->status();

    CHECK(status.namespace_operations_recovered == 3);
    CHECK(status.namespace_publication_batches == 1);
    CHECK(status.namespace_operations_batched == 3);
    CHECK(status.namespace_operations_published == 3);
    CHECK(status.namespace_operations_confirmed == 3);
    CHECK(status.journal_append_batches == 2);
    CHECK(status.journal_records_appended == 6);
    CHECK(service.filesystem().local_committed_metadata_generation() == generation_before + 1);
    bool missing = false;
    try {
        (void)service.filesystem().getattr("/doomed");
    } catch (const FsError& error) {
        missing = error.code() == ENOENT;
    }
    CHECK(missing);
}

MACHA_TEST("filesystem_fuse", test_fuse_recovery_all_idempotent_batch_uses_no_generation) {
    TestService fixture("fuse-namespace-idempotent-batch");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.publication_quiet = 30s;
    auto& service = fixture.start();
    constexpr size_t operations = 4;

    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);
        for (size_t i = 0; i < operations; ++i)
            frontend->mkdir("/already-" + std::to_string(i), 0755, getuid(), getgid());
        frontend->stop();
    }
    for (size_t i = 0; i < operations; ++i)
        service.filesystem().mkdir("/already-" + std::to_string(i), 0755, getuid(), getgid());

    const auto generation_before = service.filesystem().local_committed_metadata_generation();
    auto replay = config.fuse;
    replay.publication_quiet = 0ms;
    auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), replay);
    REQUIRE(recovered->wait_for_idle(20s));
    const auto status = recovered->status();

    CHECK(status.namespace_operations_recovered == operations);
    CHECK(status.namespace_publication_attempts == 0);
    CHECK(status.namespace_publication_batches == 0);
    CHECK(status.namespace_operations_batched == 0);
    CHECK(status.namespace_operations_published == operations);
    CHECK(status.namespace_operations_confirmed == operations);
    CHECK(status.journal_append_batches == 2);
    CHECK(status.journal_records_appended == operations * 2);
    CHECK(service.filesystem().local_committed_metadata_generation() == generation_before);
}

MACHA_TEST("filesystem_fuse", test_fuse_recovery_commits_largest_valid_namespace_prefix) {
    TestService fixture("fuse-namespace-valid-prefix");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.publication_quiet = 30s;
    auto& service = fixture.start();

    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);
        frontend->mkdir("/prefix", 0755, getuid(), getgid());
        frontend->mkdir("/concurrent", 0755, getuid(), getgid());
        frontend->mkdir("/after", 0755, getuid(), getgid());
        frontend->stop();
    }

    // Make operation two already true in the backend. The first recovery
    // transaction must commit only operation one; operation three cannot pass
    // the semantic boundary and is committed by the following transaction.
    service.filesystem().mkdir("/concurrent", 0755, getuid(), getgid());
    const auto generation_before = service.filesystem().local_committed_metadata_generation();
    auto replay = config.fuse;
    replay.publication_quiet = 0ms;
    auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), replay);
    REQUIRE(recovered->wait_for_idle(20s));
    const auto status = recovered->status();

    CHECK(status.namespace_operations_recovered == 3);
    CHECK(status.namespace_publication_attempts == 2);
    CHECK(status.namespace_publication_batches == 2);
    CHECK(status.namespace_operations_batched == 2);
    CHECK(status.namespace_operations_published == 3);
    CHECK(status.namespace_operations_confirmed == 3);
    CHECK(status.journal_append_batches == 4);
    CHECK(status.journal_records_appended == 6);
    CHECK(service.filesystem().local_committed_metadata_generation() == generation_before + 2);
    CHECK(service.filesystem().getattr("/prefix").type == EntryType::directory);
    CHECK(service.filesystem().getattr("/concurrent").type == EntryType::directory);
    CHECK(service.filesystem().getattr("/after").type == EntryType::directory);
}

MACHA_TEST("filesystem_fuse", test_fuse_durable_journal_trims_torn_tail) {
    TestService fixture("fuse-journal-torn-tail");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 30s;

    auto& service = fixture.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);
        frontend->mkdir("/pending", 0755, getuid(), getgid());
        REQUIRE(frontend->inode_for_path("/pending").has_value());
        frontend->stop();
    }

    const auto journal = config.state_path / "fuse-spool" / "operations.log";
    const auto valid_size = std::filesystem::file_size(journal);
    REQUIRE(valid_size > 8);
    {
        std::ofstream out(journal, std::ios::binary | std::ios::app);
        REQUIRE(out.good());
        const char torn[] = {char(0), char(0), char(0)};
        out.write(torn, sizeof(torn));
        REQUIRE(out.good());
    }
    REQUIRE(std::filesystem::file_size(journal) == valid_size + 3);

    {
        auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        REQUIRE(recovered->inode_for_path("/pending").has_value());
        CHECK(std::filesystem::file_size(journal) == valid_size);
        recovered->stop();
    }
}

MACHA_TEST("filesystem_fuse", test_fuse_durable_journal_trims_checksum_invalid_complete_tail) {
    TestService fixture("fuse-journal-checksum-tail");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    // This test needs publication deferred long enough to leave a durable
    // namespace record in the journal; it does not test a 30-second quiet
    // policy. One second gives the same state with a bounded worst-case delay.
    config.fuse.publication_quiet = 1s;

    auto& service = fixture.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);
        frontend->mkdir("/pending-checksum", 0755, getuid(), getgid());
        frontend->stop();
    }

    const auto journal = config.state_path / "fuse-spool" / "operations.log";
    const auto valid_size = std::filesystem::file_size(journal);
    REQUIRE(valid_size > 8);
    {
        std::ofstream out(journal, std::ios::binary | std::ios::app);
        REQUIRE(out.good());
        std::array<char, 37> torn{};
        torn[3] = 1;
        torn[4] = static_cast<char>(0xff);
        out.write(torn.data(), static_cast<std::streamsize>(torn.size()));
        REQUIRE(out.good());
    }
    REQUIRE(std::filesystem::file_size(journal) == valid_size + 37);

    {
        auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        REQUIRE(recovered->inode_for_path("/pending-checksum").has_value());
        CHECK(std::filesystem::file_size(journal) == valid_size);
        recovered->stop();
    }
}

MACHA_FAST_TEST("filesystem_fuse", test_fuse_journal_frame_scanner_exhaustive_tail_model) {
    const Bytes header{'M', 'A', 'C', 'H', 'F', 'U', 'S', '1'};
    const Bytes first_payload{1, 2, 3, 4};
    const Bytes second_payload{5, 6, 7};
    const auto first = fuse_journal_frame(first_payload);
    const auto second = fuse_journal_frame(second_payload);

    Bytes prefix = header;
    prefix.insert(prefix.end(), first.begin(), first.end());
    const auto first_end = prefix.size();

    // Every possible crash boundary inside a valid next frame must retain only
    // the preceding durable prefix. This is the exhaustive state-space check;
    // the two integration tests above prove the disk loader applies the result.
    for (size_t cut = 1; cut < second.size(); ++cut) {
        auto bytes = prefix;
        bytes.insert(bytes.end(), second.begin(), second.begin() + static_cast<ptrdiff_t>(cut));
        size_t records = 0;
        const auto scan = scan_fuse_journal_frames(
            bytes, header.size(), [&](std::span<const uint8_t> payload, size_t) {
                ++records;
                CHECK(std::equal(payload.begin(), payload.end(), first_payload.begin(),
                                 first_payload.end()));
            });
        CHECK(records == 1);
        CHECK(scan.last_good == first_end);
        CHECK(scan.discarded_tail == cut);
    }

    // A crash may expose the full logical final frame with non-durable checksum
    // sectors. EOF checksum failure is recoverable and is trimmed as one unit.
    auto invalid_eof = second;
    invalid_eof.back() ^= 0x5a;
    {
        auto bytes = prefix;
        bytes.insert(bytes.end(), invalid_eof.begin(), invalid_eof.end());
        size_t records = 0;
        const auto scan = scan_fuse_journal_frames(
            bytes, header.size(), [&](std::span<const uint8_t>, size_t) { ++records; });
        CHECK(records == 1);
        CHECK(scan.last_good == first_end);
        CHECK(scan.discarded_tail == invalid_eof.size());
    }

    // The same checksum failure cannot be dismissed as a torn append when a
    // later complete frame exists; that is durable middle-of-journal corruption.
    {
        auto bytes = prefix;
        bytes.insert(bytes.end(), invalid_eof.begin(), invalid_eof.end());
        bytes.insert(bytes.end(), first.begin(), first.end());
        bool rejected = false;
        try {
            (void)scan_fuse_journal_frames(bytes, header.size(),
                                           [](std::span<const uint8_t>, size_t) {});
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        CHECK(rejected);
    }

    // Fully valid framing consumes both records and reports no tail loss.
    {
        auto bytes = prefix;
        bytes.insert(bytes.end(), second.begin(), second.end());
        std::vector<Bytes> records;
        const auto scan = scan_fuse_journal_frames(
            bytes, header.size(), [&](std::span<const uint8_t> payload, size_t) {
                records.emplace_back(payload.begin(), payload.end());
            });
        REQUIRE(records.size() == 2);
        CHECK(records[0] == first_payload);
        CHECK(records[1] == second_payload);
        CHECK(scan.last_good == bytes.size());
        CHECK(scan.discarded_tail == 0);
    }
}

MACHA_TEST("filesystem_fuse", test_fuse_durable_journal_preserves_unreferenced_spool) {
    TestService fixture("fuse-journal-orphan");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;

    auto& service = fixture.start();
    const auto spool_dir = config.state_path / "fuse-spool";
    std::filesystem::create_directories(spool_dir);
    {
        std::ofstream out(spool_dir / "inode-999.spool", std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out << "unattributed bytes";
        REQUIRE(out.good());
    }

    {
        auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        recovered->stop();
    }

    const auto original = spool_dir / "inode-999.spool";
    CHECK(!std::filesystem::exists(original));
    bool preserved = false;
    for (const auto& entry : std::filesystem::directory_iterator(spool_dir)) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with("inode-999.spool.orphan."))
            continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(bytes == "unattributed bytes");
        preserved = true;
    }
    CHECK(preserved);
}

#if defined(__linux__)
size_t linux_open_fd_count() {
    std::error_code ec;
    size_t count = 0;
    for (std::filesystem::directory_iterator it("/proc/self/fd", ec), end; !ec && it != end;
         it.increment(ec))
        ++count;
    REQUIRE(!ec);
    return count;
}
#endif

MACHA_TEST("filesystem_fuse", test_fuse_recovery_spool_descriptors_are_bounded) {
#if defined(__linux__)
    TestService fixture("fuse-recovery-fd-bound");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 30s;

    auto& service = fixture.start();
    constexpr size_t dirty_inodes = 16;
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);
        const auto before_dirty = linux_open_fd_count();
        for (size_t i = 0; i < dirty_inodes; ++i) {
            auto handle = frontend->create("/fd-" + std::to_string(i), 0644, getuid(), getgid(),
                                           true, true, false);
            const Bytes byte{static_cast<uint8_t>(i)};
            REQUIRE(frontend->write(handle.inode, 0, byte) == byte.size());
            frontend->release(handle.inode, true);
        }
        // Publication remains deliberately blocked, so every inode still has a
        // durable dirty spool. Those spools must not retain one live descriptor
        // each after their local durability batches have completed.
        const auto after_dirty = linux_open_fd_count();
        CHECK(after_dirty <= before_dirty + 8);
        frontend->stop();
    }

    const auto before_recovery = linux_open_fd_count();
    {
        auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        const auto after_recovery = linux_open_fd_count();
        CHECK(after_recovery <= before_recovery + 8);
        recovered->stop();
    }

    auto drain = config.fuse;
    drain.publication_quiet = 0ms;
    const auto before_drain = linux_open_fd_count();
    {
        auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), drain);
        REQUIRE(recovered->wait_for_idle(30s));
        const auto after_drain = linux_open_fd_count();
        CHECK(after_drain <= before_drain + 8);
        recovered->stop();
    }
#endif
}

MACHA_TEST("filesystem_fuse", test_fuse_idle_spool_descriptor_reopens_for_append) {
    TestService fixture("fuse-idle-spool-reopen");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 30s;

    auto& service = fixture.start();
    const auto first = pattern(64 * 1024 + 13, 17);
    const auto second = pattern(48 * 1024 + 7, 93);
    Bytes expected = first;
    expected.insert(expected.end(), second.begin(), second.end());

    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);
        auto created =
            frontend->create("/append-after-idle.bin", 0644, getuid(), getgid(), true, true, false);
        const auto inode = created.inode;
        REQUIRE(frontend->write(inode, 0, first) == first.size());
        frontend->release(inode, true);

#if defined(__linux__)
        // release() waits for local durability. The idle dirty inode may retain
        // its spool pathname and bytes, but not the write-time descriptor.
        const auto before_reopen = linux_open_fd_count();
#endif

        auto reopened = frontend->open("/append-after-idle.bin", true, true, true, false);
        REQUIRE(reopened.inode == inode);
        REQUIRE(frontend->write(inode, 0, second, true) == second.size());
        frontend->release(inode, true);

#if defined(__linux__)
        const auto after_reopen = linux_open_fd_count();
        CHECK(after_reopen <= before_reopen + 2);
#endif

        Bytes local(expected.size());
        REQUIRE(frontend->read(inode, 0, local) == local.size());
        CHECK(local == expected);
        frontend->stop();
    }

    auto drain = config.fuse;
    drain.publication_quiet = 0ms;
    {
        auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), drain);
        REQUIRE(recovered->wait_for_idle(15s));
        auto committed = service.filesystem().getattr("/append-after-idle.bin");
        CHECK(committed.size == expected.size());
        auto reader = service.filesystem().open_read("/append-after-idle.bin");
        Bytes actual(expected.size());
        size_t offset = 0;
        while (offset < actual.size()) {
            auto n = reader->read(offset, {actual.data() + offset, actual.size() - offset});
            REQUIRE(n > 0);
            offset += n;
        }
        CHECK(actual == expected);
        recovered->stop();
    }
}

MACHA_HEAVY_TEST("filesystem_fuse", test_fuse_recovered_loader_starts_without_new_fuse_activity) {
    TestService fixture("fuse-recovery-autostart");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.commit_workers = 4;
    config.fuse.recovery_commit_workers = 2;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 80ms;

    auto& service = fixture.start();

    // Create the paths through FUSE first and let their namespace operations
    // fully settle. The subsequent journal therefore contains inode descriptors
    // with historical namespace sequence numbers but no unpublished namespace
    // work -- the shape seen after a long-running copy is restarted.
    constexpr size_t files = 4;
    {
        // Use a deliberately long quiet window while staging the crash backlog.
        // A short wall-clock quiet window plus a helper refresh thread is
        // scheduler-sensitive under a parallel test run: if that helper misses
        // its timeslice for >publication_quiet, a publisher can legitimately
        // start before the fixture is stopped. The production behaviour is the
        // thing under test here, not host scheduler latency.
        auto staging_fuse = config.fuse;
        staging_fuse.publication_quiet = 5s;
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), staging_fuse);
        for (size_t i = 0; i < files; ++i) {
            auto created = frontend->create("/recover-autostart-" + std::to_string(i) + ".bin",
                                            0644, getuid(), getgid(), true, true, false);
            frontend->release(created.inode, true);
        }
        REQUIRE(frontend->wait_for_idle(10s));

        // Hold the viewer/foreground gate closed while constructing the durable
        // backlog. The five-second staging quiet window above is comfortably
        // larger than this bounded fixture, so a single activity sample is a
        // deterministic gate even under a heavily loaded test runner.
        auto payload = pattern(4 * config.extent_size);
        service.filesystem().store().foreground_activity(1);
        for (size_t i = 0; i < files; ++i) {
            auto handle = frontend->open("/recover-autostart-" + std::to_string(i) + ".bin", true,
                                         true, false, false);
            REQUIRE(frontend->write(handle.inode, 0, payload) == payload.size());
            frontend->release(handle.inode, true);
        }
        const auto staged = frontend->status();
        CHECK(staged.pending_data >= files);
        CHECK(staged.active_data == 0);
        frontend->stop();
    }

    // Let the real playback gate expire, then deliberately keep only the generic
    // read-ahead/interactive activity clock hot. Recovery must ignore that clock:
    // its own object writes use the same accounting and would otherwise throttle
    // themselves. Do not issue any FUSE request after the restarted frontend is
    // constructed.
    REQUIRE(wait_until(
        [&] { return service.filesystem().foreground_idle_for() >= config.fuse.publication_quiet; },
        2s));
    service.filesystem().store().interactive_activity(1);

    // Journal restoration is provenance, not a background scheduling class.
    // The four user-requested files may use loader capacity beyond the legacy
    // recovery budget without waiting for a new rsync/getattr to kick them.
    size_t max_recovery_active = 0;
    {
        auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        const auto deadline = Clock::now() + 3s;
        while (Clock::now() < deadline) {
            const auto status = recovered->status();
            max_recovery_active = std::max(max_recovery_active, status.active_recovery_data);
            if (max_recovery_active > config.fuse.recovery_commit_workers)
                break;
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(max_recovery_active > config.fuse.recovery_commit_workers);
        CHECK(max_recovery_active <= config.fuse.commit_workers);
        recovered->stop();
    }
}

MACHA_TEST("filesystem_fuse", test_fuse_recovered_loader_uses_loader_worker_bound) {
    TestService fixture("fuse-recovery-concurrency");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.commit_workers = 4;
    config.fuse.recovery_commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 30s;

    auto& service = fixture.start();
    constexpr size_t files = 4;
    auto payload = pattern(4 * config.extent_size);
    for (size_t i = 0; i < files; ++i)
        service.filesystem().create_file("/recover-" + std::to_string(i) + ".bin", 0644, getuid(),
                                         getgid());

    // Make the first frontend leave a real durable backlog rather than racing
    // the local object store while the test is constructing it.
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);
        for (size_t i = 0; i < files; ++i) {
            auto handle =
                frontend->open("/recover-" + std::to_string(i) + ".bin", true, true, false, false);
            REQUIRE(frontend->write(handle.inode, 0, payload) == payload.size());
            frontend->release(handle.inode, true);
        }
        CHECK(frontend->status().pending_data >= files);
        frontend->stop();
    }

    auto drain = config.fuse;
    drain.publication_quiet = 0ms;
    size_t max_recovery_active = 0;
    {
        auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), drain);
        const auto deadline = Clock::now() + 30s;
        while (Clock::now() < deadline) {
            auto status = recovered->status();
            max_recovery_active = std::max(max_recovery_active, status.active_recovery_data);
            CHECK(status.active_recovery_data <= drain.commit_workers);
            CHECK(status.pending_recovery_data <= status.pending_data);
            if (!status.pending_data && !status.active_data)
                break;
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(recovered->wait_for_idle(30s));
        recovered->stop();
    }

    // At least one recovered publisher must have been observed unless the
    // complete 16-MiB backlog drained between constructor return and the first
    // status sample. Either way, final content proves the recovery path ran.
    CHECK(max_recovery_active > drain.recovery_commit_workers);
    CHECK(max_recovery_active <= drain.commit_workers);
    for (size_t i = 0; i < files; ++i) {
        auto entry = service.filesystem().getattr("/recover-" + std::to_string(i) + ".bin");
        CHECK(entry.size == payload.size());
    }
}

void append_fuse_journal_test_records(const std::filesystem::path& journal,
                                      std::span<const Bytes> payloads) {
    Bytes bytes;
    for (const auto& payload : payloads) {
        auto frame = fuse_journal_frame(payload);
        bytes.insert(bytes.end(), frame.begin(), frame.end());
    }

    const int fd = ::open(journal.c_str(), O_WRONLY | O_APPEND);
    REQUIRE(fd >= 0);
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        REQUIRE(written > 0);
        offset += static_cast<size_t>(written);
    }
    REQUIRE(::fsync(fd) == 0);
    REQUIRE(::close(fd) == 0);
}

void append_fuse_journal_test_record(const std::filesystem::path& journal,
                                     std::span<const uint8_t> payload) {
    const std::array<Bytes, 1> records{Bytes(payload.begin(), payload.end())};
    append_fuse_journal_test_records(journal, records);
}

Bytes fuse_namespace_marker(uint8_t type, uint64_t sequence) {
    Writer payload;
    payload.u8(type);
    payload.u64(sequence);
    return payload.take();
}

std::vector<Bytes> fuse_namespace_markers(uint8_t type, uint64_t first, uint64_t last_exclusive) {
    std::vector<Bytes> records;
    records.reserve(static_cast<size_t>(last_exclusive - first));
    for (uint64_t sequence = first; sequence < last_exclusive; ++sequence)
        records.push_back(fuse_namespace_marker(type, sequence));
    return records;
}

std::vector<uint8_t> fuse_journal_record_types(const std::filesystem::path& journal) {
    std::ifstream in(journal, std::ios::binary);
    REQUIRE(in.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Bytes bytes(raw.begin(), raw.end());
    constexpr size_t header_size = 8; // "MACHFUS1"
    REQUIRE(bytes.size() >= header_size);
    std::vector<uint8_t> types;
    const auto scan =
        scan_fuse_journal_frames(bytes, header_size, [&](std::span<const uint8_t> payload, size_t) {
            REQUIRE(!payload.empty());
            types.push_back(payload.front());
        });
    REQUIRE(scan.discarded_tail == 0);
    return types;
}

MACHA_TEST("filesystem_fuse",
           test_fuse_namespace_recovery_survives_partial_published_marker_group) {
    TestService fixture("fuse-namespace-partial-published-group");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.publication_quiet = 30s;
    auto& service = fixture.start();
    constexpr uint64_t operations = 6;
    constexpr uint64_t published_prefix = 3;

    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);
        for (uint64_t i = 0; i < operations; ++i)
            frontend->mkdir("/published-crash-" + std::to_string(i), 0755, getuid(), getgid());
        frontend->stop();
    }

    std::vector<FilesystemNamespaceMutation> committed;
    committed.reserve(operations);
    for (uint64_t i = 0; i < operations; ++i) {
        FilesystemNamespaceMutation mutation;
        mutation.kind = FilesystemNamespaceMutation::Kind::mkdir;
        mutation.from = "/published-crash-" + std::to_string(i);
        mutation.mode = 0755;
        mutation.uid = getuid();
        mutation.gid = getgid();
        committed.push_back(std::move(mutation));
    }
    CHECK(service.filesystem().apply_namespace_batch(committed).applied == operations);
    const auto generation_after_commit = service.filesystem().local_committed_metadata_generation();

    const auto spool_dir = config.fuse.spool_path.value_or(config.state_path / "fuse-spool");
    const auto journal = config.fuse.operation_journal_path.value_or(spool_dir / "operations.log");
    // A crash can expose any prefix of a grouped append. Sequence zero is
    // reserved; this fresh journal's namespace operations are 1..operations.
    const auto published = fuse_namespace_markers(4, 1, 1 + published_prefix);
    append_fuse_journal_test_records(journal, published);

    auto replay = config.fuse;
    replay.publication_quiet = 0ms;
    auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), replay);
    REQUIRE(recovered->wait_for_idle(20s));
    const auto status = recovered->status();

    CHECK(status.namespace_operations_recovered == operations - published_prefix);
    CHECK(status.namespace_publication_attempts == 0);
    CHECK(status.namespace_operations_published == operations - published_prefix);
    CHECK(status.namespace_operations_confirmed == operations - published_prefix);
    CHECK(service.filesystem().local_committed_metadata_generation() == generation_after_commit);
    CHECK(std::filesystem::file_size(journal) == 8);
    for (uint64_t i = 0; i < operations; ++i)
        CHECK(service.filesystem().getattr("/published-crash-" + std::to_string(i)).type ==
              EntryType::directory);
}

MACHA_TEST("filesystem_fuse", test_fuse_namespace_recovery_survives_partial_done_marker_group) {
    TestService fixture("fuse-namespace-partial-done-group");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.publication_quiet = 30s;
    auto& service = fixture.start();
    constexpr uint64_t operations = 6;
    constexpr uint64_t done_prefix = 2;

    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);
        for (uint64_t i = 0; i < operations; ++i)
            frontend->mkdir("/done-crash-" + std::to_string(i), 0755, getuid(), getgid());
        frontend->stop();
    }

    std::vector<FilesystemNamespaceMutation> committed;
    committed.reserve(operations);
    for (uint64_t i = 0; i < operations; ++i) {
        FilesystemNamespaceMutation mutation;
        mutation.kind = FilesystemNamespaceMutation::Kind::mkdir;
        mutation.from = "/done-crash-" + std::to_string(i);
        mutation.mode = 0755;
        mutation.uid = getuid();
        mutation.gid = getgid();
        committed.push_back(std::move(mutation));
    }
    CHECK(service.filesystem().apply_namespace_batch(committed).applied == operations);
    const auto generation_after_commit = service.filesystem().local_committed_metadata_generation();

    const auto spool_dir = config.fuse.spool_path.value_or(config.state_path / "fuse-spool");
    const auto journal = config.fuse.operation_journal_path.value_or(spool_dir / "operations.log");
    const auto published = fuse_namespace_markers(4, 1, 1 + operations);
    append_fuse_journal_test_records(journal, published);
    const auto done = fuse_namespace_markers(5, 1, 1 + done_prefix);
    append_fuse_journal_test_records(journal, done);

    auto replay = config.fuse;
    replay.publication_quiet = 0ms;
    auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), replay);
    REQUIRE(recovered->wait_for_idle(20s));
    const auto status = recovered->status();

    // Constructor reconciliation confirms the surviving published suffix and
    // retires it in one done-marker append; no publication worker is needed.
    CHECK(status.namespace_operations_recovered == 0);
    CHECK(status.namespace_publication_attempts == 0);
    CHECK(status.namespace_operations_published == 0);
    CHECK(status.namespace_operations_confirmed == 0);
    CHECK(status.journal_append_batches == 1);
    CHECK(status.journal_records_appended == operations - done_prefix);
    CHECK(service.filesystem().local_committed_metadata_generation() == generation_after_commit);
    CHECK(std::filesystem::file_size(journal) == 8);
    for (uint64_t i = 0; i < operations; ++i)
        CHECK(service.filesystem().getattr("/done-crash-" + std::to_string(i)).type ==
              EntryType::directory);
}

MACHA_TEST("filesystem_fuse", test_fuse_live_admission_during_recovery_publication_is_not_blocked) {
    TestGate publication_gate;
    TestNode fixture("fuse-live-admission-during-recovery");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.publication_quiet = 30s;
    fixture.start();
    // TestNode deliberately omits Service's background metadata owner. A
    // synchronous seed mutation forms the one-node replica set before the
    // generation baseline below, removing that unrelated startup race.
    fixture.filesystem().mkdir("/fixture-ready", 0755, getuid(), getgid());
    constexpr size_t recovered_operations = 4;

    {
        auto frontend = std::make_shared<FuseFrontend>(fixture.filesystem(), config.fuse);
        fixture.store().foreground_activity(1);
        for (size_t i = 0; i < recovered_operations; ++i)
            frontend->mkdir("/recovery-live-" + std::to_string(i), 0755, getuid(), getgid());
        frontend->stop();
    }

    std::atomic_bool gate_once{};
    fixture.metadata().set_publication_retention([&](const MetadataPublicationContext&) {
        if (!gate_once.exchange(true))
            publication_gate.enter_and_wait();
    });

    auto replay = config.fuse;
    replay.publication_quiet = 0ms;
    replay.namespace_batch_operations = recovered_operations;
    const auto generation_before = fixture.filesystem().local_committed_metadata_generation();
    auto recovered = std::make_shared<FuseFrontend>(fixture.filesystem(), replay);
    const bool publication_entered = publication_gate.wait_for_entries(1, 5s);
    CHECK(publication_entered);

    bool live_admitted = false;
    if (publication_entered) {
        try {
            recovered->mkdir("/live-during-recovery", 0755, getuid(), getgid());
            live_admitted = recovered->inode_for_path("/live-during-recovery").has_value();
        } catch (...) {
            publication_gate.open();
            throw;
        }
    }
    publication_gate.open();
    REQUIRE(publication_entered);
    CHECK(live_admitted);
    REQUIRE(recovered->wait_for_idle(20s));
    const auto status = recovered->status();

    CHECK(status.namespace_operations_recovered == recovered_operations);
    CHECK(status.namespace_operations_admitted == 1);
    CHECK(status.namespace_publication_batches == 2);
    CHECK(status.namespace_operations_batched == recovered_operations + 1);
    CHECK(status.namespace_operations_published == recovered_operations + 1);
    CHECK(status.namespace_operations_confirmed == recovered_operations + 1);
    CHECK(fixture.filesystem().local_committed_metadata_generation() == generation_before + 2);
    for (size_t i = 0; i < recovered_operations; ++i)
        CHECK(fixture.filesystem().getattr("/recovery-live-" + std::to_string(i)).type ==
              EntryType::directory);
    CHECK(fixture.filesystem().getattr("/live-during-recovery").type == EntryType::directory);
}

MACHA_TEST("filesystem_fuse",
           test_fuse_durable_journal_accepts_authoritative_data_done_without_published_prefix) {
    TestService fixture("fuse-journal-done-recovery");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 1s;

    auto& service = fixture.start();

    uint64_t inode = 0;
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        auto handle = frontend->create("/done-authoritative.bin", 0644, getuid(), getgid(), true,
                                       true, false);
        inode = handle.inode;
        const Bytes payload{0x10, 0x20, 0x30, 0x40};
        REQUIRE(frontend->write(inode, 0, payload) == payload.size());
        // Start the real publication quiet window immediately before close.
        // release() still waits only for local spool+journal durability, leaving
        // the distributed publication marker absent without paying a 30-second
        // test delay.
        service.filesystem().store().foreground_activity(1);
        frontend->release(inode, true);
        frontend->stop();
    }

    const auto spool_dir = config.fuse.spool_path.value_or(config.state_path / "fuse-spool");
    const auto journal = config.fuse.operation_journal_path.value_or(spool_dir / "operations.log");

    // Prove the setup actually produced the intended pre-publication state;
    // this test must not pass merely because publication raced the quiet gate.
    const auto record_types = fuse_journal_record_types(journal);
    CHECK(std::find(record_types.begin(), record_types.end(), 3) != record_types.end());
    CHECK(std::find(record_types.begin(), record_types.end(), 6) == record_types.end());
    CHECK(std::find(record_types.begin(), record_types.end(), 7) == record_types.end());

    // Model the precise crash state seen in production: the checksum-valid
    // completion marker survives but its earlier data_published proof does not.
    // A newly created inode's first data operation has sequence 1.
    Writer done;
    done.u8(7); // persisted JournalRecord::data_done value
    done.u64(inode);
    done.u64(1);
    append_fuse_journal_test_record(journal, done.data());

    {
        auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        CHECK(recovered->inode_for_path("/done-authoritative.bin").has_value());
        recovered->stop();
    }
}

MACHA_TEST("filesystem_fuse", test_fuse_durable_journal_rejects_unbacked_data_done) {
    TestService fixture("fuse-journal-unbacked-done");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;

    auto& service = fixture.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        frontend->stop();
    }

    const auto spool_dir = config.fuse.spool_path.value_or(config.state_path / "fuse-spool");
    const auto journal = config.fuse.operation_journal_path.value_or(spool_dir / "operations.log");

    Writer done;
    done.u8(7); // persisted JournalRecord::data_done value
    done.u64(999);
    done.u64(1);
    append_fuse_journal_test_record(journal, done.data());

    bool rejected = false;
    try {
        auto should_fail = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        should_fail->stop();
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
}

MACHA_TEST("filesystem_fuse", test_fuse_durable_journal_drops_only_inode_with_missing_spool) {
    TestService fixture("fuse-journal-missing-spool");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 30s;

    auto& service = fixture.start();
    service.filesystem().create_file("/recover.bin", 0600, getuid(), getgid());
    uint64_t inode = 0;
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        auto handle = frontend->open("/recover.bin", true, true, false, false);
        inode = handle.inode;
        auto payload = pattern(128 * 1024);
        REQUIRE(frontend->write(inode, 0, payload) == payload.size());
        service.filesystem().store().foreground_activity(1);
        frontend->release(inode, true);
        frontend->stop();
    }

    const auto spool =
        config.state_path / "fuse-spool" / ("inode-" + std::to_string(inode) + ".spool");
    REQUIRE(std::filesystem::exists(spool));
    REQUIRE(std::filesystem::remove(spool));

    // The spool/journal is a per-inode WAL. Losing one dirty spool invalidates
    // that generation, not the complete filesystem. Recovery journals the
    // abandonment and falls back to the last committed manifest (empty here).
    config.fuse.publication_quiet = 0ms;
    auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
    REQUIRE(recovered->wait_for_idle(10s));
    CHECK(service.filesystem().getattr("/recover.bin").size == 0);
    recovered->stop();
}

MACHA_TEST("filesystem_fuse",
           test_fuse_recovery_checksum_drops_corrupt_generation_and_preserves_published_file) {
    TestService fixture("fuse-journal-corrupt-spool");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 30s;

    auto& service = fixture.start();
    const auto published = pattern(768 * 1024, 61);
    write_file(service.filesystem(), "/recover-corrupt.bin", published);

    uint64_t inode = 0;
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        auto handle = frontend->open("/recover-corrupt.bin", true, true, false, false);
        inode = handle.inode;
        const auto replacement = pattern(published.size(), 62);
        REQUIRE(frontend->write(inode, 0, replacement) == replacement.size());
        service.filesystem().store().foreground_activity(1);
        frontend->release(inode, true);
        frontend->stop();
    }

    const auto spool =
        config.state_path / "fuse-spool" / ("inode-" + std::to_string(inode) + ".spool");
    REQUIRE(std::filesystem::exists(spool));
    {
        std::fstream file(spool, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(file.good());
        char byte{};
        file.read(&byte, 1);
        REQUIRE(file.good());
        byte ^= 0x5a;
        file.seekp(0);
        file.write(&byte, 1);
        file.flush();
        REQUIRE(file.good());
    }

    config.fuse.publication_quiet = 0ms;
    auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
    REQUIRE(recovered->wait_for_idle(10s));

    const auto committed = service.filesystem().getattr("/recover-corrupt.bin");
    CHECK(committed.size == published.size());
    auto reader = service.filesystem().open_read("/recover-corrupt.bin");
    Bytes actual(published.size());
    size_t done = 0;
    while (done < actual.size()) {
        const auto count = reader->read(done, {actual.data() + done, actual.size() - done});
        REQUIRE(count > 0);
        done += count;
    }
    CHECK(actual == published);
    if (std::filesystem::exists(spool))
        CHECK(std::filesystem::file_size(spool) == 0);
    recovered->stop();
}

MACHA_TEST("filesystem_fuse", test_fuse_read_only_release_does_not_publish_writer_data) {
    TestService fixture("fuse-read-release");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.commit_workers = 1;

    auto& service = fixture.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        auto writer = frontend->create("/growing.bin", 0600, getuid(), getgid(), true, true, false);
        auto bytes = pattern(256 * 1024);
        REQUIRE(frontend->write(writer.inode, 0, bytes) == bytes.size());

        // A second process such as rsync --append-verify may open the file for
        // basis reads while the writer still has dirty local data. Closing that
        // reader must not turn into an implicit writer flush/publication.
        auto reader = frontend->open("/growing.bin", true, false, false, false);
        CHECK(reader.inode == writer.inode);
        frontend->release(reader.inode, false);
        REQUIRE(frontend->wait_for_idle(2s));

        auto backend_before_writer_close = service.filesystem().getattr("/growing.bin");
        CHECK(backend_before_writer_close.size == 0);
        REQUIRE(frontend->dirty_ranges(writer.inode).size() == 1);

        frontend->release(writer.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));
        auto backend_after_writer_close = service.filesystem().getattr("/growing.bin");
        CHECK(backend_after_writer_close.size == bytes.size());

        auto stored = service.filesystem().open_read("/growing.bin");
        Bytes actual(bytes.size());
        size_t offset = 0;
        while (offset < actual.size()) {
            auto n = stored->read(offset, {actual.data() + offset, actual.size() - offset});
            REQUIRE(n > 0);
            offset += n;
        }
        CHECK(actual == bytes);
    }
}

MACHA_TEST("filesystem_fuse", test_fuse_frontend_unlink_and_rename_over_open_inode_ordering) {
    TestService fixture("fuse-replace");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.commit_workers = 2;

    auto& service = fixture.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);

        // A dirty inode that is unlinked before release must never recreate its
        // old pathname when the data-publication worker eventually sees it.
        auto doomed = frontend->create("/doomed.bin", 0600, getuid(), getgid(), true, true, false);
        auto doomed_data = pattern(65536);
        REQUIRE(frontend->write(doomed.inode, 0, doomed_data) == doomed_data.size());
        frontend->unlink("/doomed.bin");
        frontend->release(doomed.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));
        CHECK(!frontend->inode_for_path("/doomed.bin").has_value());
        bool missing = false;
        try {
            (void)service.filesystem().getattr("/doomed.bin");
        } catch (const FsError& e) {
            missing = e.code() == ENOENT;
        }
        CHECK(missing);

        // More subtle: POSIX rename may replace a destination which still has
        // an open descriptor. The displaced inode remains a valid open identity,
        // but it no longer owns that pathname. Releasing dirty data through the
        // old descriptor must not overwrite/resurrect the new destination.
        auto destination =
            frontend->create("/target.bin", 0600, getuid(), getgid(), true, true, false);
        auto old_bytes = pattern(32768);
        REQUIRE(frontend->write(destination.inode, 0, old_bytes) == old_bytes.size());
        frontend->release(destination.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));

        auto old_open = frontend->open("/target.bin", true, true, false, false);
        std::array<uint8_t, 8> stale{{'S', 'T', 'A', 'L', 'E', '!', '!', '!'}};
        REQUIRE(frontend->write(old_open.inode, 0, stale) == stale.size());

        auto source =
            frontend->create("/replacement.bin", 0600, getuid(), getgid(), true, true, false);
        auto replacement = pattern(98304);
        for (auto& byte : replacement)
            byte ^= 0x7d;
        REQUIRE(frontend->write(source.inode, 0, replacement) == replacement.size());
        frontend->flush(source.inode);
        frontend->rename("/replacement.bin", "/target.bin");
        REQUIRE(frontend->inode_for_path("/target.bin") == source.inode);
        frontend->release(source.inode, true);
        frontend->release(old_open.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));

        CHECK(frontend->path_for_inode(old_open.inode).empty());
        auto final = service.filesystem().getattr("/target.bin");
        CHECK(final.size == replacement.size());
        auto reader = service.filesystem().open_read("/target.bin");
        Bytes actual(replacement.size());
        size_t offset = 0;
        while (offset < actual.size()) {
            auto n = reader->read(offset, {actual.data() + offset, actual.size() - offset});
            REQUIRE(n > 0);
            offset += n;
        }
        CHECK(actual == replacement);
    }
}

MACHA_TEST("filesystem_fuse", test_fuse_frontend_read_overlay_truncate_and_hydration_hints) {
    TestService fixture("fuse-read");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.read_ahead_extents = 2;
    config.fuse.hydration_priority = 2718;

    auto& service = fixture.start();
    auto committed = pattern(4 * config.extent_size + 4096);
    service.filesystem().create_file("/read.bin", 0644, getuid(), getgid());
    auto seed = service.filesystem().open_write("/read.bin", true);
    REQUIRE(seed->write(0, committed) == committed.size());
    seed->commit();
    seed.reset();
    auto base_entry = service.filesystem().getattr("/read.bin");
    REQUIRE(base_entry.extents.size() >= 5);

    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        auto handle = frontend->open("/read.bin", true, true, false, false);

        auto patch = pattern(16384);
        for (auto& byte : patch)
            byte ^= 0x91;
        const uint64_t patch_offset = 4096;
        REQUIRE(frontend->write(handle.inode, patch_offset, patch) == patch.size());
        Bytes view(32768);
        REQUIRE(frontend->read(handle.inode, 0, view) == view.size());
        auto expected_view =
            Bytes(committed.begin(), committed.begin() + static_cast<ptrdiff_t>(view.size()));
        std::copy(patch.begin(), patch.end(),
                  expected_view.begin() + static_cast<ptrdiff_t>(patch_offset));
        CHECK(view == expected_view);

        // A committed-range read emits one high-priority FUSE run into the
        // ordinary hydration scheduler. Re-reading the same range replaces the
        // inode's hint rather than duplicating work, and object IDs are unique.
        // This must be tested while the committed extent is still inside the
        // inode's logical EOF.
        Bytes demand(4096);
        REQUIRE(frontend->read(handle.inode, config.extent_size + 1024, demand) == demand.size());
        REQUIRE(frontend->read(handle.inode, config.extent_size + 1024, demand) == demand.size());
        auto hints = frontend->hints();
        REQUIRE(hints.size() == 1);
        CHECK(hints.front().run_id == "fuse:" + std::to_string(handle.inode));
        CHECK(hints.front().priority == config.fuse.hydration_priority);
        CHECK(hints.front().frame_type == FrameType::read_ahead);
        REQUIRE(hints.front().objects.size() == 3);
        CHECK(hints.front().objects[0] == base_entry.extents[1].id);
        CHECK(hints.front().objects[1] == base_entry.extents[2].id);
        CHECK(hints.front().objects[2] == base_entry.extents[3].id);
        std::set<ObjectId> unique(hints.front().objects.begin(), hints.front().objects.end());
        CHECK(unique.size() == hints.front().objects.size());

        // Shrink then extend before publication. Bytes from the old committed
        // suffix must not reappear; the extended region is logically zero until
        // a later write overlays it.
        frontend->truncate(handle.inode, 32768);
        frontend->truncate(handle.inode, 65536);
        Bytes extended(32768, 0xff);
        REQUIRE(frontend->read(handle.inode, 32768, extended) == extended.size());
        CHECK(std::all_of(extended.begin(), extended.end(), [](uint8_t b) { return b == 0; }));
        Bytes beyond_eof(4096, 0xff);
        CHECK(frontend->read(handle.inode, config.extent_size + 1024, beyond_eof) == 0);
        std::array<uint8_t, 6> marker{{'M', 'A', 'C', 'H', 'A', '!'}};
        REQUIRE(frontend->write(handle.inode, 40000, marker) == marker.size());
        Bytes marker_view(64, 0xff);
        REQUIRE(frontend->read(handle.inode, 39984, marker_view) == marker_view.size());
        CHECK(std::equal(marker.begin(), marker.end(), marker_view.begin() + 16));

        frontend->release(handle.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));
        auto final = service.filesystem().getattr("/read.bin");
        CHECK(final.size == 65536);
        auto reader = service.filesystem().open_read("/read.bin");
        Bytes final_bytes(65536);
        size_t offset = 0;
        while (offset < final_bytes.size()) {
            auto n =
                reader->read(offset, {final_bytes.data() + offset, final_bytes.size() - offset});
            REQUIRE(n > 0);
            offset += n;
        }
        CHECK(std::equal(patch.begin(), patch.end(),
                         final_bytes.begin() + static_cast<ptrdiff_t>(patch_offset)));
        CHECK(std::equal(marker.begin(), marker.end(), final_bytes.begin() + 40000));
        CHECK(std::all_of(final_bytes.begin() + 32768, final_bytes.begin() + 40000,
                          [](uint8_t b) { return b == 0; }));
    }
}

MACHA_TEST("filesystem_fuse", test_fuse_buffered_writes_batch_until_close_durability) {
    TestService fixture("fuse-group-commit");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.publication_quiet = 30s;
    config.fuse.request_workers = 24;

    auto& service = fixture.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        auto handle = frontend->create("/batch.bin", 0644, getuid(), getgid(), true, true, false);

        // Sequential callbacks are the important case: write() no longer waits
        // for each local fsync pair, so one ordinary writer can build a batch
        // before close/release establishes the local durability boundary.
        constexpr size_t writes = 128;
        constexpr size_t chunk_size = 4096;
        std::vector<Bytes> chunks;
        chunks.reserve(writes);
        for (size_t i = 0; i < writes; ++i) {
            auto chunk = pattern(chunk_size);
            for (auto& byte : chunk)
                byte ^= static_cast<uint8_t>(i * 17U + 3U);
            chunks.push_back(std::move(chunk));
            REQUIRE(frontend->write(handle.inode, i * chunk_size, chunks.back()) == chunk_size);
        }

        // POSIX-buffered writes are immediately visible through the local FUSE
        // view before their close-time stable-storage barrier.
        Bytes actual(writes * chunk_size);
        REQUIRE(frontend->read(handle, 0, actual) == actual.size());
        for (size_t i = 0; i < writes; ++i)
            CHECK(std::equal(chunks[i].begin(), chunks[i].end(),
                             actual.begin() + static_cast<ptrdiff_t>(i * chunk_size)));

        // close/release must not return until every accepted write has completed
        // spool fsync -> journal append -> journal fsync.
        frontend->release(handle.inode, true);
        auto status = frontend->status();
        CHECK(status.durability_writes == writes);
        CHECK(status.durability_batches < status.durability_writes);
    }
}

MACHA_TEST("filesystem_fuse", test_fuse_fsync_waits_for_distributed_publication) {
    TestService fixture("fuse-fsync-publication");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.publication_quiet = 0ms;
    config.fuse.timeouts.sync = 10s;

    auto& service = fixture.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        auto handle = frontend->create("/sync.bin", 0644, getuid(), getgid(), true, true, false);
        auto bytes = pattern(2 * 1024 * 1024 + 12345);
        REQUIRE(frontend->write(handle.inode, 0, bytes) == bytes.size());

        // fsync is Macha's cluster-durability boundary: after it returns, the
        // ordinary FileSystem view (which has no access to the FUSE spool
        // overlay) must already expose the complete committed generation.
        frontend->fsync(handle.inode);
        auto committed = service.filesystem().getattr("/sync.bin");
        CHECK(committed.size == bytes.size());
        auto reader = service.filesystem().open_read("/sync.bin");
        Bytes actual(bytes.size());
        size_t done = 0;
        while (done < actual.size()) {
            auto n = reader->read(done, {actual.data() + done, actual.size() - done});
            REQUIRE(n > 0);
            done += n;
        }
        CHECK(actual == bytes);
        frontend->release(handle.inode, true);
    }
}

MACHA_TEST("filesystem_fuse", test_fuse_open_read_reuses_extent_until_manifest_changes) {
    TestService fixture("fuse-open-read-cache");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.extent_size = 1024 * 1024;

    auto& service = fixture.start();
    auto bytes = pattern(config.extent_size * 2);
    service.filesystem().create_file("/read-cache.bin", 0644, getuid(), getgid());
    auto writer = service.filesystem().open_write("/read-cache.bin", true);
    REQUIRE(writer->write(0, bytes) == bytes.size());
    writer->commit();
    writer.reset();
    auto entry = service.filesystem().getattr("/read-cache.bin");
    REQUIRE(entry.extents.size() >= 2);

    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        auto handle = frontend->open("/read-cache.bin", true, false, false, false);

        Bytes first(4096);
        REQUIRE(frontend->read(handle, 0, first) == first.size());
        CHECK(std::equal(first.begin(), first.end(), bytes.begin()));

        // ReadHandle caches the whole immutable extent. Removing the backing
        // object after the first callback makes reuse observable: a second read
        // through the same open FUSE handle must still be served from that
        // retained extent, whereas constructing a new ReadHandle per callback
        // would immediately fail here.
        // erase_all() is a policy-level delete and correctly refuses to remove
        // a still-retained live object.  This test needs a simulated physical
        // loss beneath the manifest, so remove the local copy directly and also
        // clear any opportunistic block-cache copy.
        REQUIRE(service.node().local_store().remove(entry.extents.front().id));
        (void)service.node().block_cache().remove(entry.extents.front().id);
        Bytes second(4096);
        REQUIRE(frontend->read(handle, 8192, second) == second.size());
        CHECK(std::equal(second.begin(), second.end(), bytes.begin() + 8192));

        auto fresh = frontend->open("/read-cache.bin", true, false, false, false);
        bool unavailable = false;
        try {
            Bytes probe(4096);
            (void)frontend->read(fresh, 16384, probe);
        } catch (const FsError& e) {
            unavailable = e.code() == EIO || e.code() == ETIMEDOUT;
        }
        CHECK(unavailable);
        frontend->release(fresh.inode, false);
        frontend->release(handle.inode, false);
    }
}

MACHA_TEST("filesystem_fuse", test_fuse_frontend_namespace_refresh_is_demand_driven) {
    TestService fixture("fuse-demand-refresh");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_min_write_replicas = 1;
    config.fuse.commit_workers = 1;

    auto& service = fixture.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);

        bool missing = false;
        try {
            (void)frontend->getattr("/external");
        } catch (const FsError& e) {
            missing = e.code() == ENOENT;
        }
        CHECK(missing);

        // Mutate the namespace outside the FUSE frontend. There is no refresh
        // timer: the next namespace-facing FUSE request observes the metadata
        // generation advance and adopts MetadataManager's shared decoded view.
        service.filesystem().mkdir("/external", 0755, getuid(), getgid());
        auto external = frontend->getattr("/external");
        CHECK(external.type == EntryType::directory);

        service.filesystem().create_file("/external/media.bin", 0644, getuid(), getgid());
        auto entries = frontend->readdir("/external");
        CHECK(std::any_of(entries.begin(), entries.end(),
                          [](const auto& item) { return item.first == "media.bin"; }));

        service.filesystem().unlink("/external/media.bin");
        missing = false;
        try {
            (void)frontend->getattr("/external/media.bin");
        } catch (const FsError& e) {
            missing = e.code() == ENOENT;
        }
        CHECK(missing);
    }
}

} // namespace
