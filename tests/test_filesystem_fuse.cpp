// SPDX-License-Identifier: GPL-3.0-or-later
#include "fuse/fuse_journal.hpp"
#include "fuse/fuse_adapter.hpp"
#include "test_backend_support.hpp"
#include <cerrno>
#include <iostream>
#include <csignal>
#include <fcntl.h>

using namespace macha;
using namespace std::chrono_literals;
using namespace macha::test_support;

namespace {

// One node's FileSystem and stores with no Service around them: everything the
// FUSE frontend and the publication writers take, and nothing more. Behaviours
// owned by those components share one node per test; each frontend started on
// it gets its own spool and journal, so frontends started in turn recover only
// their own work.
class FilesystemNode {
    TestNode node_;

  public:
    explicit FilesystemNode(std::string_view name,
                            const std::function<void(Config&)>& configure = {})
        : node_(name) {
        auto& config = node_.config();
        config.replication = 1;
        config.metadata_write_copies = 1;
        config.write_copies = 1;
        config.extent_size = 1024 * 1024;
        if (configure)
            configure(config);
        node_.start();
        // No Service runs the metadata owner here; the first commit forms the
        // one-node replica set.
        node_.filesystem().mkdir("/.ready", 0755, getuid(), getgid());
    }

    TestNode& node() { return node_; }
    FileSystem& fs() { return node_.filesystem(); }
    RetainedMemoryLedger& memory() { return node_.resources().memory; }
    LocalState& local() { return node_.node().local_state(); }
    const Config& config() { return node_.config(); }
    const std::filesystem::path& path() const { return node_.path(); }

    FuseConfig fuse(std::string_view name) {
        auto fuse = node_.config().fuse;
        const auto spool = node_.path() / "fuse" / std::string(name);
        fuse.spool_path = spool;
        fuse.operation_journal_path = spool / "operations.log";
        return fuse;
    }

    std::shared_ptr<FuseFrontend> frontend(const FuseConfig& fuse) {
        return make_fuse_frontend(fs(), memory(), fuse);
    }
    std::shared_ptr<FuseFrontend> frontend(const FuseConfig& fuse,
                                           std::unique_ptr<LoaderAdmission> admission) {
        return make_fuse_frontend(fs(), memory(), fuse, std::move(admission));
    }
    std::shared_ptr<FuseFrontend> frontend(const FuseConfig& fuse, PublicationTarget& target,
                                           std::unique_ptr<LoaderAdmission> admission = {}) {
        if (!admission)
            admission = std::make_unique<ViewerWeightedAdmission>(fs(), fuse);
        return std::make_shared<FuseFrontend>(fs(), memory(), fuse, std::move(admission), target);
    }
};

// A frontend whose loader publication (data and namespace) the test holds and
// releases.
struct HeldFrontend {
    HeldLoaderAdmission* loader{};
    std::shared_ptr<FuseFrontend> frontend;
    FuseFrontend* operator->() const { return frontend.get(); }
};

HeldFrontend held_frontend(FilesystemNode& node, const FuseConfig& fuse, bool hold = true) {
    auto admission = std::make_unique<HeldLoaderAdmission>();
    auto* loader = admission.get();
    if (hold)
        loader->hold();
    return {loader, node.frontend(fuse, std::move(admission))};
}

HeldFrontend held_frontend(FilesystemNode& node, const FuseConfig& fuse,
                           PublicationTarget& target, bool hold = true) {
    auto admission = std::make_unique<HeldLoaderAdmission>();
    auto* loader = admission.get();
    if (hold)
        loader->hold();
    return {loader, node.frontend(fuse, target, std::move(admission))};
}

// Publishes into a real target; a test hooks each writer's open, staging
// drain and commit to fail, hold or record them. Hooks are set before the
// frontend starts.
class InterposedTarget final : public PublicationTarget {
    class Writer final : public PublicationWriter {
        InterposedTarget& target_;
        const std::string path_;
        std::shared_ptr<PublicationWriter> inner_;
        std::atomic_uint64_t written_{};

      public:
        Writer(InterposedTarget& target, std::string path,
               std::shared_ptr<PublicationWriter> inner)
            : target_(target), path_(std::move(path)), inner_(std::move(inner)) {}

        WritePreparation prepare_write(uint64_t offset, uint64_t byte_budget) override {
            return inner_->prepare_write(offset, byte_budget);
        }
        WritePreparation prepare_commit(uint64_t byte_budget) override {
            return inner_->prepare_commit(byte_budget);
        }
        size_t write(uint64_t offset, std::span<const uint8_t> bytes) override {
            const auto written = inner_->write(offset, bytes);
            const auto total = written_.fetch_add(written, std::memory_order_relaxed) + written;
            if (target_.after_write)
                target_.after_write(total);
            return written;
        }
        void truncate(uint64_t size) override { inner_->truncate(size); }
        void drain_staging() override {
            if (target_.before_drain)
                target_.before_drain(written_.load(std::memory_order_relaxed));
            inner_->drain_staging();
        }
        void set_committed_mtime(int64_t mtime_ns) override {
            inner_->set_committed_mtime(mtime_ns);
        }
        void commit() override {
            inner_->commit();
            if (target_.after_commit)
                target_.after_commit(path_);
        }
        FsEntry committed_entry() const override { return inner_->committed_entry(); }
        WriteHandleDiagnostics diagnostics() const override { return inner_->diagnostics(); }
    };

    PublicationTarget& real_;

  public:
    // Throws to fail the open; may block.
    std::function<void(const std::string& path)> before_open;
    // Given the bytes the writer has taken; throws to fail the drain, which
    // leaves the writer as it was.
    std::function<void(uint64_t written)> before_drain;
    // Given the bytes the writer has taken, after each write.
    std::function<void(uint64_t written)> after_write;
    std::function<void(const std::string& path)> after_commit;
    // What reachability_epoch() answers; a test advances it.
    std::atomic_uint64_t reachability{};

    explicit InterposedTarget(PublicationTarget& real) : real_(real) {}

    uint64_t reachability_epoch() const noexcept override { return reachability.load(); }

