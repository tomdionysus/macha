// SPDX-License-Identifier: GPL-3.0-or-later
//
// The torrent's disk backend (src/torrent_disk_io.cpp), driven through
// libtorrent's disk_interface exactly as a session drives it: jobs issued on
// one thread, completions arriving on the io_context.

#include "test_framework.hpp"
#include "test_support.hpp"

#include "crypto.hpp"
#include "data_work.hpp"
#include "io_pressure.hpp"
#include "torrent_disk_io.hpp"
#include "torrent_extent_journal.hpp"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/address.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/disk_buffer_holder.hpp>
#include <libtorrent/disk_interface.hpp>
#include <libtorrent/disk_observer.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/io_context.hpp>
#include <libtorrent/performance_counters.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_handle.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/storage_defs.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/version.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace macha {
namespace {
namespace lt = libtorrent;
using test_support::TempDir;

constexpr int block = lt::default_block_size;

// Non-repeating: a periodic pattern gives equal extents equal object ids, which
// content addressing then (correctly) deduplicates.
std::vector<char> pattern_bytes(size_t size, int seed) {
    std::vector<char> out(size);
    uint64_t state = 0x9e3779b97f4a7c15ULL ^ static_cast<uint64_t>(seed);
    for (size_t i = 0; i < size; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        out[i] = static_cast<char>(state & 0xff);
    }
    return out;
}

lt::file_storage layout(const std::vector<int64_t>& sizes, int piece_length) {
    lt::file_storage files;
    for (size_t i = 0; i < sizes.size(); ++i)
        files.add_file("payload/f" + std::to_string(i), sizes[i]);
    files.set_piece_length(piece_length);
    files.set_num_pieces(static_cast<int>((files.total_size() + piece_length - 1) / piece_length));
    return files;
}

size_t open_descriptors() {
    size_t count = 0;
#ifdef __APPLE__
    const char* dir_path = "/dev/fd";
#else
    const char* dir_path = "/proc/self/fd";
#endif
    if (DIR* dir = ::opendir(dir_path)) {
        while (::readdir(dir)) ++count;
        ::closedir(dir);
    }
    return count;
}

struct Harness {
    lt::io_context ios;
    lt::settings_pack settings;
    lt::counters counters;
    lt::file_storage files;
    lt::aux::vector<lt::download_priority_t, lt::file_index_t> priorities;
#if LIBTORRENT_VERSION_NUM >= 20100
    lt::renamed_files renamed;
#endif
    std::unique_ptr<lt::disk_interface> disk;
    lt::storage_holder storage;
    std::string save_path;

    Harness(lt::file_storage f, const std::filesystem::path& dir, TorrentDiskHooks hooks,
            int queue_limit = 1024 * 1024)
        : files(std::move(f)), save_path(dir.string()) {
        settings.set_int(lt::settings_pack::max_queued_disk_bytes, queue_limit);
        disk = macha_disk_io_constructor(std::move(hooks))(ios, settings, counters);
#if LIBTORRENT_VERSION_NUM >= 20100
        lt::storage_params params(files, renamed, save_path, "", lt::storage_mode_sparse, priorities,
                                  lt::sha1_hash(), true, false);
#else
        lt::storage_params params(files, nullptr, save_path, lt::storage_mode_sparse, priorities,
                                  lt::sha1_hash());
#endif
        storage = disk->new_torrent(params, {});
    }

    ~Harness() {
        storage.reset();
        disk->abort(true);
        pump_for(50ms);
    }

    lt::storage_index_t index() const { return static_cast<lt::storage_index_t>(storage); }

    void pump_for(std::chrono::milliseconds duration) {
        ios.restart();
        ios.run_for(duration);
    }

    template <class Predicate>
    bool pump_until(Predicate done, std::chrono::milliseconds timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            pump_for(5ms);
        }
        return true;
    }

