// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_backend_support.hpp"
#include "fuse_journal.hpp"

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

MACHA_TEST("filesystem_fuse", test_open_write_metadata_merge) {
    TestService fixture("single-write");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_replication = 1;
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

MACHA_TEST("filesystem_fuse", test_fresh_and_resumed_write_exactness) {
    TestService fixture("write-exactness");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_replication = 1;
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
    config.metadata_replication = 1;
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
    config.metadata_replication = 1;
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

MACHA_TEST("filesystem_fuse", test_fuse_frontend_accepts_metadata_after_genesis_wait) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();

    auto c1 = config_for(cluster.path() / "genesis-n1", cluster.keyfile(), p1);
    auto c2 = config_for(cluster.path() / "genesis-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_replication = c2.metadata_replication = 2;

    Service s1(c1, keys);
    Service s2(c2, keys);
    s1.start();

    bool waiting = false;
    try {
        (void)s1.filesystem().local_snapshot_view();
    } catch (const MetadataNotReady&) {
        waiting = true;
    }
    REQUIRE(waiting);

    s2.start();
    REQUIRE(wait_until([&] {
        return s1.node().membership().active().size() >= 2 &&
               s2.node().membership().active().size() >= 2;
    }));

    REQUIRE(wait_until([&] {
        try {
            (void)s1.filesystem().local_snapshot_view();
            return true;
        } catch (const MetadataNotReady&) {
            return false;
        }
    }));
    REQUIRE(wait_until([&] {
        try {
            (void)s2.filesystem().local_snapshot_view();
            return true;
        } catch (const MetadataNotReady&) {
            return false;
        }
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

MACHA_TEST("filesystem_fuse", test_fuse_frontend_ordering_merging_and_cache) {
    TestService fixture("fuse-ordering");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_replication = 1;
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
        std::copy(adjacent.begin(), adjacent.end(), expected.begin() + static_cast<ptrdiff_t>(first.size()));
        auto patch = pattern(131072);
        const uint64_t patch_offset = config.extent_size - 65536;
        for (auto& byte : patch) byte ^= 0xa5;
        REQUIRE(frontend->write(inode, patch_offset, patch) == patch.size());
        std::copy(patch.begin(), patch.end(), expected.begin() + static_cast<ptrdiff_t>(patch_offset));

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
        for (auto& byte : tail) byte ^= 0x3c;
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

MACHA_TEST("filesystem_fuse", test_fuse_publication_yields_to_playback) {
    TestService fixture("fuse-playback-yield");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_replication = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 80ms;

    auto& service = fixture.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        auto handle = frontend->create("/playback-yield.bin", 0600, getuid(), getgid(), true, true, false);
        const auto inode = handle.inode;
        REQUIRE(inode != 0);
        REQUIRE(frontend->wait_for_idle(5s));

        auto payload = pattern(512 * 1024);
        REQUIRE(frontend->write(inode, 0, payload) == payload.size());

        // Playback demand is recorded before its storage read begins.  Bytes
        // already accepted by FUSE stay in the local spool, while distributed
        // publication waits for the viewer-critical quiet window to expire.
        service.filesystem().store().foreground_activity(1);
        frontend->release(inode, true);
        std::this_thread::sleep_for(20ms);
        auto during = frontend->status();
        CHECK(during.pending_data + during.active_data >= 1);
        CHECK(service.filesystem().getattr("/playback-yield.bin").size == 0);

        REQUIRE(frontend->wait_for_idle(5s));
        CHECK(service.filesystem().getattr("/playback-yield.bin").size == payload.size());
    }
}

MACHA_HEAVY_TEST("filesystem_fuse", test_fuse_durable_journal_recovers_namespace_and_data) {
    TestService fixture("fuse-journal-recovery");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_replication = 1;
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
        auto handle = frontend->create("/TV/Buffy/S07E01.mp4", 0644, getuid(), getgid(),
                                       true, true, false);
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
    config.metadata_replication = 1;
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
        try { (void)service.filesystem().getattr(std::string(old_path)); }
        catch (const FsError& e) { old_missing = e.code() == ENOENT; }
        CHECK(old_missing);

        bool removed_missing = false;
        try { (void)service.filesystem().getattr(std::string(removed_path)); }
        catch (const FsError& e) { removed_missing = e.code() == ENOENT; }
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

MACHA_TEST("filesystem_fuse", test_fuse_durable_journal_trims_torn_tail) {
    TestService fixture("fuse-journal-torn-tail");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_replication = 1;
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
    config.metadata_replication = 1;
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
    config.metadata_replication = 1;

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
        std::string bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
        CHECK(bytes == "unattributed bytes");
        preserved = true;
    }
    CHECK(preserved);
}

#if defined(__linux__)
size_t linux_open_fd_count() {
    std::error_code ec;
    size_t count = 0;
    for (std::filesystem::directory_iterator it("/proc/self/fd", ec), end;
         !ec && it != end; it.increment(ec))
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
    config.metadata_replication = 1;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 30s;

    auto& service = fixture.start();
    constexpr size_t dirty_inodes = 16;
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);
        for (size_t i = 0; i < dirty_inodes; ++i) {
            auto handle = frontend->create("/fd-" + std::to_string(i), 0644,
                                           getuid(), getgid(), true, true, false);
            const Bytes byte{static_cast<uint8_t>(i)};
            REQUIRE(frontend->write(handle.inode, 0, byte) == byte.size());
            frontend->release(handle.inode, true);
        }
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

MACHA_HEAVY_TEST("filesystem_fuse", test_fuse_recovery_starts_without_new_fuse_activity) {
    TestService fixture("fuse-recovery-autostart");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_replication = 1;
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
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        for (size_t i = 0; i < files; ++i) {
            auto created = frontend->create("/recover-autostart-" + std::to_string(i) + ".bin",
                                            0644, getuid(), getgid(), true, true, false);
            frontend->release(created.inode, true);
        }
        REQUIRE(frontend->wait_for_idle(10s));

        // Hold the viewer/foreground gate closed while constructing the durable
        // backlog. publication_quiet normally reduces live FUSE publication to
        // foreground_commit_workers rather than disabling it, so merely checking
        // pending_data here is timing-sensitive: one publisher may already be active.
        // Refresh foreground activity until the frontend has been stopped so no
        // distributed data publication can race this fixture.
        auto payload = pattern(4 * config.extent_size);
        service.filesystem().store().foreground_activity(1);
        std::jthread hold_foreground([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                service.filesystem().store().foreground_activity(1);
                std::this_thread::sleep_for(10ms);
            }
        });
        for (size_t i = 0; i < files; ++i) {
            auto handle = frontend->open("/recover-autostart-" + std::to_string(i) + ".bin",
                                         true, true, false, false);
            REQUIRE(frontend->write(handle.inode, 0, payload) == payload.size());
            frontend->release(handle.inode, true);
        }
        const auto staged = frontend->status();
        CHECK(staged.pending_data >= files);
        CHECK(staged.active_data == 0);
        frontend->stop();
        hold_foreground.request_stop();
        hold_foreground.join();
    }

    // Let the real playback gate expire, then deliberately keep only the generic
    // read-ahead/interactive activity clock hot. Recovery must ignore that clock:
    // its own object writes use the same accounting and would otherwise throttle
    // themselves. Do not issue any FUSE request after the restarted frontend is
    // constructed.
    REQUIRE(wait_until([&] {
        return service.filesystem().foreground_idle_for() >= config.fuse.publication_quiet;
    }, 2s));
    service.filesystem().store().interactive_activity(1);

    // With a recovery budget of two, both slots should become runnable immediately
    // rather than waiting for a new rsync/getattr to kick the scheduler.
    size_t max_recovery_active = 0;
    {
        auto recovered = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        const auto deadline = Clock::now() + 3s;
        while (Clock::now() < deadline) {
            const auto status = recovered->status();
            max_recovery_active = std::max(max_recovery_active, status.active_recovery_data);
            if (max_recovery_active >= config.fuse.recovery_commit_workers)
                break;
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(max_recovery_active == config.fuse.recovery_commit_workers);
        recovered->stop();
    }
}

MACHA_TEST("filesystem_fuse", test_fuse_recovery_publication_concurrency_is_bounded) {
    TestService fixture("fuse-recovery-concurrency");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_replication = 1;
    config.extent_size = 1024 * 1024;
    config.fuse.commit_workers = 4;
    config.fuse.recovery_commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 30s;

    auto& service = fixture.start();
    constexpr size_t files = 4;
    auto payload = pattern(4 * config.extent_size);
    for (size_t i = 0; i < files; ++i)
        service.filesystem().create_file("/recover-" + std::to_string(i) + ".bin",
                                         0644, getuid(), getgid());

    // Make the first frontend leave a real durable backlog rather than racing
    // the local object store while the test is constructing it.
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        service.filesystem().store().foreground_activity(1);
        for (size_t i = 0; i < files; ++i) {
            auto handle = frontend->open("/recover-" + std::to_string(i) + ".bin",
                                         true, true, false, false);
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
            CHECK(status.active_recovery_data <= drain.recovery_commit_workers);
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
    CHECK(max_recovery_active <= drain.recovery_commit_workers);
    for (size_t i = 0; i < files; ++i) {
        auto entry = service.filesystem().getattr("/recover-" + std::to_string(i) + ".bin");
        CHECK(entry.size == payload.size());
    }
}

void append_fuse_journal_test_record(const std::filesystem::path& journal,
                                     std::span<const uint8_t> payload) {
    Writer frame;
    frame.u32(static_cast<uint32_t>(payload.size()));
    frame.raw(payload);
    frame.fixed(sha256(payload).bytes);
    auto bytes = frame.take();

    std::ofstream out(journal, std::ios::binary | std::ios::app);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.flush();
    REQUIRE(out.good());
}

std::vector<uint8_t> fuse_journal_record_types(const std::filesystem::path& journal) {
    std::ifstream in(journal, std::ios::binary);
    REQUIRE(in.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Bytes bytes(raw.begin(), raw.end());
    constexpr size_t header_size = 8; // "MACHFUS1"
    REQUIRE(bytes.size() >= header_size);
    std::vector<uint8_t> types;
    const auto scan = scan_fuse_journal_frames(
        bytes, header_size,
        [&](std::span<const uint8_t> payload, size_t) {
            REQUIRE(!payload.empty());
            types.push_back(payload.front());
        });
    REQUIRE(scan.discarded_tail == 0);
    return types;
}


MACHA_TEST("filesystem_fuse", test_fuse_durable_journal_accepts_authoritative_data_done_without_published_prefix) {
    TestService fixture("fuse-journal-done-recovery");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_replication = 1;
    config.fuse.commit_workers = 1;
    config.fuse.foreground_commit_workers = 1;
    config.fuse.publication_quiet = 1s;

    auto& service = fixture.start();

    uint64_t inode = 0;
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        auto handle =
            frontend->create("/done-authoritative.bin", 0644, getuid(), getgid(),
                             true, true, false);
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

    const auto spool_dir =
        config.fuse.spool_path.value_or(config.state_path / "fuse-spool");
    const auto journal = config.fuse.operation_journal_path.value_or(
        spool_dir / "operations.log");

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
    config.metadata_replication = 1;

    auto& service = fixture.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        frontend->stop();
    }

    const auto spool_dir =
        config.fuse.spool_path.value_or(config.state_path / "fuse-spool");
    const auto journal = config.fuse.operation_journal_path.value_or(
        spool_dir / "operations.log");

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

MACHA_TEST("filesystem_fuse", test_fuse_durable_journal_rejects_missing_spool) {
    TestService fixture("fuse-journal-missing-spool");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_replication = 1;
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

    const auto spool = config.state_path / "fuse-spool" /
                       ("inode-" + std::to_string(inode) + ".spool");
    REQUIRE(std::filesystem::exists(spool));
    REQUIRE(std::filesystem::remove(spool));

    bool rejected = false;
    try {
        auto should_fail = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);
        should_fail->stop();
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
}

MACHA_TEST("filesystem_fuse", test_fuse_read_only_release_does_not_publish_writer_data) {
    TestService fixture("fuse-read-release");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_replication = 1;
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
    config.metadata_replication = 1;
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
        try { (void)service.filesystem().getattr("/doomed.bin"); }
        catch (const FsError& e) { missing = e.code() == ENOENT; }
        CHECK(missing);

        // More subtle: POSIX rename may replace a destination which still has
        // an open descriptor. The displaced inode remains a valid open identity,
        // but it no longer owns that pathname. Releasing dirty data through the
        // old descriptor must not overwrite/resurrect the new destination.
        auto destination = frontend->create("/target.bin", 0600, getuid(), getgid(), true, true, false);
        auto old_bytes = pattern(32768);
        REQUIRE(frontend->write(destination.inode, 0, old_bytes) == old_bytes.size());
        frontend->release(destination.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));

        auto old_open = frontend->open("/target.bin", true, true, false, false);
        std::array<uint8_t, 8> stale{{'S','T','A','L','E','!','!','!'}};
        REQUIRE(frontend->write(old_open.inode, 0, stale) == stale.size());

        auto source = frontend->create("/replacement.bin", 0600, getuid(), getgid(), true, true, false);
        auto replacement = pattern(98304);
        for (auto& byte : replacement) byte ^= 0x7d;
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
    config.metadata_replication = 1;
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
        for (auto& byte : patch) byte ^= 0x91;
        const uint64_t patch_offset = 4096;
        REQUIRE(frontend->write(handle.inode, patch_offset, patch) == patch.size());
        Bytes view(32768);
        REQUIRE(frontend->read(handle.inode, 0, view) == view.size());
        auto expected_view = Bytes(committed.begin(), committed.begin() + static_cast<ptrdiff_t>(view.size()));
        std::copy(patch.begin(), patch.end(), expected_view.begin() + static_cast<ptrdiff_t>(patch_offset));
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
        std::array<uint8_t, 6> marker{{'M','A','C','H','A','!'}};
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
            auto n = reader->read(offset, {final_bytes.data() + offset, final_bytes.size() - offset});
            REQUIRE(n > 0);
            offset += n;
        }
        CHECK(std::equal(patch.begin(), patch.end(), final_bytes.begin() + static_cast<ptrdiff_t>(patch_offset)));
        CHECK(std::equal(marker.begin(), marker.end(), final_bytes.begin() + 40000));
        CHECK(std::all_of(final_bytes.begin() + 32768, final_bytes.begin() + 40000,
                          [](uint8_t b) { return b == 0; }));
    }
}

MACHA_TEST("filesystem_fuse", test_fuse_buffered_writes_batch_until_close_durability) {
    TestService fixture("fuse-group-commit");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_replication = 1;
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
    config.metadata_replication = 1;
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
    config.metadata_replication = 1;
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
        service.filesystem().store().erase_all(entry.extents.front().id);
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
    config.metadata_replication = 1;
    config.fuse.commit_workers = 1;

    auto& service = fixture.start();
    {
        auto frontend = std::make_shared<FuseFrontend>(service.filesystem(), config.fuse);

        bool missing = false;
        try { (void)frontend->getattr("/external"); }
        catch (const FsError& e) { missing = e.code() == ENOENT; }
        CHECK(missing);

        // Mutate the namespace outside the FUSE frontend. There is no refresh
        // timer: the next namespace-facing FUSE request observes the metadata
        // generation advance and adopts MetadataManager's shared decoded view.
        service.filesystem().mkdir("/external", 0755, getuid(), getgid());
        auto external = frontend->getattr("/external");
        CHECK(external.type == EntryType::directory);

        service.filesystem().create_file("/external/media.bin", 0644, getuid(), getgid());
        auto entries = frontend->readdir("/external");
        CHECK(std::any_of(entries.begin(), entries.end(), [](const auto& item) {
            return item.first == "media.bin";
        }));

        service.filesystem().unlink("/external/media.bin");
        missing = false;
        try { (void)frontend->getattr("/external/media.bin"); }
        catch (const FsError& e) { missing = e.code() == ENOENT; }
        CHECK(missing);
    }
}

} // namespace