    std::shared_ptr<PublicationWriter> open_publication(const std::string& path, bool cache_puts,
                                                        uint64_t pipeline_bytes,
                                                        DataWorkContext work_context) override {
        if (before_open)
            before_open(path);
        return std::make_shared<Writer>(
            *this, path,
            real_.open_publication(path, cache_puts, pipeline_bytes, std::move(work_context)));
    }
};

// Opens a gate when it goes out of scope, so a failing check never leaves a
// worker parked in it.
struct GateOpener {
    TestGate& gate;
    ~GateOpener() { gate.open(); }
};

bool absent(FileSystem& fs, const std::string& path) {
    try {
        (void)fs.getattr(path);
    } catch (const FsError& e) {
        return e.code() == ENOENT;
    }
    return false;
}

bool absent(FuseFrontend& frontend, std::string_view path) {
    try {
        (void)frontend.getattr(path);
    } catch (const FsError& e) {
        return e.code() == ENOENT;
    }
    return false;
}

Bytes read_back(FileSystem& fs, const std::string& path, size_t size) {
    auto reader = fs.open_read(path);
    Bytes output(size);
    size_t offset = 0;
    while (offset < output.size()) {
        const auto n = reader->read(
            offset, {output.data() + offset, std::min<size_t>(131071, output.size() - offset)});
        REQUIRE(n > 0);
        offset += n;
    }
    return output;
}

uint64_t publication_bytes(RetainedMemoryLedger& memory) {
    return memory.stats().owner_bytes[static_cast<size_t>(MemoryOwner::publication)];
}

template <class Fn>
int error_code_of(Fn&& fn) {
    try {
        fn();
    } catch (const FsError& e) {
        return e.code();
    }
    return 0;
}

MACHA_TEST("filesystem_fuse", test_fuse_signal_exit_is_a_clean_service_shutdown) {
    CHECK(!fuse_loop_result_is_error(0));
    CHECK(!fuse_loop_result_is_error(SIGTERM));
    CHECK(!fuse_loop_result_is_error(SIGINT));
    CHECK(fuse_loop_result_is_error(-EIO));
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
    // the preceding durable prefix. The journal recovery test proves the disk
    // loader applies the result.
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
    // later complete frame exists; that is durable middle-of-journal
    // corruption. The scanner reports where, keeps the good prefix, and
    // leaves the tail to the caller (quarantined by the frontend).
    {
        auto bytes = prefix;
        bytes.insert(bytes.end(), invalid_eof.begin(), invalid_eof.end());
        bytes.insert(bytes.end(), first.begin(), first.end());
        size_t records = 0;
        const auto scan = scan_fuse_journal_frames(
            bytes, header.size(), [&](std::span<const uint8_t>, size_t) { ++records; });
        CHECK(records == 1);
        REQUIRE(scan.corrupt_frame_offset.has_value());
        CHECK(*scan.corrupt_frame_offset == first_end);
        CHECK(scan.last_good == first_end);
        CHECK(scan.discarded_tail == invalid_eof.size() + first.size());
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

// FileSystem's publication writers (WriteHandle): generation staging, the
// changed-range overlay, the extent pipeline, the no-progress budget, size
// projection and the commit guard. WriteHandle's collaborators are the node's
// concrete stores, so it runs against one node's.
MACHA_TEST("filesystem_fuse", test_publication_writers_stage_and_commit_generations) {
    FilesystemNode node("publication-writers");
    auto& fs = node.fs();
    const auto extent = node.config().extent_size;

    // Metadata-only changes through an open writer (macOS cp applies mode,
    // owner and times before close) do not invalidate it; a real concurrent
    // content update is still a conflict.
    {
        fs.create_file("/copy.mkv", 0644, getuid(), getgid());
        auto writer = fs.open_write("/copy.mkv", true);
        CHECK(publication_bytes(node.memory()) == 0);
        const auto input = pattern(3 * extent + 12345);
        for (size_t offset = 0; offset < input.size();) {
            const size_t n = std::min<size_t>(4096, input.size() - offset);
            REQUIRE(writer->write(offset, {input.data() + offset, n}) == n);
            offset += n;
        }
        CHECK(publication_bytes(node.memory()) > 0);

        const int64_t preserved_mtime = 1700000000123456789LL;
        fs.chmod("/copy.mkv", 0600);
        fs.chown("/copy.mkv", getuid(), getgid(), true, true);
        fs.utimens("/copy.mkv", preserved_mtime);
        writer->commit();
        CHECK(publication_bytes(node.memory()) == 0);

        const auto entry = fs.getattr("/copy.mkv");
        CHECK(entry.size == input.size());
        CHECK(entry.mode == 0600);
        CHECK(entry.uid == static_cast<uint32_t>(getuid()));
        CHECK(entry.gid == static_cast<uint32_t>(getgid()));
        CHECK(entry.mtime_ns == preserved_mtime);
        CHECK(read_back(fs, "/copy.mkv", input.size()) == input);

        fs.create_file("/conflict.bin", 0644, getuid(), getgid());
        auto first = fs.open_write("/conflict.bin", true);
        auto second = fs.open_write("/conflict.bin", true);
        const auto a = pattern(8192);
        const auto b = pattern(8193);
        REQUIRE(first->write(0, a) == a.size());
        REQUIRE(second->write(0, b) == b.size());
        first->commit();
        CHECK(error_code_of([&] { second->commit(); }) == EAGAIN);
    }

    // A publication whose entry moved underneath it reports ESTALE, so the
    // frontend replays the generation from the spool; a foreground handle
    // gets EAGAIN, where retrying is meaningful.
    {
        const auto first = pattern(32 * 1024, 7);
        const auto longer = pattern(96 * 1024, 9);
        const auto stale_commit_code = [&](WriteDurability durability) {
            const std::string path = durability == WriteDurability::publication_generation
                                         ? "/stale-pub.bin"
                                         : "/stale-fg.bin";
            fs.create_file(path, 0644, getuid(), getgid());
            auto writer = fs.open_write(path, false, false, durability);
            REQUIRE(writer->write(0, first) == first.size());
            auto other = fs.open_write(path, false);
            REQUIRE(other->write(0, longer) == longer.size());
            other->commit();
            return error_code_of([&] { writer->commit(); });
        };
        CHECK(stale_commit_code(WriteDurability::publication_generation) == ESTALE);
        CHECK(stale_commit_code(WriteDurability::immediate) == EAGAIN);
    }

    // Retained-memory admission fails a writer only when nothing anywhere in
    // the pipeline progresses for its budget; progress re-arms the window, so
    // a merely slow node is never failed for being slow.
    {
        fs.create_file("/stalled.bin", 0644, getuid(), getgid());
        fs.create_file("/moving.bin", 0644, getuid(), getgid());
        const auto contents = pattern(4096);
        auto& ledger = node.memory();
        // Viewer class, because it is bounded only by the non-control
        // capacity: a loader-class lease could not take the whole budget a
        // wedged publication pipeline fills.
        auto hog = ledger.try_acquire(MemoryClass::viewer, MemoryOwner::playback_segment,
                                      ledger.stats().capacity_bytes -
                                          ledger.stats().control_reserve_bytes);
        REQUIRE(hog.has_value());

        std::atomic_uint64_t stalled_progress{0};
        auto stalled = fs.open_write("/stalled.bin", false, false,
                                     WriteDurability::publication_generation, 0,
                                     DataWorkContext(FrameType::loader, extent, {}, nullptr,
                                                     &stalled_progress, 100ms));
        const auto stalled_started = std::chrono::steady_clock::now();
        CHECK(error_code_of([&] { (void)stalled->write(0, contents); }) == EAGAIN);
        CHECK(std::chrono::steady_clock::now() - stalled_started < 10s);

        // Progress for 500 ms, then none: the write must outlast the progress,
        // which a plain 100 ms timeout would not.
        std::atomic_uint64_t moving_progress{0};
        const auto moving_started = std::chrono::steady_clock::now();
        std::jthread progress_thread([&] {
            while (std::chrono::steady_clock::now() - moving_started < 500ms) {
                moving_progress.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(20ms);
            }
        });
        auto moving = fs.open_write("/moving.bin", false, false,
                                    WriteDurability::publication_generation, 0,
                                    DataWorkContext(FrameType::loader, extent, {}, nullptr,
                                                    &moving_progress, 100ms));
        CHECK(error_code_of([&] { (void)moving->write(0, contents); }) == EAGAIN);
        CHECK(std::chrono::steady_clock::now() - moving_started > 500ms);
    }

    // A loader context keeps loader provenance through a changed-range
    // rebuild without rereading the unchanged prefix; only viewer classes may
    // refresh viewer demand accounting. A control context is refused.
    {
        bool rejected_control = false;
        try {
            (void)DataWorkContext(FrameType::control, extent);
        } catch (const std::invalid_argument&) {
            rejected_control = true;
        }
        CHECK(rejected_control);

        fs.create_file("/loader.bin", 0644, getuid(), getgid());
        const auto contents = pattern(2 * extent + 123);
        write_file(fs, "/loader.bin", contents);
        (void)fs.store().take_interactive_bytes();

        auto loader = fs.open_write("/loader.bin", false, false,
                                    WriteDurability::publication_generation, 0,
                                    DataWorkContext(FrameType::loader, extent));
        const uint8_t replacement = static_cast<uint8_t>(contents[17] ^ 0x5a);
        REQUIRE(loader->write(17, {&replacement, 1}) == 1);
        loader->commit();
        const auto diagnostics = loader->diagnostics();
        CHECK(!diagnostics.temp_open);
        CHECK(diagnostics.materialize_source_reads == 0);
        CHECK(diagnostics.materialize_source_bytes == 0);
        CHECK(diagnostics.rebuild_source_bytes == extent + 1);
        CHECK(diagnostics.rebuild_reused_extents == 2);
        CHECK(diagnostics.rebuild_put_extents == 1);
        CHECK(diagnostics.work_frame_type == FrameType::loader);
        CHECK(diagnostics.work_quantum_bytes == extent);
        CHECK(fs.store().take_interactive_bytes() == 0);
        CHECK(fs.store().take_foreground_bytes() == 0);

        fs.create_file("/interactive.bin", 0644, getuid(), getgid());
        auto interactive = fs.open_write("/interactive.bin", true, false,
                                         WriteDurability::immediate, 0,
                                         DataWorkContext(FrameType::read_ahead, extent));
        const auto bytes = pattern(4096);
        REQUIRE(interactive->write(0, bytes) == bytes.size());
        CHECK(fs.store().take_interactive_bytes() == bytes.size());
    }

    // Loader materialisation is resumable and byte bounded: one canonical
    // extent per grant, with the old generation visible throughout.
    {
        auto contents = pattern(4 * extent);
        write_file(fs, "/large.bin", contents);
        auto loader = fs.open_write("/large.bin", false, false,
                                    WriteDurability::publication_generation, 0,
                                    DataWorkContext(FrameType::loader, extent));
        const auto overlay = loader->prepare_write(17, extent);
        CHECK(overlay.ready);
        CHECK(overlay.bytes_processed == 0);
        CHECK(fs.getattr("/large.bin").size == contents.size());
        CHECK(loader->diagnostics().materialize_source_reads == 0);

        const auto original = contents;
        const uint8_t replacement = static_cast<uint8_t>(contents[17] ^ 0x6d);
        contents[17] = replacement;
        REQUIRE(loader->write(17, {&replacement, 1}) == 1);
        CHECK(loader->diagnostics().materialize_source_reads == 0);
        for (size_t step = 0; step < 4; ++step) {
            const auto preparation = loader->prepare_commit(extent);
            CHECK(preparation.bytes_processed == extent);
            CHECK(preparation.ready == (step == 3));
            const auto diagnostics = loader->diagnostics();
            CHECK(diagnostics.rebuild_source_bytes == extent + 1);
            CHECK(diagnostics.rebuild_steps == step + 1);
            CHECK(read_back(fs, "/large.bin", original.size()) == original);
        }
        const auto rebuilt = loader->diagnostics();
        CHECK(rebuilt.rebuild_reused_extents == 3);
        CHECK(rebuilt.rebuild_put_extents == 1);
        loader->commit();
        CHECK(read_back(fs, "/large.bin", contents.size()) == contents);
    }

    // Sparse changed-range overlay: overlapping writes merge, only touched
    // base extents are reread, and a direction change after appends seeds
    // from this handle's provisional extent.
    {
        auto expected = pattern(8 * extent);
        write_file(fs, "/changed.bin", expected);
        const auto original = expected;
        auto writer = fs.open_write("/changed.bin", false, false,
                                    WriteDurability::publication_generation, 0,
                                    DataWorkContext(FrameType::loader, extent));
        const Bytes first{0xa1, 0xa2, 0xa3};
        const Bytes overlapping{0xb1, 0xb2, 0xb3};
        const Bytes distant{0xc1, 0xc2};
        REQUIRE(writer->write(17, first) == first.size());
        REQUIRE(writer->write(18, overlapping) == overlapping.size());
        const uint64_t distant_offset = 5 * extent + 10;
        REQUIRE(writer->write(distant_offset, distant) == distant.size());
        std::copy(first.begin(), first.end(), expected.begin() + 17);
        std::copy(overlapping.begin(), overlapping.end(), expected.begin() + 18);
        std::copy(distant.begin(), distant.end(), expected.begin() + distant_offset);
        for (size_t step = 0; step < 8; ++step) {
            const auto preparation = writer->prepare_commit(extent);
            CHECK(preparation.bytes_processed == extent);
            CHECK(preparation.ready == (step == 7));
            CHECK(read_back(fs, "/changed.bin", original.size()) == original);
        }
        const auto diagnostics = writer->diagnostics();
        CHECK(diagnostics.materialize_source_bytes == 0);
        CHECK(diagnostics.rebuild_source_bytes == 2 * extent + 6);
        CHECK(diagnostics.rebuild_reused_extents == 6);
        CHECK(diagnostics.rebuild_put_extents == 2);
        writer->commit();
        CHECK(read_back(fs, "/changed.bin", expected.size()) == expected);

        auto combined = pattern(4 * extent, 91);
        write_file(fs, "/append-then-overwrite.bin", {combined.data(), 2 * extent});
        auto direction_change = fs.open_write("/append-then-overwrite.bin", false);
        REQUIRE(direction_change->write(2 * extent, {combined.data() + 2 * extent, 2 * extent}) ==
                2 * extent);
        const uint64_t appended_change = 2 * extent + 11;
        const uint8_t final_byte = static_cast<uint8_t>(combined[appended_change] ^ 0x7c);
        combined[appended_change] = final_byte;
        REQUIRE(direction_change->write(appended_change, {&final_byte, 1}) == 1);
        direction_change->commit();
        const auto direction = direction_change->diagnostics();
        CHECK(direction.materialize_source_bytes == 0);
        CHECK(direction.rebuild_source_bytes == extent + 1);
        CHECK(direction.rebuild_reused_extents == 3);
        CHECK(direction.rebuild_put_extents == 1);
        CHECK(read_back(fs, "/append-then-overwrite.bin", combined.size()) == combined);
    }

    // The extent pipeline is bounded (a third extent retires the oldest) and
    // atomic: drained provisional extents are not namespace-visible until the
    // commit.
    {
        fs.create_file("/pipeline.bin", 0644, getuid(), getgid());
        auto writer = fs.open_write("/pipeline.bin", true, false,
                                    WriteDurability::publication_generation, 2 * extent);
        const auto input = pattern(4 * extent);
        for (size_t offset = 0; offset < input.size(); offset += extent)
            REQUIRE(writer->write(offset, {input.data() + offset, extent}) == extent);
        auto staged = writer->diagnostics();
        CHECK(staged.peak_pending_extent_puts == 2);
        CHECK(staged.pending_extent_puts == 2);
        CHECK(staged.new_extent_puts == 2);
        CHECK(publication_bytes(node.memory()) == 2 * extent);
        CHECK(fs.getattr("/pipeline.bin").size == 0);

        writer->drain_staging();
        staged = writer->diagnostics();
        CHECK(staged.pending_extent_puts == 0);
        CHECK(staged.new_extent_puts == 4);
        CHECK(publication_bytes(node.memory()) == 0);
        CHECK(fs.getattr("/pipeline.bin").size == 0);

        writer->commit();
        CHECK(fs.getattr("/pipeline.bin").size == input.size());
        CHECK(read_back(fs, "/pipeline.bin", input.size()) == input);
    }

    // rsync-style writes: a fresh sequential write across many extents, an
    // --append resume reopening only the committed tail, an extent-aligned
    // resume fetching nothing, re-appending after a flush, and a one-byte
    // overwrite rebuilding only its extent.
    {
        auto fresh = pattern(5 * extent + 123457);
        fs.create_file("/fresh.bin", 0644, getuid(), getgid());
        auto fresh_writer = fs.open_write("/fresh.bin", true);
        for (size_t offset = 0; offset < fresh.size();) {
            const auto n = std::min<size_t>(65537, fresh.size() - offset);
            REQUIRE(fresh_writer->write(offset, {fresh.data() + offset, n}) == n);
            offset += n;
        }
        fresh_writer->commit();
        CHECK(read_back(fs, "/fresh.bin", fresh.size()) == fresh);

        auto resumed = pattern(6 * extent + 654321);
        const size_t prefix = 2 * extent + 77777;
        write_file(fs, "/resumed.bin", {resumed.data(), prefix});
        const auto prefix_entry = fs.getattr("/resumed.bin");
        REQUIRE(prefix_entry.extents.size() == 3);
        auto resumed_writer = fs.open_write("/resumed.bin", false);
        for (size_t offset = prefix; offset < resumed.size();) {
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
        const auto resumed_entry = fs.getattr("/resumed.bin");
        REQUIRE(resumed_entry.extents.size() >= 2);
        CHECK(resumed_entry.extents[0] == prefix_entry.extents[0]);
        CHECK(resumed_entry.extents[1] == prefix_entry.extents[1]);
        CHECK(read_back(fs, "/resumed.bin", resumed.size()) == resumed);

        auto aligned = pattern(5 * extent + 333);
        write_file(fs, "/aligned.bin", {aligned.data(), 3 * extent});
        const auto aligned_before = fs.getattr("/aligned.bin");
        REQUIRE(aligned_before.extents.size() == 3);
        auto aligned_writer = fs.open_write("/aligned.bin", false);
        for (size_t offset = 3 * extent; offset < aligned.size();) {
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
        const auto aligned_after = fs.getattr("/aligned.bin");
        REQUIRE(aligned_after.extents.size() >= aligned_before.extents.size());
        for (size_t i = 0; i < aligned_before.extents.size(); ++i)
            CHECK(aligned_after.extents[i] == aligned_before.extents[i]);
        CHECK(read_back(fs, "/aligned.bin", aligned.size()) == aligned);

        // A FUSE flush does not close the handle: appending again reopens only
        // the newly committed partial tail.
        const auto more = pattern(777);
        const auto aligned_old_size = aligned.size();
        aligned.insert(aligned.end(), more.begin(), more.end());
        REQUIRE(aligned_writer->write(aligned_old_size, more) == more.size());
        aligned_writer->commit();
        const auto again = aligned_writer->diagnostics();
        CHECK(!again.temp_open);
        CHECK(again.append_tail_fetches == 1);
        CHECK(again.materialize_source_reads == 0);
        CHECK(again.rebuild_put_extents == 0);
        CHECK(read_back(fs, "/aligned.bin", aligned.size()) == aligned);

        auto random_write = pattern(4 * extent);
        write_file(fs, "/random.bin", random_write);
        auto random_writer = fs.open_write("/random.bin", false);
        const uint64_t changed_offset = extent + 1234;
        const uint8_t changed = static_cast<uint8_t>(random_write[changed_offset] ^ 0x5a);
        random_write[changed_offset] = changed;
        REQUIRE(random_writer->write(changed_offset, {&changed, 1}) == 1);
        random_writer->commit();
        const auto random_diag = random_writer->diagnostics();
        CHECK(!random_diag.temp_open);
        CHECK(random_diag.materialize_source_reads == 0);
        CHECK(random_diag.rebuild_source_bytes == extent + 1);
        CHECK(random_diag.rebuild_reused_extents == 3);
        CHECK(random_diag.rebuild_put_extents == 1);
        CHECK(read_back(fs, "/random.bin", random_write.size()) == random_write);
    }

    // An open writer's size is projected before commit, follows truncate and
    // rename, and is gone once the writer is.
    {
        fs.create_file("/.active.tmp", 0600, getuid(), getgid());
        CHECK(!fs.active_write_size("/.active.tmp").has_value());
        auto writer = fs.open_write("/.active.tmp", true);
        const auto input = pattern(2 * extent + 12345);
        REQUIRE(writer->write(0, input) == input.size());
        CHECK(fs.getattr("/.active.tmp").size == 0);
        CHECK(fs.active_write_size("/.active.tmp") == std::optional<uint64_t>(input.size()));
        writer->truncate(65536);
        CHECK(fs.active_write_size("/.active.tmp") == std::optional<uint64_t>(65536));
        fs.rename("/.active.tmp", "/active.bin");
        CHECK(!fs.active_write_size("/.active.tmp").has_value());
        CHECK(fs.active_write_size("/active.bin") == std::optional<uint64_t>(65536));
        writer->commit();
        CHECK(fs.getattr("/active.bin").size == 65536);
        writer.reset();
        CHECK(!fs.active_write_size("/active.bin").has_value());
    }

    // rsync renames its temporary file while the descriptor is still open;
    // the commit lands on the new name.
    {
        fs.create_file("/.upload.tmp", 0600, getuid(), getgid());
        auto writer = fs.open_write("/.upload.tmp", true);
        const auto input = pattern(3 * extent + 12345);
        for (size_t offset = 0; offset < input.size();) {
            const size_t n = std::min<size_t>(128 * 1024, input.size() - offset);
            REQUIRE(writer->write(offset, {input.data() + offset, n}) == n);
            offset += n;
        }
        fs.rename("/.upload.tmp", "/movie.mkv");
        writer->commit();
        CHECK(absent(fs, "/.upload.tmp"));
        CHECK(fs.getattr("/movie.mkv").size == input.size());
        CHECK(read_back(fs, "/movie.mkv", input.size()) == input);
    }

    // The local snapshot view is the local replica's, cached per generation.
    {
        const auto first = fs.local_snapshot_view();
        CHECK(fs.local_snapshot_view().snapshot == first.snapshot);
        fs.mkdir("/local-view", 0755, getuid(), getgid());
        const auto advanced = fs.local_snapshot_view();
        CHECK(advanced.generation > first.generation);
        CHECK(advanced.snapshot != first.snapshot);
        CHECK(advanced.snapshot->entries.contains("/local-view"));
    }
}

// A staged extent put that fails leaves the abandoned generation invisible:
// its successful provisional extents are not a partial file.
MACHA_TEST("filesystem_fuse", test_publication_writer_failure_stays_invisible) {
    FilesystemNode node("publication-writer-failure", [](Config& config) {
        config.storage_backends.front().limit = 2 * config.extent_size;
    });
    auto& fs = node.fs();
    const auto extent = node.config().extent_size;
    fs.create_file("/pipeline-failure.bin", 0644, getuid(), getgid());
    auto writer = fs.open_write("/pipeline-failure.bin", true, false,
                                WriteDurability::publication_generation, 2 * extent);
    const auto input = pattern(4 * extent);
    bool failed = false;
    try {
        for (size_t offset = 0; offset < input.size(); offset += extent)
            writer->write(offset, {input.data() + offset, extent});
        writer->drain_staging();
    } catch (const std::exception&) {
        failed = true;
    }
    CHECK(failed);
    writer.reset();
    CHECK(fs.getattr("/pipeline-failure.bin").size == 0);
}

// Before the metadata write floor forms, the local snapshot view reflects the
// local replica alone and never enters MetadataManager, which would throw
// MetadataNotReady.
MACHA_TEST("filesystem_fuse", test_local_snapshot_view_is_local_before_cluster_forms) {
    TestNode fixture("local-view-before-floor");
    fixture.config().replication = 1;
    fixture.config().metadata_write_copies = 2;
    fixture.start();
    auto& fs = fixture.filesystem();
    CHECK(!fixture.metadata().available_snapshot_view().has_value());
    const auto local_record = fixture.node().local_state().replica().current();
    const auto first = fs.local_snapshot_view();
    CHECK(first.generation == local_record.generation);
    CHECK(first.hash == local_record.hash);
    CHECK(first.snapshot->entries.contains("/"));
    CHECK(fs.local_snapshot_view().snapshot == first.snapshot);
    CHECK(!fixture.metadata().available_snapshot_view().has_value());
}

// The FUSE frontend's local semantics over one node: the pending overlay,
// reads, truncation, hydration hints, write ordering and merging, inode
// identity across rename and unlink, inode ownership, close and fsync
// durability, demand-driven namespace refresh and mtime preservation. The
// frontend's namespace collaborator is the concrete FileSystem, so it runs
// against one node's.
MACHA_TEST("filesystem_fuse", test_fuse_frontend_local_semantics) {
    FilesystemNode node("fuse-frontend-semantics", [](Config& config) {
        config.cache.path = config.state_path.parent_path() / "cache";
        config.cache.max_blocks = 64;
    });
    auto& fs = node.fs();
    const auto extent = node.config().extent_size;

    // The derived overlay index coalesces contiguous spool mappings, so a
    // small read examines one intersecting descriptor rather than replaying
    // the history; a shrink then regrowth exposes zeros.
    {
        auto fuse = node.fuse("overlay");
        fuse.max_spool_bytes = 64ULL * 1024 * 1024;
        auto frontend = held_frontend(node, fuse);
        auto handle = frontend->create("/append-verify.bin", 0644, getuid(), getgid(), true, true,
                                       false);
        constexpr size_t chunk_size = 4096;
        constexpr size_t chunks = 256;
        Bytes expected(chunk_size * chunks);
        for (size_t chunk = 0; chunk < chunks; ++chunk) {
            auto bytes = pattern(chunk_size, static_cast<uint8_t>(chunk));
            std::copy(bytes.begin(), bytes.end(), expected.begin() + chunk * chunk_size);
            REQUIRE(frontend->write(handle.inode, chunk * chunk_size, bytes, false) ==
                    bytes.size());
        }
        const auto before = frontend->diagnostics();
        CHECK(before.retained_data_operations == chunks);
        CHECK(before.retained_data_operation_bytes >= chunks * sizeof(uint64_t));
        CHECK(before.retained_overlay_ranges == 1);
        CHECK(before.retained_overlay_bytes > 0);
        CHECK(before.retained_publication_operations == 0);
        CHECK(before.retained_publication_operation_bytes == 0);
        Bytes probe(1024);
        const uint64_t probe_offset = 173 * chunk_size + 777;
        REQUIRE(frontend->read(handle, probe_offset, probe) == probe.size());
        CHECK(std::equal(probe.begin(), probe.end(), expected.begin() + probe_offset));
        const auto after = frontend->diagnostics();
        CHECK(after.data_overlay_read_queries == before.data_overlay_read_queries + 1);
        CHECK(after.data_overlay_ranges_examined - before.data_overlay_ranges_examined == 1);
        CHECK(after.data_overlay_descriptors_copied - before.data_overlay_descriptors_copied == 1);

        const auto replacement = pattern(257, 0xe3);
        const uint64_t replacement_offset = 91 * chunk_size + 123;
        REQUIRE(frontend->write(handle.inode, replacement_offset, replacement, false) ==
                replacement.size());
        std::copy(replacement.begin(), replacement.end(), expected.begin() + replacement_offset);
        const auto overwrite_before = frontend->diagnostics();
        Bytes overwritten(replacement.size());
        REQUIRE(frontend->read(handle, replacement_offset, overwritten) == overwritten.size());
        CHECK(overwritten == replacement);
        CHECK(frontend->diagnostics().data_overlay_ranges_examined -
                  overwrite_before.data_overlay_ranges_examined ==
              1);

        const uint64_t truncated = expected.size() / 2;
        frontend->truncate(handle.inode, truncated);
        frontend->truncate(handle.inode, truncated + 8192);
        Bytes zero_tail(8192, 0xff);
        REQUIRE(frontend->read(handle, truncated, zero_tail) == zero_tail.size());
        CHECK(std::all_of(zero_tail.begin(), zero_tail.end(), [](uint8_t b) { return b == 0; }));
        frontend->stop();
    }

    // Adjacent and overlapping writes keep exact byte order behind one
    // coalesced dirty range; repeated flushes never double-count; renames
    // keep the open inode; publication is loader traffic and, when asked,
    // writes extents through to the persistent block cache.
    {
        auto fuse = node.fuse("ordering");
        fuse.commit_workers = 2;
        fuse.read_ahead_extents = 2;
        fuse.write_through_cache = true;
        auto frontend = node.frontend(fuse);
        auto handle = frontend->create("/.rsync.tmp", 0600, getuid(), getgid(), true, true, false);
        const auto inode = handle.inode;
        REQUIRE(inode != 0);

        Bytes expected(3 * extent + 8192, 0);
        const auto first = pattern(extent + 32768);
        REQUIRE(frontend->write(inode, 0, first) == first.size());
        std::copy(first.begin(), first.end(), expected.begin());
        const auto adjacent = pattern(extent);
        REQUIRE(frontend->write(inode, first.size(), adjacent) == adjacent.size());
        std::copy(adjacent.begin(), adjacent.end(),
                  expected.begin() + static_cast<ptrdiff_t>(first.size()));
        auto patch = pattern(131072);
        const uint64_t patch_offset = extent - 65536;
        for (auto& byte : patch)
            byte ^= 0xa5;
        REQUIRE(frontend->write(inode, patch_offset, patch) == patch.size());
        std::copy(patch.begin(), patch.end(),
                  expected.begin() + static_cast<ptrdiff_t>(patch_offset));
        const auto ranges = frontend->dirty_ranges(inode);
        REQUIRE(ranges.size() == 1);
        CHECK(ranges.front().offset == 0);
        CHECK(ranges.front().length == first.size() + adjacent.size());

        frontend->flush(inode);
        frontend->flush(inode);
        const auto during = frontend->status();
        CHECK(during.pending_data <= 1);
        CHECK(during.active_data <= fuse.commit_workers);

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
        const uint64_t tail_offset = 3 * extent;
        REQUIRE(frontend->write(inode, tail_offset, tail) == tail.size());
        std::copy(tail.begin(), tail.end(), expected.begin() + static_cast<ptrdiff_t>(tail_offset));
        frontend->release(inode, true);
        REQUIRE(frontend->wait_for_idle(10s));

        // Publication replay must not mark its own chunks as foreground, or
        // the quiet policy would throttle it by one quiet interval per chunk.
        CHECK(fs.foreground_idle_for() >= 1h);
        CHECK(frontend->status().pending_data == 0);
        const auto entry = fs.getattr("/movie.bin");
        CHECK(entry.size == expected.size());

        // A read-only FUSE handle is loader traffic too: neither viewer clock
        // moves.
        auto fuse_reader = frontend->open("/movie.bin", true, false, false, false);
        Bytes fuse_probe(4096);
        const auto foreground_before = fs.foreground_idle_for();
        const auto interactive_before = fs.store().interactive_idle_for();
        REQUIRE(frontend->read(fuse_reader, 0, fuse_probe) == fuse_probe.size());
        CHECK(fs.foreground_idle_for() >= foreground_before);
        CHECK(fs.store().interactive_idle_for() >= interactive_before);
        frontend->release(fuse_reader.inode, false);

        CHECK(read_back(fs, "/movie.bin", expected.size()) == expected);
        REQUIRE(!entry.extents.empty());
        for (const auto& stored : entry.extents)
            if (!stored.hole)
                CHECK(node.local().cache().has(stored.id));
        frontend->stop();
    }

    // Reads overlay pending writes on the committed base; a committed-range
    // read emits one deduplicated FUSE hydration run per inode; truncation
    // before publication never resurrects the old suffix.
    {
        const auto committed = pattern(4 * extent + 4096);
        write_file(fs, "/read.bin", committed);
        const auto base_entry = fs.getattr("/read.bin");
        REQUIRE(base_entry.extents.size() >= 5);

        auto fuse = node.fuse("read-overlay");
        fuse.read_ahead_extents = 2;
        fuse.hydration_priority = 2718;
        auto frontend = node.frontend(fuse);
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

        Bytes demand(4096);
        REQUIRE(frontend->read(handle.inode, extent + 1024, demand) == demand.size());
        REQUIRE(frontend->read(handle.inode, extent + 1024, demand) == demand.size());
        const auto hints = frontend->hints();
        REQUIRE(hints.size() == 1);
        CHECK(hints.front().run_id == "fuse:" + std::to_string(handle.inode));
        CHECK(hints.front().priority == fuse.hydration_priority);
        CHECK(hints.front().frame_type == FrameType::read_ahead);
        REQUIRE(hints.front().objects.size() == 3);
        CHECK(hints.front().objects[0] == base_entry.extents[1].id);
        CHECK(hints.front().objects[1] == base_entry.extents[2].id);
        CHECK(hints.front().objects[2] == base_entry.extents[3].id);
        const std::set<ObjectId> unique(hints.front().objects.begin(),
                                        hints.front().objects.end());
        CHECK(unique.size() == hints.front().objects.size());

        frontend->truncate(handle.inode, 32768);
        frontend->truncate(handle.inode, 65536);
        Bytes extended(32768, 0xff);
        REQUIRE(frontend->read(handle.inode, 32768, extended) == extended.size());
        CHECK(std::all_of(extended.begin(), extended.end(), [](uint8_t b) { return b == 0; }));
        Bytes beyond_eof(4096, 0xff);
        CHECK(frontend->read(handle.inode, extent + 1024, beyond_eof) == 0);
        const std::array<uint8_t, 6> marker{{'M', 'A', 'C', 'H', 'A', '!'}};
        REQUIRE(frontend->write(handle.inode, 40000, marker) == marker.size());
        Bytes marker_view(64, 0xff);
        REQUIRE(frontend->read(handle.inode, 39984, marker_view) == marker_view.size());
        CHECK(std::equal(marker.begin(), marker.end(), marker_view.begin() + 16));

        frontend->release(handle.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));
        CHECK(fs.getattr("/read.bin").size == 65536);
        const auto final_bytes = read_back(fs, "/read.bin", 65536);
        CHECK(std::equal(patch.begin(), patch.end(),
                         final_bytes.begin() + static_cast<ptrdiff_t>(patch_offset)));
        CHECK(std::equal(marker.begin(), marker.end(), final_bytes.begin() + 40000));
        CHECK(std::all_of(final_bytes.begin() + 32768, final_bytes.begin() + 40000,
                          [](uint8_t b) { return b == 0; }));
        frontend->stop();
    }

    // An open read handle caches its whole immutable extent until the
    // manifest changes; a fresh handle reads the store and finds the object
    // gone. Cached payload memory is returned when the handles close.
    {
        const auto bytes = pattern(extent * 2);
        write_file(fs, "/read-cache.bin", bytes);
        const auto entry = fs.getattr("/read-cache.bin");
        REQUIRE(entry.extents.size() >= 2);
        const auto payload_bytes = [&] {
            return node.memory().stats().owner_bytes[static_cast<size_t>(
                MemoryOwner::object_payload)];
        };
        {
            auto frontend = node.frontend(node.fuse("read-cache"));
            auto handle = frontend->open("/read-cache.bin", true, false, false, false);
            Bytes first(4096);
            REQUIRE(frontend->read(handle, 0, first) == first.size());
            CHECK(std::equal(first.begin(), first.end(), bytes.begin()));
            CHECK(payload_bytes() >= extent);

            // erase_all() refuses to remove a retained live object, so remove
            // the local and cached copies to model its loss.
            REQUIRE(node.local().data().remove(entry.extents.front().id));
            (void)node.local().cache().remove(entry.extents.front().id);
            Bytes second(4096);
            REQUIRE(frontend->read(handle, 8192, second) == second.size());
            CHECK(std::equal(second.begin(), second.end(), bytes.begin() + 8192));

            auto fresh = frontend->open("/read-cache.bin", true, false, false, false);
            const auto code = error_code_of([&] {
                Bytes probe(4096);
                (void)frontend->read(fresh, 16384, probe);
            });
            CHECK((code == EIO || code == ETIMEDOUT));
            frontend->release(fresh.inode, false);
            frontend->release(handle.inode, false);
            frontend->stop();
        }
        CHECK(payload_bytes() == 0);
    }

    // write() does not wait for a local fsync pair, so one sequential writer
    // builds a batch; release() returns only once every accepted write has
    // reached spool fsync, journal append and journal fsync, and buffered
    // writes are visible through the mount before then.
    {
        auto fuse = node.fuse("group-commit");
        fuse.request_workers = 24;
        auto frontend = held_frontend(node, fuse);
        auto handle = frontend->create("/batch.bin", 0644, getuid(), getgid(), true, true, false);
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
        Bytes actual(writes * chunk_size);
        REQUIRE(frontend->read(handle, 0, actual) == actual.size());
        for (size_t i = 0; i < writes; ++i)
            CHECK(std::equal(chunks[i].begin(), chunks[i].end(),
                             actual.begin() + static_cast<ptrdiff_t>(i * chunk_size)));
        frontend->release(handle.inode, true);
        const auto status = frontend->status();
        CHECK(status.durability_writes == writes);
        CHECK(status.durability_batches < status.durability_writes);
        frontend->stop();
    }

    // fsync is the cluster-durability boundary: once it returns, the plain
    // FileSystem view, which cannot see the spool overlay, holds the whole
    // generation.
    {
        auto fuse = node.fuse("fsync");
        fuse.timeouts.sync = 10s;
        auto frontend = node.frontend(fuse);
        auto handle = frontend->create("/sync.bin", 0644, getuid(), getgid(), true, true, false);
        const auto bytes = pattern(2 * extent + 12345);
        REQUIRE(frontend->write(handle.inode, 0, bytes) == bytes.size());
        frontend->fsync(handle.inode);
        CHECK(fs.getattr("/sync.bin").size == bytes.size());
        CHECK(read_back(fs, "/sync.bin", bytes.size()) == bytes);
        frontend->release(handle.inode, true);
        frontend->stop();
    }

    // A completed publication unlinks the retired spool file.
    {
        auto fuse = node.fuse("retire-spool");
        fuse.commit_workers = 1;
        auto frontend = node.frontend(fuse);
        auto handle =
            frontend->create("/retire-spool.bin", 0600, getuid(), getgid(), true, true, false);
        const auto payload = pattern(2 * extent + 17, 71);
        REQUIRE(frontend->write(handle.inode, 0, payload) == payload.size());
        frontend->release(handle.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));
        CHECK(!std::filesystem::exists(*fuse.spool_path /
                                       ("inode-" + std::to_string(handle.inode) + ".spool")));
        frontend->stop();
    }

    // Closing a basis reader (rsync --append-verify) while the writer still
    // holds dirty data does not publish the writer's data.
    {
        auto fuse = node.fuse("read-release");
        fuse.commit_workers = 1;
        auto frontend = node.frontend(fuse);
        auto writer = frontend->create("/growing.bin", 0600, getuid(), getgid(), true, true, false);
        const auto bytes = pattern(256 * 1024);
        REQUIRE(frontend->write(writer.inode, 0, bytes) == bytes.size());
        auto reader = frontend->open("/growing.bin", true, false, false, false);
        CHECK(reader.inode == writer.inode);
        frontend->release(reader.inode, false);
        REQUIRE(frontend->wait_for_idle(2s));
        CHECK(fs.getattr("/growing.bin").size == 0);
        REQUIRE(frontend->dirty_ranges(writer.inode).size() == 1);
        frontend->release(writer.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));
        CHECK(fs.getattr("/growing.bin").size == bytes.size());
        CHECK(read_back(fs, "/growing.bin", bytes.size()) == bytes);
        frontend->stop();
    }

    // A dirty inode unlinked before release never recreates its path; a
    // destination replaced by rename while still open keeps its open
    // identity but loses the path, so its dirty release cannot overwrite the
    // new file, and its inode is reclaimed once released.
    {
        auto fuse = node.fuse("replace");
        fuse.commit_workers = 2;
        auto frontend = node.frontend(fuse);
        auto doomed = frontend->create("/doomed.bin", 0600, getuid(), getgid(), true, true, false);
        const auto doomed_data = pattern(65536);
        REQUIRE(frontend->write(doomed.inode, 0, doomed_data) == doomed_data.size());
        frontend->unlink("/doomed.bin");
        frontend->release(doomed.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));
        CHECK(!frontend->inode_for_path("/doomed.bin").has_value());
        CHECK(absent(fs, "/doomed.bin"));

        auto destination =
            frontend->create("/target.bin", 0600, getuid(), getgid(), true, true, false);
        const auto old_bytes = pattern(32768);
        REQUIRE(frontend->write(destination.inode, 0, old_bytes) == old_bytes.size());
        frontend->release(destination.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));

        auto old_open = frontend->open("/target.bin", true, true, false, false);
        const std::array<uint8_t, 8> stale{{'S', 'T', 'A', 'L', 'E', '!', '!', '!'}};
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
        CHECK(error_code_of([&] { (void)frontend->path_for_inode(old_open.inode); }) == EBADF);
        CHECK(fs.getattr("/target.bin").size == replacement.size());
        CHECK(read_back(fs, "/target.bin", replacement.size()) == replacement);
        frontend->stop();
    }