    // Writes `data` as the torrent's whole payload, block by block, and waits
    // for every completion.
    bool write_all(const std::vector<char>& data) {
        int pending = 0;
        bool failed = false;
        const int piece_length = files.piece_length();
        for (int64_t offset = 0; offset < static_cast<int64_t>(data.size()); offset += block) {
            lt::peer_request request;
            request.piece = lt::piece_index_t(static_cast<int>(offset / piece_length));
            request.start = static_cast<int>(offset % piece_length);
            request.length = static_cast<int>(std::min<int64_t>(block, static_cast<int64_t>(data.size()) - offset));
            ++pending;
            disk->async_write(index(), request, data.data() + offset, nullptr,
                              [&](const lt::storage_error& error) {
                                  failed = failed || bool(error.ec);
                                  --pending;
                              });
        }
        disk->submit_jobs();
        return pump_until([&] { return pending == 0; }) && !failed;
    }

    std::optional<lt::sha1_hash> hash(lt::piece_index_t piece) {
        std::optional<lt::sha1_hash> out;
        bool done = false;
        disk->async_hash(index(), piece, {}, lt::disk_interface::v1_hash,
                         [&](lt::piece_index_t, const lt::sha1_hash& h, const lt::storage_error& error) {
                             if (!error.ec) out = h;
                             done = true;
                         });
        disk->submit_jobs();
        pump_until([&] { return done; });
        return out;
    }

    lt::status_t check(const lt::add_torrent_params* resume) {
        std::optional<lt::status_t> status;
        disk->async_check_files(index(), resume, {},
                                [&](lt::status_t s, const lt::storage_error&) { status = s; });
        disk->submit_jobs();
        pump_until([&] { return status.has_value(); });
        return status.value_or(lt::status_t{});
    }
};

lt::sha1_hash expected_piece_hash(const std::vector<char>& data, int piece_length, int piece) {
    const auto start = static_cast<size_t>(piece) * static_cast<size_t>(piece_length);
    const auto end = std::min(data.size(), start + static_cast<size_t>(piece_length));
    lt::hasher hasher;
    hasher.update({data.data() + start, static_cast<std::ptrdiff_t>(end - start)});
    return hasher.final();
}

std::vector<char> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

#if LIBTORRENT_VERSION_NUM >= 20100
const lt::status_t status_ok{};
const lt::status_t status_full_check = lt::disk_status::need_full_check;
#else
const lt::status_t status_ok = lt::status_t::no_error;
const lt::status_t status_full_check = lt::status_t::need_full_check;
#endif

// Descriptors this process holds on files under `dir`: the payload files a
// backend has open, and nothing else (sockets and pipes come and go with the
// swarm and say nothing about the backend).
size_t open_files_under(const std::filesystem::path& dir) {
    const auto prefix = std::filesystem::weakly_canonical(dir).string();
    size_t count = 0;
    for (int fd = 0; fd < 4096; ++fd) {
#ifdef __APPLE__
        char path[PATH_MAX] = {};
        if (::fcntl(fd, F_GETPATH, path) != 0) continue;
        const std::string target(path);
#else
        char path[PATH_MAX] = {};
        const auto link = "/proc/self/fd/" + std::to_string(fd);
        const auto n = ::readlink(link.c_str(), path, sizeof(path) - 1);
        if (n <= 0) continue;
        const std::string target(path, static_cast<size_t>(n));
#endif
        if (target.rfind(prefix, 0) == 0) ++count;
    }
    return count;
}