    // A detached inode stays owned while a descriptor is open, even after its
    // unlink is confirmed, and is reclaimed at the last release; repeated
    // create/close/unlink cycles return the owner table to its baseline.
    {
        auto fuse = node.fuse("inode-ownership");
        auto frontend = node.frontend(fuse);
        const auto baseline = frontend->status().inode_count;
        auto created = frontend->create("/open-unlinked.bin", 0600, getuid(), getgid(), true,
                                        false, false);
        REQUIRE(frontend->wait_for_idle(10s));
        frontend->unlink("/open-unlinked.bin");
        REQUIRE(frontend->wait_for_idle(10s));
        const auto detached = frontend->status();
        CHECK(detached.inode_count == baseline + 1);
        CHECK(detached.detached_inode_count == 1);
        frontend->release(created.inode, false);
        const auto released = frontend->status();
        CHECK(released.inode_count == baseline);
        CHECK(released.detached_inode_count == 0);
        CHECK(released.reclaimed_inode_count == 1);

        constexpr size_t cycles = 64;
        for (size_t i = 0; i < cycles; ++i) {
            const auto path = "/lifecycle-" + std::to_string(i);
            auto handle = frontend->create(path, 0600, getuid(), getgid(), true, false, false);
            frontend->release(handle.inode, false);
            frontend->unlink(path);
        }
        REQUIRE(frontend->wait_for_idle(20s));
        const auto final_status = frontend->status();
        CHECK(final_status.inode_count == baseline);
        CHECK(final_status.detached_inode_count == 0);
        CHECK(final_status.reclaimed_inode_count == cycles + 1);
        CHECK(final_status.peak_inode_count <= baseline + cycles);
        frontend->stop();
    }

    // There is no refresh timer: the next namespace-facing request observes
    // a namespace change made outside the frontend.
    {
        auto frontend = node.frontend(node.fuse("demand-refresh"));
        CHECK(absent(*frontend, "/external"));
        fs.mkdir("/external", 0755, getuid(), getgid());
        CHECK(frontend->getattr("/external").type == EntryType::directory);
        fs.create_file("/external/media.bin", 0644, getuid(), getgid());
        const auto entries = frontend->readdir("/external");
        CHECK(std::any_of(entries.begin(), entries.end(),
                          [](const auto& item) { return item.first == "media.bin"; }));
        fs.unlink("/external/media.bin");
        CHECK(absent(*frontend, "/external/media.bin"));

        // After the first, each refresh applies what differs between the tree
        // it last took and the tree now. Whatever is changed outside, mixed
        // with the frontend's own operations, the frontend lists what the
        // filesystem lists.
        const auto names = [](const auto& listing) {
            std::vector<std::string> out;
            for (const auto& item : listing)
                out.push_back(item.first);
            std::sort(out.begin(), out.end());
            return out;
        };
        const auto agrees = [&](const std::string& directory) {
            return names(frontend->readdir(directory)) == names(fs.readdir(directory));
        };
        fs.mkdir("/external/show", 0755, getuid(), getgid());
        for (const char* name : {"/external/show/e1.bin", "/external/show/e2.bin"})
            fs.create_file(name, 0644, getuid(), getgid());
        fs.create_file("/external/show.nfo", 0644, getuid(), getgid());
        CHECK(agrees("/external"));
        CHECK(agrees("/external/show"));
        frontend->mkdir("/external/own", 0755, getuid(), getgid());
        REQUIRE(frontend->wait_for_idle(20s));
        fs.rename("/external/show", "/external/renamed", false);
        CHECK(absent(*frontend, "/external/show"));
        CHECK(absent(*frontend, "/external/show/e1.bin"));
        CHECK(frontend->getattr("/external/renamed/e2.bin").type == EntryType::file);
        CHECK(agrees("/external"));
        CHECK(agrees("/external/renamed"));
        fs.chmod("/external/renamed/e1.bin", 0600);
        CHECK((frontend->getattr("/external/renamed/e1.bin").mode & 0777U) == 0600U);
        fs.unlink("/external/renamed/e1.bin");
        fs.unlink("/external/renamed/e2.bin");
        fs.rmdir("/external/renamed");
        fs.rmdir("/external/own");
        CHECK(absent(*frontend, "/external/renamed"));
        CHECK(absent(*frontend, "/external/own"));
        CHECK(agrees("/external"));
        CHECK(agrees("/"));
        frontend->stop();
    }

    // rsync's write, close, utimens, rename: a data publication committing
    // after the utimens has published keeps the inode's visible mtime rather
    // than stamping its own.
    {
        InterposedTarget target(fs);
        TestGate data_gate;
        target.before_open = [&](const std::string&) { data_gate.enter_and_wait(); };
        auto frontend = node.frontend(node.fuse("utimens"), target);
        GateOpener open_on_exit{data_gate};
        auto handle = frontend->create("/.song.tmp", 0600, getuid(), getgid(), true, true, false);
        const auto payload = pattern(48 * 1024, 5);
        REQUIRE(frontend->write(handle.inode, 0, payload) == payload.size());
        frontend->release(handle.inode, true);
        constexpr int64_t source_mtime = 1600000000000000000LL;
        frontend->utimens("/.song.tmp", source_mtime);
        frontend->rename("/.song.tmp", "/song.mp3", false);
        REQUIRE(data_gate.wait_for_entries(1, 10s));
        REQUIRE(wait_until([&] { return frontend->status().pending_namespace == 0; }, 10s));
        CHECK(fs.getattr("/song.mp3").mtime_ns == source_mtime);
        data_gate.open();
        REQUIRE(frontend->wait_for_idle(30s));
        const auto entry = fs.getattr("/song.mp3");
        CHECK(entry.size == payload.size());
        CHECK(entry.mtime_ns == source_mtime);
        frontend->stop();
    }
}

// The FUSE frontend's data-publication scheduler over one node: notification
// coalescing, closed-file and nearest-retirement priority, fair byte-bounded
// quanta, worker concurrency, yielding to loader admission, retry cursors,
// the progress counter and interrupted fsync waits.
MACHA_TEST("filesystem_fuse", test_fuse_publication_scheduling) {
    FilesystemNode node("fuse-publication-scheduling");
    auto& fs = node.fs();
    const auto extent = node.config().extent_size;

    // Repeated flushes of one durable watermark coalesce into one queued
    // owner; a new watermark is one more request.
    {
        auto frontend = held_frontend(node, node.fuse("notifications"), false);
        auto handle = frontend->create("/notification-watermark.bin", 0600, getuid(), getgid(),
                                       true, true, false);
        REQUIRE(frontend->wait_for_idle(5s));
        frontend.loader->hold();
        const auto first = pattern(64 * 1024, 71);
        REQUIRE(frontend->write(handle.inode, 0, first) == first.size());
        REQUIRE(wait_until([&] { return frontend->status().durability_writes == 1; }, 5s));
        frontend->flush(handle.inode);
        for (size_t i = 0; i < 500; ++i)
            frontend->flush(handle.inode);
        const auto second = pattern(64 * 1024, 72);
        REQUIRE(frontend->write(handle.inode, first.size(), second) == second.size());
        REQUIRE(wait_until([&] { return frontend->status().durability_writes == 2; }, 5s));
        frontend->flush(handle.inode);
        for (size_t i = 0; i < 500; ++i)
            frontend->flush(handle.inode);
        const auto status = frontend->status();
        CHECK(status.data_publication_requests == 2);
        CHECK(status.data_publication_notifications_suppressed == 1000);
        CHECK(status.data_publication_coalesced_queued == 1);
        CHECK(status.data_publication_coalesced_running == 0);
        CHECK(status.data_publication_coalesced_unconfirmed == 0);
        CHECK(status.data_publications_started == 0);
        CHECK(status.pending_data == 1);
        frontend->stop();
    }

    // A closed file queued behind a large open one is selected first.
    {
        auto fuse = node.fuse("closed-first");
        fuse.commit_workers = 1;
        auto frontend = held_frontend(node, fuse, false);
        auto open_large = frontend->create("/open-large.bin", 0644, getuid(), getgid(), false, true,
                                           false);
        auto closed_small = frontend->create("/closed-small.bin", 0644, getuid(), getgid(), false,
                                             true, false);
        REQUIRE(frontend->wait_for_idle(10s));
        const auto large = pattern(8 * extent, 51);
        const auto small = pattern(64 * 1024, 52);
        REQUIRE(frontend->write(open_large.inode, 0, large) == large.size());
        REQUIRE(frontend->write(closed_small.inode, 0, small) == small.size());
        REQUIRE(wait_until([&] { return frontend->status().durability_writes == 2; }, 10s));
        frontend.loader->hold();
        frontend->flush(open_large.inode);
        frontend->release(closed_small.inode, true);
        frontend.loader->release();
        REQUIRE(frontend->wait_for_idle(30s));
        CHECK(frontend->status().data_closed_priority_selections >= 1);
        CHECK(fs.getattr("/closed-small.bin").size == small.size());
        CHECK(fs.getattr("/open-large.bin").size == large.size());
        frontend->release(open_large.inode, true);
        frontend->stop();
    }

    // Under spool pressure the drain selects the nearest closed retirement
    // ahead of earlier queued work, and that retirement admits the blocked
    // writer: backpressure wakes on retirement, never by polling.
    {
        auto fuse = node.fuse("nearest-retirement");
        fuse.commit_workers = 1;
        fuse.publication_quantum_bytes = extent;
        fuse.publication_inflight_bytes = extent;
        fuse.publication_pipeline_bytes = extent;
        fuse.max_spool_bytes = 16 * extent;
        auto frontend = held_frontend(node, fuse, false);
        auto pathological = frontend->create("/open-pathological.bin", 0644, getuid(), getgid(),
                                             false, true, false);
        auto closed_large = frontend->create("/closed-large.bin", 0644, getuid(), getgid(), false,
                                             true, false);
        auto closed_small = frontend->create("/closed-small-2.bin", 0644, getuid(), getgid(),
                                             false, true, false);
        auto blocked_follower = frontend->create("/blocked-follower.bin", 0644, getuid(), getgid(),
                                                 false, true, false);
        REQUIRE(frontend->wait_for_idle(10s));
        frontend.loader->hold();
        // Deliberately bad FIFO order filling the spool exactly to its 50%
        // pressure threshold: an open inode, a large closed file, then a much
        // nearer closed retirement.
        const auto open_bytes = pattern(3 * extent, 81);
        const auto large_bytes = pattern(4 * extent, 82);
        const auto small_bytes = pattern(extent, 83);
        REQUIRE(frontend->write(pathological.inode, 0, open_bytes) == open_bytes.size());
        REQUIRE(frontend->write(closed_large.inode, 0, large_bytes) == large_bytes.size());
        REQUIRE(frontend->write(closed_small.inode, 0, small_bytes) == small_bytes.size());
        REQUIRE(wait_until([&] { return frontend->status().durability_writes == 3; }, 10s));
        frontend->flush(pathological.inode);
        frontend->release(closed_large.inode, true);
        frontend->release(closed_small.inode, true);

        const auto throttled = frontend->status().spool_throttle_waits;
        auto admitted = std::async(std::launch::async, [&] {
            const auto byte = pattern(1, 84);
            return frontend->write(blocked_follower.inode, 0, byte);
        });
        REQUIRE(wait_until(
            [&] { return frontend->status().spool_throttle_waits > throttled; }, 10s));
        CHECK(admitted.wait_for(scaled(0s)) == std::future_status::timeout);
        CHECK(fs.getattr("/closed-small-2.bin").size == 0);
        CHECK(fs.getattr("/closed-large.bin").size == 0);

        frontend.loader->release();
        REQUIRE(admitted.wait_for(scaled(10s)) == std::future_status::ready);
        CHECK(admitted.get() == 1);
        // Its size reaches the namespace with the namespace publication that
        // follows the retirement.
        REQUIRE(wait_until(
            [&] { return fs.getattr("/closed-small-2.bin").size == small_bytes.size(); }, 10s));
        const auto selected = frontend->status();
        CHECK(selected.spool_pressure_publication_sweeps == 1);
        CHECK(selected.data_retirement_priority_selections >= 1);
        frontend->release(pathological.inode, true);
        frontend->release(blocked_follower.inode, true);
        REQUIRE(frontend->wait_for_idle(30s));
        CHECK(fs.getattr("/closed-large.bin").size == large_bytes.size());
        frontend->stop();
    }

    // Quanta are fair and byte bounded: with one quantum in flight, a large
    // generation selected first returns to the tail after its quantum, so a
    // small one queued after it commits first, and the cursor survives every
    // yield without rereading.
    {
        auto fuse = node.fuse("quanta");
        fuse.commit_workers = 4;
        fuse.publication_quantum_bytes = extent;
        fuse.publication_inflight_bytes = fuse.publication_quantum_bytes;
        fuse.publication_pipeline_bytes = extent;
        InterposedTarget target(fs);
        std::mutex order_mutex;
        std::vector<std::string> commit_order;
        target.after_commit = [&](const std::string& path) {
            std::lock_guard lock(order_mutex);
            commit_order.push_back(path);
        };
        auto frontend = held_frontend(node, fuse, target, false);
        auto large_handle =
            frontend->create("/quantum-large.bin", 0644, getuid(), getgid(), false, true, false);
        auto small_handle =
            frontend->create("/quantum-small.bin", 0644, getuid(), getgid(), false, true, false);
        REQUIRE(frontend->wait_for_idle(10s));
        const auto large = pattern(8 * extent, 61);
        const auto small = pattern(64 * 1024, 62);
        REQUIRE(frontend->write(large_handle.inode, 0, large) == large.size());
        REQUIRE(frontend->write(small_handle.inode, 0, small) == small.size());
        REQUIRE(wait_until([&] { return frontend->status().durability_writes == 2; }, 10s));
        frontend.loader->hold();
        frontend->release(large_handle.inode, true);
        frontend->release(small_handle.inode, true);
        frontend.loader->release();
        REQUIRE(frontend->wait_for_idle(30s));
        {
            std::lock_guard lock(order_mutex);
            const std::vector<std::string> small_first{"/quantum-small.bin",
                                                       "/quantum-large.bin"};
            CHECK(commit_order == small_first);
        }
        const auto status = frontend->status();
        CHECK(status.data_publications_started == 2);
        CHECK(status.data_publications_completed == 2);
        CHECK(status.data_publication_yields >= large.size() / fuse.publication_quantum_bytes - 1);
        CHECK(status.data_publication_quanta > status.data_publications_completed);
        CHECK(status.data_publication_peak_active == 1);
        CHECK(status.data_publication_peak_inflight_bytes == fuse.publication_inflight_bytes);
        CHECK(status.data_publication_bytes_read == large.size() + small.size());
        CHECK(fs.getattr("/quantum-large.bin").size == large.size());
        frontend->stop();
    }

    // Open loader writers use every available publication worker, repeated
    // demand coalesces, and the extent executor stays within its bounds.
    {
        auto fuse = node.fuse("open-loaders");
        fuse.commit_workers = 4;
        // Open loader writers are neither viewers nor capped at one publisher.
        fuse.foreground_commit_workers = 1;
        InterposedTarget target(fs);
        TestGate concurrent;
        target.before_open = [&](const std::string&) { concurrent.enter_and_wait(); };
        auto frontend = node.frontend(fuse, target);
        GateOpener open_on_exit{concurrent};
        constexpr size_t files = 4;
        std::vector<FuseOpenHandle> handles;
        for (size_t i = 0; i < files; ++i)
            handles.push_back(frontend->create("/loader-" + std::to_string(i) + ".bin", 0644,
                                               getuid(), getgid(), false, true, false));
        REQUIRE(frontend->wait_for_idle(10s));
        const auto payload = pattern(8 * extent, 37);
        for (const auto& handle : handles)
            REQUIRE(frontend->write(handle.inode, 0, payload) == payload.size());
        REQUIRE(wait_until([&] { return frontend->status().durability_writes == files; }, 10s));
        for (const auto& handle : handles) {
            frontend->flush(handle.inode);
            frontend->flush(handle.inode);
        }
        // Two publications inside the target at once: neither leaves until
        // the gate opens.
        CHECK(concurrent.wait_for_entries(2, 10s));
        concurrent.open();
        REQUIRE(frontend->wait_for_idle(30s));
        const auto status = frontend->status();
        CHECK(status.data_publications_started == files);
        CHECK(status.data_publications_completed == files);
        CHECK(status.data_publication_requests == files);
        CHECK(status.data_publication_notifications_suppressed >= files);
        CHECK(status.data_publication_peak_active >= 2);
        CHECK(status.data_publication_peak_active <= fuse.commit_workers);
        CHECK(status.data_publication_coalesced_queued + status.data_publication_coalesced_running +
                  status.data_publication_coalesced_unconfirmed ==
              0);
        CHECK(status.data_publication_bytes_read == files * payload.size());
        CHECK(status.data_publication_bytes_committed == files * payload.size());
        CHECK(status.data_publication_bytes_confirmed == files * payload.size());
        // The extent executor is the FileSystem's, sized by the node.
        const auto executor_workers = node.config().fuse.commit_workers;
        CHECK(status.extent_executor_workers == executor_workers);
        CHECK(status.extent_executor_submitted >= files * 8);
        CHECK(status.extent_executor_peak_active >= 1);
        CHECK(status.extent_executor_peak_active <= executor_workers);
        CHECK(status.extent_executor_peak_queued <= executor_workers * 2);
        CHECK(status.extent_executor_active == 0);
        CHECK(status.extent_executor_queued == 0);
        for (const auto& handle : handles)
            frontend->release(handle.inode, true);
        frontend->stop();
    }

    // A one-byte overwrite publishes by bounded quanta: spool replay yields
    // before the extent-aligned rebuild, which yields at four checkpoints;
    // the file is never materialised whole.
    {
        auto contents = pattern(4 * extent);
        write_file(fs, "/overwrite.bin", contents);
        auto fuse = node.fuse("overwrite-quanta");
        fuse.commit_workers = 1;
        fuse.publication_quantum_bytes = extent;
        fuse.publication_inflight_bytes = extent;
        fuse.publication_pipeline_bytes = extent;
        auto frontend = node.frontend(fuse);
        auto handle = frontend->open("/overwrite.bin", true, true, false, false);
        const uint8_t replacement = static_cast<uint8_t>(contents[17] ^ 0x39);
        contents[17] = replacement;
        REQUIRE(frontend->write(handle.inode, 17, {&replacement, 1}) == 1);
        frontend->release(handle.inode, true);
        REQUIRE(frontend->wait_for_idle(20s));
        const auto status = frontend->status();
        CHECK(status.data_publications_completed == 1);
        CHECK(status.data_publication_bytes_read == 1);
        CHECK(status.data_publication_completed_spool_bytes_read == 1);
        CHECK(status.data_publication_completed_source_bytes_read == extent + 1);
        CHECK(status.data_publication_completed_reused_extents == 3);
        CHECK(status.data_publication_completed_put_extents == 1);
        CHECK(status.data_publication_yields >= 5);
        CHECK(read_back(fs, "/overwrite.bin", contents.size()) == contents);
        frontend->stop();
    }

    // The no-progress deadline watches events that release retained memory
    // (retired extents, commits), not admitted quanta, which a publication
    // takes far more often.
    {
        auto fuse = node.fuse("progress-counter");
        fuse.commit_workers = 1;
        fuse.publication_quantum_bytes = extent;
        fuse.publication_inflight_bytes = fuse.publication_quantum_bytes;
        fuse.publication_pipeline_bytes = extent;
        auto frontend = node.frontend(fuse);
        // The progress counter is the FileSystem's, shared by every writer.
        const auto progress_before = frontend->diagnostics().data_publication_progress_events;
        auto handle = frontend->create("/progress-counter.bin", 0644, getuid(), getgid(), false,
                                       true, false);
        // Sixteen writes, each its own quantum (a quantum equal to the extent
        // yields after one write), retiring four extents and one commit.
        const auto contents = pattern(4 * extent, 77);
        const auto piece = extent / 4;
        for (size_t offset = 0; offset < contents.size(); offset += piece)
            REQUIRE(frontend->write(handle.inode, offset, {contents.data() + offset, piece}) ==
                    piece);
        frontend->release(handle.inode, true);
        REQUIRE(frontend->wait_for_idle(60s));
        const auto diagnostics = frontend->diagnostics();
        CHECK(diagnostics.data_publications_completed == 1);
        const auto progress = diagnostics.data_publication_progress_events - progress_before;
        CHECK(progress > 0);
        CHECK(diagnostics.data_publication_quanta >= 2 * progress);
        CHECK(fs.getattr("/progress-counter.bin").size == contents.size());
        frontend->stop();
    }

    // A retryable failure of a staged drain keeps the publication and its
    // cursor: the retry resumes after the failed drain rather than rereading
    // the spool from the start.
    {
        auto fuse = node.fuse("transient-cursor");
        fuse.commit_workers = 1;
        fuse.publication_quantum_bytes = extent;
        fuse.publication_inflight_bytes = extent;
        fuse.publication_pipeline_bytes = extent;
        InterposedTarget target(fs);
        std::atomic_bool failed{};
        target.before_drain = [&](uint64_t written) {
            if (written >= extent && !failed.exchange(true))
                throw FsError(EIO, "staged extent put failed");
        };
        auto frontend = node.frontend(fuse, target);
        auto handle = frontend->create("/transient-cursor.bin", 0644, getuid(), getgid(), false,
                                       true, false);
        REQUIRE(frontend->wait_for_idle(10s));
        const auto contents = pattern(3 * extent + 12345, 73);
        REQUIRE(frontend->write(handle.inode, 0, contents) == contents.size());
        frontend->release(handle.inode, true);
        REQUIRE(frontend->wait_for_idle(20s));
        const auto status = frontend->status();
        CHECK(failed.load());
        CHECK(status.backend_failures == 1);
        CHECK(status.data_publications_started == 1);
        CHECK(status.data_publications_completed == 1);
        CHECK(status.data_publication_bytes_read == contents.size());
        CHECK(status.data_publication_completed_spool_bytes_read == contents.size());
        CHECK(read_back(fs, "/transient-cursor.bin", contents.size()) == contents);
        frontend->stop();
    }

    // A running publication yields at its next chunk when its admission
    // asks it to, well inside its quantum, and resumes from its cursor when
    // admitted again.
    {
        auto fuse = node.fuse("yield");
        fuse.commit_workers = 1;
        fuse.publication_quantum_bytes = 4 * extent;
        fuse.publication_inflight_bytes = 4 * extent;
        fuse.publication_pipeline_bytes = extent;
        InterposedTarget target(fs);
        HeldLoaderAdmission* loader = nullptr;
        std::atomic_bool held{};
        target.after_write = [&](uint64_t) {
            if (!held.exchange(true))
                loader->hold();
        };
        auto frontend = held_frontend(node, fuse, target, false);
        loader = frontend.loader;
        auto handle =
            frontend->create("/playback-yield.bin", 0600, getuid(), getgid(), true, true, false);
        REQUIRE(frontend->wait_for_idle(5s));
        const auto payload = pattern(8 * extent);
        REQUIRE(frontend->write(handle.inode, 0, payload) == payload.size());
        frontend->release(handle.inode, true);
        REQUIRE(wait_until([&] { return held.load() && frontend->status().active_data == 0; },
                           10s));
        const auto paused = frontend->status();
        // Held after its first chunk, it stops before taking a whole extent.
        CHECK(paused.data_publication_bytes_read < extent);
        CHECK(paused.pending_data == 1);
        CHECK(paused.data_publications_completed == 0);
        loader->release();
        REQUIRE(frontend->wait_for_idle(10s));
        const auto resumed = frontend->status();
        CHECK(resumed.data_publications_completed == 1);
        CHECK(resumed.data_publication_bytes_read == payload.size());
        CHECK(fs.getattr("/playback-yield.bin").size == payload.size());
        frontend->stop();
    }

    // An fsync waits, with no deadline, for its publication. A stopping node
    // can never finish it, so interrupt_waits() ends the wait with EIO; the
    // data is journalled and publishes after the restart.
    {
        auto frontend = held_frontend(node, node.fuse("fsync-interrupted"), false);
        auto handle =
            frontend->create("/fsync-held.bin", 0644, getuid(), getgid(), false, true, false);
        REQUIRE(frontend->wait_for_idle(10s));
        frontend.loader->hold();
        const auto bytes = pattern(64 * 1024, 91);
        REQUIRE(frontend->write(handle.inode, 0, bytes) == bytes.size());
        auto synced = std::async(std::launch::async, [&] {
            return error_code_of([&] { frontend->fsync(handle.inode); });
        });
        REQUIRE(wait_until([&] { return frontend->status().pending_data == 1; }, 10s));
        CHECK(synced.wait_for(scaled(0s)) == std::future_status::timeout);
        frontend->interrupt_waits();
        REQUIRE(synced.wait_for(scaled(10s)) == std::future_status::ready);
        CHECK(synced.get() == EIO);
        frontend.loader->release();
        frontend->release(handle.inode, true);
        frontend->stop();
    }

    // Production admission (law 2): only the foreground clock, which HTTP
    // playback alone advances, makes a viewer active; loader work is then
    // paced by its weighted share, never stopped. The interactive clock,
    // which the node's own object writes feed, does not count.
    {
        auto fuse = node.fuse("viewer-admission");
        fuse.publication_quiet = 1h;
        ViewerWeightedAdmission admission(fs, fuse);
        const LoaderAdmission::TimePoint t0{};
        CHECK(admission.can_start(t0));
        CHECK(!admission.should_yield(t0));
        CHECK(admission.retry_after(t0) == std::optional<std::chrono::milliseconds>(0ms));
        fs.note_interactive_activity(1);
        CHECK(admission.can_start(t0));
        CHECK(!admission.should_yield(t0));

        fs.note_foreground_activity(1);
        CHECK(admission.can_start(t0));
        admission.started(t0, true);
        CHECK(!admission.should_yield(t0));
        CHECK(admission.should_yield(t0 + 1s));
        admission.finished(t0 + 1s);
        CHECK(!admission.can_start(t0 + 2s));
        const auto retry = admission.retry_after(t0 + 2s);
        REQUIRE(retry.has_value());
        CHECK(*retry > 0ms);
        CHECK(admission.can_start(t0 + 2s + *retry));
    }
}

// The FUSE frontend's admission bounds over one node: spool capacity, the
// spool's soft threshold, the operation journal, pending write payloads and
// operation metadata. Saturation is backpressure that wakes on progress or
// shutdown; it never manufactures ENOSPC for data and never polls.
MACHA_TEST("filesystem_fuse", test_fuse_admission_backpressure) {
    FilesystemNode node("fuse-admission-backpressure");
    auto& fs = node.fs();

    // A write that cannot fit waits rather than failing; spool pressure
    // starts publication of the durable prefix and the writer wakes once
    // that progress frees capacity.
    {
        auto fuse = node.fuse("spool-capacity");
        fuse.commit_workers = 1;
        fuse.max_spool_bytes = 384 * 1024;
        auto frontend = held_frontend(node, fuse);
        auto handle =
            frontend->create("/bounded-spool.bin", 0600, getuid(), getgid(), true, true, false);
        const auto first = pattern(128 * 1024, 41);
        REQUIRE(frontend->write(handle.inode, 0, first) == first.size());
        auto second_write = std::async(std::launch::async, [&] {
            const auto second = pattern(300 * 1024, 42);
            return frontend->write(handle.inode, first.size(), second);
        });
        REQUIRE(wait_until([&] { return frontend->status().spool_throttle_waits >= 1; }, 10s));
        CHECK(second_write.wait_for(scaled(0s)) == std::future_status::timeout);
        frontend.loader->release();
        REQUIRE(second_write.wait_for(scaled(10s)) == std::future_status::ready);
        CHECK(second_write.get() == 300 * 1024);

        const auto pressure = frontend->status();
        CHECK(pressure.spool_limit_bytes == fuse.max_spool_bytes);
        CHECK(pressure.spool_bytes <= pressure.spool_limit_bytes);
        CHECK(pressure.spool_pressure_publication_sweeps == 1);
        CHECK(pressure.spool_publish_rate_bytes_per_second > 0);
        CHECK(pressure.spool_publish_rate_window_bytes >= first.size());
        CHECK(pressure.spool_publish_rate_window_ms > 0);
        const auto spool = *fuse.spool_path / ("inode-" + std::to_string(handle.inode) + ".spool");
        // Once the second write is published too, the spool is retired.
        std::error_code retired;
        const auto spool_size = std::filesystem::file_size(spool, retired);
        CHECK((retired || spool_size <= fuse.max_spool_bytes));
        frontend->release(handle.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));
        frontend->stop();
    }

    // Past the 50% soft threshold, before any whole-file retirement has
    // established a rate, a drained partial quantum of an open file is
    // enough progress to admit the next writer.
    {
        auto fuse = node.fuse("spool-bootstrap");
        fuse.commit_workers = 1;
        fuse.publication_quantum_bytes = 1024 * 1024;
        fuse.publication_inflight_bytes = 1024 * 1024;
        fuse.publication_pipeline_bytes = 1024 * 1024;
        fuse.max_spool_bytes = 8 * 1024 * 1024;
        auto frontend = held_frontend(node, fuse);
        auto first =
            frontend->create("/large-open.bin", 0600, getuid(), getgid(), true, true, false);
        auto follower =
            frontend->create("/follower.bin", 0600, getuid(), getgid(), true, true, false);
        const auto initial = pattern(7 * 1024 * 1024, 61);
        REQUIRE(frontend->write(first.inode, 0, initial) == initial.size());
        auto admitted = std::async(std::launch::async, [&] {
            const auto next = pattern(1024 * 1024, 62);
            return frontend->write(follower.inode, 0, next);
        });
        REQUIRE(wait_until([&] { return frontend->status().spool_throttle_waits >= 1; }, 10s));
        CHECK(admitted.wait_for(scaled(0s)) == std::future_status::timeout);
        frontend.loader->release();
        REQUIRE(admitted.wait_for(scaled(10s)) == std::future_status::ready);
        CHECK(admitted.get() == 1024 * 1024);
        const auto progress = frontend->status();
        CHECK(progress.spool_bytes <= progress.spool_limit_bytes);
        CHECK(progress.spool_pressure_publication_sweeps == 1);
        CHECK(progress.data_publication_yields >= 1);
        // Released by drained partial publication, not by the whole-file
        // retirement rate, which is still unavailable.
        CHECK(progress.spool_publish_rate_bytes_per_second == 0);
        CHECK(progress.data_publications_completed == 0);
        frontend->stop();
    }

    // A permanently stalled publisher blocks the writer without busy-polling
    // or ENOSPC; stopping the mount cancels the wait with EINTR.
    {
        auto fuse = node.fuse("spool-stalled");
        fuse.commit_workers = 1;
        fuse.max_spool_bytes = 384 * 1024;
        auto frontend = held_frontend(node, fuse);
        auto handle =
            frontend->create("/stalled-spool.bin", 0600, getuid(), getgid(), true, true, false);
        const auto first = pattern(256 * 1024, 51);
        REQUIRE(frontend->write(handle.inode, 0, first) == first.size());
        auto blocked = std::async(std::launch::async, [&] {
            const auto second = pattern(256 * 1024, 52);
            return error_code_of([&] { (void)frontend->write(handle.inode, first.size(), second); });
        });
        REQUIRE(wait_until([&] { return frontend->status().spool_throttle_waits >= 1; }, 10s));
        CHECK(blocked.wait_for(scaled(0s)) == std::future_status::timeout);
        const auto pressure = frontend->status();
        CHECK(pressure.spool_bytes <= pressure.spool_limit_bytes);
        CHECK(pressure.spool_pressure_publication_sweeps == 1);
        frontend->stop();
        REQUIRE(blocked.wait_for(scaled(2s)) == std::future_status::ready);
        CHECK(blocked.get() == EINTR);
    }

    // With one durable data operation outstanding, so the journal cannot take
    // its idle reset, new namespace work is refused with ENOSPC at the
    // journal bound; completion records for admitted work may drain past it,
    // but refused work never extends the journal.
    {
        auto fuse = node.fuse("journal-budget");
        fuse.commit_workers = 1;
        fuse.max_operation_journal_bytes = 12 * 1024;
        InterposedTarget target(fs);
        TestGate data_gate;
        target.before_open = [&](const std::string&) { data_gate.enter_and_wait(); };
        auto frontend = node.frontend(fuse, target);
        GateOpener open_on_exit{data_gate};
        auto hold =
            frontend->create("/journal-hold.bin", 0600, getuid(), getgid(), true, true, false);
        const auto payload = pattern(64 * 1024, 91);
        REQUIRE(frontend->write(hold.inode, 0, payload) == payload.size());
        frontend->release(hold.inode, true);
        REQUIRE(data_gate.wait_for_entries(1, 10s));
        const auto journal = *fuse.operation_journal_path;
        REQUIRE(std::filesystem::exists(journal));
        bool refused = false;
        for (size_t i = 0; i < 256 && !refused; ++i) {
            const auto code = error_code_of(
                [&] { frontend->mkdir("/journal-budget-" + std::to_string(i), 0700, getuid(), getgid()); });
            REQUIRE((code == 0 || code == ENOSPC));
            refused = code == ENOSPC;
        }
        REQUIRE(refused);
        REQUIRE(wait_until([&] { return frontend->status().pending_namespace == 0; }, 5s));
        const auto bounded_size = std::filesystem::file_size(journal);
        for (size_t i = 0; i < 8; ++i)
            CHECK(error_code_of([&] {
                      frontend->mkdir("/journal-refused-" + std::to_string(i), 0700, getuid(),
                                      getgid());
                  }) == ENOSPC);
        CHECK(std::filesystem::file_size(journal) == bounded_size);
        data_gate.open();
        frontend->stop();
    }

    // Write payloads not yet in the spool are byte bounded: with one write
    // waiting on the spool, the next waits before copying its payload; both
    // end with EINTR at shutdown.
    {
        auto fuse = node.fuse("write-bytes");
        fuse.commit_workers = 1;
        fuse.max_spool_bytes = 128 * 1024;
        fuse.max_pending_write_bytes = 128 * 1024;
        fuse.timeouts.write = 2s;
        auto frontend = held_frontend(node, fuse);
        auto handle = frontend->create("/write-byte-bound.bin", 0600, getuid(), getgid(), true,
                                       true, false);
        const auto payload = pattern(128 * 1024, 91);
        REQUIRE(frontend->write(handle.inode, 0, payload) == payload.size());
        REQUIRE(wait_until([&] { return frontend->status().durability_writes == 1; }, 5s));
        auto blocked_in_spool = std::async(std::launch::async, [&] {
            return error_code_of(
                [&] { (void)frontend->write(handle.inode, payload.size(), payload); });
        });
        REQUIRE(wait_until(
            [&] { return frontend->status().pending_write_request_bytes == payload.size(); }, 5s));
        auto blocked_before_copy = std::async(std::launch::async, [&] {
            return error_code_of(
                [&] { (void)frontend->write(handle.inode, payload.size() * 2, payload); });
        });
        // Nothing reports a payload waiting for admission, so this one
        // negative check is bounded by time: in the window the second payload
        // must not be admitted beside the first.
        CHECK(blocked_before_copy.wait_for(scaled(100ms)) == std::future_status::timeout);
        const auto bounded = frontend->status();
        CHECK(bounded.pending_write_request_bytes == payload.size());
        CHECK(bounded.peak_pending_write_request_bytes == payload.size());
        CHECK(bounded.pending_write_request_limit_bytes == payload.size());
        frontend->stop();
        REQUIRE(blocked_in_spool.wait_for(scaled(2s)) == std::future_status::ready);
        REQUIRE(blocked_before_copy.wait_for(scaled(2s)) == std::future_status::ready);
        CHECK(blocked_in_spool.get() == EINTR);
        CHECK(blocked_before_copy.get() == EINTR);
        CHECK(frontend->status().peak_pending_write_request_bytes == payload.size());
    }

    // Operation metadata is bounded on the heap: a mutation past the bound
    // waits owning no durable operation, and shutdown ends it with EINTR.
    {
        auto fuse = node.fuse("operation-metadata");
        fuse.commit_workers = 1;
        fuse.max_operation_metadata_bytes = 2048;
        fuse.max_spool_bytes = 1024 * 1024;
        auto frontend = held_frontend(node, fuse);
        auto handle =
            frontend->create("/metadata-bound.bin", 0600, getuid(), getgid(), true, true, false);
        const auto payload = pattern(4096, 37);
        for (size_t i = 0; i < 3; ++i)
            REQUIRE(frontend->write(handle.inode, payload.size() * i, payload) == payload.size());
        auto blocked = std::async(std::launch::async, [&] {
            return error_code_of(
                [&] { (void)frontend->write(handle.inode, payload.size() * 3, payload); });
        });
        REQUIRE(wait_until([&] { return frontend->status().operation_metadata_waits >= 1; }, 5s));
        CHECK(blocked.wait_for(scaled(0s)) == std::future_status::timeout);
        const auto bounded = frontend->status();
        CHECK(bounded.operation_metadata_bytes <= bounded.operation_metadata_limit_bytes);
        CHECK(bounded.peak_operation_metadata_bytes <= bounded.operation_metadata_limit_bytes);
        frontend->stop();
        REQUIRE(blocked.wait_for(scaled(2s)) == std::future_status::ready);
        CHECK(blocked.get() == EINTR);
    }

    // Retiring operation metadata through publication wakes the blocked
    // writer, and every request and operation lease is returned.
    {
        auto fuse = node.fuse("metadata-retirement");
        fuse.commit_workers = 1;
        fuse.max_operation_metadata_bytes = 1024;
        fuse.max_spool_bytes = 1024 * 1024;
        auto frontend = node.frontend(fuse);
        auto handle = frontend->create("/metadata-retirement.bin", 0600, getuid(), getgid(), true,
                                       true, false);
        const auto payload = pattern(4096, 73);
        REQUIRE(frontend->write(handle.inode, 0, payload) == payload.size());
        REQUIRE(wait_until([&] { return frontend->status().durability_writes == 1; }, 5s));
        auto waiting = std::async(std::launch::async, [&] {
            return frontend->write(handle.inode, payload.size(), payload);
        });
        REQUIRE(waiting.wait_for(scaled(10s)) == std::future_status::ready);
        CHECK(waiting.get() == payload.size());
        const auto progressed = frontend->status();
        CHECK(progressed.operation_metadata_waits >= 1);
        CHECK(progressed.data_publications_completed >= 1);
        CHECK(progressed.operation_metadata_bytes <= progressed.operation_metadata_limit_bytes);
        CHECK(progressed.peak_operation_metadata_bytes <= progressed.operation_metadata_limit_bytes);
        frontend->release(handle.inode, true);
        REQUIRE(frontend->wait_for_idle(10s));
        REQUIRE(wait_until(
            [&] {
                const auto memory = node.memory().stats();
                return memory.owner_bytes[static_cast<size_t>(MemoryOwner::fuse_request)] == 0 &&
                       memory.owner_bytes[static_cast<size_t>(MemoryOwner::fuse_operation)] == 0;
            },
            5s));
        frontend->stop();
    }
}