// A hybrid (v1 + v2) torrent over files written under `seed_dir`. Hybrid on
// purpose: v2 inserts pad files between files, and hashing runs both
// async_hash's block hashes and async_hash2.
std::shared_ptr<lt::torrent_info> make_torrent(const std::filesystem::path& seed_dir,
                                               const std::vector<int64_t>& sizes, int piece_length) {
    std::filesystem::create_directories(seed_dir / "payload");
    for (size_t i = 0; i < sizes.size(); ++i) {
        const auto bytes = pattern_bytes(static_cast<size_t>(sizes[i]), static_cast<int>(i) + 100);
        std::ofstream(seed_dir / "payload" / ("f" + std::to_string(i)), std::ios::binary)
            .write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
#if LIBTORRENT_VERSION_NUM >= 20100
    std::vector<lt::create_file_entry> entries;
    for (size_t i = 0; i < sizes.size(); ++i)
        entries.emplace_back("payload/f" + std::to_string(i), sizes[i]);
    lt::create_torrent creator(std::move(entries), piece_length);
#else
    lt::file_storage files;
    for (size_t i = 0; i < sizes.size(); ++i)
        files.add_file("payload/f" + std::to_string(i), sizes[i]);
    lt::create_torrent creator(files, piece_length);
#endif
    lt::set_piece_hashes(creator, seed_dir.string());
    const auto buffer = creator.generate_buf();
    return std::make_shared<lt::torrent_info>(lt::span<const char>(buffer), lt::from_span);
}

lt::session_params loopback_session_params() {
    lt::settings_pack settings;
    settings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
    settings.set_bool(lt::settings_pack::enable_dht, false);
    settings.set_bool(lt::settings_pack::enable_lsd, false);
    settings.set_bool(lt::settings_pack::enable_upnp, false);
    settings.set_bool(lt::settings_pack::enable_natpmp, false);
    settings.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
    settings.set_int(lt::settings_pack::alert_mask, lt::alert_category::error | lt::alert_category::status |
                                                        lt::alert_category::piece_progress);
    return lt::session_params(std::move(settings));
}

template <class Predicate>
bool wait_for(Predicate done, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

} // namespace

MACHA_TEST("torrent_disk_io", test_a_real_swarm_downloads_through_the_backend_and_leaves_nothing_behind) {
    // A seeder on libtorrent's own backend and a leecher on macha's, over
    // loopback, three times over: every byte arrives intact, every read,
    // write and hash went through a real DATA arbiter at loader class, every
    // credit is returned, and deleting the torrent leaves no payload and no
    // open descriptor behind.
    TempDir dir;
    const std::vector<int64_t> sizes{300000, 5000, 131072, 70001};
    const auto torrent = make_torrent(dir.path() / "seed", sizes, 2 * block);

    DataResourceArbiter arbiter(64 * 1024 * 1024, 16 * 1024 * 1024, 2, 500ms);
    std::atomic<uint64_t> observed_bytes{0};
    std::mutex published_mutex;
    std::map<ObjectId, Bytes> published;
    std::atomic<size_t> publishes{0};
    TorrentDiskHooks hooks;
    hooks.admit = loader_admission(arbiter);
    hooks.observe = [&](std::chrono::nanoseconds, uint64_t bytes) { observed_bytes += bytes; };
    // Stage 2: every extent is published as its pieces verify.
    constexpr uint64_t extent_size = 64 * 1024;
    hooks.extent_size = extent_size;
    hooks.verifications = std::make_shared<TorrentPieceVerifications>();
    hooks.publish = [&](std::span<const uint8_t> bytes) -> std::optional<ObjectId> {
        const auto id = object_id(bytes);
        std::lock_guard lock(published_mutex);
        published[id] = Bytes(bytes.begin(), bytes.end());
        ++publishes;
        return id;
    };
    const auto verifications = hooks.verifications;
    size_t expected_extents = 0;
    for (const auto size : sizes) expected_extents += static_cast<size_t>((size + extent_size - 1) / extent_size);

    lt::session seeder(loopback_session_params());
    auto leecher_params = loopback_session_params();
    leecher_params.disk_io_constructor = macha_disk_io_constructor(hooks);
    lt::session leecher(std::move(leecher_params));

    lt::add_torrent_params seed;
    seed.ti = torrent;
    seed.save_path = (dir.path() / "seed").string();
    seed.flags |= lt::torrent_flags::seed_mode;
    auto seeding = seeder.add_torrent(seed);
    REQUIRE(wait_for([&] { return seeder.listen_port() != 0; }, 10s));
    const lt::tcp::endpoint seed_endpoint(lt::make_address("127.0.0.1"), seeder.listen_port());

    const auto leech_dir = dir.path() / "leech";
    for (int cycle = 0; cycle < 3; ++cycle) {
        lt::add_torrent_params leech;
        leech.ti = torrent;
        leech.save_path = leech_dir.string();
        auto downloading = leecher.add_torrent(leech);
        downloading.connect_peer(seed_endpoint);
        // What TorrentManager::drain_alerts does in production.
        auto pump = [&] {
            std::vector<lt::alert*> alerts;
            leecher.pop_alerts(&alerts);
            for (const auto* alert : alerts)
                if (const auto* piece = lt::alert_cast<lt::piece_finished_alert>(alert))
                    verifications->piece_verified(leech.save_path, static_cast<int>(piece->piece_index));
        };
        publishes = 0;
        REQUIRE(wait_for([&] { pump(); return downloading.status().is_seeding; }, 30s));
        REQUIRE(wait_for([&] { pump(); return publishes.load() == expected_extents; }, 30s));

        // The journal's manifests rebuild every file from published extents.
        const auto journal = TorrentExtentJournal::load(leech_dir);
        for (size_t i = 0; i < sizes.size(); ++i) {
            const auto name = "payload/f" + std::to_string(i);
            const auto found = journal.find(name);
            REQUIRE(found != journal.end());
            const auto manifest = TorrentExtentJournal::manifest(found->second, static_cast<uint64_t>(sizes[i]));
            REQUIRE(manifest.has_value());
            Bytes rebuilt;
            for (const auto& extent : *manifest) {
                std::lock_guard lock(published_mutex);
                const auto& bytes = published.at(extent.id);
                rebuilt.insert(rebuilt.end(), bytes.begin(), bytes.end());
            }
            const auto original = read_file(dir.path() / "seed" / "payload" / ("f" + std::to_string(i)));
            REQUIRE(rebuilt.size() == original.size());
            CHECK(std::memcmp(rebuilt.data(), original.data(), original.size()) == 0);
        }

        for (size_t i = 0; i < sizes.size(); ++i) {
            const auto name = "f" + std::to_string(i);
            CHECK(read_file(leech_dir / "payload" / name) == read_file(dir.path() / "seed" / "payload" / name));
        }
        const auto stats = arbiter.stats();
        CHECK(stats.loader_admissions > 0);
        CHECK(stats.speculative_admissions == 0);
        CHECK(stats.viewer_admissions == 0);
        CHECK(observed_bytes.load() > 0);

        leecher.remove_torrent(downloading, lt::session_handle::delete_files);
        CHECK(wait_for([&] { return !std::filesystem::exists(leech_dir / "payload"); }, 10s));
        // And the journal that described it, so a later add starts clean.
        CHECK(wait_for([&] { return !std::filesystem::exists(TorrentExtentJournal::path_for(leech_dir)); }, 10s));
        CHECK(wait_for([&] { return open_files_under(leech_dir) == 0; }, 10s));
        CHECK(wait_for([&] { return arbiter.stats().used_bytes == 0; }, 10s));
    }
    seeder.remove_torrent(seeding);
}

MACHA_TEST("torrent_disk_io", test_extents_publish_once_their_pieces_verify_and_journal_the_manifest) {
    // Stage 2. Extents are file-relative and deliberately not aligned to
    // pieces here (48 KiB extents over 32 KiB pieces, files that end mid-piece),
    // so an extent is published only when every piece covering it has
    // verified, each exactly once, and the journal's manifests rebuild every
    // file byte for byte. The first attempt fails, to prove it is retried.
    TempDir dir;
    std::mutex published_mutex;
    std::map<ObjectId, Bytes> published;
    std::atomic<int> attempts{0};
    TorrentDiskHooks hooks;
    hooks.extent_size = 3 * block;
    hooks.verifications = std::make_shared<TorrentPieceVerifications>();
    hooks.publish_retry = 50ms;
    hooks.publish = [&](std::span<const uint8_t> bytes) -> std::optional<ObjectId> {
        if (attempts.fetch_add(1) == 0) return std::nullopt;
        const auto id = object_id(bytes);
        std::lock_guard lock(published_mutex);
        published[id] = Bytes(bytes.begin(), bytes.end());
        return id;
    };
    const auto verifications = hooks.verifications;
    const std::vector<int64_t> sizes{100000, 30000, 5000};
    Harness h(layout(sizes, 2 * block), dir.path(), hooks);
    const auto data = pattern_bytes(135000, 9);
    REQUIRE(h.write_all(data));

    auto published_count = [&] {
        std::lock_guard lock(published_mutex);
        return published.size();
    };
    // Piece 0 alone completes no extent: f0's first extent is [0, 48 KiB),
    // which piece 1 also covers.
    verifications->piece_verified(h.save_path, 0);
    std::this_thread::sleep_for(200ms);
    CHECK(published_count() == 0);

    for (int piece = 1; piece < h.files.num_pieces(); ++piece)
        verifications->piece_verified(h.save_path, piece);
    // f0: 48K + 48K + 4.1K; f1: 30000; f2: 5000.
    constexpr size_t expected_extents = 5;
    REQUIRE(wait_for([&] { return published_count() == expected_extents; }, 5s));
    // Reporting a piece again publishes nothing twice.
    verifications->piece_verified(h.save_path, 0);
    std::this_thread::sleep_for(200ms);
    CHECK(published_count() == expected_extents);
    CHECK(attempts.load() == static_cast<int>(expected_extents) + 1);

    const auto journal = TorrentExtentJournal::load(dir.path());
    int64_t file_start = 0;
    for (size_t i = 0; i < sizes.size(); ++i) {
        const auto name = "payload/f" + std::to_string(i);
        const auto found = journal.find(name);
        REQUIRE(found != journal.end());
        const auto manifest = TorrentExtentJournal::manifest(found->second, static_cast<uint64_t>(sizes[i]));
        REQUIRE(manifest.has_value());
        Bytes rebuilt;
        for (const auto& extent : *manifest) {
            std::lock_guard lock(published_mutex);
            const auto& bytes = published.at(extent.id);
            rebuilt.insert(rebuilt.end(), bytes.begin(), bytes.end());
        }
        REQUIRE(rebuilt.size() == static_cast<size_t>(sizes[i]));
        CHECK(std::memcmp(rebuilt.data(), data.data() + file_start, rebuilt.size()) == 0);
        file_start += sizes[i];
    }
}

MACHA_TEST("torrent_disk_io", test_a_restarted_backend_does_not_republish_journalled_extents) {
    // Resume: extents the journal already records are not published again
    // when the same payload is added to a new backend and its pieces verify.
    TempDir dir;
    std::atomic<int> publishes{0};
    auto make_hooks = [&] {
        TorrentDiskHooks hooks;
        hooks.extent_size = 3 * block;
        hooks.verifications = std::make_shared<TorrentPieceVerifications>();
        hooks.publish = [&](std::span<const uint8_t> bytes) -> std::optional<ObjectId> {
            ++publishes;
            return object_id(bytes);
        };
        return hooks;
    };
    const auto data = pattern_bytes(100000, 10);
    {
        auto hooks = make_hooks();
        const auto verifications = hooks.verifications;
        Harness h(layout({100000}, 2 * block), dir.path(), hooks);
        REQUIRE(h.write_all(data));
        for (int piece = 0; piece < h.files.num_pieces(); ++piece)
            verifications->piece_verified(h.save_path, piece);
        REQUIRE(wait_for([&] { return publishes.load() == 3; }, 5s));
    }
    {
        auto hooks = make_hooks();
        const auto verifications = hooks.verifications;
        Harness h(layout({100000}, 2 * block), dir.path(), hooks);
        for (int piece = 0; piece < h.files.num_pieces(); ++piece)
            verifications->piece_verified(h.save_path, piece);
        std::this_thread::sleep_for(300ms);
        CHECK(publishes.load() == 3);
    }
}

MACHA_TEST("torrent_disk_io", test_pieces_spanning_files_round_trip_through_the_payload_layout) {
    // Three files, none aligned to the 32 KiB piece: pieces straddle file
    // boundaries and the last piece is short.
    TempDir dir;
    Harness h(layout({40000, 30000, 5000}, 2 * block), dir.path(), {});
    const auto data = pattern_bytes(75000, 1);
    REQUIRE(h.write_all(data));

    // Each file on disk holds exactly its slice of the payload.
    const auto f0 = read_file(dir.path() / "payload" / "f0");
    const auto f1 = read_file(dir.path() / "payload" / "f1");
    const auto f2 = read_file(dir.path() / "payload" / "f2");
    CHECK(f0 == std::vector<char>(data.begin(), data.begin() + 40000));
    CHECK(f1 == std::vector<char>(data.begin() + 40000, data.begin() + 70000));
    CHECK(f2 == std::vector<char>(data.begin() + 70000, data.end()));

    // And every piece hashes to what was written, read back across files.
    for (int piece = 0; piece < h.files.num_pieces(); ++piece) {
        const auto got = h.hash(lt::piece_index_t(piece));
        REQUIRE(got.has_value());
        CHECK(*got == expected_piece_hash(data, 2 * block, piece));
    }

    // A block read returns the bytes, including one that straddles f0/f1.
    lt::peer_request request;
    request.piece = lt::piece_index_t(1);
    request.start = block / 2;
    request.length = block;
    std::vector<char> read_back;
    bool done = false;
    h.disk->async_read(h.index(), request,
                       [&](lt::disk_buffer_holder buffer, const lt::storage_error& error) {
                           if (!error.ec) read_back.assign(buffer.data(), buffer.data() + request.length);
                           done = true;
                       });
    REQUIRE(h.pump_until([&] { return done; }));
    const auto from = static_cast<size_t>(2 * block + block / 2);
    CHECK(read_back == std::vector<char>(data.begin() + static_cast<std::ptrdiff_t>(from),
                                         data.begin() + static_cast<std::ptrdiff_t>(from + block)));
}

MACHA_TEST("torrent_disk_io", test_a_hash_issued_before_its_writes_complete_sees_them) {
    // libtorrent may ask for a piece's hash before the writes of its blocks
    // have completed. With four workers and no pumping in between, the hash
    // must still read what the writes wrote: jobs for one torrent run in order.
    //
    // Writes are made slow inside admission, as a pressured device makes them,
    // so a hash that were not ordered behind them would run on another worker
    // and read the previous round's bytes.
    TempDir dir;
    TorrentDiskHooks hooks;
    hooks.threads = 4;
    hooks.admit = [](uint64_t bytes, const std::atomic_bool&) -> std::shared_ptr<void> {
        if (bytes == static_cast<uint64_t>(block)) std::this_thread::sleep_for(2ms);
        return std::make_shared<int>(0);
    };
    Harness h(layout({8 * block}, 8 * block), dir.path(), hooks);
    for (int round = 0; round < 20; ++round) {
        const auto data = pattern_bytes(8 * block, round);
        for (int i = 0; i < 8; ++i) {
            lt::peer_request request;
            request.piece = lt::piece_index_t(0);
            request.start = i * block;
            request.length = block;
            h.disk->async_write(h.index(), request, data.data() + i * block, nullptr,
                                [](const lt::storage_error&) {});
        }
        const auto got = h.hash(lt::piece_index_t(0));
        REQUIRE(got.has_value());
        CHECK(*got == expected_piece_hash(data, 8 * block, 0));
    }
}

MACHA_TEST("torrent_disk_io", test_every_read_write_and_hash_is_admitted_and_measured) {
    TempDir dir;
    std::atomic<uint64_t> admitted_bytes{0};
    std::atomic<int> admissions{0};
    std::atomic<uint64_t> observed_bytes{0};
    TorrentDiskHooks hooks;
    hooks.admit = [&](uint64_t bytes, const std::atomic_bool&) -> std::shared_ptr<void> {
        admitted_bytes += bytes;
        ++admissions;
        return std::make_shared<int>(0);
    };
    hooks.observe = [&](std::chrono::nanoseconds, uint64_t bytes) { observed_bytes += bytes; };
    Harness h(layout({4 * block}, 2 * block), dir.path(), hooks);
    const auto data = pattern_bytes(4 * block, 3);
    REQUIRE(h.write_all(data));
    CHECK(admissions.load() == 4);
    CHECK(admitted_bytes.load() == 4ULL * block);
    CHECK(observed_bytes.load() == 4ULL * block);

    REQUIRE(h.hash(lt::piece_index_t(0)).has_value());
    CHECK(admissions.load() == 5);
    CHECK(admitted_bytes.load() == 6ULL * block);
    CHECK(observed_bytes.load() == 6ULL * block);
}

MACHA_TEST("torrent_disk_io", test_a_blocked_admission_pushes_back_on_the_network) {
    // When the device is not admitting, queued writes pass the queue limit,
    // async_write says so, and libtorrent stops reading from peers until
    // on_disk(). That is how the network rate comes to follow the disk.
    TempDir dir;
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool open = false;
    TorrentDiskHooks hooks;
    hooks.threads = 1;
    hooks.admit = [&](uint64_t, const std::atomic_bool& aborting) -> std::shared_ptr<void> {
        std::unique_lock lock(gate_mutex);
        gate_cv.wait(lock, [&] { return open || aborting.load(); });
        return aborting.load() ? nullptr : std::make_shared<int>(0);
    };
    Harness h(layout({16 * block}, 16 * block), dir.path(), hooks, 4 * block);

    struct Observer final : lt::disk_observer {
        std::atomic<int> calls{0};
        void on_disk() override { ++calls; }
    };
    auto observer = std::make_shared<Observer>();
    const auto data = pattern_bytes(16 * block, 4);
    int exceeded_at = -1;
    std::atomic<int> completed{0};
    for (int i = 0; i < 16; ++i) {
        lt::peer_request request;
        request.piece = lt::piece_index_t(0);
        request.start = i * block;
        request.length = block;
        const bool exceeded = h.disk->async_write(h.index(), request, data.data() + i * block, observer,
                                                  [&](const lt::storage_error&) { ++completed; });
        if (exceeded && exceeded_at < 0) exceeded_at = i;
    }
    // Four blocks fit; the fifth is over the limit.
    CHECK(exceeded_at == 4);
    h.pump_for(100ms);
    CHECK(completed.load() == 0);
    CHECK(observer->calls.load() == 0);

    {
        std::lock_guard lock(gate_mutex);
        open = true;
    }
    gate_cv.notify_all();
    REQUIRE(h.pump_until([&] { return completed.load() == 16; }));
    CHECK(observer->calls.load() >= 1);
}

MACHA_TEST("torrent_disk_io", test_abort_releases_jobs_blocked_in_admission) {
    TempDir dir;
    TorrentDiskHooks hooks;
    hooks.threads = 2;
    hooks.admit = [](uint64_t, const std::atomic_bool& aborting) -> std::shared_ptr<void> {
        while (!aborting.load()) std::this_thread::sleep_for(5ms);
        return nullptr;
    };
    auto h = std::make_unique<Harness>(layout({4 * block}, 4 * block), dir.path(), hooks);
    const auto data = pattern_bytes(4 * block, 5);
    std::atomic<int> aborted{0};
    for (int i = 0; i < 4; ++i) {
        lt::peer_request request;
        request.piece = lt::piece_index_t(0);
        request.start = i * block;
        request.length = block;
        h->disk->async_write(h->index(), request, data.data() + i * block, nullptr,
                             [&](const lt::storage_error& error) {
                                 if (error.ec == boost::asio::error::operation_aborted) ++aborted;
                             });
    }
    const auto started = std::chrono::steady_clock::now();
    h->disk->abort(true);
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    h->pump_for(50ms);
    CHECK(aborted.load() == 4);
    // Nothing reached the disk.
    CHECK(!std::filesystem::exists(dir.path() / "payload" / "f0"));
}

MACHA_TEST("torrent_disk_io", test_resume_is_trusted_only_while_files_still_hold_it) {
    TempDir dir;
    Harness h(layout({2 * block, 2 * block}, 2 * block), dir.path(), {});
    // Nothing on disk, no resume data: nothing to check.
    CHECK(h.check(nullptr) == status_ok);

    const auto data = pattern_bytes(4 * block, 6);
    REQUIRE(h.write_all(data));
    // Files exist but no resume data vouches for them: check in full.
    CHECK(h.check(nullptr) == status_full_check);

    lt::add_torrent_params resume;
    resume.have_pieces.resize(2);
    resume.have_pieces.set_bit(lt::piece_index_t(0));
    resume.have_pieces.set_bit(lt::piece_index_t(1));
    CHECK(h.check(&resume) == status_ok);

    // A payload file shorter than a piece it claims: check in full.
    h.disk->async_release_files(h.index(), {});
    h.pump_for(20ms);
    std::filesystem::resize_file(dir.path() / "payload" / "f1", block);
    CHECK(h.check(&resume) == status_full_check);
}

MACHA_TEST("torrent_disk_io", test_delete_removes_the_payload_and_nothing_else) {
    TempDir dir;
    Harness h(layout({block, block}, block), dir.path(), {});
    REQUIRE(h.write_all(pattern_bytes(2 * block, 7)));
    std::ofstream(dir.path() / "not-payload") << "keep";
    bool done = false;
    h.disk->async_delete_files(h.index(), lt::session_handle::delete_files,
                               [&](const lt::storage_error&) { done = true; });
    REQUIRE(h.pump_until([&] { return done; }));
    CHECK(!std::filesystem::exists(dir.path() / "payload"));
    CHECK(std::filesystem::exists(dir.path() / "not-payload"));
    CHECK(std::filesystem::exists(dir.path()));
}

MACHA_TEST("torrent_disk_io", test_a_three_hundred_file_torrent_holds_a_bounded_number_of_descriptors) {
    // A discography is thousands of small files; the backend caches at most
    // 64 descriptors per torrent and releases them all on release_files.
    TempDir dir;
    std::vector<int64_t> sizes(300, 1000);
    Harness h(layout(sizes, block), dir.path(), {});
    const auto before = open_descriptors();
    REQUIRE(h.write_all(pattern_bytes(300 * 1000, 8)));
    CHECK(open_descriptors() <= before + 64);
    bool released = false;
    h.disk->async_release_files(h.index(), [&] { released = true; });
    REQUIRE(h.pump_until([&] { return released; }));
    CHECK(open_descriptors() == before);
}

MACHA_TEST("torrent_disk_io", test_loader_admission_waits_for_credit_and_yields_on_abort) {
    // The arbiter-backed admission the node installs: loader class, so it
    // is admitted on a pressured device with no viewer (law 2), waits while
    // the loader ceiling is full, proceeds when a lease is released, and gives
    // up promptly once the backend aborts.
    DiskServiceMonitor monitor;
    DataResourceArbiter arbiter(16 * 1024 * 1024, 4 * 1024 * 1024, 1, 500ms);
    arbiter.observe_device(&monitor, 1);
    arbiter.observe_viewers([] { return false; });
    for (int i = 0; i < 30; ++i)
        monitor.note(9000ms, 4 * 1024 * 1024);
    REQUIRE(monitor.pressured());

    auto admit = loader_admission(arbiter);
    std::atomic_bool aborting{false};
    auto first = admit(block, aborting);
    REQUIRE(first != nullptr);

    // The single loader slot is taken: the second waits, across more than one
    // of admission's one-second slices, and proceeds when the first is freed.
    std::atomic_bool second_admitted{false};
    std::thread waiter([&] {
        auto second = admit(block, aborting);
        second_admitted = second != nullptr;
    });
    std::this_thread::sleep_for(1500ms);
    CHECK(!second_admitted.load());
    first.reset();
    waiter.join();
    CHECK(second_admitted.load());

    // With the slot held again, an abort ends the wait within a slice.
    auto held = admit(block, aborting);
    REQUIRE(held != nullptr);
    std::atomic_bool returned{false};
    std::thread blocked([&] {
        auto none = admit(block, aborting);
        returned = none == nullptr;
    });
    std::this_thread::sleep_for(100ms);
    aborting = true;
    const auto started = std::chrono::steady_clock::now();
    blocked.join();
    CHECK(returned.load());
    CHECK(std::chrono::steady_clock::now() - started < 1500ms);
}

} // namespace macha