// Publication failures over one node, whose target fails every attempt: a
// long failure run is reported, then the file is parked until membership or
// storage changes or the operator retries or abandons it; a recovered write
// whose path has left the namespace fails once and is not readmitted.
MACHA_TEST("filesystem_fuse", test_fuse_publication_failures_back_off_and_park) {
    FilesystemNode node("fuse-publication-failures");
    auto& fs = node.fs();
    InterposedTarget failing(fs);
    failing.before_open = [](const std::string&) {
        throw FsError(EIO, "no storage backend is online");
    };

    // Crossing the escalation threshold (10 failures) is counted once for the
    // run, before and independently of parking.
    {
        auto fuse = node.fuse("escalation");
        fuse.commit_workers = 1;
        fuse.publication_retry = RetryPolicy{500, 60s, 1ms, 2ms};
        fuse.publication_retry.max_failing_duration = 60s;
        auto frontend = node.frontend(fuse, failing);
        auto handle = frontend->create("/noisy.bin", 0644, getuid(), getgid(), true, true, false);
        const auto payload = pattern(64 * 1024 + 3, 51);
        REQUIRE(frontend->write(handle.inode, 0, payload) == payload.size());
        frontend->release(handle.inode, true);
        REQUIRE(wait_until(
            [&] { return frontend->diagnostics().publications_retrying_persistently == 1; }, 10s));
        REQUIRE(wait_until(
            [&] { return frontend->diagnostics().publication_retries_backed_off >= 30; }, 10s));
        CHECK(frontend->diagnostics().publications_retrying_persistently == 1);
        CHECK(frontend->diagnostics().parked_publications == 0);
        frontend->stop();
    }

    // Past its budget (more than 3 failures) the publication is parked:
    // visible, actionable, quiet and not poisoning the inode. An operator
    // retry resets the budget; an abandon drops the dirty generation.
    {
        auto fuse = node.fuse("park");
        fuse.commit_workers = 1;
        fuse.publication_retry = RetryPolicy{3, 60s, 5ms, 20ms};
        fuse.parked_recheck = 10ms;
        auto frontend = node.frontend(fuse, failing);
        auto handle = frontend->create("/parked.bin", 0644, getuid(), getgid(), true, true, false);
        const auto payload = pattern(64 * 1024 + 3, 44);
        REQUIRE(frontend->write(handle.inode, 0, payload) == payload.size());
        frontend->release(handle.inode, true);
        REQUIRE(wait_until([&] { return frontend->diagnostics().parked_publications == 1; }, 10s));
        const auto parked = frontend->parked_publications();
        REQUIRE(parked.size() == 1);
        CHECK(parked.front().inode == handle.inode);
        CHECK(parked.front().path == "/parked.bin");
        CHECK(parked.front().attempts == 4);
        CHECK(parked.front().pending_bytes == payload.size());
        CHECK(frontend->diagnostics().publication_retries_backed_off == 3);

        // Parked is not scheduled: nothing pending or running, so nothing
        // can attempt it again.
        const auto failures_at_park = frontend->status().backend_failures;
        REQUIRE(frontend->wait_for_idle(5s));
        CHECK(frontend->status().pending_data == 0);
        CHECK(frontend->status().backend_failures == failures_at_park);
        Bytes back(payload.size());
        REQUIRE(frontend->read(handle.inode, 0, back) == back.size());
        CHECK(back == payload);

        REQUIRE(frontend->retry_parked_publication(handle.inode));
        CHECK(frontend->diagnostics().parked_publications == 0);
        REQUIRE(wait_until([&] { return frontend->diagnostics().parked_publications == 1; }, 10s));
        CHECK(frontend->status().backend_failures > failures_at_park);

        // A membership or storage change tries it again with a fresh budget,
        // with nobody asking; with no change it stays parked.
        const auto failures_at_second_park = frontend->status().backend_failures;
        REQUIRE(frontend->wait_for_idle(5s));
        ++failing.reachability;
        REQUIRE(wait_until(
            [&] { return frontend->status().backend_failures >= failures_at_second_park + 4; },
            10s));
        REQUIRE(wait_until([&] { return frontend->diagnostics().parked_publications == 1; }, 10s));
        const auto failures_at_third_park = frontend->status().backend_failures;
        std::this_thread::sleep_for(100ms);
        CHECK(frontend->status().backend_failures == failures_at_third_park);

        CHECK(!frontend->retry_parked_publication(handle.inode + 1000));
        REQUIRE(frontend->abandon_parked_publication(handle.inode));
        CHECK(frontend->parked_publications().empty());
        CHECK(frontend->diagnostics().parked_publications == 0);
        CHECK(frontend->getattr("/parked.bin").size == 0);
        REQUIRE(frontend->wait_for_idle(5s));
        frontend->stop();
    }

    // A recovered write whose path was removed while the node was down (a
    // cluster generation accepted meanwhile) fails once, terminally, and is
    // not readmitted as an unbounded recovery retry.
    {
        auto fuse = node.fuse("terminal-recovery");
        fuse.commit_workers = 1;
        fs.create_file("/healthy.bin", 0644, getuid(), getgid());
        fs.create_file("/removed-before-replay.bin", 0644, getuid(), getgid());
        const auto contents = pattern(256 * 1024 + 17, 91);
        {
            auto frontend = held_frontend(node, fuse);
            auto handle =
                frontend->open("/removed-before-replay.bin", true, true, false, false);
            REQUIRE(frontend->write(handle.inode, 0, contents) == contents.size());
            frontend->release(handle.inode, true);
            REQUIRE(wait_until([&] { return frontend->status().pending_data == 1; }, 10s));
            frontend->stop();
        }
        fs.unlink("/removed-before-replay.bin");
        auto recovered = node.frontend(fuse);
        REQUIRE(wait_until([&] { return recovered->status().backend_failures >= 1; }, 10s));
        REQUIRE(recovered->wait_for_idle(5s));
        const auto settled = recovered->status();
        CHECK(settled.backend_failures == 1);
        CHECK(settled.pending_data == 0);
        CHECK(settled.active_data == 0);
        CHECK(recovered->getattr("/healthy.bin").type == EntryType::file);
        recovered->stop();
    }
}

// A publication writer keeps a retained-memory extent lease across clean
// yields, and scheduling is breadth-first, so unbounded open writers would fill
// the durable-lower budget with partial buffers and none could finish. The
// backlog here is wider than the ledger can hold writers for; the open-writer
// bound makes publication depth-first over the open set and every file
// completes. Its own node, for the small ledger.
MACHA_TEST("filesystem_fuse", test_fuse_publication_backlog_wider_than_ledger_completes) {
    FilesystemNode node("fuse-publication-backlog-width", [](Config& config) {
        // Durable-lower is capacity - control - viewer = 16M: at most 16
        // concurrent extent leases for publication.
        config.runtime.retained_memory_bytes = 32ULL * 1024 * 1024;
        config.runtime.control_memory_reserve_bytes = 8ULL * 1024 * 1024;
        config.runtime.viewer_memory_reserve_bytes = 8ULL * 1024 * 1024;
        config.runtime.loader_memory_reserve_bytes = 8ULL * 1024 * 1024;
        config.runtime.reassembly_memory_reserve_bytes = 4ULL * 1024 * 1024;
    });
    auto& fs = node.fs();
    const auto extent = node.config().extent_size;
    auto fuse = node.fuse("backlog");
    fuse.commit_workers = 2;
    // Quantum == extent size makes every quantum yield mid-extent, leaving a
    // partial buffer, and its lease, on the retained writer.
    fuse.publication_quantum_bytes = extent;
    fuse.publication_inflight_bytes = 2 * fuse.publication_quantum_bytes;
    fuse.publication_pipeline_bytes = extent;
    fuse.publication_no_progress_deadline = 2s;
    // Worst case 4 x (1M buffer + 1M pipeline) = 8M, the loader reserve.
    fuse.publication_max_open_writers = 4;

    auto frontend = held_frontend(node, fuse);
    constexpr size_t files = 20;
    const auto contents = pattern(2 * 1024 * 1024 + 12345, 91);
    for (size_t i = 0; i < files; ++i) {
        auto handle = frontend->create("/backlog-" + std::to_string(i) + ".bin", 0644, getuid(),
                                       getgid(), false, true, false);
        REQUIRE(frontend->write(handle.inode, 0, contents) == contents.size());
        frontend->release(handle.inode, true);
    }
    // Released, publication is work-conserving with the whole width queued.
    frontend.loader->release();
    REQUIRE(frontend->wait_for_idle(180s));
    const auto status = frontend->status();
    const auto diagnostics = frontend->diagnostics();
    CHECK(status.data_publications_started == files);
    CHECK(status.data_publications_completed == files);
    CHECK(diagnostics.parked_publications == 0);
    CHECK(diagnostics.peak_open_publications <= fuse.publication_max_open_writers);
    // The bound must have bitten, or this passes for the wrong reason.
    CHECK(diagnostics.data_publication_selections_under_writer_cap > 0);
    CHECK(diagnostics.open_publications == 0);
    CHECK(diagnostics.backend_failures == 0);
    for (size_t i = 0; i < files; ++i)
        CHECK(fs.getattr("/backlog-" + std::to_string(i) + ".bin").size == contents.size());
    CHECK(publication_bytes(node.memory()) == 0);
    frontend->stop();
}

void append_fuse_journal_records(const std::filesystem::path& journal,
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

void append_fuse_journal_record(const std::filesystem::path& journal,
                                std::span<const uint8_t> payload) {
    const std::array<Bytes, 1> records{Bytes(payload.begin(), payload.end())};
    append_fuse_journal_records(journal, records);
}

// Persisted JournalRecord values.
constexpr uint8_t journal_namespace_published = 4;
constexpr uint8_t journal_namespace_done = 5;
constexpr uint8_t journal_data_done = 7;
constexpr uint8_t journal_namespace_batch = 9;

Bytes fuse_namespace_marker(uint8_t type, uint64_t sequence) {
    Writer payload;
    payload.u8(type);
    payload.u64(sequence);
    return payload.take();
}

std::vector<Bytes> fuse_namespace_markers(uint8_t type, uint64_t first, uint64_t last_exclusive) {
    std::vector<Bytes> records;
    for (uint64_t sequence = first; sequence < last_exclusive; ++sequence)
        records.push_back(fuse_namespace_marker(type, sequence));
    return records;
}

Bytes fuse_data_done(uint64_t inode, uint64_t sequence) {
    Writer payload;
    payload.u8(journal_data_done);
    payload.u64(inode);
    payload.u64(sequence);
    return payload.take();
}

Bytes read_all_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Bytes(raw.begin(), raw.end());
}

void write_all_bytes(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

void restore_directory(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::filesystem::remove_all(to);
    std::filesystem::copy(from, to, std::filesystem::copy_options::recursive);
}

std::vector<uint8_t> fuse_journal_record_types(const std::filesystem::path& journal) {
    const auto bytes = read_all_bytes(journal);
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

// The sequences of the namespace operations a journal holds, in order. A
// node's namespace sequences continue above its committed batch clock, so a
// fresh journal's do not start at one.
std::vector<uint64_t> journal_namespace_sequences(const std::filesystem::path& journal) {
    const auto bytes = read_all_bytes(journal);
    std::vector<uint64_t> sequences;
    const auto scan =
        scan_fuse_journal_frames(bytes, 8, [&](std::span<const uint8_t> payload, size_t) {
            Reader reader(payload);
            if (reader.u8() != 2) // JournalRecord::namespace_op
                return;
            (void)reader.u8(); // kind
            sequences.push_back(reader.u64());
        });
    REQUIRE(scan.discarded_tail == 0);
    return sequences;
}

FilesystemNamespaceMutation namespace_mutation(FilesystemNamespaceMutation::Kind kind,
                                               std::string from, std::string to = {}) {
    FilesystemNamespaceMutation mutation;
    mutation.kind = kind;
    mutation.from = std::move(from);
    mutation.to = std::move(to);
    mutation.mode = kind == FilesystemNamespaceMutation::Kind::mkdir ? 0755 : 0644;
    mutation.uid = getuid();
    mutation.gid = getgid();
    return mutation;
}

// Admits `operations` through a frontend whose publication is held, then
// stops it: the journal holds them unpublished, as at a crash.
void admit_unpublished(FilesystemNode& node, const FuseConfig& fuse,
                       const std::function<void(FuseFrontend&)>& operations) {
    auto frontend = held_frontend(node, fuse);
    operations(*frontend.frontend);
    frontend->stop();
}

// Recovery of the FUSE namespace journal over one node, case by case: each
// case admits operations whose publication is held, then changes the backend
// or the journal as a crash or the cluster would, then recovers. Batching and
// its bounds, idempotent and already-achieved operations, partial marker
// groups, superseded effects, batch identity across a crash, an operator skip
// across a restart, live admission during recovery and ordered mixed
// mutations.
MACHA_TEST("filesystem_fuse", test_fuse_namespace_journal_recovery) {
    FilesystemNode node("fuse-namespace-recovery");
    auto& fs = node.fs();
    const auto make_root = [&](const std::string& name) {
        const auto root = "/" + name;
        fs.mkdir(root, 0755, getuid(), getgid());
        return root;
    };
    using Kind = FilesystemNamespaceMutation::Kind;

    // An ordered backlog publishes in batches of namespace_batch_operations,
    // each journalling its identity, then all published, then all done markers.
    {
        const auto root = make_root("batched");
        auto fuse = node.fuse("batched");
        constexpr size_t operations = 8;
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            for (size_t i = 0; i < operations; ++i)
                frontend.mkdir(root + "/pending-" + std::to_string(i), 0755, getuid(), getgid());
            CHECK(frontend.status().namespace_operations_admitted == operations);
        });
        fuse.namespace_batch_operations = 3;
        const auto generation_before = fs.local_committed_metadata_generation();
        auto recovered = node.frontend(fuse);
        REQUIRE(recovered->wait_for_idle(20s));
        const auto status = recovered->status();
        constexpr size_t batches = (operations + 3 - 1) / 3;
        CHECK(status.namespace_operations_recovered == operations);
        CHECK(status.namespace_publication_attempts == batches);
        CHECK(status.namespace_publication_batches == batches);
        CHECK(status.namespace_operations_batched == operations);
        CHECK(status.namespace_operations_published == operations);
        CHECK(status.namespace_operations_confirmed == operations);
        CHECK(fs.local_committed_metadata_generation() == generation_before + batches);
        CHECK(status.journal_append_batches == batches * 3);
        CHECK(status.journal_records_appended == operations * 2 + batches);
        CHECK(status.journal_durability_barriers == batches * 3);
        recovered->stop();
    }

    // A thousand operations publish in a bounded number of commits.
    {
        const auto root = make_root("thousand");
        auto fuse = node.fuse("thousand");
        constexpr size_t operations = 1000;
        constexpr size_t batch_limit = 256;
        std::vector<FilesystemNamespaceMutation> creates;
        for (size_t i = 0; i < operations; ++i)
            creates.push_back(namespace_mutation(Kind::create, root + "/bulk-" + std::to_string(i)));
        CHECK(fs.apply_namespace_batch(creates).applied == operations);
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            for (size_t i = 0; i < operations; ++i)
                frontend.unlink(root + "/bulk-" + std::to_string(i));
            CHECK(frontend.status().namespace_operations_admitted == operations);
        });
        fuse.namespace_batch_operations = batch_limit;
        const auto generation_before = fs.local_committed_metadata_generation();
        auto recovered = node.frontend(fuse);
        REQUIRE(recovered->wait_for_idle(30s));
        const auto status = recovered->status();
        constexpr size_t batches = (operations + batch_limit - 1) / batch_limit;
        CHECK(status.namespace_operations_recovered == operations);
        CHECK(status.namespace_publication_attempts == batches);
        CHECK(status.namespace_publication_batches == batches);
        CHECK(status.namespace_operations_batched == operations);
        CHECK(status.namespace_operations_published == operations);
        CHECK(status.namespace_operations_confirmed == operations);
        CHECK(status.journal_append_batches == batches * 3);
        CHECK(status.journal_records_appended == operations * 2 + batches);
        CHECK(status.journal_durability_barriers == batches * 3);
        CHECK(fs.local_committed_metadata_generation() == generation_before + batches);
        recovered->stop();
    }

    // The encoded-byte bound is hard: one oversized operation may progress
    // alone, but no second joins it.
    {
        const auto root = make_root("byte-limited");
        auto fuse = node.fuse("byte-limited");
        constexpr size_t operations = 4;
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            for (size_t i = 0; i < operations; ++i)
                frontend.mkdir(root + "/op-" + std::to_string(i), 0755, getuid(), getgid());
        });
        fuse.namespace_batch_bytes = 1;
        auto recovered = node.frontend(fuse);
        REQUIRE(recovered->wait_for_idle(20s));
        const auto status = recovered->status();
        CHECK(status.namespace_publication_batches == operations);
        CHECK(status.namespace_operations_batched == operations);
        CHECK(status.journal_append_batches == operations * 2); // singletons: published, done
        recovered->stop();
    }

    // Unlinks and their parent's rmdir publish as one commit.
    {
        const auto root = make_root("delete-batch");
        fs.mkdir(root + "/doomed", 0755, getuid(), getgid());
        fs.create_file(root + "/doomed/one", 0644, getuid(), getgid());
        fs.create_file(root + "/doomed/two", 0644, getuid(), getgid());
        auto fuse = node.fuse("delete-batch");
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            frontend.unlink(root + "/doomed/one");
            frontend.unlink(root + "/doomed/two");
            frontend.rmdir(root + "/doomed");
        });
        const auto generation_before = fs.local_committed_metadata_generation();
        auto recovered = node.frontend(fuse);
        REQUIRE(recovered->wait_for_idle(20s));
        const auto status = recovered->status();
        CHECK(status.namespace_operations_recovered == 3);
        CHECK(status.namespace_publication_batches == 1);
        CHECK(status.namespace_operations_batched == 3);
        CHECK(status.namespace_operations_published == 3);
        CHECK(status.namespace_operations_confirmed == 3);
        CHECK(status.journal_append_batches == 3); // batch identity, published, done
        CHECK(status.journal_records_appended == 7);
        CHECK(fs.local_committed_metadata_generation() == generation_before + 1);
        CHECK(absent(fs, root + "/doomed"));
        recovered->stop();
    }

    // A batch whose every effect is already present retires with no commit.
    {
        const auto root = make_root("idempotent");
        auto fuse = node.fuse("idempotent");
        constexpr size_t operations = 4;
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            for (size_t i = 0; i < operations; ++i)
                frontend.mkdir(root + "/already-" + std::to_string(i), 0755, getuid(), getgid());
        });
        for (size_t i = 0; i < operations; ++i)
            fs.mkdir(root + "/already-" + std::to_string(i), 0755, getuid(), getgid());
        const auto generation_before = fs.local_committed_metadata_generation();
        auto recovered = node.frontend(fuse);
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
        CHECK(fs.local_committed_metadata_generation() == generation_before);
        recovered->stop();
    }

    // With the middle operation already true in the backend, the identity
    // batch [1,2,3] is refused atomically at op two, so op one publishes
    // alone; then in [2,3] op two is achieved and op three commits.
    {
        const auto root = make_root("valid-prefix");
        auto fuse = node.fuse("valid-prefix");
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            frontend.mkdir(root + "/prefix", 0755, getuid(), getgid());
            frontend.mkdir(root + "/concurrent", 0755, getuid(), getgid());
            frontend.mkdir(root + "/after", 0755, getuid(), getgid());
        });
        fs.mkdir(root + "/concurrent", 0755, getuid(), getgid());
        const auto generation_before = fs.local_committed_metadata_generation();
        auto recovered = node.frontend(fuse);
        REQUIRE(recovered->wait_for_idle(20s));
        const auto status = recovered->status();
        CHECK(status.namespace_operations_recovered == 3);
        CHECK(status.namespace_publication_attempts == 3);
        CHECK(status.namespace_publication_batches == 2);
        CHECK(status.namespace_operations_batched == 2);
        CHECK(status.namespace_operations_published == 3);
        CHECK(status.namespace_operations_confirmed == 3);
        CHECK(status.journal_append_batches == 5);
        CHECK(status.journal_records_appended == 7);
        CHECK(fs.local_committed_metadata_generation() == generation_before + 2);
        for (const auto* name : {"/prefix", "/concurrent", "/after"})
            CHECK(fs.getattr(root + name).type == EntryType::directory);
        recovered->stop();
    }

    // A crash can expose any prefix of a grouped published append; the
    // surviving markers retire their operations and the rest confirm
    // against the backend without a commit.
    {
        const auto root = make_root("published-group");
        auto fuse = node.fuse("published-group");
        constexpr uint64_t operations = 6;
        constexpr uint64_t published_prefix = 3;
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            for (uint64_t i = 0; i < operations; ++i)
                frontend.mkdir(root + "/op-" + std::to_string(i), 0755, getuid(), getgid());
        });
        std::vector<FilesystemNamespaceMutation> committed;
        for (uint64_t i = 0; i < operations; ++i)
            committed.push_back(namespace_mutation(Kind::mkdir, root + "/op-" + std::to_string(i)));
        CHECK(fs.apply_namespace_batch(committed).applied == operations);
        const auto generation_after_commit = fs.local_committed_metadata_generation();
        const auto first = journal_namespace_sequences(*fuse.operation_journal_path).front();
        append_fuse_journal_records(
            *fuse.operation_journal_path,
            fuse_namespace_markers(journal_namespace_published, first, first + published_prefix));
        auto recovered = node.frontend(fuse);
        REQUIRE(recovered->wait_for_idle(20s));
        const auto status = recovered->status();
        CHECK(status.namespace_operations_recovered == operations - published_prefix);
        CHECK(status.namespace_publication_attempts == 0);
        CHECK(status.namespace_operations_published == operations - published_prefix);
        CHECK(status.namespace_operations_confirmed == operations - published_prefix);
        CHECK(fs.local_committed_metadata_generation() == generation_after_commit);
        CHECK(std::filesystem::file_size(*fuse.operation_journal_path) == 8);
        for (uint64_t i = 0; i < operations; ++i)
            CHECK(fs.getattr(root + "/op-" + std::to_string(i)).type == EntryType::directory);
        recovered->stop();
    }

    // With every published marker and a prefix of the done group surviving,
    // construction confirms the published suffix and retires it in one
    // done-marker append; no publication worker is needed.
    {
        const auto root = make_root("done-group");
        auto fuse = node.fuse("done-group");
        constexpr uint64_t operations = 6;
        constexpr uint64_t done_prefix = 2;
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            for (uint64_t i = 0; i < operations; ++i)
                frontend.mkdir(root + "/op-" + std::to_string(i), 0755, getuid(), getgid());
        });
        std::vector<FilesystemNamespaceMutation> committed;
        for (uint64_t i = 0; i < operations; ++i)
            committed.push_back(namespace_mutation(Kind::mkdir, root + "/op-" + std::to_string(i)));
        CHECK(fs.apply_namespace_batch(committed).applied == operations);
        const auto generation_after_commit = fs.local_committed_metadata_generation();
        const auto first = journal_namespace_sequences(*fuse.operation_journal_path).front();
        append_fuse_journal_records(
            *fuse.operation_journal_path,
            fuse_namespace_markers(journal_namespace_published, first, first + operations));
        append_fuse_journal_records(
            *fuse.operation_journal_path,
            fuse_namespace_markers(journal_namespace_done, first, first + done_prefix));
        auto recovered = node.frontend(fuse);
        REQUIRE(recovered->wait_for_idle(20s));
        const auto status = recovered->status();
        CHECK(status.namespace_operations_recovered == 0);
        CHECK(status.namespace_publication_attempts == 0);
        CHECK(status.namespace_operations_published == 0);
        CHECK(status.namespace_operations_confirmed == 0);
        CHECK(status.journal_append_batches == 1);
        CHECK(status.journal_records_appended == operations - done_prefix);
        CHECK(fs.local_committed_metadata_generation() == generation_after_commit);
        CHECK(std::filesystem::file_size(*fuse.operation_journal_path) == 8);
        recovered->stop();
    }

    // A published marker means the backend held the operation at the write
    // floor, so whatever the head shows now is its effect or a legitimate
    // successor: recovery retires it rather than waiting for an effect a
    // superseded operation never shows, and the mount is not left stale.
    {
        const auto root = make_root("superseded");
        auto fuse = node.fuse("superseded");
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            frontend.mkdir(root + "/superseded", 0755, getuid(), getgid());
        });
        const std::array<FilesystemNamespaceMutation, 1> committed{
            namespace_mutation(Kind::mkdir, root + "/superseded")};
        REQUIRE(fs.apply_namespace_batch(committed).applied == 1);
        const auto first = journal_namespace_sequences(*fuse.operation_journal_path).front();
        append_fuse_journal_records(
            *fuse.operation_journal_path,
            fuse_namespace_markers(journal_namespace_published, first, first + 1));
        fs.rmdir(root + "/superseded");
        fs.mkdir(root + "/external", 0755, getuid(), getgid());
        auto recovered = node.frontend(fuse);
        REQUIRE(recovered->wait_for_idle(20s));
        const auto status = recovered->status();
        CHECK(status.pending_namespace == 0);
        CHECK(status.namespace_operations_recovered == 0);
        CHECK(status.namespace_publication_attempts == 0);
        CHECK(std::filesystem::file_size(*fuse.operation_journal_path) == 8);
        CHECK(recovered->getattr(root + "/external").type == EntryType::directory);
        CHECK(absent(*recovered, root + "/superseded"));
        recovered->stop();
    }

    // A batch [create temp, rename temp -> final] committed under its batch
    // identity, but the crash came before its published markers. Re-deriving
    // effects would re-create an empty temp and rename it over the real file;
    // the snapshot's clock says the batch committed, so recovery leaves it.
    {
        const auto root = make_root("batch-identity");
        auto fuse = node.fuse("batch-identity");
        const auto payload = pattern(32 * 1024, 77);
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            auto handle =
                frontend.create(root + "/.film.tmp", 0600, getuid(), getgid(), true, true, false);
            REQUIRE(frontend.write(handle.inode, 0, payload) == payload.size());
            frontend.release(handle.inode, true);
            frontend.rename(root + "/.film.tmp", root + "/film.mkv", false);
            CHECK(frontend.status().namespace_operations_admitted == 2);
        });
        // The journal and spool as the crash leaves them: the operations, the
        // batch record, and no marker.
        const auto journal = *fuse.operation_journal_path;
        const auto first = journal_namespace_sequences(journal).front();
        Writer batch_record;
        batch_record.u8(journal_namespace_batch);
        batch_record.u64(first);
        batch_record.u32(2);
        append_fuse_journal_record(journal, batch_record.data());
        const auto crash_image = node.path() / "batch-identity-crash";
        std::filesystem::copy(*fuse.spool_path, crash_image,
                              std::filesystem::copy_options::recursive);
        {
            auto committing = node.frontend(fuse);
            REQUIRE(committing->wait_for_idle(20s));
            const auto status = committing->status();
            CHECK(status.namespace_publication_batches == 1);
            CHECK(status.namespace_operations_batched == 2);
            committing->stop();
        }
        REQUIRE(fs.getattr(root + "/film.mkv").size == payload.size());
        restore_directory(crash_image, *fuse.spool_path);

        auto recovered = node.frontend(fuse);
        // Operations recovered by identity retire at the next namespace-facing
        // request, as every confirmation does.
        CHECK(recovered->status().pending_namespace == 2);
        CHECK(recovered->getattr(root + "/film.mkv").size == payload.size());
        REQUIRE(recovered->wait_for_idle(20s));
        CHECK(recovered->status().namespace_publication_attempts == 0);
        CHECK(fs.getattr(root + "/film.mkv").size == payload.size());
        CHECK(read_back(fs, root + "/film.mkv", payload.size()) == payload);
        CHECK(absent(fs, root + "/.film.tmp"));
        recovered->stop();
    }

    // rsync's create-temp, write, utimens, rename per file: mixed namespace
    // kinds, renames included, batch into a few commits.
    {
        const auto root = make_root("rsync-batching");
        auto fuse = node.fuse("rsync-batching");
        constexpr size_t files = 8;
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            frontend.mkdir(root + "/album", 0755, getuid(), getgid());
            for (size_t i = 0; i < files; ++i) {
                const auto temp = root + "/album/.track-" + std::to_string(i) + ".tmp";
                const auto final_name = root + "/album/track-" + std::to_string(i) + ".mp3";
                auto handle = frontend.create(temp, 0600, getuid(), getgid(), true, true, false);
                const auto payload = pattern(16 * 1024, static_cast<uint8_t>(i));
                REQUIRE(frontend.write(handle.inode, 0, payload) == payload.size());
                frontend.release(handle.inode, true);
                frontend.utimens(temp, 1700000000000000000LL + static_cast<int64_t>(i));
                frontend.rename(temp, final_name, false);
            }
            CHECK(frontend.status().namespace_operations_admitted == 1 + files * 3);
        });
        auto recovered = node.frontend(fuse);
        REQUIRE(recovered->wait_for_idle(30s));
        const auto status = recovered->status();
        CHECK(status.namespace_operations_published + status.namespace_operations_recovered >=
              1 + files * 3);
        CHECK(status.namespace_publication_batches <= 4);
        for (size_t i = 0; i < files; ++i) {
            const auto entry = fs.getattr(root + "/album/track-" + std::to_string(i) + ".mp3");
            CHECK(entry.size == 16 * 1024);
            CHECK(entry.mtime_ns == 1700000000000000000LL + static_cast<int64_t>(i));
            CHECK(absent(fs, root + "/album/.track-" + std::to_string(i) + ".tmp"));
        }
        recovered->stop();
    }

    // A queued mkdir whose path is concurrently a file can never be reconciled,
    // so it blocks, reported, until the operator skips it by its sequence; the
    // skip journals namespace_done with no published marker, which a restart
    // replays cleanly, and the abandoned mkdir is never claimed as achieved.
    {
        const auto root = make_root("operator-skip");
        auto fuse = node.fuse("operator-skip");
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            frontend.mkdir(root + "/wedge", 0755, getuid(), getgid());
            frontend.mkdir(root + "/wedge2", 0755, getuid(), getgid());
        });
        fs.create_file(root + "/wedge", 0644, getuid(), getgid());
        fs.create_file(root + "/wedge2", 0644, getuid(), getgid());
        const auto blocked_on = [](FuseFrontend& frontend, const std::string& path) {
            return wait_until(
                [&] {
                    const auto blocked = frontend.blocked_namespace_operation();
                    return blocked && blocked->path == path;
                },
                10s);
        };
        {
            auto recovered = node.frontend(fuse);
            REQUIRE(blocked_on(*recovered, root + "/wedge"));
            const auto blocked = recovered->blocked_namespace_operation();
            REQUIRE(blocked.has_value());
            CHECK(blocked->kind == "mkdir");
            CHECK(blocked->error_code == EEXIST);
            CHECK(!recovered->skip_blocked_namespace_operation(blocked->sequence + 1));
            CHECK(recovered->blocked_namespace_operation().has_value());
            REQUIRE(recovered->skip_blocked_namespace_operation(blocked->sequence));
            // The second wedge stops the journal compacting, so the lone
            // namespace_done is still there to replay.
            REQUIRE(blocked_on(*recovered, root + "/wedge2"));
            recovered->stop();
        }
        auto restarted = node.frontend(fuse);
        REQUIRE(blocked_on(*restarted, root + "/wedge2"));
        CHECK(fs.getattr(root + "/wedge").type == EntryType::file);
        CHECK(fs.getattr(root + "/wedge2").type == EntryType::file);
        REQUIRE(restarted->skip_blocked_namespace_operation(
            restarted->blocked_namespace_operation()->sequence));
        REQUIRE(restarted->wait_for_idle(10s));
        CHECK(!restarted->blocked_namespace_operation().has_value());
        CHECK(fs.getattr(root + "/wedge2").type == EntryType::file);
        restarted->stop();
    }

    // Live namespace admission is not blocked while a recovered batch is
    // mid-commit; it publishes in the batch that follows.
    {
        const auto root = make_root("live-during-recovery");
        auto fuse = node.fuse("live-during-recovery");
        constexpr size_t recovered_operations = 4;
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            for (size_t i = 0; i < recovered_operations; ++i)
                frontend.mkdir(root + "/recovered-" + std::to_string(i), 0755, getuid(), getgid());
        });
        TestGate publication_gate;
        std::atomic_bool gated{};
        node.node().set_publication_guard([&](const MetadataPublicationContext&) {
            if (!gated.exchange(true))
                publication_gate.enter_and_wait();
        });
        fuse.namespace_batch_operations = recovered_operations;
        const auto generation_before = fs.local_committed_metadata_generation();
        auto recovered = node.frontend(fuse);
        GateOpener open_on_exit{publication_gate};
        REQUIRE(publication_gate.wait_for_entries(1, 5s));
        recovered->mkdir(root + "/live", 0755, getuid(), getgid());
        CHECK(recovered->inode_for_path(root + "/live").has_value());
        publication_gate.open();
        REQUIRE(recovered->wait_for_idle(20s));
        const auto status = recovered->status();
        CHECK(status.namespace_operations_recovered == recovered_operations);
        CHECK(status.namespace_operations_admitted == 1);
        CHECK(status.namespace_publication_batches == 2);
        CHECK(status.namespace_operations_batched == recovered_operations + 1);
        CHECK(status.namespace_operations_published == recovered_operations + 1);
        CHECK(status.namespace_operations_confirmed == recovered_operations + 1);
        CHECK(fs.local_committed_metadata_generation() == generation_before + 2);
        for (size_t i = 0; i < recovered_operations; ++i)
            CHECK(fs.getattr(root + "/recovered-" + std::to_string(i)).type ==
                  EntryType::directory);
        CHECK(fs.getattr(root + "/live").type == EntryType::directory);
        recovered->stop();
        node.node().set_publication_guard({});
    }

    // Ordered mixed mutations (write, truncate, sparse extend, directory
    // rename, create then unlink, and the root's own inode) are reconstructed
    // from committed metadata plus the journal while publication is held, and
    // converge to the backend once it is not.
    {
        const auto root = make_root("ordered");
        auto fuse = node.fuse("ordered");
        const auto root_mode = fs.getattr("/").mode;
        const auto initial = pattern(192 * 1024 + 31);
        const auto tail = pattern(24 * 1024 + 7);
        Bytes expected(initial.begin(), initial.begin() + 64 * 1024);
        expected.resize(96 * 1024, 0);
        expected.insert(expected.end(), tail.begin(), tail.end());
        const auto old_path = root + "/TV/Buffy/S07E01.mp4";
        const auto new_dir = root + "/TV/Buffy The Vampire Slayer";
        const auto new_path = new_dir + "/S07E01.mp4";
        const auto removed_path = new_dir + "/S07E02.mp4";

        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            frontend.mkdir(root + "/TV", 0755, getuid(), getgid());
            frontend.mkdir(root + "/TV/Buffy", 0755, getuid(), getgid());
            auto first = frontend.create(old_path, 0644, getuid(), getgid(), true, true, false);
            REQUIRE(frontend.write(first.inode, 0, initial) == initial.size());
            frontend.truncate(first.inode, 64 * 1024);
            REQUIRE(frontend.write(first.inode, 96 * 1024, tail) == tail.size());
            frontend.release(first.inode, true);
            frontend.rename(root + "/TV/Buffy", new_dir);
            auto removed = frontend.create(removed_path, 0644, getuid(), getgid(), true, true, false);
            const auto removed_bytes = pattern(32 * 1024 + 3);
            REQUIRE(frontend.write(removed.inode, 0, removed_bytes) == removed_bytes.size());
            frontend.release(removed.inode, true);
            frontend.unlink(removed_path);
            // The root is inode 1, the one stable inode never allocated from
            // next_inode.
            frontend.chmod("/", 0700);
            CHECK(!frontend.inode_for_path(old_path).has_value());
            REQUIRE(frontend.inode_for_path(new_path).has_value());
            CHECK(!frontend.inode_for_path(removed_path).has_value());
            CHECK(frontend.getattr(new_path).size == expected.size());
            CHECK(frontend.getattr("/").mode == 0700);
        });
        {
            auto recovered = held_frontend(node, fuse);
            CHECK(!recovered->inode_for_path(old_path).has_value());
            const auto inode = recovered->inode_for_path(new_path);
            REQUIRE(inode.has_value());
            CHECK(!recovered->inode_for_path(removed_path).has_value());
            CHECK(recovered->getattr(new_path).size == expected.size());
            CHECK(recovered->getattr("/").mode == 0700);
            Bytes local(expected.size());
            REQUIRE(recovered->read(*inode, 0, local) == local.size());
            CHECK(local == expected);
            recovered->stop();
        }
        {
            auto recovered = node.frontend(fuse);
            REQUIRE(recovered->wait_for_idle(20s));
            CHECK(absent(fs, old_path));
            CHECK(absent(fs, removed_path));
            CHECK(fs.getattr(new_path).size == expected.size());
            CHECK(fs.getattr("/").mode == 0700);
            CHECK(read_back(fs, new_path, expected.size()) == expected);
            recovered->stop();
        }
        fs.chmod("/", root_mode);
    }
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

struct JournalFrame {
    size_t offset{};
    size_t size{};
};

std::vector<JournalFrame> fuse_journal_frames(const Bytes& bytes) {
    std::vector<JournalFrame> frames;
    const auto scan = scan_fuse_journal_frames(
        bytes, 8, [&](std::span<const uint8_t> payload, size_t offset) {
            frames.push_back({offset, 4 + payload.size() + 32});
        });
    REQUIRE(scan.discarded_tail == 0);
    REQUIRE(!scan.corrupt_frame_offset.has_value());
    return frames;
}

void corrupt_first_byte(const std::filesystem::path& path) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
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

// Recovery of the FUSE data journal and spool over one node, case by case:
// accepted data and namespace survive a frontend restart and publish; torn,
// corrupt, unbacked or orphaned journal and spool state is resolved (trimmed,
// skipped, quarantined within a byte bound, or dropped for the one inode it
// affects) rather than refused; recovered publications start unprompted at
// the loader worker bound; and every single-frame mutation of a journal still
// starts.
MACHA_TEST("filesystem_fuse", test_fuse_data_journal_recovery) {
    FilesystemNode node("fuse-data-recovery");
    auto& fs = node.fs();
    const auto extent = node.config().extent_size;
    const auto spool_of = [](const FuseConfig& fuse, uint64_t inode) {
        return *fuse.spool_path / ("inode-" + std::to_string(inode) + ".spool");
    };

    // Accepted namespace and data are reconstructable from local state alone
    // after a restart, readable through the mount before publication, and
    // converge to the backend; the journal resets once retired, and the spool
    // and journal live where configured.
    {
        auto fuse = node.fuse("namespace-and-data");
        fuse.commit_workers = 1;
        fuse.operation_journal_path = node.path() / "external-fuse-journal" / "operations.log";
        const auto payload = pattern(384 * 1024 + 17);
        uint64_t inode = 0;
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            frontend.mkdir("/TV", 0755, getuid(), getgid());
            frontend.mkdir("/TV/Buffy", 0755, getuid(), getgid());
            auto handle =
                frontend.create("/TV/Buffy/S07E01.mp4", 0644, getuid(), getgid(), true, true, false);
            inode = handle.inode;
            REQUIRE(frontend.write(inode, 0, payload) == payload.size());
            frontend.release(inode, true);
            CHECK(frontend.inode_for_path("/TV").has_value());
            CHECK(frontend.inode_for_path("/TV/Buffy/S07E01.mp4") == inode);
            CHECK(frontend.getattr("/TV/Buffy/S07E01.mp4").size == payload.size());
        });
        auto recovered = node.frontend(fuse);
        REQUIRE(recovered->inode_for_path("/TV").has_value());
        REQUIRE(recovered->inode_for_path("/TV/Buffy").has_value());
        const auto recovered_inode = recovered->inode_for_path("/TV/Buffy/S07E01.mp4");
        REQUIRE(recovered_inode.has_value());
        CHECK(recovered->getattr("/TV/Buffy/S07E01.mp4").size == payload.size());
        Bytes local(payload.size());
        REQUIRE(recovered->read(*recovered_inode, 0, local) == local.size());
        CHECK(local == payload);
        REQUIRE(recovered->wait_for_idle(15s));
        CHECK(fs.getattr("/TV/Buffy/S07E01.mp4").size == payload.size());
        CHECK(read_back(fs, "/TV/Buffy/S07E01.mp4", payload.size()) == payload);
        CHECK(std::filesystem::file_size(*fuse.operation_journal_path) == 8);
        CHECK(std::filesystem::exists(*fuse.spool_path));
        CHECK(!std::filesystem::exists(node.config().state_path / "fuse-spool"));
        recovered->stop();
    }

    // A torn tail, and a complete final frame failing its checksum, are both
    // a crash mid-append: trimmed to the last good frame.
    for (const bool complete_frame : {false, true}) {
        auto fuse = node.fuse(complete_frame ? "checksum-tail" : "torn-tail");
        const std::string path = complete_frame ? "/pending-checksum" : "/pending-torn";
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            frontend.mkdir(path, 0755, getuid(), getgid());
        });
        const auto journal = *fuse.operation_journal_path;
        const auto valid_size = std::filesystem::file_size(journal);
        REQUIRE(valid_size > 8);
        Bytes tail(complete_frame ? 37 : 3, 0);
        if (complete_frame) {
            tail[3] = 1;
            tail[4] = 0xff;
        }
        {
            std::ofstream out(journal, std::ios::binary | std::ios::app);
            out.write(reinterpret_cast<const char*>(tail.data()),
                      static_cast<std::streamsize>(tail.size()));
            REQUIRE(out.good());
        }
        REQUIRE(std::filesystem::file_size(journal) == valid_size + tail.size());
        auto recovered = held_frontend(node, fuse);
        REQUIRE(recovered->inode_for_path(path).has_value());
        CHECK(std::filesystem::file_size(journal) == valid_size);
        recovered->stop();
    }

    // A spool file no journal record refers to is preserved as an orphan,
    // not deleted; orphans are kept within max_orphan_bytes, oldest dropped.
    {
        auto fuse = node.fuse("orphans");
        fuse.max_orphan_bytes = 1024;
        const auto spool_dir = *fuse.spool_path;
        std::filesystem::create_directories(spool_dir);
        const auto unattributed = spool_dir / "inode-999.spool";
        const auto older = spool_dir / "inode-900.spool.orphan.1";
        const auto newer = spool_dir / "inode-901.spool.orphan.2";
        {
            std::ofstream out(unattributed, std::ios::binary | std::ios::trunc);
            out << "unattributed bytes";
            std::ofstream a(older, std::ios::binary | std::ios::trunc);
            a << std::string(800, 'a');
            std::ofstream b(newer, std::ios::binary | std::ios::trunc);
            b << std::string(800, 'b');
        }
        const auto now = std::filesystem::file_time_type::clock::now();
        std::filesystem::last_write_time(older, now - 2h);
        std::filesystem::last_write_time(newer, now - 1h);
        node.frontend(fuse)->stop();

        CHECK(!std::filesystem::exists(unattributed));
        CHECK(!std::filesystem::exists(older));
        uint64_t orphan_bytes = 0;
        bool preserved = false;
        for (const auto& entry : std::filesystem::directory_iterator(spool_dir)) {
            const auto name = entry.path().filename().string();
            if (name.find(".orphan.") == std::string::npos)
                continue;
            orphan_bytes += entry.file_size();
            if (name.starts_with("inode-999.spool.orphan.")) {
                std::ifstream in(entry.path(), std::ios::binary);
                const std::string bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
                CHECK(bytes == "unattributed bytes");
                preserved = true;
            }
        }
        CHECK(preserved);
        CHECK(orphan_bytes <= fuse.max_orphan_bytes);
    }

    // An idle dirty inode keeps its spool path and bytes but not its
    // descriptor; reopening for append continues the same spool, and
    // recovery publishes both writes. Recovery holds a bounded number of
    // spool descriptors however many inodes are dirty.
    {
        auto fuse = node.fuse("idle-spool");
        const auto first = pattern(64 * 1024 + 13, 17);
        const auto second = pattern(48 * 1024 + 7, 93);
        Bytes expected = first;
        expected.insert(expected.end(), second.begin(), second.end());
        // The descriptor bound is measurable only through /proc.
#if defined(__linux__)
        constexpr size_t dirty_inodes = 16;
#else
        constexpr size_t dirty_inodes = 0;
#endif
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            auto created = frontend.create("/append-after-idle.bin", 0644, getuid(), getgid(), true,
                                           true, false);
            const auto inode = created.inode;
            REQUIRE(frontend.write(inode, 0, first) == first.size());
            frontend.release(inode, true);
#if defined(__linux__)
            const auto before_reopen = linux_open_fd_count();
#endif
            auto reopened = frontend.open("/append-after-idle.bin", true, true, true, false);
            REQUIRE(reopened.inode == inode);
            REQUIRE(frontend.write(inode, 0, second, true) == second.size());
            frontend.release(inode, true);
#if defined(__linux__)
            CHECK(linux_open_fd_count() <= before_reopen + 2);
            const auto before_dirty = linux_open_fd_count();
#endif
            Bytes local(expected.size());
            REQUIRE(frontend.read(inode, 0, local) == local.size());
            CHECK(local == expected);
            for (size_t i = 0; i < dirty_inodes; ++i) {
                auto handle = frontend.create("/fd-" + std::to_string(i), 0644, getuid(), getgid(),
                                              true, true, false);
                const Bytes byte{static_cast<uint8_t>(i)};
                REQUIRE(frontend.write(handle.inode, 0, byte) == byte.size());
                frontend.release(handle.inode, true);
            }
#if defined(__linux__)
            CHECK(linux_open_fd_count() <= before_dirty + 8);
#endif
        });
#if defined(__linux__)
        const auto before_recovery = linux_open_fd_count();
        {
            auto recovered = held_frontend(node, fuse);
            CHECK(linux_open_fd_count() <= before_recovery + 8);
            recovered->stop();
        }
#endif
        auto recovered = node.frontend(fuse);
        REQUIRE(recovered->wait_for_idle(30s));
#if defined(__linux__)
        CHECK(linux_open_fd_count() <= before_recovery + 8);
#endif
        CHECK(fs.getattr("/append-after-idle.bin").size == expected.size());
        CHECK(read_back(fs, "/append-after-idle.bin", expected.size()) == expected);
        for (size_t i = 0; i < dirty_inodes; ++i)
            CHECK(fs.getattr("/fd-" + std::to_string(i)).size == 1);
        recovered->stop();
    }

    // Restored spool is provenance, not a scheduling class: recovered
    // publications start with no new FUSE request, ignore the interactive
    // clock the node's own object writes feed, and use the loader worker
    // bound rather than recovery_commit_workers.
    {
        auto fuse = node.fuse("recovered-loader");
        fuse.commit_workers = 4;
        fuse.recovery_commit_workers = 2;
        constexpr size_t files = 4;
        const auto payload = pattern(4 * extent);
        for (size_t i = 0; i < files; ++i)
            fs.create_file("/recover-" + std::to_string(i) + ".bin", 0644, getuid(), getgid());
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            for (size_t i = 0; i < files; ++i) {
                auto handle = frontend.open("/recover-" + std::to_string(i) + ".bin", true, true,
                                            false, false);
                REQUIRE(frontend.write(handle.inode, 0, payload) == payload.size());
                frontend.release(handle.inode, true);
            }
            const auto staged = frontend.status();
            CHECK(staged.pending_data >= files);
            CHECK(staged.active_data == 0);
        });
        fs.note_interactive_activity(1);
        InterposedTarget target(fs);
        TestGate concurrent;
        target.before_open = [&](const std::string&) { concurrent.enter_and_wait(); };
        auto recovered = node.frontend(fuse, target);
        GateOpener open_on_exit{concurrent};
        CHECK(concurrent.wait_for_entries(fuse.recovery_commit_workers + 1, 10s));
        const auto running = recovered->status();
        CHECK(running.active_recovery_data > fuse.recovery_commit_workers);
        CHECK(running.active_recovery_data <= fuse.commit_workers);
        CHECK(running.pending_recovery_data <= running.pending_data);
        concurrent.open();
        REQUIRE(recovered->wait_for_idle(30s));
        CHECK(recovered->status().data_publication_peak_active <= fuse.commit_workers);
        for (size_t i = 0; i < files; ++i)
            CHECK(fs.getattr("/recover-" + std::to_string(i) + ".bin").size == payload.size());
        recovered->stop();
    }

    // A checksum-valid data_done whose data_published record did not survive
    // the crash is authoritative and the frontend starts.
    {
        auto fuse = node.fuse("authoritative-done");
        uint64_t inode = 0;
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            auto handle = frontend.create("/done-authoritative.bin", 0644, getuid(), getgid(), true,
                                          true, false);
            inode = handle.inode;
            const Bytes payload{0x10, 0x20, 0x30, 0x40};
            REQUIRE(frontend.write(inode, 0, payload) == payload.size());
            frontend.release(inode, true);
        });
        const auto journal = *fuse.operation_journal_path;
        // The intended pre-publication state: a data op, nothing published.
        const auto record_types = fuse_journal_record_types(journal);
        CHECK(std::find(record_types.begin(), record_types.end(), 3) != record_types.end());
        CHECK(std::find(record_types.begin(), record_types.end(), 6) == record_types.end());
        CHECK(std::find(record_types.begin(), record_types.end(), 7) == record_types.end());
        // A new inode's first data operation has sequence 1.
        append_fuse_journal_record(journal, fuse_data_done(inode, 1));
        auto recovered = held_frontend(node, fuse);
        CHECK(recovered->inode_for_path("/done-authoritative.bin").has_value());
        recovered->stop();
    }

    // A completion marker with no operation behind it retires nothing: it is
    // skipped and counted, and the frontend starts.
    {
        auto fuse = node.fuse("unbacked-done");
        node.frontend(fuse)->stop();
        append_fuse_journal_record(*fuse.operation_journal_path, fuse_data_done(999, 1));
        auto recovered = node.frontend(fuse);
        CHECK(recovered->diagnostics().journal_recovery_skipped_frames == 1);
        recovered->stop();
    }

    // The spool and journal are a per-inode WAL: a lost dirty spool, or one
    // failing its checksum, invalidates that generation alone, which recovery
    // abandons, falling back to the last committed manifest.
    for (const bool corrupt : {false, true}) {
        auto fuse = node.fuse(corrupt ? "corrupt-spool" : "missing-spool");
        const std::string path = corrupt ? "/recover-corrupt.bin" : "/recover-missing.bin";
        const auto published = corrupt ? pattern(768 * 1024, 61) : Bytes{};
        if (corrupt)
            write_file(fs, path, published);
        else
            fs.create_file(path, 0600, getuid(), getgid());
        uint64_t inode = 0;
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            auto handle = frontend.open(path, true, true, false, false);
            inode = handle.inode;
            const auto replacement = pattern(corrupt ? published.size() : 128 * 1024, 62);
            REQUIRE(frontend.write(inode, 0, replacement) == replacement.size());
            frontend.release(inode, true);
        });
        const auto spool = spool_of(fuse, inode);
        REQUIRE(std::filesystem::exists(spool));
        if (corrupt)
            corrupt_first_byte(spool);
        else
            REQUIRE(std::filesystem::remove(spool));
        auto recovered = node.frontend(fuse);
        REQUIRE(recovered->wait_for_idle(10s));
        CHECK(fs.getattr(path).size == published.size());
        if (corrupt)
            CHECK(read_back(fs, path, published.size()) == published);
        CHECK(!std::filesystem::exists(spool) || std::filesystem::file_size(spool) == 0);
        recovered->stop();
    }

    // A recovered publication whose file has left the namespace is abandoned
    // (journalled) on the first boot, freeing its spool and the journal; the
    // second boot sees nothing.
    {
        auto fuse = node.fuse("removed-file");
        fuse.commit_workers = 1;
        fs.create_file("/gone.bin", 0600, getuid(), getgid());
        uint64_t inode = 0;
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            auto handle = frontend.open("/gone.bin", true, true, false, false);
            inode = handle.inode;
            const auto payload = pattern(256 * 1024, 73);
            REQUIRE(frontend.write(inode, 0, payload) == payload.size());
            frontend.release(inode, true);
        });
        const auto spool = spool_of(fuse, inode);
        REQUIRE(std::filesystem::file_size(spool) == 256 * 1024);
        REQUIRE(std::filesystem::file_size(*fuse.operation_journal_path) > 8);
        fs.unlink("/gone.bin");
        {
            auto recovered = node.frontend(fuse);
            REQUIRE(recovered->wait_for_idle(10s));
            const auto diagnostics = recovered->diagnostics();
            CHECK(diagnostics.publications_abandoned == 1);
            CHECK(diagnostics.parked_publications == 0);
            CHECK(recovered->status().pending_data == 0);
            CHECK(!recovered->inode_for_path("/gone.bin").has_value());
            recovered->stop();
        }
        CHECK(!std::filesystem::exists(spool) || std::filesystem::file_size(spool) == 0);
        CHECK(std::filesystem::file_size(*fuse.operation_journal_path) == 8);
        auto again = node.frontend(fuse);
        REQUIRE(again->wait_for_idle(10s));
        CHECK(again->diagnostics().publications_abandoned == 0);
        CHECK(again->status().pending_data == 0);
        again->stop();
    }

    // Truncate the journal at every frame boundary, or drop, duplicate or
    // corrupt any frame: the frontend starts every time; mid-journal
    // corruption is quarantined, an EOF checksum failure trimmed; and the
    // pristine journal still recovers everything.
    {
        auto fuse = node.fuse("fuzz");
        fuse.commit_workers = 1;
        fs.mkdir("/fz-seeded", 0755, getuid(), getgid());
        uint64_t inode_a = 0;
        admit_unpublished(node, fuse, [&](FuseFrontend& frontend) {
            frontend.mkdir("/fz", 0755, getuid(), getgid());
            auto a = frontend.create("/fz/a.bin", 0600, getuid(), getgid(), true, true, false);
            inode_a = a.inode;
            const auto bytes_a = pattern(48 * 1024, 71);
            REQUIRE(frontend.write(inode_a, 0, bytes_a) == bytes_a.size());
            frontend.release(inode_a, true);
            frontend.rename("/fz/a.bin", "/fz/b.bin", false);
            frontend.mkdir("/fz/sub", 0755, getuid(), getgid());
            auto c = frontend.create("/fz-seeded/c.bin", 0600, getuid(), getgid(), true, true, false);
            const auto bytes_c = pattern(8 * 1024, 72);
            REQUIRE(frontend.write(c.inode, 0, bytes_c) == bytes_c.size());
            frontend.release(c.inode, true);
        });
        const auto spool_dir = *fuse.spool_path;
        const auto journal = *fuse.operation_journal_path;
        // Marker kinds the loader must also tolerate losing or duplicating.
        const std::array<Bytes, 2> markers{
            fuse_namespace_marker(journal_namespace_published,
                                  journal_namespace_sequences(journal).front()),
            fuse_data_done(inode_a, 1)};
        append_fuse_journal_records(journal, markers);

        const auto pristine = read_all_bytes(journal);
        const auto frames = fuse_journal_frames(pristine);
        REQUIRE(frames.size() >= 6);
        const auto pristine_spool = node.path() / "spool-pristine";
        std::filesystem::copy(spool_dir, pristine_spool, std::filesystem::copy_options::recursive);

        size_t variants = 0;
        const auto run_variant = [&](const std::string& name, const Bytes& bytes,
                                     const std::function<void(FuseFrontend&)>& check) {
            restore_directory(pristine_spool, spool_dir);
            write_all_bytes(journal, bytes);
            HeldFrontend frontend;
            try {
                frontend = held_frontend(node, fuse);
            } catch (const std::exception& e) {
                const auto message = name + ": frontend refused to start: " + e.what();
                ::macha::test::check(false, message.c_str(), __FILE__, __LINE__);
                return;
            }
            check(*frontend.frontend);
            frontend->stop();
            ++variants;
        };
        for (size_t i = 0; i < frames.size(); ++i) {
            const auto& frame = frames[i];
            const auto label = "frame " + std::to_string(i) + " of " + std::to_string(frames.size());
            const Bytes truncated(pristine.begin(),
                                  pristine.begin() + static_cast<ptrdiff_t>(frame.offset + frame.size));
            // Recovery may append (re-journalled descriptors, markers for
            // dropped operations), so only the start is asserted.
            run_variant("truncate after " + label, truncated, [&](FuseFrontend&) {
                CHECK(std::filesystem::file_size(journal) >= 8);
            });
            Bytes dropped = pristine;
            dropped.erase(dropped.begin() + static_cast<ptrdiff_t>(frame.offset),
                          dropped.begin() + static_cast<ptrdiff_t>(frame.offset + frame.size));
            run_variant("drop " + label, dropped, [](FuseFrontend&) {});
            Bytes duplicated = pristine;
            duplicated.insert(duplicated.begin() + static_cast<ptrdiff_t>(frame.offset + frame.size),
                              pristine.begin() + static_cast<ptrdiff_t>(frame.offset),
                              pristine.begin() + static_cast<ptrdiff_t>(frame.offset + frame.size));
            run_variant("duplicate " + label, duplicated, [](FuseFrontend&) {});
            Bytes corrupted = pristine;
            corrupted[frame.offset + 4] ^= 0x5a;
            run_variant("corrupt " + label, corrupted, [&](FuseFrontend& frontend) {
                const auto status = frontend.diagnostics();
                if (i + 1 == frames.size()) {
                    CHECK(status.journal_recovery_quarantined_bytes == 0);
                } else {
                    CHECK(status.journal_recovery_quarantined_bytes ==
                          pristine.size() - frame.offset);
                    bool quarantined = false;
                    for (const auto& entry : std::filesystem::directory_iterator(spool_dir))
                        if (entry.path().filename().string().starts_with("operations.log.corrupt."))
                            quarantined = true;
                    CHECK(quarantined);
                }
            });
        }
        CHECK(variants == frames.size() * 4);

        restore_directory(pristine_spool, spool_dir);
        write_all_bytes(journal, pristine);
        auto recovered = held_frontend(node, fuse);
        CHECK(recovered->inode_for_path("/fz/b.bin").has_value());
        CHECK(recovered->inode_for_path("/fz/sub").has_value());
        CHECK(recovered->diagnostics().journal_recovery_skipped_frames == 0);
        recovered->stop();
    }
}

// Waits until the maintenance pass is parked in its wait with no wake-up for
// five consecutive looks: done with all it can at the clock's current time.
void settle_maintenance(Service& service) {
    const auto deadline = Clock::now() + 20s;
    uint64_t seen = service.maintenance_wakeups();
    int quiet = 0;
    while (Clock::now() < deadline && quiet < 5) {
        std::this_thread::sleep_for(20ms);
        const auto wakeups = service.maintenance_wakeups();
        const bool parked = std::string_view(service.maintenance_stage()) == "wait";
        quiet = parked && wakeups == seen ? quiet + 1 : 0;
        seen = wakeups;
    }
    REQUIRE(quiet >= 5);
}

// The deadline the parked pass sleeps to, in ms from when it slept; -1 when
// only an event can wake it.
int64_t parked_wait_ms(const Service& service) {
    const auto diagnostic = service.maintenance_sleep_diagnostic();
    const auto at = diagnostic.find("wait_ms=");
    REQUIRE(at != std::string::npos);
    return std::stoll(diagnostic.substr(at + 8));
}

Service make_service(const Config& config, const ClusterKeys& keys,
                     std::shared_ptr<MaintenanceClock> clock,
                     Service::MaintenanceStageHook stage_hook = {}) {
    ServiceInstruments instruments;
    instruments.clock = std::move(clock);
    return Service(config, keys, test_durability_window, NodeRuntime::StartupStageHook{},
                   std::move(stage_hook), Service::StartupStallHandler{}, std::move(instruments));
}

// The management API over a Service: Status reports the published FUSE
// frontend's counters beside the node's metadata convergence and withdraws
// them with the frontend; a blocked namespace operation and a parked
// publication are each reported and acted on over HTTP. Integrated: the
// behaviour is the Service's wiring of the subsystem registry into its routes.
MACHA_TEST("filesystem_fuse", test_management_api_reports_and_acts_on_fuse_state) {
    TestService fixture("fuse-management-api", ConfigProfile::isolated);
    auto& config = fixture.config();
    config.catalogue.api.enabled = true;
    config.catalogue.api.listen = "127.0.0.1";
    config.catalogue.api.port = free_port();
    config.fuse.publication_quiet = 0ms;
    auto& service = fixture.start();
    const auto fuse_config = [&](std::string_view name) {
        auto fuse = config.fuse;
        fuse.spool_path = fixture.path() / "fuse" / std::string(name);
        fuse.operation_journal_path = *fuse.spool_path / "operations.log";
        return fuse;
    };

    {
        auto frontend =
            make_fuse_frontend(service.filesystem(), service.resources().memory, fuse_config("status"));
        service.registry().publish_fuse(frontend);
        frontend->mkdir("/status-counter", 0755, getuid(), getgid());
        REQUIRE(frontend->wait_for_idle(10s));
        REQUIRE(wait_until([&] {
            const auto current = service.metadata_convergence_diagnostics();
            return !current.scheduled && current.runs_scheduled == current.runs_completed;
        }));

        const auto response =
            raw_http_get(config.catalogue.api.port, "/api/v1/status", bearer_header(service));
        CHECK(response.find("HTTP/1.1 200") != std::string::npos);
        const auto diagnostics_root = status_diagnostics_response(config.catalogue.api.port, service);
        const auto* diagnostics = diagnostics_root.find("diagnostics");
        REQUIRE(diagnostics != nullptr);

        const auto* data_store = diagnostics->find("data_store");
        REQUIRE(data_store != nullptr);
        CHECK(data_store->find("available")->asBool());
        REQUIRE(data_store->find("loose_reaffirmation_fast_paths") != nullptr);
        REQUIRE(data_store->find("loose_reaffirmation_full_validations") != nullptr);

        const auto* retained_memory = diagnostics->find("retained_memory");
        REQUIRE(retained_memory != nullptr);
        CHECK(retained_memory->find("capacity_bytes")->asUInt64() ==
              config.runtime.retained_memory_bytes);
        REQUIRE(retained_memory->find("owners") != nullptr);
        REQUIRE(retained_memory->find("owners")->find("rpc_frame") != nullptr);
        REQUIRE(retained_memory->find("owners")->find("fuse_operation") != nullptr);

        const auto* filesystem = diagnostics->find("filesystem");
        REQUIRE(filesystem != nullptr);
        CHECK(filesystem->find("available")->asBool());
        CHECK(filesystem->find("namespace_operations_admitted")->asUInt64() == 1);
        CHECK(filesystem->find("namespace_publication_batches")->asUInt64() == 1);
        CHECK(filesystem->find("namespace_operations_batched")->asUInt64() == 1);
        CHECK(filesystem->find("namespace_operations_published")->asUInt64() == 1);
        CHECK(filesystem->find("namespace_operations_confirmed")->asUInt64() == 1);
        // A new live inode appends its descriptor and operation before
        // returning, durable under one barrier, then published and done.
        CHECK(filesystem->find("journal_append_batches")->asUInt64() == 4);
        CHECK(filesystem->find("journal_records_appended")->asUInt64() == 4);
        CHECK(filesystem->find("journal_durability_barriers")->asUInt64() == 3);
        CHECK(filesystem->find("spool_bytes")->asUInt64() == 0);
        CHECK(filesystem->find("spool_limit_bytes")->asUInt64() == config.fuse.max_spool_bytes);
        CHECK(filesystem->find("pending_write_request_limit_bytes")->asUInt64() ==
              config.fuse.max_pending_write_bytes);
        CHECK(filesystem->find("data_publication_pipeline_limit_bytes")->asUInt64() ==
              2 * config.extent_size);
        CHECK(filesystem->find("operation_metadata_limit_bytes")->asUInt64() ==
              config.fuse.max_operation_metadata_bytes);
        for (const auto* field :
             {"spool_publish_rate_bytes_per_second", "spool_publish_rate_window_bytes",
              "spool_publish_rate_window_ms", "spool_throttle_waits", "spool_throttle_wait_ms",
              "pending_write_request_bytes", "peak_pending_write_request_bytes",
              "extent_executor_workers", "extent_executor_queued", "extent_executor_active",
              "extent_executor_peak_queued", "extent_executor_peak_active",
              "extent_executor_submitted", "inode_count", "peak_inode_count",
              "reclaimed_inode_count", "data_publication_requests",
              "data_publication_notifications_suppressed", "spool_pressure_publication_sweeps",
              "data_publication_coalesced_queued", "data_publication_coalesced_running",
              "data_publication_coalesced_unconfirmed", "data_publications_started",
              "data_publications_completed", "data_publication_peak_active",
              "data_publication_quanta", "data_publication_yields",
              "data_publication_peak_inflight_bytes", "data_publication_peak_pipeline_extents",
              "data_closed_priority_selections", "data_retirement_priority_selections",
              "data_publication_bytes_read", "data_publication_bytes_committed",
              "data_publication_bytes_confirmed", "data_publication_completed_spool_bytes_read",
              "data_publication_completed_source_bytes_read",
              "data_publication_completed_reused_extents",
              "data_publication_completed_put_extents", "data_overlay_read_queries",
              "data_overlay_ranges_examined", "data_overlay_descriptors_copied",
              "retained_data_operations", "retained_data_operation_bytes",
              "retained_overlay_ranges", "retained_overlay_bytes",
              "retained_publication_operations", "retained_publication_operation_bytes",
              "operation_metadata_bytes", "peak_operation_metadata_bytes",
              "operation_metadata_waits", "retained_durability_tickets",
              "data_publication_inflight_bytes"}) {
            if (filesystem->find(field) == nullptr)
                std::cerr << "status filesystem lacks " << field << "\n";
            CHECK(filesystem->find(field) != nullptr);
        }

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
              service.metadata_server().known_generation());
        CHECK(!convergence->find("scheduled")->asBool());

        service.registry().withdraw_fuse(frontend.get());
        const auto detached = status_diagnostics_response(config.catalogue.api.port, service);
        CHECK(!detached.find("diagnostics")->find("filesystem")->find("available")->asBool());
        frontend->stop();
    }

    const auto port = config.catalogue.api.port;
    const auto admin = bearer_header(service);

    // A namespace operation the cluster refuses blocks the journal behind it.
    // The operator reads it and skips it by the sequence read, so a skip aimed
    // at an operation that has since changed fails closed.
    {
        const std::string route = "/api/v1/manage/filesystem/blocked-namespace-operation";
        const auto skip = [&](const std::string& query) {
            return http_request(port, "POST", route + "/skip" + query, admin);
        };

        // No mount: nothing blocked, nothing to skip.
        CHECK(http_request(port, "GET", route, admin).status == 404);
        CHECK(skip("?sequence=1").status == 409);

        struct Wedge {
            const char* name;
            const char* kind;
            const char* path;
            const char* destination;
        };
        for (const Wedge& wedge : {Wedge{"skip-mkdir", "mkdir", "/wedge", nullptr},
                                   Wedge{"skip-rename", "rename", "/moved", "/moved-away"}}) {
            const bool rename = wedge.destination != nullptr;
            const auto fuse = fuse_config(wedge.name);
            if (rename)
                service.filesystem().mkdir("/moved", 0755, getuid(), getgid());
            {
                auto admission = std::make_unique<HeldLoaderAdmission>();
                admission->hold();
                auto frontend = make_fuse_frontend(service.filesystem(),
                                                   service.resources().memory, fuse,
                                                   std::move(admission));
                if (rename)
                    frontend->rename("/moved", "/moved-away");
                else
                    frontend->mkdir("/wedge", 0755, getuid(), getgid());
                frontend->stop();
            }
            // The namespace changes underneath the journalled operation.
            if (rename)
                service.filesystem().rmdir("/moved");
            else
                service.filesystem().create_file("/wedge", 0644, getuid(), getgid());
            auto recovered =
                make_fuse_frontend(service.filesystem(), service.resources().memory, fuse);
            service.registry().publish_fuse(recovered);
            REQUIRE(wait_until(
                [&] { return recovered->blocked_namespace_operation().has_value(); }, 10s));
            const auto blocked = recovered->blocked_namespace_operation();
            REQUIRE(blocked.has_value());

            CHECK(http_request(port, "POST", route, admin).status == 405);
            const auto read = http_request(port, "GET", route, admin);
            REQUIRE(read.status == 200);
            const auto json = read.json();
            CHECK(json.find("sequence")->asUInt64() == blocked->sequence);
            CHECK(json.find("kind")->asString() == wedge.kind);
            CHECK(json.find("path")->asString() == wedge.path);
            CHECK(json.find("error_code")->asInt64() == blocked->error_code);
            CHECK(json.find("error_message")->asString() == blocked->error_message);
            CHECK(json.find("blocked_for_ms") != nullptr);
            if (rename)
                CHECK(json.find("destination_path")->asString() == wedge.destination);
            else
                CHECK(json.find("destination_path") == nullptr);

            // A skip names the operation it means, or changes nothing.
            CHECK(http_request(port, "GET", route + "/skip?sequence=1", admin).status == 405);
            for (const char* query : {"", "?sequence="}) {
                const auto refused = skip(query);
                CHECK(refused.status == 400);
                CHECK(refused.has("missing_sequence"));
            }
            for (const char* query : {"?sequence=first", "?sequence=7th"}) {
                const auto refused = skip(query);
                CHECK(refused.status == 400);
                CHECK(refused.has("bad_sequence"));
            }
            const auto other = skip("?sequence=" + std::to_string(blocked->sequence + 1));
            CHECK(other.status == 409);
            CHECK(other.has("not_blocked"));
            CHECK(recovered->blocked_namespace_operation().has_value());

            CHECK(skip("?sequence=" + std::to_string(blocked->sequence)).status == 204);
            REQUIRE(recovered->wait_for_idle(10s));
            CHECK(!recovered->blocked_namespace_operation().has_value());
            CHECK(http_request(port, "GET", route, admin).status == 404);
            if (rename)
                CHECK(absent(service.filesystem(), "/moved-away"));
            else
                CHECK(service.filesystem().getattr("/wedge").type == EntryType::file);
            service.registry().withdraw_fuse(recovered.get());
            recovered->stop();
        }
    }

    // A publication past its retry budget is parked. The operator lists it
    // and retries or abandons it by inode.
    {
        const std::string route = "/api/v1/manage/filesystem/parked-publications";
        const auto act = [&](const std::string& tail) {
            return http_request(port, "POST", route + "/" + tail, admin);
        };

        // No mount: nothing parked, nothing to act on.
        const auto none = http_request(port, "GET", route, admin);
        CHECK(none.status == 200);
        CHECK(none.json().find("parked")->asArray().empty());
        CHECK(act("7/retry").status == 409);
        CHECK(act("7/abandon").status == 409);

        InterposedTarget failing(service.filesystem());
        failing.before_open = [](const std::string&) {
            throw FsError(EIO, "no storage backend is online");
        };
        auto fuse = fuse_config("parked");
        fuse.commit_workers = 1;
        fuse.publication_retry = RetryPolicy{3, 60s, 5ms, 20ms};
        auto frontend = std::make_shared<FuseFrontend>(
            service.filesystem(), service.resources().memory, fuse,
            std::make_unique<ViewerWeightedAdmission>(service.filesystem(), fuse), failing);
        service.registry().publish_fuse(frontend);
        auto handle = frontend->create("/parked.bin", 0644, getuid(), getgid(), true, true, false);
        const auto payload = pattern(64 * 1024 + 3, 44);
        REQUIRE(frontend->write(handle.inode, 0, payload) == payload.size());
        frontend->release(handle.inode, true);
        REQUIRE(wait_until([&] { return frontend->diagnostics().parked_publications == 1; }, 10s));
        const auto parked = frontend->parked_publications();
        REQUIRE(parked.size() == 1);
        const auto inode = std::to_string(handle.inode);

        CHECK(http_request(port, "POST", route, admin).status == 405);
        const auto listed = http_request(port, "GET", route, admin);
        REQUIRE(listed.status == 200);
        const auto listing = listed.json();
        const auto& items = listing.find("parked")->asArray();
        REQUIRE(items.size() == 1);
        CHECK(items.front().find("inode")->asUInt64() == handle.inode);
        CHECK(items.front().find("path")->asString() == "/parked.bin");
        CHECK(items.front().find("error_code")->asInt64() == parked.front().error_code);
        CHECK(items.front().find("error_message")->asString() == parked.front().error_message);
        CHECK(items.front().find("attempts")->asUInt64() == parked.front().attempts);
        CHECK(items.front().find("pending_bytes")->asUInt64() == payload.size());
        CHECK(items.front().find("failing_for_ms") != nullptr);
        CHECK(items.front().find("parked_for_ms") != nullptr);

        // An action names an inode and is one of retry or abandon.
        CHECK(http_request(port, "GET", route + "/" + inode + "/retry", admin).status == 405);
        CHECK(act(inode).status == 404);
        CHECK(act(inode + "/forget").status == 404);
        for (const char* bad : {"first/retry", "7th/retry"}) {
            const auto refused = act(bad);
            CHECK(refused.status == 400);
            CHECK(refused.has("bad_inode"));
        }
        const auto elsewhere = std::to_string(handle.inode + 1000);
        CHECK(act(elsewhere + "/retry").status == 409);
        CHECK(act(elsewhere + "/abandon").status == 409);
        CHECK(frontend->parked_publications().size() == 1);

        // A retry gives it a fresh budget; the target still fails, so it
        // parks again.
        const auto failures_at_park = frontend->status().backend_failures;
        CHECK(act(inode + "/retry").status == 204);
        REQUIRE(wait_until(
            [&] {
                return frontend->status().backend_failures > failures_at_park &&
                       frontend->diagnostics().parked_publications == 1;
            },
            10s));

        // An abandon drops the unpublished bytes.
        CHECK(act(inode + "/abandon").status == 204);
        CHECK(frontend->parked_publications().empty());
        CHECK(http_request(port, "GET", route, admin).json().find("parked")->asArray().empty());
        CHECK(frontend->getattr("/parked.bin").size == 0);
        REQUIRE(frontend->wait_for_idle(5s));
        service.registry().withdraw_fuse(frontend.get());
        frontend->stop();
    }
}

// A burst of empty-directory removals, as `find -depth -type d -empty
// -delete` issues, with concurrent readers, keeps the node serving. Every
// removal is a metadata change and every metadata change asks the
// media-information service to prune. Integrated: the load lands on the
// Service's maintenance pass and media-information service at once.
MACHA_HEAVY_TEST("filesystem_fuse", test_removing_empty_directories_in_a_burst_keeps_the_node_up) {
    TestService fixture("fuse-empty-directory-burst");
    auto& config = fixture.config();
    config.replication = 1;
    config.metadata_write_copies = 1;
    config.write_copies = 1;
    config.fuse.publication_quiet = 0ms;
    auto& service = fixture.start();
    auto frontend = make_fuse_frontend(service.filesystem(), service.resources().memory, config.fuse);

    constexpr int titles = 60;
    frontend->mkdir("/Movies", 0755, getuid(), getgid());
    for (int i = 0; i < titles; ++i) {
        const auto title = "/Movies/Title " + std::to_string(i);
        frontend->mkdir(title, 0755, getuid(), getgid());
        frontend->mkdir(title + "/Subs", 0755, getuid(), getgid());
        frontend->mkdir(title + "/Featurettes", 0755, getuid(), getgid());
        if (i % 2 == 0) {
            auto handle = frontend->create(title + "/film.mkv", 0644, getuid(), getgid(), false,
                                           true, false);
            const auto bytes = pattern(4096 + static_cast<size_t>(i), static_cast<uint8_t>(i));
            REQUIRE(frontend->write(handle.inode, 0, bytes) == bytes.size());
            frontend->release(handle.inode, true);
        }
    }
    REQUIRE(frontend->wait_for_idle(60s));

    std::mutex unexpected_mutex;
    std::vector<std::string> unexpected;
    const auto note_unexpected = [&](std::string what) {
        std::lock_guard lock(unexpected_mutex);
        unexpected.push_back(std::move(what));
    };
    // jthreads, so a failure below stops and joins them on the way out.
    std::vector<std::jthread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&, r](std::stop_token stop) {
            while (!stop.stop_requested()) {
                for (int i = r; i < titles; i += 4) {
                    const auto title = "/Movies/Title " + std::to_string(i);
                    for (const auto& path : {title, title + "/Subs", title + "/Featurettes"}) {
                        try {
                            (void)frontend->getattr(path);
                            (void)frontend->readdir(path);
                        } catch (const FsError& error) {
                            if (error.code() != ENOENT)
                                note_unexpected(path + ": FsError " + std::to_string(error.code()) +
                                                " " + error.what());
                        } catch (const std::exception& error) {
                            note_unexpected(path + ": " + error.what());
                        }
                    }
                }
                try {
                    (void)frontend->readdir("/Movies");
                } catch (const std::exception& error) {
                    note_unexpected(std::string("/Movies: ") + error.what());
                }
            }
        });
    }

    // Bottom-up, as find -depth does: each directory is listed, then removed.
    for (int i = 0; i < titles; ++i) {
        const auto title = "/Movies/Title " + std::to_string(i);
        (void)frontend->readdir(title);
        frontend->rmdir(title + "/Subs");
        frontend->rmdir(title + "/Featurettes");
        if (i % 2 == 1)
            frontend->rmdir(title);
    }
    // The readers go first: their own requests keep the request broker busy,
    // and idle is only ever observed with none in flight.
    for (auto& reader : readers)
        reader.request_stop();
    readers.clear();
    REQUIRE(frontend->wait_for_idle(60s));
    for (const auto& what : unexpected)
        std::cerr << "unexpected: " << what << "\n";
    CHECK(unexpected.empty());

    const auto listed = frontend->readdir("/Movies");
    size_t directories = 0;
    for (const auto& [name, attributes] : listed)
        if (name != "." && name != "..")
            ++directories;
    CHECK(directories == titles / 2);
    CHECK(service.filesystem().getattr("/Movies/Title 0/film.mkv").type == EntryType::file);
    frontend->stop();
}

// With no peer and no new input, the maintenance pass parks rather than
// rediscovering an unavailable write floor on an interval; a peer joining
// wakes it at once and the floor forms; identical membership exchanges
// (heartbeats) then wake neither node. On a manual clock, so a pass that
// polled would wake when the clock moves past its interval. Integrated: the
// wake under test is a peer joining over the network.
MACHA_TEST("filesystem_fuse", test_disconnected_maintenance_sleeps_until_peer_event) {
    TestCluster cluster;
    const auto& keys = cluster.keys();
    const auto p1 = free_port();
    const auto p2 = free_port();
    auto c1 = config_for(cluster.path() / "event-maint-n1", cluster.keyfile(), p1);
    auto c2 =
        config_for(cluster.path() / "event-maint-n2", cluster.keyfile(), p2, {{"127.0.0.1", p1}});
    c1.replication = c2.replication = 1;
    c1.metadata_write_copies = c2.metadata_write_copies = 2;
    c1.heartbeat = c2.heartbeat = 50ms;
    c1.dead_after = c2.dead_after = 500ms;
    c1.maintenance.no_progress_backoff = c2.maintenance.no_progress_backoff = 30s;
    // Event-driven parking, not credit accrual: the startup slice is
    // immediately affordable, so no credit deadline is pending.
    c1.maintenance.initial_bandwidth = c2.maintenance.initial_bandwidth = 1024ULL * 1024 * 1024;
    c1.maintenance.cpu_target = c2.maintenance.cpu_target = 1.0;
    c1.catalogue.scanner.enabled = c2.catalogue.scanner.enabled = false;
    c1.catalogue.api.enabled = c2.catalogue.api.enabled = false;

    auto clock = std::make_shared<ManualMaintenanceClock>();
    auto s1 = make_service(c1, keys, clock);
    auto s2 = make_service(c2, keys, clock);
    s1.start();
    REQUIRE(s1.node().wait_local_state_ready(5s));
    (void)s1.filesystem();

    // Steps the clock through the pass's own short deadlines (formation
    // settling, the GC quiet window) until every pass sleeps to its long
    // back-off or to an event alone, then moves the clock well inside that
    // back-off: nothing may wake.
    const auto check_parked = [&](std::initializer_list<Service*> services) {
        const auto longest = std::chrono::milliseconds(c1.maintenance.no_progress_backoff).count();
        bool parked = false;
        for (int step = 0; step < 10 && !parked; ++step) {
            int64_t next = -1;
            for (auto* service : services) {
                settle_maintenance(*service);
                const auto wait_ms = parked_wait_ms(*service);
                if (wait_ms >= 0 && wait_ms < longest)
                    next = next < 0 ? wait_ms : std::min(next, wait_ms);
            }
            parked = next < 0;
            if (!parked)
                clock->advance(std::chrono::milliseconds(next));
        }
        REQUIRE(parked);
        std::vector<std::pair<uint64_t, std::string>> before;
        for (auto* service : services)
            before.emplace_back(service->maintenance_wakeups(),
                                service->maintenance_sleep_diagnostic());
        clock->advance(5s);
        size_t index = 0;
        for (auto* service : services) {
            settle_maintenance(*service);
            if (service->maintenance_wakeups() != before[index].first)
                std::cerr << "node " << index << " woke while parked: before "
                          << before[index].second << ", after "
                          << service->maintenance_sleep_diagnostic() << ", stage "
                          << service->maintenance_stage() << "\n";
            CHECK(service->maintenance_wakeups() == before[index].first);
            ++index;
        }
    };

    check_parked({&s1});

    // A peer event bypasses the outstanding retry deadline and forms the floor
    // without the clock moving.
    s2.start();
    REQUIRE(wait_until(
        [&] {
            return s1.local_state().replica().current().generation > 1 &&
                   s2.local_state().replica().current().generation > 1;
        },
        5s));

    // Heartbeats keep arriving in real time while the passes settle.
    check_parked({&s1, &s2});
    s2.stop();
    s1.stop();
}

// A coalesced burst of deletes schedules exactly one follow-up convergence
// run and arms one wake at the exact end of the garbage grace: the objects
// survive to the last moment before it and are collected once it passes.
// On a manual clock, so the boundary is stepped rather than slept through.
// Integrated: convergence demand, the garbage inventory and the collector
// meet only in the Service's maintenance pass.
MACHA_TEST("filesystem_fuse", test_coalesced_delete_burst_wakes_at_exact_garbage_grace) {
    TestCluster cluster(ConfigProfile::isolated);
    auto config = cluster.node_config("coalesced-garbage-grace");
    config.replication = 1;
    config.metadata_write_copies = 1;
    config.maintenance.garbage_grace = 750ms;
    config.maintenance.foreground_quiet = 10ms;
    config.maintenance.no_progress_backoff = 500ms;

    TestGate repair_gate;
    std::atomic_bool gate_repair{};
    std::atomic_bool gate_once{};
    auto clock = std::make_shared<ManualMaintenanceClock>();
    auto service = make_service(config, cluster.keys(), clock, [&](std::string_view stage) {
        if (stage == "metadata-repair-begin" && gate_repair.load(std::memory_order_acquire) &&
            !gate_once.exchange(true, std::memory_order_acq_rel))
            repair_gate.enter_and_wait();
    });
    GateOpener open_on_exit{repair_gate};
    service.start();
    auto& fs = service.filesystem();

    const auto converged = [&] {
        const auto diagnostics = service.metadata_convergence_diagnostics();
        return !diagnostics.scheduled && diagnostics.runs_scheduled == diagnostics.runs_completed;
    };
    std::vector<ObjectId> retired_ids;
    for (size_t index = 0; index < 3; ++index) {
        const auto path = "/garbage-grace-" + std::to_string(index);
        write_file(fs, path, pattern(64 * 1024 + index, static_cast<uint8_t>(index + 7)));
        const auto entry = fs.getattr(path);
        REQUIRE(entry.extents.size() == 1);
        retired_ids.push_back(entry.extents.front().id);
        REQUIRE(service.local_state().data().has(retired_ids.back()));
    }
    settle_maintenance(service);
    REQUIRE(converged());

    const auto before = service.metadata_convergence_diagnostics();
    gate_repair.store(true, std::memory_order_release);
    fs.unlink("/garbage-grace-0");
    REQUIRE(repair_gate.wait_for_entries(1, 5s));
    fs.unlink("/garbage-grace-1");
    fs.unlink("/garbage-grace-2");

    const auto snapshot = service.metadata_manager().snapshot();
    int64_t latest_retirement{};
    for (const auto& id : retired_ids) {
        const auto found = std::find_if(snapshot.garbage.begin(), snapshot.garbage.end(),
                                        [&](const GarbageRef& garbage) { return garbage.id == id; });
        REQUIRE(found != snapshot.garbage.end());
        latest_retirement = std::max(latest_retirement, found->retired_at_ns);
        CHECK(service.local_state().data().has(id));
    }
    repair_gate.open();
    settle_maintenance(service);
    const auto held = [&] {
        return std::all_of(retired_ids.begin(), retired_ids.end(), [&](const ObjectId& id) {
            return service.local_state().data().has(id);
        });
    };
    const auto collected = [&] {
        return std::none_of(retired_ids.begin(), retired_ids.end(), [&](const ObjectId& id) {
            return service.local_state().data().has(id);
        });
    };

    // The last moment before the grace ends.
    const auto grace_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config.maintenance.garbage_grace)
            .count();
    const auto to_grace = latest_retirement + grace_ns - clock->wall_ns();
    REQUIRE(to_grace > 1'000'000);
    clock->advance(std::chrono::nanoseconds(to_grace - 1'000'000));
    settle_maintenance(service);
    CHECK(held());
    const auto before_grace = service.metadata_convergence_diagnostics();
    // On mismatch, report the direction: an extra run means a metadata or
    // topology event arrived after the follow-up began; a missing one means
    // the unlinks coalesced differently.
    if (before_grace.runs_scheduled != before.runs_scheduled + 2 ||
        before_grace.runs_completed != before.runs_completed + 2)
        std::cerr << "convergence before grace: runs_scheduled " << before.runs_scheduled << " -> "
                  << before_grace.runs_scheduled << ", runs_completed " << before.runs_completed
                  << " -> " << before_grace.runs_completed << ", events "
                  << before.events_received << " -> " << before_grace.events_received
                  << ", epoch requested/completed " << before_grace.requested_epoch << "/"
                  << before_grace.completed_epoch << ", scheduled " << before_grace.scheduled
                  << "\n";
    CHECK(before_grace.runs_scheduled == before.runs_scheduled + 2);
    CHECK(before_grace.runs_completed == before.runs_completed + 2);
    // The pass sleeps to the grace's end exactly: the one millisecond left.
    CHECK(parked_wait_ms(service) >= 0);
    CHECK(parked_wait_ms(service) <= 1);

    // Past the grace the pass wakes at once; collection follows its GC quiet
    // window, which that wake arms.
    clock->advance(1ms);
    settle_maintenance(service);
    CHECK(held());
    CHECK(parked_wait_ms(service) >= 0);
    CHECK(parked_wait_ms(service) <=
          std::chrono::milliseconds(config.maintenance.foreground_quiet).count());
    // The collector also ages an unclaimed object by its file's own time,
    // which the clock seam does not cover: step the pass's deadlines until
    // that age has passed as well.
    REQUIRE(wait_until(
        [&] {
            if (collected())
                return true;
            const auto wait_ms = parked_wait_ms(service);
            if (wait_ms >= 0 && wait_ms < 10'000)
                clock->advance(std::chrono::milliseconds(std::max<int64_t>(wait_ms, 1)));
            settle_maintenance(service);
            return collected();
        },
        10s));
    settle_maintenance(service);
    CHECK(collected());
    const auto current = service.metadata_manager().snapshot();
    CHECK(std::none_of(current.garbage.begin(), current.garbage.end(),
                       [&](const GarbageRef& garbage) {
                           return std::find(retired_ids.begin(), retired_ids.end(), garbage.id) !=
                                  retired_ids.end();
                       }));
    const auto after = service.metadata_convergence_diagnostics();
    CHECK(converged());
    CHECK(after.runs_scheduled >= before.runs_scheduled + 3);
    CHECK(after.runs_scheduled <= before.runs_scheduled + 5);
    service.stop();
}

} // namespace
