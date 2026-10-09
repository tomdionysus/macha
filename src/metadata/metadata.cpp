// SPDX-License-Identifier: GPL-3.0-or-later
#include "metadata/metadata.hpp"
#include "codec.hpp"
#include "durable_file.hpp"
#include "log.hpp"
#include "observation.hpp"
#include "resident_bytes.hpp"
#include "startup_progress.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <tuple>
#include <unistd.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
namespace macha {
namespace {
constexpr std::array<uint8_t, 8> SM5{'D', 'H', 'T', 'M', 'E', 'T', 'A', '5'},
    SM6{'D', 'H', 'T', 'M', 'E', 'T', 'A', '6'}, SM7{'D', 'H', 'T', 'M', 'E', 'T', 'A', '7'},
    SM8{'D', 'H', 'T', 'M', 'E', 'T', 'A', '8'}, SM9{'D', 'H', 'T', 'M', 'E', 'T', 'A', '9'},
    SM10{'D', 'H', 'T', 'M', 'E', 'T', 'B', '0'}, SM11{'D', 'H', 'T', 'M', 'E', 'T', 'B', '1'},
    SM12{'D', 'H', 'T', 'M', 'E', 'T', 'B', '2'}, SM13{'D', 'H', 'T', 'M', 'E', 'T', 'B', '3'},
    SM14{'D', 'H', 'T', 'M', 'E', 'T', 'B', '4'},
    // SM15 is SM13 with every section, then torrent requests; SM16 is SM14
    // then torrent requests. Written only while the collection is non-empty.
    SM15{'D', 'H', 'T', 'M', 'E', 'T', 'B', '5'}, SM16{'D', 'H', 'T', 'M', 'E', 'T', 'B', '6'},
    // SM17 is SM16 and SM18 is SM15, each followed by the legacy clock.
    SM17{'D', 'H', 'T', 'M', 'E', 'T', 'B', '7'}, SM18{'D', 'H', 'T', 'M', 'E', 'T', 'B', '8'},
    // SM19 is SM14 with the torrent requests, a legacy clock flag and the
    // clock when set, then the tombstone batches. Written only while the
    // head names a batch.
    SM19{'D', 'H', 'T', 'M', 'E', 'T', 'B', '9'},
    TOMBSTONE_BATCH{'M', 'T', 'O', 'M', 'B', '0', '0', '1'},
    DM{'D', 'H', 'T', 'M', 'D', 'B', '0', '1'}, MJ{'D', 'H', 'T', 'M', 'J', 'N', 'L', '1'},
    MH{'D', 'H', 'T', 'M', 'H', 'S', 'T', '1'}, MA{'D', 'H', 'T', 'M', 'A', 'C', 'C', '1'},
    MS{'D', 'H', 'T', 'M', 'S', 'E', 'Q', '1'},
    MAUTHOR{'D', 'H', 'T', 'M', 'A', 'U', 'T', '1'};
constexpr uint8_t JOURNAL_PREPARE_FULL = 1, JOURNAL_PREPARE_DELTA = 2, JOURNAL_SEED_FULL = 3,
                  JOURNAL_COMMIT = 4;

void account_entry_allocations(uint64_t& total, const FsEntry& entry) {
    resident::add(total, static_cast<uint64_t>(entry.extents.capacity()) * sizeof(ExtentRef));
}
} // namespace

NodeId legacy_file_id(std::string_view path) {
    Bytes seed;
    static constexpr std::string_view domain = "macha-legacy-file-id\0";
    seed.insert(seed.end(), domain.begin(), domain.end());
    seed.insert(seed.end(), path.begin(), path.end());
    const auto digest = sha256(seed);
    NodeId id;
    std::copy_n(digest.bytes.begin(), id.bytes.size(), id.bytes.begin());
    return id;
}

void encode_entry_provenance(Writer& w, const EntryProvenance& p) {
    w.fixed(p.file_id.bytes);
    w.fixed(p.content.author.bytes);
    w.u64(p.content.sequence);
    w.fixed(p.name.author.bytes);
    w.u64(p.name.sequence);
}

EntryProvenance decode_entry_provenance(Reader& r) {
    EntryProvenance p;
    p.file_id.bytes = r.fixed<16>();
    p.content.author.bytes = r.fixed<16>();
    p.content.sequence = r.u64();
    p.name.author.bytes = r.fixed<16>();
    p.name.sequence = r.u64();
    // The flag promised provenance; an empty one would not round-trip.
    if (p.empty())
        throw DecodeError("entry provenance is empty");
    return p;
}

namespace {
void encode_clock(Writer& w, const std::map<NodeId, uint64_t>& clock) {
    w.u32(static_cast<uint32_t>(clock.size()));
    for (const auto& [node, sequence] : clock) {
        w.fixed(node.bytes);
        w.u64(sequence);
    }
}

std::map<NodeId, uint64_t> decode_clock(Reader& r) {
    std::map<NodeId, uint64_t> clock;
    const auto count = r.u32();
    if (count > 65536)
        throw DecodeError("too many clock origins");
    for (uint32_t i = 0; i < count; ++i) {
        NodeId node{r.fixed<16>()};
        const auto sequence = r.u64();
        if (!sequence || !clock.emplace(node, sequence).second)
            throw DecodeError("bad clock entry");
    }
    return clock;
}
} // namespace

void apply_entry_append(FsEntry& entry, std::string_view path,
                        const MetadataDelta::EntryAppend& append) {
    if (entry.type != EntryType::file || entry.extents.size() != append.base_extents)
        throw DecodeError("metadata delta append base mismatch");
    entry.extents.insert(entry.extents.end(), append.extents.begin(), append.extents.end());
    entry.size = append.size;
    entry.mtime_ns = append.mtime_ns;
    entry.ctime_ns = append.ctime_ns;
    entry.version = append.version;
    if (append.content) {
        entry.provenance.content = append.content;
        if (entry.provenance.file_id == NodeId{})
            entry.provenance.file_id = legacy_file_id(path);
    }
}

void stamp_entry_provenance(FsEntry& entry, std::string_view path, const FsEntry* prior,
                            const MetadataDot& dot) {
    auto& now = entry.provenance;
    const auto prior_id = [&] {
        return prior->provenance.file_id != NodeId{} ? prior->provenance.file_id
                                                     : legacy_file_id(path);
    };
    // An entry built without an identity is the file already at the path,
    // changed; with nothing at the path it is a new file.
    if (now.file_id == NodeId{})
        now.file_id = prior ? prior_id() : random_node_id();
    const bool same_file = prior && now.file_id == prior_id();
    if (!same_file) {
        // New at this path: created here, or renamed to here.
        now.name = dot;
        if (!now.content)
            now.content = dot;
        return;
    }
    auto before = *prior;
    auto after = entry;
    before.provenance = {};
    after.provenance = {};
    if (before == after) {
        // Rewritten unchanged: it stays exactly what it was.
        now = prior->provenance;
        return;
    }
    now.name = prior->provenance.name;
    now.content = dot;
}

uint64_t snapshot_resident_bytes(const MetadataSnapshot& snapshot) {
    uint64_t total = sizeof(MetadataSnapshot);
    if (snapshot.legacy_clock)
        resident::map_nodes(total, *snapshot.legacy_clock);
    resident::add(total, snapshot.metadata_voters.capacity() * sizeof(NodeId));
    resident::map_nodes(total, snapshot.mutation_sequences);
    resident::map_nodes(total, snapshot.metadata_participants);
    resident::map_nodes(total, snapshot.entries);
    for (const auto& [path, entry] : snapshot.entries) {
        resident::string(total, path);
        account_entry_allocations(total, entry);
    }
    resident::add(total, snapshot.garbage.capacity() * sizeof(GarbageRef));
    resident::add(total, snapshot.tombstone_batches.capacity() * sizeof(TombstoneBatch));
    resident::map_nodes(total, snapshot.node_status);
    for (const auto& [_, status] : snapshot.node_status) {
        resident::string(total, status.version);
        resident::string(total, status.host);
        resident::string(total, status.failure_domain);
    }
    resident::map_nodes(total, snapshot.identity_resets);
    resident::map_nodes(total, snapshot.torrent_requests);
    for (const auto& [id, request] : snapshot.torrent_requests) {
        resident::string(total, id);
        resident::string(total, request.id);
        resident::string(total, request.info_hash);
        resident::string(total, request.source);
        resident::string(total, request.name);
        resident::string(total, request.ingest_job_id);
        resident::string(total, request.error_code);
        resident::string(total, request.error);
    }
    for (const auto& [key, reset] : snapshot.identity_resets) {
        resident::string(total, key);
        resident::string(total, reset.host);
        resident::string(total, reset.reason);
    }
    resident::add(total, snapshot.merge_parents.capacity() * sizeof(Hash256));
    resident::map_nodes(total, snapshot.conflicts);
    for (const auto& [id, conflict] : snapshot.conflicts) {
        resident::string(total, id);
        resident::string(total, conflict.key);
        if (conflict.base_entry)
            account_entry_allocations(total, *conflict.base_entry);
        if (conflict.left_entry)
            account_entry_allocations(total, *conflict.left_entry);
        if (conflict.right_entry)
            account_entry_allocations(total, *conflict.right_entry);
    }
    return total;
}

namespace {
std::shared_ptr<const MetadataMaterialization>
make_materialization(MetadataRecord record, std::shared_ptr<const MetadataSnapshot> snapshot) {
    uint64_t bytes = sizeof(MetadataMaterialization) + sizeof(Bytes) + 64;
    resident::add(bytes, record.payload.size());
    resident::add(bytes, snapshot_resident_bytes(*snapshot));
    return std::make_shared<const MetadataMaterialization>(
        MetadataMaterialization{std::move(record), std::move(snapshot), bytes});
}
void entry(Writer& w, const FsEntry& e) {
    const bool provenance = !e.provenance.empty();
    w.u8(static_cast<uint8_t>(e.type) | (provenance ? entry_type_with_provenance : 0));
    w.u32(e.mode);
    w.u32(e.uid);
    w.u32(e.gid);
    w.u64(e.size);
    w.i64(e.ctime_ns);
    w.i64(e.mtime_ns);
    w.u64(e.version);
    if (provenance)
        encode_entry_provenance(w, e.provenance);
    w.u32(e.extents.size());
    for (auto& x : e.extents) {
        w.u64(x.offset);
        w.u64(x.length);
        w.u8(x.hole);
        w.fixed(x.id.bytes);
    }
}
FsEntry entry(Reader& r) {
    FsEntry e;
    auto t = r.u8();
    const bool provenance = (t & entry_type_with_provenance) != 0;
    t &= static_cast<uint8_t>(~entry_type_with_provenance);
    if (t < 1 || t > 2)
        throw DecodeError("bad entry type");
    e.type = (EntryType)t;
    e.mode = r.u32();
    e.uid = r.u32();
    e.gid = r.u32();
    e.size = r.u64();
    e.ctime_ns = r.i64();
    e.mtime_ns = r.i64();
    e.version = r.u64();
    if (provenance)
        e.provenance = decode_entry_provenance(r);
    auto n = r.u32();
    if (n > 10000000)
        throw DecodeError("too many extents");
    // Reserve exactly: push_back growth leaves up to 2x slack, pinned for the
    // snapshot's life. `n` is untrusted, so bound it by the remaining input.
    constexpr size_t encoded_extent_bytes = 8 + 8 + 1 + 32;
    e.extents.reserve(std::min<size_t>(n, r.remaining() / encoded_extent_bytes));
    for (uint32_t i = 0; i < n; ++i) {
        ExtentRef x;
        x.offset = r.u64();
        x.length = r.u64();
        x.hole = r.u8();
        x.id.bytes = r.fixed<32>();
        e.extents.push_back(x);
    }
    return e;
}

void optional_entry(Writer& w, const std::optional<FsEntry>& value) {
    w.u8(value.has_value());
    if (value)
        entry(w, *value);
}

std::optional<FsEntry> optional_entry(Reader& r) {
    if (!r.u8())
        return {};
    return entry(r);
}

void optional_object(Writer& w, const std::optional<ObjectId>& value) {
    w.u8(value.has_value());
    if (value)
        w.fixed(value->bytes);
}

std::optional<ObjectId> optional_object(Reader& r) {
    if (!r.u8())
        return {};
    ObjectId id;
    id.bytes = r.fixed<32>();
    return id;
}

// The kind byte carries this bit when the later alternative was installed.
constexpr uint8_t conflict_later_installed = 0x80;

void encode_conflict(Writer& w, const MetadataConflict& conflict) {
    w.u8(static_cast<uint8_t>(conflict.kind) |
         (conflict.later_installed ? conflict_later_installed : 0));
    w.string(conflict.key);
    w.fixed(conflict.left_head.bytes);
    w.fixed(conflict.right_head.bytes);
    if (conflict.later_installed) {
        w.fixed(conflict.installed_dot.author.bytes);
        w.u64(conflict.installed_dot.sequence);
    }
    switch (conflict.kind) {
    case MetadataConflictKind::namespace_entry:
        optional_entry(w, conflict.base_entry);
        optional_entry(w, conflict.left_entry);
        optional_entry(w, conflict.right_entry);
        break;
    case MetadataConflictKind::catalogue_root:
        optional_object(w, conflict.base_catalogue_root);
        optional_object(w, conflict.left_catalogue_root);
        optional_object(w, conflict.right_catalogue_root);
        break;
    }
}

MetadataConflict decode_conflict(Reader& r) {
    MetadataConflict conflict;
    auto kind = r.u8();
    conflict.later_installed = (kind & conflict_later_installed) != 0;
    kind &= static_cast<uint8_t>(~conflict_later_installed);
    if (kind < static_cast<uint8_t>(MetadataConflictKind::namespace_entry) ||
        kind > static_cast<uint8_t>(MetadataConflictKind::catalogue_root))
        throw DecodeError("bad metadata conflict kind");
    conflict.kind = static_cast<MetadataConflictKind>(kind);
    conflict.key = r.string(8192);
    if (conflict.key.empty())
        throw DecodeError("empty metadata conflict key");
    conflict.left_head.bytes = r.fixed<32>();
    conflict.right_head.bytes = r.fixed<32>();
    if (conflict.later_installed) {
        conflict.installed_dot.author.bytes = r.fixed<16>();
        conflict.installed_dot.sequence = r.u64();
    }
    switch (conflict.kind) {
    case MetadataConflictKind::namespace_entry:
        conflict.base_entry = optional_entry(r);
        conflict.left_entry = optional_entry(r);
        conflict.right_entry = optional_entry(r);
        break;
    case MetadataConflictKind::catalogue_root:
        conflict.base_catalogue_root = optional_object(r);
        conflict.left_catalogue_root = optional_object(r);
        conflict.right_catalogue_root = optional_object(r);
        break;
    }
    return conflict;
}

void encode_node_status(Writer& w, const PersistedNodeStatus& status) {
    w.fixed(status.boot_id.bytes);
    w.u64(status.observed_unix_ms);
    w.string(status.version);
    w.string(status.host);
    w.string(status.failure_domain);
    w.u16(status.port);
    w.u64(status.storage_capacity);
    w.u64(status.storage_used);
    w.u64(status.cache_capacity);
    w.u64(status.cache_used);
    w.u64(status.metadata_generation);
    w.u32(status.storage_backends_online);
}

PersistedNodeStatus decode_node_status(Reader& r) {
    PersistedNodeStatus status;
    status.boot_id.bytes = r.fixed<16>();
    status.observed_unix_ms = r.u64();
    status.version = r.string(256);
    status.host = r.string(4096);
    status.failure_domain = r.string(4096);
    status.port = r.u16();
    status.storage_capacity = r.u64();
    status.storage_used = r.u64();
    status.cache_capacity = r.u64();
    status.cache_used = r.u64();
    status.metadata_generation = r.u64();
    status.storage_backends_online = r.u32();
    return status;
}

void encode_identity_reset(Writer& w, const IdentityAssociationReset& reset) {
    w.string(reset.host);
    w.u16(reset.port);
    w.fixed(reset.stale_node_id.bytes);
    w.u64(reset.epoch);
    w.u64(reset.reset_unix_ms);
    w.fixed(reset.reset_by.bytes);
    w.string(reset.reason);
}

IdentityAssociationReset decode_identity_reset(Reader& r) {
    IdentityAssociationReset reset;
    reset.host = r.string(4096);
    reset.port = r.u16();
    reset.stale_node_id.bytes = r.fixed<16>();
    reset.epoch = r.u64();
    reset.reset_unix_ms = r.u64();
    reset.reset_by.bytes = r.fixed<16>();
    reset.reason = r.string(4096);
    if (reset.host.empty() || !reset.epoch)
        throw DecodeError("bad identity reset");
    return reset;
}

Bytes encode_snapshot_v7(const MetadataSnapshot& s) {
    // SM7 bytes for replaying DLT1 journal records, whose successor hashes
    // were computed over SM7.
    Writer w;
    w.raw(SM7);
    w.u32(s.metadata_voters.size());
    for (const auto& v : s.metadata_voters)
        w.fixed(v.bytes);
    w.u32(s.data_replication);
    w.u64(s.extent_size);
    w.u32(s.mutation_sequences.size());
    for (const auto& [node, sequence] : s.mutation_sequences) {
        w.fixed(node.bytes);
        w.u64(sequence);
    }
    w.u32(s.entries.size());
    for (const auto& [path, value] : s.entries) {
        w.string(path);
        entry(w, value);
    }
    w.u8(s.catalogue_root.has_value());
    if (s.catalogue_root)
        w.fixed(s.catalogue_root->bytes);
    w.u32(s.garbage.size());
    for (const auto& garbage : s.garbage)
        w.fixed(garbage.id.bytes);
    return w.take();
}

Bytes encode_snapshot_v8(const MetadataSnapshot& s) {
    // SM8 bytes for replaying DLT2 journal records, whose successor hashes
    // were computed over SM8.
    Writer w;
    w.raw(SM8);
    w.u32(s.metadata_voters.size());
    for (const auto& v : s.metadata_voters)
        w.fixed(v.bytes);
    w.u32(s.data_replication);
    w.u64(s.extent_size);
    w.u32(s.mutation_sequences.size());
    for (const auto& [node, sequence] : s.mutation_sequences) {
        w.fixed(node.bytes);
        w.u64(sequence);
    }
    w.u32(s.entries.size());
    for (const auto& [path, value] : s.entries) {
        w.string(path);
        entry(w, value);
    }
    w.u8(s.catalogue_root.has_value());
    if (s.catalogue_root)
        w.fixed(s.catalogue_root->bytes);
    w.u32(s.garbage.size());
    for (const auto& garbage : s.garbage) {
        w.fixed(garbage.id.bytes);
        w.i64(garbage.retired_at_ns);
        w.fixed(garbage.retirement_id.bytes);
    }
    return w.take();
}

int metadata_delta_version(std::span<const uint8_t> data) {
    static constexpr std::array<uint8_t, 7> prefix{'D', 'H', 'T', 'M', 'D', 'L', 'T'};
    if (data.size() < 8 || !std::equal(prefix.begin(), prefix.end(), data.begin()))
        return 0;
    // DLT10 and DLT11 are written 'A' and 'B'.
    if (data[7] == 'A')
        return 10;
    if (data[7] == 'B')
        return 11;
    if (data[7] < '1' || data[7] > '9')
        return 0;
    return static_cast<int>(data[7] - '0');
}


Bytes encode_snapshot_for_delta(std::span<const uint8_t> delta, const MetadataSnapshot& snapshot) {
    switch (metadata_delta_version(delta)) {
    case 1:
        return encode_snapshot_v7(snapshot);
    case 2:
        return encode_snapshot_v8(snapshot);
    case 3:
        // DLT3 successors are SM9.
        if (!snapshot.identity_resets.empty())
            throw DecodeError("DLT3 cannot contain identity resets");
        return encode_snapshot(snapshot);
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
        // A tree-backed successor is SM14; encode_snapshot refuses a namespace
        // root, which would force every commit to a full record.
        return snapshot.namespace_root ? encode_snapshot_v14(snapshot)
                                       : encode_snapshot(snapshot);
    default:
        throw DecodeError("bad metadata delta");
    }
}


void hash_u8(Sha256Hasher& hash, uint8_t value) {
    hash.update(std::span<const uint8_t>(&value, 1));
}

void hash_u32(Sha256Hasher& hash, uint32_t value) {
    std::array<uint8_t, 4> encoded{};
    for (int shift = 24, i = 0; shift >= 0; shift -= 8, ++i)
        encoded[static_cast<size_t>(i)] = static_cast<uint8_t>(value >> shift);
    hash.update(encoded);
}

void hash_u64(Sha256Hasher& hash, uint64_t value) {
    std::array<uint8_t, 8> encoded{};
    for (int shift = 56, i = 0; shift >= 0; shift -= 8, ++i)
        encoded[static_cast<size_t>(i)] = static_cast<uint8_t>(value >> shift);
    hash.update(encoded);
}

void hash_bytes(Sha256Hasher& hash, std::span<const uint8_t> bytes) {
    if (bytes.size() > UINT32_MAX)
        throw std::runtime_error("encoded blob too large");
    hash_u32(hash, static_cast<uint32_t>(bytes.size()));
    hash.update(bytes);
}

void hash_string(Sha256Hasher& hash, const std::string& value) {
    hash_bytes(hash, {reinterpret_cast<const uint8_t*>(value.data()), value.size()});
}

void writefile(const std::filesystem::path& p, std::span<const uint8_t> d) {
    durable_replace_file(p, std::string_view(reinterpret_cast<const char*>(d.data()), d.size()));
}

void truncate_durable(const std::filesystem::path& path, size_t size) {
    int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0)
        throw std::runtime_error("cannot open metadata journal for truncation " + path.string() +
                                 ": " + strerror(errno));
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        const auto error = errno;
        close(fd);
        throw std::runtime_error("cannot truncate metadata journal " + path.string() + ": " +
                                 strerror(error));
    }
    if (fsync(fd) != 0) {
        const auto error = errno;
        close(fd);
        throw std::runtime_error("cannot sync truncated metadata journal " + path.string() + ": " +
                                 strerror(error));
    }
    if (close(fd) != 0)
        throw std::runtime_error("cannot close truncated metadata journal " + path.string() + ": " +
                                 strerror(errno));
}

std::filesystem::path quarantine_journal_tail(const std::filesystem::path& path,
                                              std::span<const uint8_t> tail) {
    const auto quarantine = path.string() + ".corrupt." + std::to_string(wall_time_ns());
    durable_replace_file(quarantine,
                         std::string_view(reinterpret_cast<const char*>(tail.data()), tail.size()));
    return quarantine;
}

void sync_directory(const std::filesystem::path& path) {
    const auto directory =
        path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        throw std::runtime_error("cannot open metadata directory " + directory.string() + ": " +
                                 strerror(errno));
    if (fsync(fd) != 0) {
        const auto error = errno;
        close(fd);
        throw std::runtime_error("cannot sync metadata directory " + directory.string() + ": " +
                                 strerror(error));
    }
    if (close(fd) != 0)
        throw std::runtime_error("cannot close metadata directory " + directory.string() + ": " +
                                 strerror(errno));
}

std::optional<std::filesystem::path> quarantine_metadata_file(const std::filesystem::path& path,
                                                              const std::string& suffix) {
    if (!std::filesystem::exists(path))
        return {};
    const auto quarantine = std::filesystem::path(path.string() + suffix);
    if (rename(path.c_str(), quarantine.c_str()) != 0)
        throw std::runtime_error("cannot quarantine metadata file " + path.string() + ": " +
                                 strerror(errno));
    sync_directory(path);
    return quarantine;
}
} // namespace

namespace {
bool extents_appended(const FsEntry& before, const FsEntry& after) {
    return before.type == EntryType::file && after.type == EntryType::file &&
           before.mode == after.mode && before.uid == after.uid && before.gid == after.gid &&
           before.extents.size() < after.extents.size() &&
           std::equal(before.extents.begin(), before.extents.end(), after.extents.begin());
}
} // namespace

void record_entry_change(MetadataDelta& delta, const std::string& path, const FsEntry* before,
                         const FsEntry& after) {
    // An append carries the content dot and nothing else of the provenance:
    // the identity and name must be what applying it gives.
    const auto appends_provenance = [&] {
        auto expected = before->provenance;
        if (after.provenance.content) {
            expected.content = after.provenance.content;
            if (expected.file_id == NodeId{})
                expected.file_id = legacy_file_id(path);
        }
        return expected == after.provenance;
    };
    if (before && extents_appended(*before, after) && appends_provenance()) {
        MetadataDelta::EntryAppend append;
        append.content = after.provenance.content;
        append.size = after.size;
        append.mtime_ns = after.mtime_ns;
        append.ctime_ns = after.ctime_ns;
        append.version = after.version;
        append.base_extents = static_cast<uint32_t>(before->extents.size());
        append.extents.assign(after.extents.begin() +
                                  static_cast<ptrdiff_t>(before->extents.size()),
                              after.extents.end());
        delta.upsert_entries.erase(path);
        delta.append_entries[path] = std::move(append);
        return;
    }
    delta.append_entries.erase(path);
    delta.upsert_entries[path] = after;
}

Bytes encode_tombstone_batch(int64_t retired_at_ns, const std::vector<ObjectId>& ids) {
    if (ids.empty() || retired_at_ns < 0)
        throw std::invalid_argument("empty or unretired tombstone batch");
    for (size_t i = 1; i < ids.size(); ++i)
        if (!(ids[i - 1] < ids[i]))
            throw std::invalid_argument("tombstone batch ids not sorted and unique");
    Writer w;
    w.raw(TOMBSTONE_BATCH);
    w.i64(retired_at_ns);
    w.u32(static_cast<uint32_t>(ids.size()));
    for (const auto& id : ids)
        w.fixed(id.bytes);
    return w.take();
}

TombstoneBatchContent decode_tombstone_batch(std::span<const uint8_t> bytes) {
    Reader r(bytes);
    const auto magic = r.raw(8);
    if (!std::equal(magic.begin(), magic.end(), TOMBSTONE_BATCH.begin()))
        throw DecodeError("not a tombstone batch");
    TombstoneBatchContent batch;
    batch.retired_at_ns = r.i64();
    const auto count = r.u32();
    if (batch.retired_at_ns < 0 || count == 0 || count > r.remaining() / 32)
        throw DecodeError("bad tombstone batch");
    batch.ids.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ObjectId id{r.fixed<32>()};
        if (!batch.ids.empty() && !(batch.ids.back() < id))
            throw DecodeError("tombstone batch ids out of order");
        batch.ids.push_back(id);
    }
    r.finish();
    return batch;
}

namespace {
void record_garbage_upsert(MetadataDelta& delta, const GarbageRef& garbage) {
    auto existing = std::find_if(delta.upsert_garbage.begin(), delta.upsert_garbage.end(),
                                 [&](const GarbageRef& value) { return value.id == garbage.id; });
    if (existing == delta.upsert_garbage.end())
        delta.upsert_garbage.push_back(garbage);
    else
        *existing = garbage;
}
} // namespace

void retire_objects(MetadataSnapshot& snapshot, MetadataDelta& delta, std::vector<ObjectId> ids) {
    if (ids.empty())
        return;
    if (snapshot.namespace_root) {
        auto group = std::find_if(delta.pending_tombstones.begin(), delta.pending_tombstones.end(),
                                  [](const auto& pending) { return pending.at_commit; });
        if (group == delta.pending_tombstones.end()) {
            delta.pending_tombstones.push_back({});
            group = std::prev(delta.pending_tombstones.end());
        }
        group->ids.insert(group->ids.end(), ids.begin(), ids.end());
        return;
    }
    // One pass over the (possibly huge) inline set, not one per object.
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::vector<bool> found(ids.size(), false);
    for (auto& garbage : snapshot.garbage) {
        auto target = std::lower_bound(ids.begin(), ids.end(), garbage.id);
        if (target == ids.end() || *target != garbage.id)
            continue;
        const auto index = static_cast<size_t>(target - ids.begin());
        if (found[index])
            continue;
        auto retired = wall_time_ns();
        if (retired <= garbage.retired_at_ns &&
            garbage.retired_at_ns < std::numeric_limits<int64_t>::max())
            retired = garbage.retired_at_ns + 1;
        garbage.retired_at_ns = retired;
        garbage.retirement_id = random_node_id();
        record_garbage_upsert(delta, garbage);
        found[index] = true;
    }
    for (size_t i = 0; i < ids.size(); ++i) {
        if (found[i])
            continue;
        snapshot.garbage.push_back({ids[i], wall_time_ns(), random_node_id()});
        record_garbage_upsert(delta, snapshot.garbage.back());
    }
}

std::vector<GarbageRef>
tombstones_of(const MetadataSnapshot& snapshot,
              const std::function<std::optional<Bytes>(const ObjectId&)>& read_batch,
              size_t* unreadable) {
    std::vector<GarbageRef> all = snapshot.garbage;
    for (const auto& batch : snapshot.tombstone_batches) {
        std::optional<TombstoneBatchContent> content;
        try {
            if (const auto bytes = read_batch(batch.id))
                content = decode_tombstone_batch(*bytes);
        } catch (const std::exception&) {
        }
        if (!content || content->ids.size() != batch.count ||
            content->retired_at_ns != batch.retired_at_ns) {
            if (unreadable)
                ++*unreadable;
            continue;
        }
        // The batch's own retirement: its id, so an equal one is never
        // mistaken for another.
        NodeId retirement;
        std::copy_n(batch.id.bytes.begin(), retirement.bytes.size(), retirement.bytes.begin());
        for (const auto& id : content->ids)
            all.push_back({id, batch.retired_at_ns, retirement});
    }
    // The latest retirement per object.
    std::sort(all.begin(), all.end(), [](const GarbageRef& a, const GarbageRef& b) {
        if (a.id != b.id)
            return a.id < b.id;
        return std::tie(a.retired_at_ns, a.retirement_id) >
               std::tie(b.retired_at_ns, b.retirement_id);
    });
    all.erase(std::unique(all.begin(), all.end(),
                          [](const GarbageRef& a, const GarbageRef& b) { return a.id == b.id; }),
              all.end());
    return all;
}

uint64_t tombstone_count(const MetadataSnapshot& snapshot) noexcept {
    uint64_t count = snapshot.garbage.size();
    for (const auto& batch : snapshot.tombstone_batches)
        count += batch.count;
    return count;
}

bool garbage_is_canonical(const std::vector<GarbageRef>& garbage) {
    for (size_t i = 1; i < garbage.size(); ++i)
        if (!(garbage[i - 1].id < garbage[i].id))
            return false;
    return true;
}

void canonicalise_garbage(std::vector<GarbageRef>& garbage) {
    std::stable_sort(garbage.begin(), garbage.end(),
                     [](const GarbageRef& a, const GarbageRef& b) { return a.id < b.id; });
}

bool same_content(const FsEntry& a, const FsEntry& b) {
    return a.type == b.type && a.mode == b.mode && a.uid == b.uid && a.gid == b.gid &&
           a.size == b.size && a.extents == b.extents;
}

std::optional<FsEntry> conflict_installed_entry(const MetadataConflict& conflict) {
    // A merge of two heads puts the alternative it installed on the left.
    return conflict.later_installed ? conflict.left_entry : conflict.base_entry;
}

std::optional<ObjectId> conflict_installed_catalogue_root(const MetadataConflict& conflict) {
    return conflict.later_installed ? conflict.left_catalogue_root
                                    : conflict.base_catalogue_root;
}

const FsEntry& later_entry(const FsEntry& a, const FsEntry& b) {
    if (a.mtime_ns != b.mtime_ns)
        return a.mtime_ns > b.mtime_ns ? a : b;
    return a < b ? b : a;
}

size_t prune_superseded_conflicts(MetadataSnapshot& snapshot, const NamespaceLookup& lookup) {
    size_t pruned = 0;
    for (auto it = snapshot.conflicts.begin(); it != snapshot.conflicts.end();) {
        const auto& conflict = it->second;
        bool superseded = false;
        if (conflict.kind == MetadataConflictKind::namespace_entry) {
            std::optional<FsEntry> live;
            if (lookup)
                live = lookup(conflict.key);
            else if (auto found = snapshot.entries.find(conflict.key);
                     found != snapshot.entries.end())
                live = found->second;
            // The merge left one value in place; any other is a decision.
            superseded = live != conflict_installed_entry(conflict);
            // Identical alternatives over a base need no decision: the
            // lesser is installed.
            if (!superseded && !lookup && !conflict.later_installed && conflict.left_entry &&
                conflict.right_entry &&
                same_content(*conflict.left_entry, *conflict.right_entry)) {
                snapshot.entries[conflict.key] = (*conflict.left_entry < *conflict.right_entry)
                                                     ? *conflict.left_entry
                                                     : *conflict.right_entry;
                superseded = true;
            }
        } else if (conflict.kind == MetadataConflictKind::catalogue_root) {
            superseded = snapshot.catalogue_root != conflict_installed_catalogue_root(conflict) ||
                         (conflict.later_installed &&
                          snapshot.catalogue_dot != conflict.installed_dot);
        }
        if (superseded) {
            it = snapshot.conflicts.erase(it);
            ++pruned;
        } else {
            ++it;
        }
    }
    return pruned;
}
void encode_torrent_requests(Writer& w, const MetadataSnapshot& s) {
    if (s.torrent_requests.size() > max_torrent_requests)
        throw std::runtime_error("too many torrent requests");
    w.u32(static_cast<uint32_t>(s.torrent_requests.size()));
    for (const auto& [id, request] : s.torrent_requests) {
        if (id != request.id) throw std::runtime_error("torrent request keyed by another id");
        encode_torrent_request(w, request);
    }
}

void decode_torrent_requests(Reader& r, MetadataSnapshot& s) {
    const auto count = r.u32();
    if (count > max_torrent_requests)
        throw DecodeError("too many torrent requests");
    for (uint32_t i = 0; i < count; ++i) {
        auto request = decode_torrent_request(r);
        auto id = request.id;
        if (id.empty() || !s.torrent_requests.emplace(std::move(id), std::move(request)).second)
            throw DecodeError("bad torrent request key");
    }
}

Bytes encode_snapshot(const MetadataSnapshot& s) {
    // SM13 and earlier cannot carry a namespace root; a tree-backed snapshot's
    // entries live in the tree, so encoding it would publish an empty namespace.
    if (s.namespace_root)
        throw std::runtime_error("namespace root cannot be encoded before SM14");
    if (!s.tombstone_batches.empty())
        throw std::runtime_error("tombstone batches need a tree-backed snapshot (SM19)");
    Writer w;
    // The smallest format that holds the state: SM11 adds branch topology and
    // conflicts, SM12 the metadata write floor, SM13 the participant roster
    // and branch floor.
    const bool legacy_clock_state = s.legacy_clock.has_value();
    const bool torrent_state = legacy_clock_state || !s.torrent_requests.empty();
    const bool branch_state = torrent_state || !s.merge_parents.empty() || !s.conflicts.empty();
    const bool policy_state = torrent_state || s.metadata_write_replicas_required != 0;
    const bool governance_state = torrent_state || !s.metadata_participants.empty() ||
                                  s.metadata_branch_floor != Hash256{} ||
                                  s.retention_baseline_complete;
    const bool include_node_status =
        policy_state || branch_state || !s.node_status.empty() || !s.identity_resets.empty();
    if (legacy_clock_state)
        w.raw(SM18);
    else if (torrent_state)
        w.raw(SM15);
    else if (governance_state)
        w.raw(SM13);
    else if (policy_state)
        w.raw(SM12);
    else if (branch_state)
        w.raw(SM11);
    else if (!s.identity_resets.empty())
        w.raw(SM10);
    else
        w.raw(s.node_status.empty() ? SM8 : SM9);
    w.u32(s.metadata_voters.size());
    for (auto& v : s.metadata_voters)
        w.fixed(v.bytes);
    w.u32(s.data_replication);
    w.u64(s.extent_size);
    w.u32(s.mutation_sequences.size());
    for (const auto& [node, sequence] : s.mutation_sequences) {
        w.fixed(node.bytes);
        w.u64(sequence);
    }
    w.u32(s.entries.size());
    for (auto& [p, e] : s.entries) {
        w.string(p);
        entry(w, e);
    }
    w.u8(s.catalogue_root.has_value());
    if (s.catalogue_root)
        w.fixed(s.catalogue_root->bytes);
    w.u32(s.garbage.size());
    for (const auto& garbage : s.garbage) {
        w.fixed(garbage.id.bytes);
        w.i64(garbage.retired_at_ns);
        w.fixed(garbage.retirement_id.bytes);
    }
    if (include_node_status) {
        w.u32(static_cast<uint32_t>(s.node_status.size()));
        for (const auto& [node, status] : s.node_status) {
            w.fixed(node.bytes);
            encode_node_status(w, status);
        }
    }
    if (policy_state || !s.identity_resets.empty() || branch_state) {
        w.u32(static_cast<uint32_t>(s.identity_resets.size()));
        for (const auto& [key, reset] : s.identity_resets) {
            w.string(key);
            encode_identity_reset(w, reset);
        }
    }
    if (policy_state || branch_state) {
        if (s.merge_parents.size() > 64)
            throw std::runtime_error("too many metadata merge parents");
        w.u32(static_cast<uint32_t>(s.merge_parents.size()));
        for (const auto& parent : s.merge_parents)
            w.fixed(parent.bytes);
        if (s.conflicts.size() > 1000000)
            throw std::runtime_error("too many metadata conflicts");
        w.u32(static_cast<uint32_t>(s.conflicts.size()));
        for (const auto& [id, conflict] : s.conflicts) {
            w.string(id);
            encode_conflict(w, conflict);
        }
    }
    if (policy_state)
        w.u32(s.metadata_write_replicas_required);
    if (governance_state) {
        if (s.metadata_participants.size() > 65536)
            throw std::runtime_error("too many metadata participants");
        w.u32(static_cast<uint32_t>(s.metadata_participants.size()));
        for (const auto& participant : s.metadata_participants)
            w.fixed(participant.bytes);
        w.fixed(s.metadata_branch_floor.bytes);
        w.u8(s.retention_baseline_complete ? 1 : 0);
    }
    if (torrent_state)
        encode_torrent_requests(w, s);
    if (legacy_clock_state) {
        encode_clock(w, *s.legacy_clock);
        w.fixed(s.catalogue_dot.author.bytes);
        w.u64(s.catalogue_dot.sequence);
    }
    return w.take();
}
Bytes encode_snapshot_v14(const MetadataSnapshot& s) {
    // SM13's fields in the same order, with the inline entry block replaced by
    // the 32-byte namespace tree root. Every section is unconditional.
    if (!s.namespace_root)
        throw std::runtime_error("SM14 snapshot has no namespace root");
    // Entries live in the tree or the map, never both; `detach_namespace`
    // clears the map as it builds the tree.
    if (!s.entries.empty())
        throw std::runtime_error("SM14 snapshot still inlines its entries");
    // SM14 requires an established write floor; zero means the snapshot has
    // not had its policy transition.
    if (!s.metadata_write_replicas_required)
        throw std::runtime_error("SM14 snapshot has no metadata write floor");
    if (s.merge_parents.size() > 64)
        throw std::runtime_error("too many metadata merge parents");
    if (s.conflicts.size() > 1000000)
        throw std::runtime_error("too many metadata conflicts");
    if (s.metadata_participants.size() > 65536)
        throw std::runtime_error("too many metadata participants");

    const bool batches = !s.tombstone_batches.empty();
    Writer w;
    w.raw(batches ? SM19 : s.legacy_clock ? SM17 : s.torrent_requests.empty() ? SM14 : SM16);
    w.u32(s.metadata_voters.size());
    for (const auto& v : s.metadata_voters)
        w.fixed(v.bytes);
    w.u32(s.data_replication);
    w.u64(s.extent_size);
    w.u32(s.mutation_sequences.size());
    for (const auto& [node, sequence] : s.mutation_sequences) {
        w.fixed(node.bytes);
        w.u64(sequence);
    }
    w.fixed(s.namespace_root->bytes);
    w.u8(s.catalogue_root.has_value());
    if (s.catalogue_root)
        w.fixed(s.catalogue_root->bytes);
    w.u32(s.garbage.size());
    for (const auto& garbage : s.garbage) {
        w.fixed(garbage.id.bytes);
        w.i64(garbage.retired_at_ns);
        w.fixed(garbage.retirement_id.bytes);
    }
    w.u32(static_cast<uint32_t>(s.node_status.size()));
    for (const auto& [node, status] : s.node_status) {
        w.fixed(node.bytes);
        encode_node_status(w, status);
    }
    w.u32(static_cast<uint32_t>(s.identity_resets.size()));
    for (const auto& [key, reset] : s.identity_resets) {
        w.string(key);
        encode_identity_reset(w, reset);
    }
    w.u32(static_cast<uint32_t>(s.merge_parents.size()));
    for (const auto& parent : s.merge_parents)
        w.fixed(parent.bytes);
    w.u32(static_cast<uint32_t>(s.conflicts.size()));
    for (const auto& [id, conflict] : s.conflicts) {
        w.string(id);
        encode_conflict(w, conflict);
    }
    w.u32(s.metadata_write_replicas_required);
    w.u32(static_cast<uint32_t>(s.metadata_participants.size()));
    for (const auto& participant : s.metadata_participants)
        w.fixed(participant.bytes);
    w.fixed(s.metadata_branch_floor.bytes);
    w.u8(s.retention_baseline_complete ? 1 : 0);
    if (batches || s.legacy_clock || !s.torrent_requests.empty())
        encode_torrent_requests(w, s);
    if (batches)
        w.u8(s.legacy_clock ? 1 : 0);
    if (s.legacy_clock) {
        encode_clock(w, *s.legacy_clock);
        w.fixed(s.catalogue_dot.author.bytes);
        w.u64(s.catalogue_dot.sequence);
    }
    if (batches) {
        if (s.tombstone_batches.size() > 10'000'000)
            throw std::runtime_error("too many tombstone batches");
        w.u32(static_cast<uint32_t>(s.tombstone_batches.size()));
        for (const auto& batch : s.tombstone_batches) {
            w.fixed(batch.id.bytes);
            w.i64(batch.retired_at_ns);
            w.u32(batch.count);
        }
    }
    return w.take();
}

namespace {
// Leaves `entries` empty: decoding has no node store. Callers needing the map
// use `attach_namespace`; others read paths via `namespace_tree_lookup`.
// SM19 (`batches`) carries the torrent requests, a legacy clock flag, and the
// tombstone batches after the clock.
MetadataSnapshot decode_snapshot_v14(Reader& r, bool torrent_requests, bool legacy_clock,
                                     bool batches = false) {
    MetadataSnapshot s;
    const auto voters = r.u32();
    if (voters > 1024)
        throw DecodeError("too many metadata voters");
    for (uint32_t i = 0; i < voters; ++i)
        s.metadata_voters.push_back(NodeId{r.fixed<16>()});
    s.data_replication = r.u32();
    s.extent_size = r.u64();
    const auto mutations = r.u32();
    if (mutations > 65536)
        throw DecodeError("too many metadata mutation origins");
    for (uint32_t i = 0; i < mutations; ++i) {
        NodeId node{r.fixed<16>()};
        const auto sequence = r.u64();
        if (!sequence || !s.mutation_sequences.emplace(node, sequence).second)
            throw DecodeError("bad metadata mutation sequence");
    }
    ObjectId namespace_root;
    namespace_root.bytes = r.fixed<32>();
    if (namespace_root == ObjectId{})
        throw DecodeError("missing namespace root");
    s.namespace_root = namespace_root;
    if (r.u8()) {
        ObjectId catalogue_root;
        catalogue_root.bytes = r.fixed<32>();
        s.catalogue_root = catalogue_root;
    }
    const auto garbage_count = r.u32();
    if (garbage_count > 10000000)
        throw DecodeError("too many garbage records");
    // 56 encoded bytes per record: bound the untrusted count by the input.
    s.garbage.reserve(std::min<size_t>(garbage_count, r.remaining() / 56));
    for (uint32_t i = 0; i < garbage_count; ++i) {
        GarbageRef garbage;
        garbage.id.bytes = r.fixed<32>();
        garbage.retired_at_ns = r.i64();
        if (garbage.retired_at_ns < 0)
            throw DecodeError("bad garbage retirement time");
        garbage.retirement_id.bytes = r.fixed<16>();
        s.garbage.push_back(garbage);
    }
    const auto statuses = r.u32();
    if (statuses > 65536)
        throw DecodeError("too many persisted node status records");
    for (uint32_t i = 0; i < statuses; ++i) {
        NodeId node{r.fixed<16>()};
        if (!s.node_status.emplace(node, decode_node_status(r)).second)
            throw DecodeError("duplicate persisted node status");
    }
    const auto resets = r.u32();
    if (resets > 65536)
        throw DecodeError("too many identity reset tombstones");
    for (uint32_t i = 0; i < resets; ++i) {
        auto key = r.string(8192);
        auto reset = decode_identity_reset(r);
        if (key != identity_reset_key(reset.host, reset.port) ||
            !s.identity_resets.emplace(std::move(key), std::move(reset)).second)
            throw DecodeError("bad identity reset tombstone key");
    }
    const auto parents = r.u32();
    if (parents > 64)
        throw DecodeError("too many metadata merge parents");
    s.merge_parents.reserve(parents);
    for (uint32_t i = 0; i < parents; ++i) {
        Hash256 parent;
        parent.bytes = r.fixed<32>();
        s.merge_parents.push_back(parent);
    }
    const auto conflicts = r.u32();
    if (conflicts > 1000000)
        throw DecodeError("too many metadata conflicts");
    for (uint32_t i = 0; i < conflicts; ++i) {
        auto id = r.string(256);
        auto conflict = decode_conflict(r);
        if (id.empty() || id != metadata_conflict_id(conflict) ||
            !s.conflicts.emplace(std::move(id), std::move(conflict)).second)
            throw DecodeError("bad metadata conflict id");
    }
    s.metadata_write_replicas_required = r.u32();
    if (!s.metadata_write_replicas_required)
        throw DecodeError("bad metadata write replica floor");
    const auto participants = r.u32();
    if (participants > 65536)
        throw DecodeError("too many metadata participants");
    for (uint32_t i = 0; i < participants; ++i) {
        NodeId participant{r.fixed<16>()};
        if (participant == NodeId{} || !s.metadata_participants.insert(participant).second)
            throw DecodeError("bad metadata participant");
    }
    s.metadata_branch_floor.bytes = r.fixed<32>();
    const auto baseline = r.u8();
    if (baseline > 1)
        throw DecodeError("bad retention baseline state");
    s.retention_baseline_complete = baseline != 0;
    if (torrent_requests || batches)
        decode_torrent_requests(r, s);
    if (batches) {
        const auto flag = r.u8();
        if (flag > 1)
            throw DecodeError("bad metadata legacy clock flag");
        legacy_clock = flag != 0;
    }
    if (legacy_clock) {
        s.legacy_clock = decode_clock(r);
        s.catalogue_dot.author.bytes = r.fixed<16>();
        s.catalogue_dot.sequence = r.u64();
    }
    if (batches) {
        const auto count = r.u32();
        if (count == 0 || count > r.remaining() / 44)
            throw DecodeError("bad tombstone batch count");
        s.tombstone_batches.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            TombstoneBatch batch;
            batch.id.bytes = r.fixed<32>();
            batch.retired_at_ns = r.i64();
            batch.count = r.u32();
            if (batch.count == 0 || batch.retired_at_ns < 0 ||
                (!s.tombstone_batches.empty() && !(s.tombstone_batches.back().id < batch.id)))
                throw DecodeError("bad tombstone batch");
            s.tombstone_batches.push_back(batch);
        }
    }
    r.finish();
    // No "/" check here: that needs a tree read, which belongs to the store's owner.
    return s;
}
} // namespace

MetadataSnapshot decode_snapshot(std::span<const uint8_t> d) {
    note_startup_progress();
    Reader r(d);
    auto m = r.raw(8);
    if (std::equal(m.begin(), m.end(), SM14.begin()))
        return decode_snapshot_v14(r, false, false);
    if (std::equal(m.begin(), m.end(), SM16.begin()))
        return decode_snapshot_v14(r, true, false);
    if (std::equal(m.begin(), m.end(), SM17.begin()))
        return decode_snapshot_v14(r, true, true);
    if (std::equal(m.begin(), m.end(), SM19.begin()))
        return decode_snapshot_v14(r, true, false, true);
    // SM15 is SM13 with every section present, then the torrent requests;
    // SM18 is SM15 and then the legacy clock.
    const bool v18 = std::equal(m.begin(), m.end(), SM18.begin());
    const bool v15 = v18 || std::equal(m.begin(), m.end(), SM15.begin());
    const bool v5 = std::equal(m.begin(), m.end(), SM5.begin());
    const bool v6 = std::equal(m.begin(), m.end(), SM6.begin());
    const bool v7 = std::equal(m.begin(), m.end(), SM7.begin());
    const bool v8 = std::equal(m.begin(), m.end(), SM8.begin());
    const bool v9 = std::equal(m.begin(), m.end(), SM9.begin());
    const bool v10 = std::equal(m.begin(), m.end(), SM10.begin());
    const bool v11 = std::equal(m.begin(), m.end(), SM11.begin());
    const bool v12 = std::equal(m.begin(), m.end(), SM12.begin());
    const bool v13 = v15 || std::equal(m.begin(), m.end(), SM13.begin());
    if (!v5 && !v6 && !v7 && !v8 && !v9 && !v10 && !v11 && !v12 && !v13)
        throw DecodeError("bad snapshot");
    auto nv = r.u32();
    if (nv > 1024)
        throw DecodeError("too many metadata voters");
    MetadataSnapshot s;
    for (uint32_t i = 0; i < nv; ++i) {
        NodeId v{r.fixed<16>()};
        s.metadata_voters.push_back(v);
    }
    s.data_replication = r.u32();
    s.extent_size = r.u64();
    if (v7 || v8 || v9 || v10 || v11 || v12 || v13) {
        auto mutations = r.u32();
        if (mutations > 65536)
            throw DecodeError("too many metadata mutation origins");
        for (uint32_t i = 0; i < mutations; ++i) {
            NodeId node{r.fixed<16>()};
            auto sequence = r.u64();
            if (!sequence || !s.mutation_sequences.emplace(node, sequence).second)
                throw DecodeError("bad metadata mutation sequence");
        }
    }
    auto n = r.u32();
    if (n > 5000000)
        throw DecodeError("too many filesystem entries");
    for (uint32_t i = 0; i < n; ++i) {
        auto p = normalize_path(r.string());
        if (!s.entries.emplace(p, entry(r)).second)
            throw DecodeError("duplicate path");
    }
    if ((v6 || v7 || v8 || v9 || v10 || v11 || v12 || v13) && r.u8()) {
        ObjectId root;
        root.bytes = r.fixed<32>();
        s.catalogue_root = root;
    }
    auto garbage_count = r.u32();
    if (garbage_count > 10000000)
        throw DecodeError("too many garbage records");
    s.garbage.reserve(garbage_count);
    for (uint32_t i = 0; i < garbage_count; ++i) {
        GarbageRef garbage;
        garbage.id.bytes = r.fixed<32>();
        if (v8 || v9 || v10 || v11 || v12 || v13) {
            garbage.retired_at_ns = r.i64();
            if (garbage.retired_at_ns < 0)
                throw DecodeError("bad garbage retirement time");
            garbage.retirement_id.bytes = r.fixed<16>();
        }
        s.garbage.push_back(garbage);
    }
    if (v9 || v10 || v11 || v12 || v13) {
        const auto count = r.u32();
        if (count > 65536)
            throw DecodeError("too many persisted node status records");
        for (uint32_t i = 0; i < count; ++i) {
            NodeId node{r.fixed<16>()};
            if (!s.node_status.emplace(node, decode_node_status(r)).second)
                throw DecodeError("duplicate persisted node status");
        }
    }
    if (v10 || v11 || v12 || v13) {
        const auto count = r.u32();
        if (count > 65536)
            throw DecodeError("too many identity reset tombstones");
        for (uint32_t i = 0; i < count; ++i) {
            auto key = r.string(8192);
            auto reset = decode_identity_reset(r);
            if (key != identity_reset_key(reset.host, reset.port) ||
                !s.identity_resets.emplace(std::move(key), std::move(reset)).second)
                throw DecodeError("bad identity reset tombstone key");
        }
    }
    if (v11 || v12 || v13) {
        const auto parents = r.u32();
        if (parents > 64)
            throw DecodeError("too many metadata merge parents");
        s.merge_parents.reserve(parents);
        for (uint32_t i = 0; i < parents; ++i) {
            Hash256 parent;
            parent.bytes = r.fixed<32>();
            s.merge_parents.push_back(parent);
        }
        const auto conflicts = r.u32();
        if (conflicts > 1000000)
            throw DecodeError("too many metadata conflicts");
        for (uint32_t i = 0; i < conflicts; ++i) {
            auto id = r.string(256);
            auto conflict = decode_conflict(r);
            if (id.empty() || id != metadata_conflict_id(conflict) ||
                !s.conflicts.emplace(std::move(id), std::move(conflict)).second)
                throw DecodeError("bad metadata conflict id");
        }
    }
    if (v12 || v13) {
        s.metadata_write_replicas_required = r.u32();
        // SM15 writes the floor whether or not one is set.
        if (!s.metadata_write_replicas_required && !v15)
            throw DecodeError("bad metadata write replica floor");
    }
    if (v13) {
        const auto participants = r.u32();
        if (participants > 65536)
            throw DecodeError("too many metadata participants");
        for (uint32_t i = 0; i < participants; ++i) {
            NodeId participant{r.fixed<16>()};
            if (participant == NodeId{} || !s.metadata_participants.insert(participant).second)
                throw DecodeError("bad metadata participant");
        }
        s.metadata_branch_floor.bytes = r.fixed<32>();
        const auto baseline = r.u8();
        if (baseline > 1)
            throw DecodeError("bad retention baseline state");
        s.retention_baseline_complete = baseline != 0;
    }
    if (v15)
        decode_torrent_requests(r, s);
    if (v18) {
        s.legacy_clock = decode_clock(r);
        s.catalogue_dot.author.bytes = r.fixed<16>();
        s.catalogue_dot.sequence = r.u64();
    }
    r.finish();
    auto x = s.entries.find("/");
    if (x == s.entries.end() || x->second.type != EntryType::directory)
        throw DecodeError("missing root");
    return s;
}

Bytes encode_metadata_delta(const MetadataDelta& delta) {
    // DLT1-DLT4 are replay-only: their successors reconstruct as SM7-SM9 and
    // would drop write-floor and governance state. DLT5+ reconstruct with the
    // current snapshot encoder.
    static constexpr std::array<uint8_t, 8> magic_v5{'D', 'H', 'T', 'M', 'D', 'L', 'T', '5'};
    static constexpr std::array<uint8_t, 8> magic_v6{'D', 'H', 'T', 'M', 'D', 'L', 'T', '6'};
    static constexpr std::array<uint8_t, 8> magic_v7{'D', 'H', 'T', 'M', 'D', 'L', 'T', '7'};
    static constexpr std::array<uint8_t, 8> magic_v8{'D', 'H', 'T', 'M', 'D', 'L', 'T', '8'};
    static constexpr std::array<uint8_t, 8> magic_v9{'D', 'H', 'T', 'M', 'D', 'L', 'T', '9'};
    static constexpr std::array<uint8_t, 8> magic_v10{'D', 'H', 'T', 'M', 'D', 'L', 'T', 'A'};
    static constexpr std::array<uint8_t, 8> magic_v11{'D', 'H', 'T', 'M', 'D', 'L', 'T', 'B'};
    if (!delta.pending_tombstones.empty())
        throw std::logic_error("metadata delta still holds pending tombstones");
    // DLT11 is DLT10 then the tombstone batches named and dropped, written
    // when a mutation names or drops one.
    const bool v11 =
        !delta.add_tombstone_batches.empty() || !delta.drop_tombstone_batches.empty();
    // DLT10 is DLT9 with a content dot on every append and a trailing legacy
    // clock, written when a mutation carries either.
    const bool v10 = v11 || delta.set_legacy_clock.has_value() || delta.set_catalogue_dot.has_value() ||
                     std::any_of(delta.append_entries.begin(), delta.append_entries.end(),
                                 [](const auto& item) { return bool(item.second.content); });
    const bool topology =
        delta.replace_merge_parents.has_value() || delta.replace_conflicts.has_value();
    // DLT9 is DLT8 plus a trailing torrent-requests section, written only when
    // a mutation touches a torrent request.
    const bool v9 =
        v10 || !delta.upsert_torrent_requests.empty() || !delta.erase_torrent_requests.empty();
    const bool v8 = v9 || !delta.append_entries.empty();
    // DLT7 when DLT5/6 cannot express it: one topology set without the other,
    // or canonical garbage order. Both sets together still encode as DLT6; a
    // peer that rejects DLT7 is covered by the sender's full-record fallback.
    const bool v7 = v8 || delta.canonical_garbage ||
                    (delta.replace_merge_parents.has_value() != delta.replace_conflicts.has_value());
    const bool v6 = !v7 && topology;
    Writer w;
    w.raw(v11   ? magic_v11
          : v10 ? magic_v10
          : v9  ? magic_v9
          : v8  ? magic_v8
          : v7  ? magic_v7
          : v6  ? magic_v6
                : magic_v5);
    w.u32(delta.mutation_sequences.size());
    for (const auto& [node, sequence] : delta.mutation_sequences) {
        w.fixed(node.bytes);
        w.u64(sequence);
    }
    w.u32(delta.erase_entries.size());
    for (const auto& path : delta.erase_entries)
        w.string(path);
    w.u32(delta.upsert_entries.size());
    for (const auto& [path, value] : delta.upsert_entries) {
        w.string(path);
        entry(w, value);
    }
    w.u8(static_cast<uint8_t>(delta.catalogue));
    if (delta.catalogue == CatalogueDelta::set) {
        if (!delta.catalogue_root)
            throw std::runtime_error("metadata delta missing catalogue root");
        w.fixed(delta.catalogue_root->bytes);
    }
    w.u32(delta.erase_garbage.size());
    for (const auto& id : delta.erase_garbage)
        w.fixed(id.bytes);
    w.u32(delta.upsert_garbage.size());
    for (const auto& garbage : delta.upsert_garbage) {
        w.fixed(garbage.id.bytes);
        w.i64(garbage.retired_at_ns);
        w.fixed(garbage.retirement_id.bytes);
    }
    w.u32(static_cast<uint32_t>(delta.upsert_node_status.size()));
    for (const auto& [node, status] : delta.upsert_node_status) {
        w.fixed(node.bytes);
        encode_node_status(w, status);
    }
    w.u32(static_cast<uint32_t>(delta.upsert_identity_resets.size()));
    for (const auto& [key, reset] : delta.upsert_identity_resets) {
        w.string(key);
        encode_identity_reset(w, reset);
    }
    if (v7) {
        uint8_t flags = 0;
        if (delta.replace_merge_parents)
            flags |= 0x01;
        if (delta.replace_conflicts)
            flags |= 0x02;
        if (delta.canonical_garbage)
            flags |= 0x04;
        w.u8(flags);
    }
    if (v6 && (!delta.replace_merge_parents || !delta.replace_conflicts))
        throw std::invalid_argument(
            "DLT6 metadata delta must replace both merge parents and conflicts");
    if (delta.replace_merge_parents) {
        const auto& parents = *delta.replace_merge_parents;
        if (parents.size() > 64)
            throw std::runtime_error("too many metadata delta merge parents");
        w.u32(static_cast<uint32_t>(parents.size()));
        for (const auto& parent : parents)
            w.fixed(parent.bytes);
    }
    if (delta.replace_conflicts) {
        const auto& conflicts = *delta.replace_conflicts;
        if (conflicts.size() > 1000000)
            throw std::runtime_error("too many metadata delta conflicts");
        w.u32(static_cast<uint32_t>(conflicts.size()));
        for (const auto& [id, conflict] : conflicts) {
            w.string(id);
            encode_conflict(w, conflict);
        }
    }
    if (v8) {
        w.u32(static_cast<uint32_t>(delta.append_entries.size()));
        for (const auto& [path, append] : delta.append_entries) {
            if (delta.upsert_entries.contains(path))
                throw std::invalid_argument("metadata delta appends and upserts the same path");
            w.string(path);
            w.u64(append.size);
            w.i64(append.mtime_ns);
            w.i64(append.ctime_ns);
            w.u64(append.version);
            w.u32(append.base_extents);
            w.u32(static_cast<uint32_t>(append.extents.size()));
            for (const auto& x : append.extents) {
                w.u64(x.offset);
                w.u64(x.length);
                w.u8(x.hole);
                w.fixed(x.id.bytes);
            }
            if (v10) {
                w.fixed(append.content.author.bytes);
                w.u64(append.content.sequence);
            }
        }
    }
    if (v9) {
        w.u32(static_cast<uint32_t>(delta.upsert_torrent_requests.size()));
        for (const auto& [id, request] : delta.upsert_torrent_requests) {
            if (id != request.id) throw std::invalid_argument("torrent request keyed by another id");
            encode_torrent_request(w, request);
        }
        w.u32(static_cast<uint32_t>(delta.erase_torrent_requests.size()));
        for (const auto& id : delta.erase_torrent_requests) w.string(id);
    }
    if (v10) {
        w.u8(delta.set_legacy_clock.has_value());
        if (delta.set_legacy_clock)
            encode_clock(w, *delta.set_legacy_clock);
        w.u8(delta.set_catalogue_dot.has_value());
        if (delta.set_catalogue_dot) {
            w.fixed(delta.set_catalogue_dot->author.bytes);
            w.u64(delta.set_catalogue_dot->sequence);
        }
    }
    if (v11) {
        w.u32(static_cast<uint32_t>(delta.add_tombstone_batches.size()));
        for (const auto& batch : delta.add_tombstone_batches) {
            w.fixed(batch.id.bytes);
            w.i64(batch.retired_at_ns);
            w.u32(batch.count);
        }
        w.u32(static_cast<uint32_t>(delta.drop_tombstone_batches.size()));
        for (const auto& id : delta.drop_tombstone_batches)
            w.fixed(id.bytes);
    }
    return w.take();
}

MetadataDelta decode_metadata_delta(std::span<const uint8_t> data) {
    static constexpr std::array<uint8_t, 8> magic_v1{'D', 'H', 'T', 'M', 'D', 'L', 'T', '1'};
    static constexpr std::array<uint8_t, 8> magic_v2{'D', 'H', 'T', 'M', 'D', 'L', 'T', '2'};
    static constexpr std::array<uint8_t, 8> magic_v3{'D', 'H', 'T', 'M', 'D', 'L', 'T', '3'};
    static constexpr std::array<uint8_t, 8> magic_v4{'D', 'H', 'T', 'M', 'D', 'L', 'T', '4'};
    static constexpr std::array<uint8_t, 8> magic_v5{'D', 'H', 'T', 'M', 'D', 'L', 'T', '5'};
    static constexpr std::array<uint8_t, 8> magic_v6{'D', 'H', 'T', 'M', 'D', 'L', 'T', '6'};
    static constexpr std::array<uint8_t, 8> magic_v7{'D', 'H', 'T', 'M', 'D', 'L', 'T', '7'};
    static constexpr std::array<uint8_t, 8> magic_v8{'D', 'H', 'T', 'M', 'D', 'L', 'T', '8'};
    static constexpr std::array<uint8_t, 8> magic_v9{'D', 'H', 'T', 'M', 'D', 'L', 'T', '9'};
    static constexpr std::array<uint8_t, 8> magic_v10{'D', 'H', 'T', 'M', 'D', 'L', 'T', 'A'};
    static constexpr std::array<uint8_t, 8> magic_v11{'D', 'H', 'T', 'M', 'D', 'L', 'T', 'B'};
    Reader r(data);
    auto got = r.raw(magic_v1.size());
    const bool v1 = std::equal(got.begin(), got.end(), magic_v1.begin());
    const bool v2 = std::equal(got.begin(), got.end(), magic_v2.begin());
    const bool v3 = std::equal(got.begin(), got.end(), magic_v3.begin());
    const bool v4 = std::equal(got.begin(), got.end(), magic_v4.begin());
    const bool v5 = std::equal(got.begin(), got.end(), magic_v5.begin());
    // DLT10 is DLT9 with a content dot on every append and a trailing legacy
    // clock.
    // DLT11 is DLT10 then the tombstone batches.
    const bool v11 = std::equal(got.begin(), got.end(), magic_v11.begin());
    const bool v10 = v11 || std::equal(got.begin(), got.end(), magic_v10.begin());
    // DLT9 is DLT8 plus a trailing torrent-requests section.
    const bool v9 = v10 || std::equal(got.begin(), got.end(), magic_v9.begin());
    const bool v8 = v9 || std::equal(got.begin(), got.end(), magic_v8.begin());
    // DLT8 is DLT7 plus a trailing append-entries section.
    const bool v7 = v8 || std::equal(got.begin(), got.end(), magic_v7.begin());
    // DLT7 is DLT6 plus a flags byte; everything before the topology sets is
    // shared, so treat v7 as v6 for the common prefix.
    const bool v6 = v7 || std::equal(got.begin(), got.end(), magic_v6.begin());
    if (!v1 && !v2 && !v3 && !v4 && !v5 && !v6)
        throw DecodeError("bad metadata delta");
    MetadataDelta delta;
    auto sequences = r.u32();
    if (sequences > 65536)
        throw DecodeError("too many metadata delta sequences");
    for (uint32_t i = 0; i < sequences; ++i) {
        NodeId node{r.fixed<16>()};
        auto sequence = r.u64();
        if (!sequence || !delta.mutation_sequences.emplace(node, sequence).second)
            throw DecodeError("bad metadata delta sequence");
    }
    auto erased = r.u32();
    if (erased > 5000000)
        throw DecodeError("too many metadata delta erases");
    delta.erase_entries.reserve(erased);
    for (uint32_t i = 0; i < erased; ++i)
        delta.erase_entries.push_back(normalize_path(r.string()));
    if (!std::is_sorted(delta.erase_entries.begin(), delta.erase_entries.end()) ||
        std::adjacent_find(delta.erase_entries.begin(), delta.erase_entries.end()) !=
            delta.erase_entries.end())
        throw DecodeError("metadata delta erases not canonical");
    auto upserts = r.u32();
    if (upserts > 5000000)
        throw DecodeError("too many metadata delta upserts");
    for (uint32_t i = 0; i < upserts; ++i) {
        auto path = normalize_path(r.string());
        if (!delta.upsert_entries.emplace(path, entry(r)).second)
            throw DecodeError("duplicate metadata delta path");
    }
    auto catalogue = r.u8();
    if (catalogue > static_cast<uint8_t>(CatalogueDelta::set))
        throw DecodeError("bad metadata catalogue delta");
    delta.catalogue = static_cast<CatalogueDelta>(catalogue);
    if (delta.catalogue == CatalogueDelta::set) {
        ObjectId id;
        id.bytes = r.fixed<32>();
        delta.catalogue_root = id;
    }

    if (v1) {
        // DLT1: journal replay only.
        auto garbage = r.u32();
        if (garbage > 10000000)
            throw DecodeError("too much metadata delta garbage");
        delta.upsert_garbage.reserve(garbage);
        for (uint32_t i = 0; i < garbage; ++i) {
            GarbageRef value;
            value.id.bytes = r.fixed<32>();
            delta.upsert_garbage.push_back(value);
        }
    } else {
        auto erased_garbage = r.u32();
        if (erased_garbage > 10000000)
            throw DecodeError("too many metadata delta garbage erases");
        delta.erase_garbage.reserve(erased_garbage);
        std::set<ObjectId> erased_ids;
        for (uint32_t i = 0; i < erased_garbage; ++i) {
            ObjectId id;
            id.bytes = r.fixed<32>();
            if (!erased_ids.insert(id).second)
                throw DecodeError("duplicate metadata delta garbage erase");
            delta.erase_garbage.push_back(id);
        }
        auto garbage = r.u32();
        if (garbage > 10000000)
            throw DecodeError("too many metadata delta garbage upserts");
        delta.upsert_garbage.reserve(garbage);
        std::set<ObjectId> upsert_ids;
        for (uint32_t i = 0; i < garbage; ++i) {
            GarbageRef value;
            value.id.bytes = r.fixed<32>();
            value.retired_at_ns = r.i64();
            value.retirement_id.bytes = r.fixed<16>();
            if (value.retired_at_ns < 0 || !upsert_ids.insert(value.id).second)
                throw DecodeError("bad metadata delta garbage upsert");
            delta.upsert_garbage.push_back(value);
        }
    }
    if (v3 || v4 || v5 || v6) {
        const auto count = r.u32();
        if (count > 65536)
            throw DecodeError("too many metadata delta node status records");
        for (uint32_t i = 0; i < count; ++i) {
            NodeId node{r.fixed<16>()};
            if (!delta.upsert_node_status.emplace(node, decode_node_status(r)).second)
                throw DecodeError("duplicate metadata delta node status");
        }
    }
    if (v4 || v5 || v6) {
        const auto count = r.u32();
        if (count > 65536)
            throw DecodeError("too many metadata delta identity resets");
        for (uint32_t i = 0; i < count; ++i) {
            auto key = r.string(8192);
            auto reset = decode_identity_reset(r);
            if (key != identity_reset_key(reset.host, reset.port) ||
                !delta.upsert_identity_resets.emplace(std::move(key), std::move(reset)).second)
                throw DecodeError("bad metadata delta identity reset key");
        }
    }
    bool read_parents = v6, read_conflicts = v6;
    if (v7) {
        const auto flags = r.u8();
        if (flags & ~0x07U)
            throw DecodeError("bad metadata delta flags");
        read_parents = flags & 0x01U;
        read_conflicts = flags & 0x02U;
        delta.canonical_garbage = flags & 0x04U;
    }
    if (read_parents) {
        const auto parent_count = r.u32();
        if (parent_count > 64)
            throw DecodeError("too many metadata delta merge parents");
        std::vector<Hash256> parents;
        parents.reserve(parent_count);
        for (uint32_t i = 0; i < parent_count; ++i)
            parents.push_back(Hash256{r.fixed<32>()});
        delta.replace_merge_parents = std::move(parents);
    }
    if (read_conflicts) {
        const auto conflict_count = r.u32();
        if (conflict_count > 1000000)
            throw DecodeError("too many metadata delta conflicts");
        std::map<std::string, MetadataConflict, std::less<>> conflicts;
        for (uint32_t i = 0; i < conflict_count; ++i) {
            auto id = r.string();
            auto conflict = decode_conflict(r);
            if (id != metadata_conflict_id(conflict) ||
                !conflicts.emplace(std::move(id), std::move(conflict)).second)
                throw DecodeError("bad metadata delta conflict");
        }
        delta.replace_conflicts = std::move(conflicts);
    }
    if (v8) {
        const auto count = r.u32();
        if (count > 5000000)
            throw DecodeError("too many metadata delta appends");
        for (uint32_t i = 0; i < count; ++i) {
            auto path = normalize_path(r.string());
            MetadataDelta::EntryAppend append;
            append.size = r.u64();
            append.mtime_ns = r.i64();
            append.ctime_ns = r.i64();
            append.version = r.u64();
            append.base_extents = r.u32();
            const auto n = r.u32();
            if (n > 10000000)
                throw DecodeError("too many appended extents");
            append.extents.reserve(n);
            for (uint32_t k = 0; k < n; ++k) {
                ExtentRef x;
                x.offset = r.u64();
                x.length = r.u64();
                x.hole = r.u8() != 0;
                x.id = ObjectId{r.fixed<32>()};
                append.extents.push_back(x);
            }
            if (v10) {
                append.content.author.bytes = r.fixed<16>();
                append.content.sequence = r.u64();
            }
            if (delta.upsert_entries.contains(path) ||
                !delta.append_entries.emplace(std::move(path), std::move(append)).second)
                throw DecodeError("duplicate metadata delta append path");
        }
    }
    if (v9) {
        const auto upserts = r.u32();
        if (upserts > max_torrent_requests)
            throw DecodeError("too many torrent requests in delta");
        for (uint32_t i = 0; i < upserts; ++i) {
            auto request = decode_torrent_request(r);
            auto id = request.id;
            if (!delta.upsert_torrent_requests.emplace(std::move(id), std::move(request)).second)
                throw DecodeError("duplicate torrent request in delta");
        }
        const auto erasures = r.u32();
        if (erasures > max_torrent_requests)
            throw DecodeError("too many torrent request erasures in delta");
        for (uint32_t i = 0; i < erasures; ++i) {
            auto id = r.string(max_torrent_request_text);
            if (delta.upsert_torrent_requests.contains(id))
                throw DecodeError("torrent request both written and erased");
            delta.erase_torrent_requests.push_back(std::move(id));
        }
    }
    if (v10) {
        const auto set = r.u8();
        if (set > 1)
            throw DecodeError("bad metadata delta legacy clock flag");
        if (set)
            delta.set_legacy_clock = decode_clock(r);
        const auto dot = r.u8();
        if (dot > 1)
            throw DecodeError("bad metadata delta catalogue dot flag");
        if (dot) {
            MetadataDot value;
            value.author.bytes = r.fixed<16>();
            value.sequence = r.u64();
            delta.set_catalogue_dot = value;
        }
    }
    if (v11) {
        const auto adds = r.u32();
        if (adds > r.remaining() / 44)
            throw DecodeError("too many tombstone batches in delta");
        for (uint32_t i = 0; i < adds; ++i) {
            TombstoneBatch batch;
            batch.id.bytes = r.fixed<32>();
            batch.retired_at_ns = r.i64();
            batch.count = r.u32();
            if (batch.count == 0 || batch.retired_at_ns < 0 ||
                (!delta.add_tombstone_batches.empty() &&
                 !(delta.add_tombstone_batches.back().id < batch.id)))
                throw DecodeError("bad tombstone batch in delta");
            delta.add_tombstone_batches.push_back(batch);
        }
        const auto drops = r.u32();
        if (drops > r.remaining() / 32)
            throw DecodeError("too many dropped tombstone batches in delta");
        for (uint32_t i = 0; i < drops; ++i) {
            ObjectId id{r.fixed<32>()};
            if (!delta.drop_tombstone_batches.empty() &&
                !(delta.drop_tombstone_batches.back() < id))
                throw DecodeError("dropped tombstone batches out of order");
            delta.drop_tombstone_batches.push_back(id);
        }
    }
    r.finish();
    return delta;
}

std::optional<MetadataDelta> metadata_delta(const MetadataSnapshot& before,
                                            const MetadataSnapshot& after) {
    if (before.metadata_voters != after.metadata_voters ||
        before.data_replication != after.data_replication ||
        before.extent_size != after.extent_size ||
        before.metadata_write_replicas_required != after.metadata_write_replicas_required ||
        before.metadata_participants != after.metadata_participants ||
        before.metadata_branch_floor != after.metadata_branch_floor ||
        before.retention_baseline_complete != after.retention_baseline_complete)
        return {};

    MetadataDelta delta;
    // Each topology set independently: DLT7 flags each set's presence, so a
    // conflict-free merge carries only its merge_parents.
    if (before.merge_parents != after.merge_parents)
        delta.replace_merge_parents = after.merge_parents;
    if (before.conflicts != after.conflicts)
        delta.replace_conflicts = after.conflicts;
    for (const auto& [node, sequence] : before.mutation_sequences) {
        auto it = after.mutation_sequences.find(node);
        if (it == after.mutation_sequences.end() || it->second < sequence)
            return {};
    }
    for (const auto& [node, sequence] : after.mutation_sequences) {
        auto it = before.mutation_sequences.find(node);
        if (it == before.mutation_sequences.end() || it->second != sequence)
            delta.mutation_sequences.emplace(node, sequence);
    }

    if (before.legacy_clock != after.legacy_clock) {
        // Set once and never changed by a mutation; a merge's join is not a
        // delta.
        if (before.legacy_clock || !after.legacy_clock)
            return {};
        delta.set_legacy_clock = after.legacy_clock;
    }
    if (before.catalogue_dot != after.catalogue_dot)
        delta.set_catalogue_dot = after.catalogue_dot;

    for (const auto& [path, value] : before.entries) {
        auto it = after.entries.find(path);
        if (it == after.entries.end())
            delta.erase_entries.push_back(path);
    }
    for (const auto& [path, value] : after.entries) {
        auto it = before.entries.find(path);
        if (it == before.entries.end())
            delta.upsert_entries.emplace(path, value);
        else if (it->second != value)
            record_entry_change(delta, path, &it->second, value);
    }

    if (before.catalogue_root != after.catalogue_root) {
        if (after.catalogue_root) {
            delta.catalogue = CatalogueDelta::set;
            delta.catalogue_root = after.catalogue_root;
        } else {
            delta.catalogue = CatalogueDelta::clear;
        }
    }

    // Tombstone batches are sorted by id and immutable: named or dropped.
    {
        const auto by_id = [](const TombstoneBatch& a, const TombstoneBatch& b) {
            return a.id < b.id;
        };
        std::set_difference(after.tombstone_batches.begin(), after.tombstone_batches.end(),
                            before.tombstone_batches.begin(), before.tombstone_batches.end(),
                            std::back_inserter(delta.add_tombstone_batches), by_id);
        std::vector<TombstoneBatch> dropped;
        std::set_difference(before.tombstone_batches.begin(), before.tombstone_batches.end(),
                            after.tombstone_batches.begin(), after.tombstone_batches.end(),
                            std::back_inserter(dropped), by_id);
        for (const auto& batch : dropped)
            delta.drop_tombstone_batches.push_back(batch.id);
    }

    // Garbage can hold millions of tombstones: diff sorted pointer indexes
    // rather than two std::map copies.
    std::vector<const GarbageRef*> before_garbage;
    std::vector<const GarbageRef*> after_garbage;
    before_garbage.reserve(before.garbage.size());
    after_garbage.reserve(after.garbage.size());
    for (const auto& garbage : before.garbage)
        before_garbage.push_back(&garbage);
    for (const auto& garbage : after.garbage)
        after_garbage.push_back(&garbage);
    const auto by_id = [](const GarbageRef* a, const GarbageRef* b) { return a->id < b->id; };
    std::sort(before_garbage.begin(), before_garbage.end(), by_id);
    std::sort(after_garbage.begin(), after_garbage.end(), by_id);
    for (size_t i = 1; i < before_garbage.size(); ++i)
        if (before_garbage[i - 1]->id == before_garbage[i]->id)
            return {};
    for (size_t i = 1; i < after_garbage.size(); ++i)
        if (after_garbage[i - 1]->id == after_garbage[i]->id)
            return {};

    // Deltas can erase, replace and append tombstones but not reorder retained
    // ones. A canonical (ObjectId-sorted) target uses DLT7's sort-after-apply;
    // otherwise the target order must be reachable by erase-then-append.
    const auto contains_id = [](const std::vector<const GarbageRef*>& sorted,
                                const ObjectId& id) {
        const auto found = std::lower_bound(
            sorted.begin(), sorted.end(), id,
            [](const GarbageRef* value, const ObjectId& candidate) {
                return value->id < candidate;
            });
        return found != sorted.end() && (*found)->id == id;
    };
    const bool garbage_changed =
        before.garbage.size() != after.garbage.size() ||
        !std::equal(before.garbage.begin(), before.garbage.end(), after.garbage.begin());
    if (garbage_changed && garbage_is_canonical(after.garbage)) {
        delta.canonical_garbage = true;
    } else if (garbage_changed) {
        size_t target = 0;
        for (const auto& garbage : before.garbage) {
            if (!contains_id(after_garbage, garbage.id))
                continue;
            if (target >= after.garbage.size() || after.garbage[target].id != garbage.id)
                return {};
            ++target;
        }
        // Newly-created tombstones are emitted below in sorted ObjectId order
        // and apply_metadata_delta_in_place() appends them in that order.
        for (const auto* garbage : after_garbage) {
            if (contains_id(before_garbage, garbage->id))
                continue;
            if (target >= after.garbage.size() || after.garbage[target].id != garbage->id)
                return {};
            ++target;
        }
        if (target != after.garbage.size())
            return {};
    }

    size_t bi = 0, ai = 0;
    while (bi < before_garbage.size() || ai < after_garbage.size()) {
        if (ai == after_garbage.size() ||
            (bi < before_garbage.size() && before_garbage[bi]->id < after_garbage[ai]->id)) {
            delta.erase_garbage.push_back(before_garbage[bi++]->id);
            continue;
        }
        if (bi == before_garbage.size() || after_garbage[ai]->id < before_garbage[bi]->id) {
            delta.upsert_garbage.push_back(*after_garbage[ai++]);
            continue;
        }
        if (*before_garbage[bi] != *after_garbage[ai])
            delta.upsert_garbage.push_back(*after_garbage[ai]);
        ++bi;
        ++ai;
    }

    for (const auto& [node, status] : after.node_status) {
        auto it = before.node_status.find(node);
        if (it == before.node_status.end() || it->second != status)
            delta.upsert_node_status.emplace(node, status);
    }
    // A snapshot with node status must keep it in every successor encoding;
    // an unchanged witness record forces the section onto the wire.
    if (!after.node_status.empty() && delta.upsert_node_status.empty())
        delta.upsert_node_status.emplace(*after.node_status.begin());

    for (const auto& [key, reset] : after.identity_resets) {
        auto it = before.identity_resets.find(key);
        if (it == before.identity_resets.end() || it->second != reset)
            delta.upsert_identity_resets.emplace(key, reset);
    }
    for (const auto& [key, _] : before.identity_resets)
        if (!after.identity_resets.contains(key))
            return {}; // reset tombstones are monotonic; never erase via a delta
    if (!after.identity_resets.empty() && delta.upsert_identity_resets.empty())
        delta.upsert_identity_resets.emplace(*after.identity_resets.begin());

    // Torrent requests travel whole. No witness is needed: the successor's
    // encoding follows from whether any request exists.
    for (const auto& [id, request] : after.torrent_requests) {
        auto it = before.torrent_requests.find(id);
        if (it == before.torrent_requests.end() || it->second != request)
            delta.upsert_torrent_requests.emplace(id, request);
    }
    for (const auto& [id, _] : before.torrent_requests)
        if (!after.torrent_requests.contains(id))
            delta.erase_torrent_requests.push_back(id);

    return delta;
}

void apply_metadata_delta_in_place(MetadataSnapshot& out, const MetadataDelta& delta,
                                   const NamespaceDeltaApplier& namespace_applier) {
    note_startup_progress();
    // A tree-backed namespace is edited through the applier, never the map;
    // without an applier the delta is refused rather than silently lost.
    if (out.namespace_root) {
        if (!namespace_applier)
            throw DecodeError("cannot apply a metadata delta to a tree-backed namespace without a "
                              "namespace node store");
        out.namespace_root = namespace_applier(*out.namespace_root, delta);
    }
    if (delta.set_legacy_clock) {
        if (out.legacy_clock)
            throw DecodeError("metadata delta sets a legacy clock already set");
        out.legacy_clock = delta.set_legacy_clock;
    }
    if (delta.set_catalogue_dot)
        out.catalogue_dot = *delta.set_catalogue_dot;
    for (const auto& [node, sequence] : delta.mutation_sequences) {
        auto it = out.mutation_sequences.find(node);
        if (it != out.mutation_sequences.end() && sequence < it->second)
            throw DecodeError("metadata delta sequence regressed");
        out.mutation_sequences[node] = sequence;
    }
    // Entry edits apply to the map form only (a snapshot must not carry both a
    // root and a map). Erasing "/" is refused in either form.
    for (const auto& path : delta.erase_entries) {
        auto normalized = normalize_path(path);
        if (normalized == "/")
            throw DecodeError("metadata delta removed root");
        if (!out.namespace_root)
            out.entries.erase(normalized);
    }
    if (!out.namespace_root) {
        for (const auto& [path, value] : delta.upsert_entries)
            out.entries[normalize_path(path)] = value;
        for (const auto& [path, append] : delta.append_entries) {
            const auto normalized = normalize_path(path);
            auto found = out.entries.find(normalized);
            if (found == out.entries.end())
                throw DecodeError("metadata delta append base mismatch");
            apply_entry_append(found->second, normalized, append);
        }
    }
    switch (delta.catalogue) {
    case CatalogueDelta::unchanged:
        break;
    case CatalogueDelta::clear:
        out.catalogue_root.reset();
        break;
    case CatalogueDelta::set:
        if (!delta.catalogue_root)
            throw DecodeError("metadata delta missing catalogue root");
        out.catalogue_root = delta.catalogue_root;
        break;
    }
    // Indexed, not scanned: per-id scans are quadratic on tombstone-heavy
    // namespaces and can exceed the startup budget on replay. Every tombstone
    // with an erased id goes (duplicates included), retained ones keep their
    // order, an upsert replaces the first with that id in place, and new ones
    // append in delta order.
    if (!delta.erase_garbage.empty()) {
        const std::set<ObjectId> erased(delta.erase_garbage.begin(), delta.erase_garbage.end());
        std::erase_if(out.garbage,
                      [&](const GarbageRef& garbage) { return erased.contains(garbage.id); });
    }
    if (!delta.upsert_garbage.empty()) {
        std::map<ObjectId, size_t> first_index;
        for (size_t i = 0; i < out.garbage.size(); ++i)
            first_index.emplace(out.garbage[i].id, i);
        for (const auto& garbage : delta.upsert_garbage) {
            auto found = first_index.find(garbage.id);
            if (found == first_index.end()) {
                first_index.emplace(garbage.id, out.garbage.size());
                out.garbage.push_back(garbage);
            } else {
                out.garbage[found->second] = garbage;
            }
        }
    }
    if (delta.canonical_garbage)
        canonicalise_garbage(out.garbage);
    if (!delta.drop_tombstone_batches.empty()) {
        const std::set<ObjectId> dropped(delta.drop_tombstone_batches.begin(),
                                         delta.drop_tombstone_batches.end());
        std::erase_if(out.tombstone_batches,
                      [&](const TombstoneBatch& batch) { return dropped.contains(batch.id); });
    }
    for (const auto& batch : delta.add_tombstone_batches) {
        auto at = std::lower_bound(
            out.tombstone_batches.begin(), out.tombstone_batches.end(), batch.id,
            [](const TombstoneBatch& held, const ObjectId& id) { return held.id < id; });
        if (at != out.tombstone_batches.end() && at->id == batch.id) {
            if (*at != batch)
                throw DecodeError("tombstone batch named twice with different contents");
            continue;
        }
        out.tombstone_batches.insert(at, batch);
    }
    for (const auto& [node, status] : delta.upsert_node_status)
        out.node_status[node] = status;
    for (const auto& [key, reset] : delta.upsert_identity_resets) {
        auto found = out.identity_resets.find(key);
        if (found == out.identity_resets.end() || found->second.epoch < reset.epoch)
            out.identity_resets[key] = reset;
    }
    for (const auto& [id, request] : delta.upsert_torrent_requests)
        out.torrent_requests[id] = request;
    for (const auto& id : delta.erase_torrent_requests)
        out.torrent_requests.erase(id);
    if (delta.replace_merge_parents)
        out.merge_parents = *delta.replace_merge_parents;
    if (delta.replace_conflicts)
        out.conflicts = *delta.replace_conflicts;
    // For a tree-backed namespace the applier checks the root.
    if (!out.namespace_root) {
        auto root = out.entries.find("/");
        if (root == out.entries.end() || root->second.type != EntryType::directory)
            throw DecodeError("metadata delta lost root");
    }
}

MetadataSnapshot apply_metadata_delta(const MetadataSnapshot& before, const MetadataDelta& delta,
                                      const NamespaceDeltaApplier& namespace_applier) {
    MetadataSnapshot out = before;
    apply_metadata_delta_in_place(out, delta, namespace_applier);
    return out;
}

Hash256 metadata_hash(uint64_t g, const Hash256& p, std::span<const uint8_t> d) {
    // Streams the canonical encoding rather than copying a potentially huge payload.
    Sha256Hasher hash;
    hash_u64(hash, g);
    hash.update(p.bytes);
    hash_bytes(hash, d);
    return hash.finish();
}
Bytes encode_metadata_record(const MetadataRecord& m) {
    Writer w;
    w.u64(m.generation);
    w.fixed(m.previous.bytes);
    w.fixed(m.hash.bytes);
    w.bytes(m.payload);
    return w.take();
}

Bytes encode_metadata_acceptance(const MetadataAcceptance& input) {
    MetadataAcceptance value = input;
    std::sort(value.replicas.begin(), value.replicas.end());
    value.replicas.erase(std::unique(value.replicas.begin(), value.replicas.end()),
                         value.replicas.end());
    if (!value.generation || value.hash == Hash256{})
        throw std::runtime_error("bad metadata acceptance identity");
    if (std::any_of(value.replicas.begin(), value.replicas.end(),
                    [](const NodeId& replica) { return replica == NodeId{}; }))
        throw std::runtime_error("metadata acceptance contains empty replica identity");
    if (value.required && value.replicas.size() < value.required)
        throw std::runtime_error("metadata acceptance does not satisfy write floor");
    if (!value.required && !value.replicas.empty())
        throw std::runtime_error("legacy metadata acceptance cannot name replicas");
    if (value.replicas.size() > 1000000)
        throw std::runtime_error("too many metadata acceptance replicas");

    Writer writer;
    writer.u64(value.generation);
    writer.fixed(value.hash.bytes);
    writer.u32(value.required);
    writer.u32(static_cast<uint32_t>(value.replicas.size()));
    for (const auto& replica : value.replicas)
        writer.fixed(replica.bytes);
    return writer.take();
}

MetadataAcceptance decode_metadata_acceptance(std::span<const uint8_t> data) {
    Reader reader(data);
    MetadataAcceptance value;
    value.generation = reader.u64();
    value.hash.bytes = reader.fixed<32>();
    value.required = reader.u32();
    const auto count = reader.u32();
    if (count > 1000000)
        throw DecodeError("too many metadata acceptance replicas");
    value.replicas.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        NodeId replica;
        replica.bytes = reader.fixed<16>();
        value.replicas.push_back(replica);
    }
    reader.finish();
    if (!value.generation || value.hash == Hash256{})
        throw DecodeError("bad metadata acceptance identity");
    if (!std::is_sorted(value.replicas.begin(), value.replicas.end()) ||
        std::adjacent_find(value.replicas.begin(), value.replicas.end()) != value.replicas.end())
        throw DecodeError("metadata acceptance replicas are not canonical");
    if (std::any_of(value.replicas.begin(), value.replicas.end(),
                    [](const NodeId& replica) { return replica == NodeId{}; }))
        throw DecodeError("metadata acceptance contains empty replica identity");
    if (value.required) {
        if (value.replicas.size() < value.required)
            throw DecodeError("metadata acceptance does not satisfy write floor");
    } else if (!value.replicas.empty()) {
        throw DecodeError("legacy metadata acceptance cannot name replicas");
    }
    return value;
}

Bytes encode_metadata_acceptance_set(const std::vector<MetadataAcceptance>& values) {
    if (values.size() > 1000000)
        throw std::runtime_error("too many metadata accepted heads");
    Writer writer;
    writer.u32(static_cast<uint32_t>(values.size()));
    for (const auto& value : values)
        writer.bytes(encode_metadata_acceptance(value));
    return writer.take();
}

std::vector<MetadataAcceptance> decode_metadata_acceptance_set(std::span<const uint8_t> data) {
    Reader reader(data);
    const auto count = reader.u32();
    if (count > 1000000)
        throw DecodeError("too many metadata accepted heads");
    std::vector<MetadataAcceptance> values;
    values.reserve(count);
    std::set<Hash256> seen;
    for (uint32_t i = 0; i < count; ++i) {
        auto value = decode_metadata_acceptance(reader.bytes());
        if (!seen.insert(value.hash).second)
            throw DecodeError("duplicate metadata accepted head");
        values.push_back(std::move(value));
    }
    reader.finish();
    return values;
}

MetadataRecord decode_metadata_record(std::span<const uint8_t> d) {
    Reader r(d);
    MetadataRecord m;
    m.generation = r.u64();
    m.previous.bytes = r.fixed<32>();
    m.hash.bytes = r.fixed<32>();
    m.payload = r.bytes();
    r.finish();
    if (!valid_metadata_record(m))
        throw DecodeError("bad metadata hash");
    return m;
}

std::vector<Hash256> metadata_history_materialization_dependencies(
    const MetadataHistoryEntry& entry) {
    if (entry.body == MetadataHistoryEntry::Body::delta)
        return {entry.previous};
    return {};
}

Bytes encode_metadata_history_entry(const MetadataHistoryEntry& entry_value) {
    Writer writer;
    writer.u8(static_cast<uint8_t>(entry_value.body));
    writer.u64(entry_value.generation);
    writer.fixed(entry_value.previous.bytes);
    writer.fixed(entry_value.hash.bytes);
    writer.u8(entry_value.previous_known);
    if (entry_value.merge_parents.size() > 64)
        throw std::runtime_error("too many metadata history merge parents");
    writer.u32(static_cast<uint32_t>(entry_value.merge_parents.size()));
    for (const auto& parent : entry_value.merge_parents)
        writer.fixed(parent.bytes);
    writer.bytes(entry_value.payload);
    return writer.take();
}

MetadataHistoryEntry decode_metadata_history_entry(std::span<const uint8_t> data) {
    Reader reader(data);
    MetadataHistoryEntry entry_value;
    const auto body = reader.u8();
    if (body < static_cast<uint8_t>(MetadataHistoryEntry::Body::full) ||
        body > static_cast<uint8_t>(MetadataHistoryEntry::Body::delta))
        throw DecodeError("bad metadata history body kind");
    entry_value.body = static_cast<MetadataHistoryEntry::Body>(body);
    entry_value.generation = reader.u64();
    entry_value.previous.bytes = reader.fixed<32>();
    entry_value.hash.bytes = reader.fixed<32>();
    entry_value.previous_known = reader.u8() != 0;
    const auto parents = reader.u32();
    if (parents > 64)
        throw DecodeError("too many metadata history merge parents");
    entry_value.merge_parents.reserve(parents);
    for (uint32_t i = 0; i < parents; ++i) {
        Hash256 parent;
        parent.bytes = reader.fixed<32>();
        entry_value.merge_parents.push_back(parent);
    }
    entry_value.payload = reader.bytes();
    reader.finish();
    if (!entry_value.generation || entry_value.hash == Hash256{})
        throw DecodeError("bad metadata history identity");
    if (entry_value.generation <= 1 && entry_value.previous_known)
        throw DecodeError("genesis metadata history has a predecessor");
    if (entry_value.body == MetadataHistoryEntry::Body::delta && !entry_value.previous_known)
        throw DecodeError("metadata delta history is missing predecessor");
    for (const auto& parent : entry_value.merge_parents) {
        if (parent == Hash256{} || parent == entry_value.hash)
            throw DecodeError("bad metadata merge parent");
    }
    return entry_value;
}
MetadataRecord genesis_metadata() {
    MetadataSnapshot s;
    FsEntry r;
    r.type = EntryType::directory;
    r.mode = 0755;
    s.entries["/"] = r;
    MetadataRecord m;
    m.generation = 1;
    m.payload = encode_snapshot(s);
    m.hash = metadata_hash(1, m.previous, m.payload);
    return m;
}
bool valid_metadata_record(const MetadataRecord& m) {
    return m.generation && metadata_hash(m.generation, m.previous, m.payload) == m.hash;
}

std::vector<Hash256> metadata_record_parents(const MetadataRecord& record) {
    if (!valid_metadata_record(record))
        throw DecodeError("invalid metadata record");
    std::vector<Hash256> parents;
    if (record.generation > 1 && record.previous != Hash256{})
        parents.push_back(record.previous);
    const auto snapshot = decode_snapshot(record.payload);
    for (const auto& parent : snapshot.merge_parents) {
        if (parent == Hash256{} || parent == record.hash)
            throw DecodeError("bad metadata merge parent");
        if (std::find(parents.begin(), parents.end(), parent) == parents.end())
            parents.push_back(parent);
    }
    return parents;
}

std::string metadata_conflict_id(const MetadataConflict& conflict) {
    Writer writer;
    static constexpr std::array<uint8_t, 8> magic{'M', 'A', 'C', 'H', 'C', 'F', 'L', '1'};
    writer.raw(magic);
    encode_conflict(writer, conflict);
    return to_string(sha256(writer.data()));
}

MetadataReplica::MetadataReplica(std::filesystem::path r, std::array<uint8_t, 32> k,
                                 std::optional<MetadataRecord> recovery_seed,
                                 bool accept_pristine_genesis_authority,
                                 uint64_t materialization_cache_limit_bytes,
                                 NamespaceDeltaApplier namespace_applier)
    : p_(r / "metadata" / "current.meta"), committed_p_(r / "metadata" / "committed.meta"),
      checkpoint_p_(r / "metadata" / "checkpoint.meta"), journal_p_(r / "metadata" / "journal.log"),
      history_p_(r / "metadata" / "history.log"), heads_p_(r / "metadata" / "heads.meta"),
      mutation_sequence_p_(r / "metadata" / "mutation-sequence.meta"),
      author_p_(r / "metadata" / "author.meta"),
      author_chain_broken_p_(r / "metadata" / "author-chain-broken"),
      set_aside_p_(r / "metadata" / "set-aside.meta"),
      recovery_p_(r / "metadata" / "recovery.required"), key_(k),
      namespace_applier_(std::move(namespace_applier)),
      accept_pristine_genesis_authority_(accept_pristine_genesis_authority),
      materialized_history_limit_bytes_(materialization_cache_limit_bytes) {
    std::filesystem::create_directories(checkpoint_p_.parent_path());

    auto valid_seed = [&]() -> std::optional<MetadataRecord> {
        if (!recovery_seed || !valid_metadata_record(*recovery_seed))
            return {};
        try {
            (void)decode_snapshot(recovery_seed->payload);
            return recovery_seed;
        } catch (...) {
            return {};
        }
    };

    // While the recovery marker exists the fallback checkpoint stays stale,
    // however readable, until replica checkpointing clears the marker.
    if (std::filesystem::exists(recovery_p_)) {
        recovery_required_ = true;
        try {
            if (auto checkpoint = load(checkpoint_p_)) {
                cur_ = *checkpoint;
                set_committed_locked(*checkpoint);
                load_history();
                ensure_history_root(committed_);
                load_journal();
                pending_recovered_ = cur_.hash != committed_.hash;
                ensure_history_root(committed_);
                load_heads();
                // This checkpoint may come from the metadata cache: no legacy
                // acceptance; only heads.meta from a peer recovery grants authority.
                refresh_materialized_head_locked();
                Log::warn("metadata replica still requires replica recovery path=" +
                          checkpoint_p_.parent_path().string() +
                          " generation=" + std::to_string(committed_.generation));
                return;
            }
        } catch (const std::exception& error) {
            Log::error("metadata recovery checkpoint unreadable: " + std::string(error.what()));
        }
        auto seed = valid_seed();
        if (!seed)
            throw std::runtime_error("metadata recovery required but no valid persistent metadata "
                                     "cache seed is available at " +
                                     recovery_p_.string());
        recover_from_seed(*seed, "continuing previously marked metadata recovery");
        return;
    }

    try {
        if (auto checkpoint = load(checkpoint_p_)) {
            cur_ = *checkpoint;
            set_committed_locked(*checkpoint);
            load_history();
            ensure_history_root(committed_);
            load_journal();
            pending_recovered_ = cur_.hash != committed_.hash;
            ensure_history_root(committed_);
            load_heads();
            migrate_legacy_head_locked();
            refresh_materialized_head_locked();
            return;
        }

        // current.meta/committed.meta layout: committed becomes the journal
        // base, a newer current becomes a seed entry, and the old files are
        // renamed once the new ones are durable.
        auto current = load(p_);
        auto committed = load(committed_p_);
        if (current.has_value() != committed.has_value())
            throw std::runtime_error(
                "incomplete metadata state: current.meta and committed.meta are both required");

        if (!current) {
            cur_ = genesis_metadata();
            set_committed_locked(cur_);
            reset_checkpoint(committed_);
            load_history();
            ensure_history_root(committed_);
            load_heads();
            migrate_legacy_head_locked();
            refresh_materialized_head_locked();
            return;
        }

        if (committed->generation > current->generation)
            throw std::runtime_error("metadata committed generation is newer than current");
        if (committed->generation == current->generation && committed->hash != current->hash)
            throw std::runtime_error("metadata current/committed generation conflict");

        cur_ = *committed;
        set_committed_locked(*committed);
        if (current->hash != committed->hash) {
            writefile(journal_p_, {});
            journal_records_ = 0;
            journal_bytes_ = 0;
            append_journal(JOURNAL_SEED_FULL, *current, current->payload);
            persist(checkpoint_p_, committed_);
            cur_ = *current;
            pending_recovered_ = true;
        } else {
            reset_checkpoint(committed_);
        }
        load_history();
        ensure_history_root(committed_);
        load_heads();
        migrate_legacy_head_locked();
        refresh_materialized_head_locked();

        std::error_code ec;
        std::filesystem::rename(p_, p_.string() + ".v10", ec);
        ec.clear();
        std::filesystem::rename(committed_p_, committed_p_.string() + ".v10", ec);
    } catch (const std::exception& error) {
        auto seed = valid_seed();
        if (!seed)
            throw;
        recover_from_seed(*seed, error.what());
    }
}

void MetadataReplica::recover_from_seed(const MetadataRecord& seed, const std::string& reason) {
    const auto stamp = ".corrupt." + std::to_string(wall_time_ns());
    durable_replace_file(recovery_p_, reason);
    // The seed may predate this node's own commits.
    durable_replace_file(author_chain_broken_p_, reason);

    std::vector<std::filesystem::path> quarantined;
    for (const auto& path :
         {checkpoint_p_, journal_p_, history_p_, heads_p_, p_, committed_p_}) {
        if (auto moved = quarantine_metadata_file(path, stamp))
            quarantined.push_back(*moved);
    }

    cur_ = seed;
    set_committed_locked(seed);
    reset_checkpoint(seed);
    load_history();
    ensure_history_root(seed);
    load_heads();
    // The cache seed is not accepted: it cannot stand in for the lost durable
    // acceptance evidence.
    refresh_materialized_head_locked();
    recovery_required_ = true;

    std::string preserved;
    for (const auto& path : quarantined) {
        if (!preserved.empty())
            preserved += ",";
        preserved += path.string();
    }
    Log::error("metadata primary state failed authentication/validation; using persistent cache "
               "seed pending replica recovery generation=" +
               std::to_string(seed.generation) + " state=" + checkpoint_p_.parent_path().string() +
               " preserved=" + preserved + " reason=" + reason);
}

MetadataRecord MetadataReplica::current() const {
    Lock g(m_);
    return cur_;
}

MetadataRecord MetadataReplica::committed() const {
    Lock g(m_);
    return committed_;
}

MetadataIdentity MetadataReplica::current_identity() const {
    Lock g(m_);
    return {cur_.generation, cur_.hash};
}

MetadataIdentity MetadataReplica::committed_identity() const {
    Lock g(m_);
    return {committed_.generation, committed_.hash};
}

uint64_t MetadataReplica::generation() const {
    Lock g(m_);
    return cur_.generation;
}

uint64_t MetadataReplica::committed_generation() const noexcept {
    return committed_generation_.load(std::memory_order_acquire);
}

bool MetadataReplica::recovery_required() const {
    Lock g(m_);
    return recovery_required_;
}

void MetadataReplica::mark_recovered() {
    Lock g(m_);
    if (!recovery_required_)
        return;
    std::error_code error;
    const bool removed = std::filesystem::remove(recovery_p_, error);
    if (error)
        throw std::runtime_error("cannot clear metadata recovery marker " + recovery_p_.string() +
                                 ": " + error.message());
    if (removed)
        sync_directory(recovery_p_);
    recovery_required_ = false;
    Log::info("metadata replica recovery complete generation=" +
              std::to_string(committed_.generation));
}

void MetadataReplica::load_author_locked(const NodeId& node_id,
                                         const std::map<NodeId, uint64_t>& head_clock) {
    if (author_loaded_)
        return;
    author_ = {};
    const auto open_sealed = [&](const std::filesystem::path& path,
                                 const std::array<uint8_t, 8>& magic) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            throw std::runtime_error("cannot open");
        Bytes bytes(std::istreambuf_iterator<char>(stream), {});
        Reader reader(bytes);
        auto found = reader.raw(magic.size());
        if (!std::equal(found.begin(), found.end(), magic.begin()))
            throw std::runtime_error("bad file header");
        auto nonce = reader.fixed<12>();
        auto tag = reader.fixed<16>();
        auto ciphertext = reader.bytes();
        reader.finish();
        return aes_gcm_open(key_, nonce, tag, ciphertext, magic);
    };
    if (std::filesystem::exists(author_p_)) {
        try {
            const auto plaintext = open_sealed(author_p_, MAUTHOR);
            Reader plain(plaintext);
            author_.id.bytes = plain.fixed<16>();
            author_.reserved = plain.u64();
            author_.accepted = plain.u64();
            const auto past = plain.u32();
            if (past > 64)
                throw std::runtime_error("too many past author ids");
            for (uint32_t i = 0; i < past; ++i)
                author_.past.push_back(NodeId{plain.fixed<16>()});
            plain.finish();
        } catch (const std::exception& error) {
            throw std::runtime_error("metadata author file " + author_p_.string() + ": " +
                                     error.what());
        }
    } else if (std::filesystem::exists(mutation_sequence_p_)) {
        // The sequence counter this file replaces: the author is the node.
        try {
            const auto plaintext = open_sealed(mutation_sequence_p_, MS);
            Reader plain(plaintext);
            author_.id = node_id;
            author_.reserved = plain.u64();
            plain.finish();
        } catch (const std::exception& error) {
            throw std::runtime_error("metadata mutation-sequence file " +
                                     mutation_sequence_p_.string() + ": " + error.what());
        }
        author_loaded_ = true;
        persist_author_locked();
        std::error_code ignored;
        std::filesystem::remove(mutation_sequence_p_, ignored);
    } else {
        // No record of what this node has authored. If a head says it has
        // authored before, that record was lost: it may not continue the
        // old author's sequence.
        author_.id = head_clock.contains(node_id) ? random_node_id() : node_id;
        if (author_.id != node_id) {
            author_.past.push_back(node_id);
            Log::warn("metadata author record missing; authoring as a new author");
        }
        author_loaded_ = true;
        persist_author_locked();
    }
    author_loaded_ = true;
}

void MetadataReplica::persist_author_locked() {
    Writer plain;
    plain.fixed(author_.id.bytes);
    plain.u64(author_.reserved);
    plain.u64(author_.accepted);
    plain.u32(static_cast<uint32_t>(author_.past.size()));
    for (const auto& id : author_.past)
        plain.fixed(id.bytes);
    auto sealed = aes_gcm_seal(key_, plain.data(), MAUTHOR);
    Writer file;
    file.raw(MAUTHOR);
    file.fixed(sealed.nonce);
    file.fixed(sealed.tag);
    file.bytes(sealed.ciphertext);
    writefile(author_p_, file.data());
}

void MetadataReplica::rotate_author_locked(std::string_view why) {
    author_.past.insert(author_.past.begin(), author_.id);
    if (author_.past.size() > 64)
        author_.past.resize(64);
    author_.id = random_node_id();
    author_.reserved = 0;
    author_.accepted = 0;
    Log::warn("metadata author chain cannot be continued (" + std::string(why) +
              "); authoring as a new author");
}

MetadataDot MetadataReplica::reserve_mutation_dot(const NodeId& node_id,
                                                  const std::map<NodeId, uint64_t>& head_clock) {
    Lock lock(m_);
    load_author_locked(node_id, head_clock);
    const auto observed_of = [&]() MACHA_REQUIRES(m_) {
        const auto found = head_clock.find(author_.id);
        return found == head_clock.end() ? uint64_t{0} : found->second;
    };
    const bool marked = std::filesystem::exists(author_chain_broken_p_);
    if (marked)
        rotate_author_locked("the head set was replaced");
    else if (observed_of() < author_.accepted)
        rotate_author_locked("the head lacks a mutation this node had accepted");

    const auto floor = std::max(author_.reserved, observed_of());
    if (floor == std::numeric_limits<uint64_t>::max())
        throw std::runtime_error("metadata mutation sequence exhausted");
    author_.reserved = floor + 1;
    persist_author_locked();
    if (marked) {
        std::error_code ignored;
        std::filesystem::remove(author_chain_broken_p_, ignored);
    }
    return {author_.id, author_.reserved};
}

void MetadataReplica::note_author_accepted(const MetadataDot& dot) {
    Lock lock(m_);
    if (author_loaded_ && dot.author == author_.id)
        author_.accepted = std::max(author_.accepted, dot.sequence);
}

std::vector<NodeId> MetadataReplica::author_ids(const NodeId& node_id,
                                                const std::map<NodeId, uint64_t>& head_clock) {
    Lock lock(m_);
    load_author_locked(node_id, head_clock);
    std::vector<NodeId> ids{author_.id};
    ids.insert(ids.end(), author_.past.begin(), author_.past.end());
    return ids;
}

void MetadataReplica::append_journal(uint8_t kind, const MetadataRecord& record,
                                     std::span<const uint8_t> body) {
    Writer plain;
    plain.u8(kind);
    plain.u64(record.generation);
    plain.fixed(record.previous.bytes);
    plain.fixed(record.hash.bytes);
    plain.bytes(body);
    auto sealed = aes_gcm_seal(key_, plain.data(), MJ);

    Writer envelope;
    envelope.fixed(sealed.nonce);
    envelope.fixed(sealed.tag);
    envelope.bytes(sealed.ciphertext);
    auto payload = envelope.take();
    if (payload.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("metadata journal record too large");

    Writer frame;
    frame.u32(static_cast<uint32_t>(payload.size()));
    frame.raw(payload);
    auto bytes = frame.take();

    int fd = open(journal_p_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        throw std::runtime_error(strerror(errno));
    size_t offset = 0;
    while (offset < bytes.size()) {
        auto count = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            auto error = errno;
            close(fd);
            throw std::runtime_error(strerror(error));
        }
        offset += static_cast<size_t>(count);
    }
    const auto sync_started = Clock::now();
    if (fsync(fd)) {
        auto error = errno;
        close(fd);
        throw std::runtime_error("cannot sync metadata journal " + journal_p_.string() + ": " +
                                 strerror(error));
    }
    observations().record("metadata.journal_sync_us", elapsed_us(sync_started));
    if (close(fd) != 0)
        throw std::runtime_error("cannot close metadata journal " + journal_p_.string() + ": " +
                                 strerror(errno));
    ++journal_records_;
    journal_bytes_ += bytes.size();
}

void MetadataReplica::load_journal() {
    journal_records_ = 0;
    journal_bytes_ = 0;
    if (!std::filesystem::exists(journal_p_))
        return;

    std::ifstream stream(journal_p_, std::ios::binary);
    if (!stream)
        throw std::runtime_error("cannot open metadata journal " + journal_p_.string());
    Bytes bytes(std::istreambuf_iterator<char>(stream), {});
    const auto checkpoint_generation = committed_.generation;
    const auto checkpoint_hash = committed_.hash;
    size_t offset = 0;
    size_t valid = 0;
    auto input = std::span<const uint8_t>(bytes);
    std::string trailing_problem;

    while (offset + 4 <= bytes.size()) {
        Reader header(input.subspan(offset, 4));
        auto length = header.u32();
        header.finish();
        if (length > 256U * 1024U * 1024U) {
            throw std::runtime_error("metadata journal " + journal_p_.string() +
                                     " offset=" + std::to_string(offset) + ": record too large");
        }
        if (offset + 4ULL + length > bytes.size()) {
            trailing_problem = "incomplete trailing frame";
            break;
        }

        const bool final_frame = offset + 4ULL + length == bytes.size();
        auto frame = input.subspan(offset + 4, length);
        std::array<uint8_t, 12> nonce{};
        std::array<uint8_t, 16> tag{};
        Bytes ciphertext;
        try {
            Reader envelope(frame);
            nonce = envelope.fixed<12>();
            tag = envelope.fixed<16>();
            ciphertext = envelope.bytes();
            envelope.finish();
        } catch (const std::exception& error) {
            trailing_problem = std::string("malformed encrypted frame: ") + error.what();
            break;
        }

        Bytes plaintext;
        try {
            plaintext = aes_gcm_open(key_, nonce, tag, ciphertext, MJ);
        } catch (const std::exception& error) {
            // The journal is a CAS chain after the checkpoint: nothing after an
            // unauthenticated frame (or, below, one that does not fit the chain)
            // can apply. Replay the prefix, which is the state of a crash before
            // that append; the tail is quarantined below.
            if (final_frame && std::string_view(error.what()) == "AES-GCM authentication failed")
                trailing_problem = "final frame failed AES-GCM authentication";
            else
                trailing_problem = std::string("frame failed authentication: ") + error.what();
            break;
        }

        try {
            Reader record_reader(plaintext);
            auto kind = record_reader.u8();
            MetadataRecord record;
            record.generation = record_reader.u64();
            record.previous.bytes = record_reader.fixed<32>();
            record.hash.bytes = record_reader.fixed<32>();
            auto body = record_reader.bytes();
            record_reader.finish();

            // Compaction writes the checkpoint before truncating the journal, so
            // frames at or below the checkpoint are skipped.
            const bool checkpoint_rollback =
                kind == JOURNAL_SEED_FULL && record.generation == checkpoint_generation &&
                record.hash == checkpoint_hash && cur_.hash != checkpoint_hash;
            if (record.generation < checkpoint_generation ||
                (record.generation == checkpoint_generation && record.hash == checkpoint_hash &&
                 !checkpoint_rollback)) {
                offset += 4 + length;
                valid = offset;
                ++journal_records_;
                journal_bytes_ += 4 + length;
                continue;
            }
            if (record.generation == checkpoint_generation && record.hash != checkpoint_hash)
                throw std::runtime_error("conflicts with checkpoint");

            switch (kind) {
            case JOURNAL_PREPARE_FULL: {
                if (record.generation != cur_.generation + 1 || record.previous != cur_.hash)
                    throw std::runtime_error("full CAS chain broken");
                record.payload = std::move(body);
                if (!valid_metadata_record(record))
                    throw std::runtime_error("full CAS hash invalid");
                (void)decode_snapshot(record.payload);
                cur_ = std::move(record);
                pending_history_ = history_for_current();
                break;
            }
            case JOURNAL_PREPARE_DELTA: {
                if (record.generation != cur_.generation + 1 || record.previous != cur_.hash)
                    throw std::runtime_error("delta CAS chain broken");
                auto replayed = decode_snapshot(cur_.payload);
                auto delta = decode_metadata_delta(body);
                apply_metadata_delta_in_place(replayed, delta, namespace_applier_);
                record.payload = encode_snapshot_for_delta(body, replayed);
                if (!valid_metadata_record(record))
                    throw std::runtime_error("delta CAS hash invalid");
                cur_ = std::move(record);
                pending_history_ = history_for_current(body);
                break;
            }
            case JOURNAL_SEED_FULL: {
                record.payload = std::move(body);
                if (!valid_metadata_record(record))
                    throw std::runtime_error("seed hash invalid");
                const auto parents = metadata_record_parents(record);

                // A seed replaces the uncommitted proposal: it may roll back to
                // the committed head, advance from it, or install a descendant
                // whose ancestry is in history. Committed history is never rewritten.
                if (record.hash == committed_.hash) {
                    cur_ = committed_;
                    pending_history_.reset();
                    break;
                }
                if (record.generation < committed_.generation)
                    throw std::runtime_error("seed predates committed metadata");
                const bool fresh = committed_.generation <= 1;
                const bool direct_parent =
                    std::find(parents.begin(), parents.end(), committed_.hash) != parents.end();
                const bool known_descendant =
                    history_.contains(record.hash) &&
                    history_is_ancestor_locked(committed_.hash, record.hash);
                const bool parent_descends_from_committed = std::any_of(
                    parents.begin(), parents.end(), [&](const Hash256& parent) MACHA_REQUIRES(m_) {
                        return history_.contains(parent) &&
                               history_is_ancestor_locked(committed_.hash, parent);
                    });
                if (!fresh && !direct_parent && !known_descendant &&
                    !parent_descends_from_committed)
                    throw std::runtime_error("seed is not descended from committed metadata");
                cur_ = std::move(record);
                pending_history_ = history_for_current();
                break;
            }
            case JOURNAL_COMMIT:
                if (record.generation == committed_.generation && record.hash == committed_.hash)
                    break;
                if (record.generation != cur_.generation || record.hash != cur_.hash)
                    throw std::runtime_error("commit does not match current");
                set_committed_locked(cur_);
                pending_history_.reset();
                break;
            default:
                throw std::runtime_error("unknown record type " + std::to_string(kind));
            }
        } catch (const std::exception& error) {
            // Ends the replayable prefix. cur_/committed_ are assigned only
            // after a record validates, so nothing partial remains.
            trailing_problem = std::string("record does not fit the chain: ") + error.what();
            break;
        }

        offset += 4 + length;
        valid = offset;
        ++journal_records_;
        journal_bytes_ += 4 + length;
    }

    if (valid != bytes.size()) {
        if (trailing_problem.empty())
            trailing_problem = "trailing bytes after last complete frame";
        auto tail = input.subspan(valid);
        const auto quarantine = quarantine_journal_tail(journal_p_, tail);
        truncate_durable(journal_p_, valid);
        Log::warn("metadata journal recovered path=" + journal_p_.string() + " offset=" +
                  std::to_string(valid) + " discarded_bytes=" + std::to_string(tail.size()) +
                  " quarantine=" + quarantine.string() + " reason=" + trailing_problem);
    }
}

Bytes MetadataReplica::encode_history_frame(const MetadataHistoryEntry& entry_value) const {
    auto plaintext = encode_metadata_history_entry(entry_value);
    auto sealed = aes_gcm_seal(key_, plaintext, MH);
    Writer envelope;
    envelope.fixed(sealed.nonce);
    envelope.fixed(sealed.tag);
    envelope.bytes(sealed.ciphertext);
    auto payload = envelope.take();
    if (payload.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("metadata history record too large");

    Writer frame;
    frame.u32(static_cast<uint32_t>(payload.size()));
    frame.raw(payload);
    return frame.take();
}

MetadataReplica::HistoryIndexEntry MetadataReplica::index_history_entry(
    const MetadataHistoryEntry& value, uint64_t file_offset, uint64_t frame_bytes) {
    return HistoryIndexEntry{value.generation, value.previous, value.hash, value.previous_known,
                             value.merge_parents, value.body, file_offset, frame_bytes};
}

MetadataHistoryEntry MetadataReplica::read_history_entry(const HistoryIndexEntry& index) const {
    if (index.frame_bytes < 4 ||
        index.frame_bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        throw std::runtime_error("invalid metadata history frame index");
    std::ifstream stream(history_p_, std::ios::binary);
    if (!stream)
        throw std::runtime_error("cannot open metadata history " + history_p_.string());
    stream.seekg(static_cast<std::streamoff>(index.file_offset), std::ios::beg);
    Bytes bytes(static_cast<size_t>(index.frame_bytes));
    if (!stream.read(reinterpret_cast<char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("cannot read indexed metadata history frame");
    Reader frame(bytes);
    const auto length = frame.u32();
    auto envelope_bytes = frame.raw(length);
    frame.finish();
    Reader envelope(envelope_bytes);
    auto nonce = envelope.fixed<12>();
    auto tag = envelope.fixed<16>();
    auto ciphertext = envelope.bytes();
    envelope.finish();
    auto value = decode_metadata_history_entry(aes_gcm_open(key_, nonce, tag, ciphertext, MH));
    if (value.hash != index.hash || value.generation != index.generation ||
        value.previous != index.previous || value.previous_known != index.previous_known ||
        value.merge_parents != index.merge_parents || value.body != index.body)
        throw std::runtime_error("indexed metadata history identity mismatch");
    return value;
}

void MetadataReplica::append_history(const MetadataHistoryEntry& entry_value) {
    if (history_.contains(entry_value.hash))
        return;

    auto bytes = encode_history_frame(entry_value);
    const auto file_offset = history_bytes_;
    write_history_frame(bytes);
    history_.emplace(entry_value.hash,
                     index_history_entry(entry_value, file_offset, bytes.size()));
    ++history_records_;
    history_bytes_ += bytes.size();
}

void MetadataReplica::write_history_frame(std::span<const uint8_t> bytes) const {
    int fd = open(history_p_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        throw std::runtime_error("cannot open metadata history " + history_p_.string() + ": " +
                                 strerror(errno));
    size_t offset = 0;
    while (offset < bytes.size()) {
        auto count = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            const auto error = errno;
            close(fd);
            throw std::runtime_error("cannot write metadata history " + history_p_.string() + ": " +
                                     strerror(error));
        }
        offset += static_cast<size_t>(count);
    }
    const auto sync_started = Clock::now();
    if (fsync(fd) != 0) {
        const auto error = errno;
        close(fd);
        throw std::runtime_error("cannot sync metadata history " + history_p_.string() + ": " +
                                 strerror(error));
    }
    observations().record("metadata.history_sync_us", elapsed_us(sync_started));
    if (close(fd) != 0)
        throw std::runtime_error("cannot close metadata history " + history_p_.string() + ": " +
                                 strerror(errno));
}

void MetadataReplica::load_history() {
    constexpr uint64_t max_record = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    constexpr uint64_t max_quarantine_tail = 16ULL * 1024ULL * 1024ULL;
    history_.clear();
    materialized_history_.clear();
    materialized_history_clock_ = 0;
    materialized_history_bytes_ = 0;
    history_records_ = 0;
    history_bytes_ = 0;
    if (!std::filesystem::exists(history_p_))
        return;

    const uint64_t file_size = std::filesystem::file_size(history_p_);
    std::ifstream stream(history_p_, std::ios::binary);
    if (!stream)
        throw std::runtime_error("cannot open metadata history " + history_p_.string());
    uint64_t offset = 0;
    uint64_t valid = 0;
    std::string trailing_problem;
    size_t skipped = 0;
    std::string first_skipped;

    // Streaming: startup RSS is bounded by one frame, not history.log's size.
    while (offset + 4 <= file_size) {
        std::array<uint8_t, 4> header_bytes{};
        if (!stream.read(reinterpret_cast<char*>(header_bytes.data()), header_bytes.size())) {
            trailing_problem = "incomplete trailing frame header";
            break;
        }
        Reader header(header_bytes);
        const auto length = static_cast<uint64_t>(header.u32());
        header.finish();
        if (length > max_record)
            throw std::runtime_error("metadata history " + history_p_.string() +
                                     " offset=" + std::to_string(offset) + ": record too large");
        if (offset > std::numeric_limits<uint64_t>::max() - 4 - length ||
            offset + 4 + length > file_size) {
            trailing_problem = "incomplete trailing frame";
            break;
        }

        Bytes frame(static_cast<size_t>(length));
        if (length && !stream.read(reinterpret_cast<char*>(frame.data()),
                                   static_cast<std::streamsize>(length))) {
            trailing_problem = "incomplete trailing frame";
            break;
        }
        const bool final_frame = offset + 4 + length == file_size;
        try {
            Reader envelope(frame);
            auto nonce = envelope.fixed<12>();
            auto tag = envelope.fixed<16>();
            auto ciphertext = envelope.bytes();
            envelope.finish();
            auto plaintext = aes_gcm_open(key_, nonce, tag, ciphertext, MH);
            auto entry_value = decode_metadata_history_entry(plaintext);

            // Linear in history bytes: validate each frame locally; full
            // reconstruction is deferred to heads that can become authority.
            if (!entry_value.generation || entry_value.hash == Hash256{})
                throw DecodeError("invalid metadata history identity");
            if (entry_value.body == MetadataHistoryEntry::Body::full) {
                MetadataRecord record;
                record.generation = entry_value.generation;
                record.previous = entry_value.previous;
                record.hash = entry_value.hash;
                record.payload = entry_value.payload;
                if (!valid_metadata_record(record))
                    throw DecodeError("invalid full metadata history record");
                // Not decoded: the authenticated frame and record hash bind the
                // identity, and heads are decoded and cross-checked when
                // materialised. Decoding every snapshot here costs gigabytes.
            } else if (entry_value.body == MetadataHistoryEntry::Body::delta) {
                if (!entry_value.previous_known || entry_value.generation <= 1)
                    throw DecodeError("metadata delta history has no predecessor");
                auto parent = history_.find(entry_value.previous);
                // Same rule as every reader: see metadata_delta_succession_valid().
                if (parent == history_.end() ||
                    !metadata_delta_succession_valid(parent->second.generation,
                                                     entry_value.generation))
                    throw DecodeError("metadata delta history predecessor missing or invalid");
                (void)decode_metadata_delta(entry_value.payload);
            } else {
                throw DecodeError("unknown metadata history body");
            }

            auto index = index_history_entry(entry_value, offset, 4 + length);
            if (auto existing = history_.find(entry_value.hash); existing != history_.end()) {
                // reanchor_history() appends a full body for an indexed hash;
                // the hash binds the identity, so prefer the full body. A
                // conflicting identity is corruption.
                if (existing->second.generation != entry_value.generation ||
                    existing->second.previous != entry_value.previous)
                    throw DecodeError("duplicate metadata history record with conflicting identity");
                if (entry_value.body == MetadataHistoryEntry::Body::full)
                    existing->second = std::move(index);
            } else {
                history_.emplace(entry_value.hash, std::move(index));
            }
        } catch (const std::exception& error) {
            if (final_frame && std::string_view(error.what()) == "AES-GCM authentication failed") {
                trailing_problem = "final frame failed AES-GCM authentication";
                break;
            }
            // Entries are independent and hash-indexed: skip a bad one (its
            // dependants fail their predecessor check too); heads needing it
            // are repaired live from peers.
            ++skipped;
            if (skipped == 1)
                first_skipped = "offset=" + std::to_string(offset) + ": " + error.what();
            offset += 4 + length;
            valid = offset;
            continue;
        }

        offset += 4 + length;
        valid = offset;
        ++history_records_;
    }
    if (skipped)
        Log::warn("metadata history skipped frames that could not be replayed path=" +
                  history_p_.string() + " count=" + std::to_string(skipped) + " first=" +
                  first_skipped + "; dependent heads are repaired live from peers");

    if (valid != file_size) {
        if (trailing_problem.empty())
            trailing_problem = "trailing bytes after last complete frame";
        const uint64_t tail_size = file_size - valid;
        std::string quarantine = "<discarded: over quarantine limit>";
        if (tail_size <= max_quarantine_tail) {
            stream.clear();
            stream.seekg(static_cast<std::streamoff>(valid), std::ios::beg);
            Bytes tail(static_cast<size_t>(tail_size));
            if (tail_size && !stream.read(reinterpret_cast<char*>(tail.data()),
                                          static_cast<std::streamsize>(tail_size)))
                throw std::runtime_error("cannot read corrupt metadata history tail");
            quarantine = quarantine_journal_tail(history_p_, tail).string();
        }
        stream.close();
        truncate_durable(history_p_, static_cast<size_t>(valid));
        Log::warn("metadata history recovered path=" + history_p_.string() + " offset=" +
                  std::to_string(valid) + " discarded_bytes=" + std::to_string(tail_size) +
                  " quarantine=" + quarantine + " reason=" + trailing_problem);
    }
    history_bytes_ = valid;
#if defined(__GLIBC__)
    // Only the index is retained; return the scan's transient arenas to the OS.
    (void)malloc_trim(0);
#endif
}

void MetadataReplica::load_heads() {
    const HeadsRevisionBump heads_bump(heads_revision_);
    accepted_heads_.clear();
    if (!std::filesystem::exists(heads_p_))
        return;
    try {
        std::ifstream stream(heads_p_, std::ios::binary);
        if (!stream)
            throw std::runtime_error("cannot open");
        Bytes bytes(std::istreambuf_iterator<char>(stream), {});
        Reader reader(bytes);
        auto magic = reader.raw(8);
        if (!std::equal(magic.begin(), magic.end(), MA.begin()))
            throw std::runtime_error("bad metadata accepted-head file header");
        auto nonce = reader.fixed<12>();
        auto tag = reader.fixed<16>();
        auto ciphertext = reader.bytes();
        reader.finish();
        auto values =
            decode_metadata_acceptance_set(aes_gcm_open(key_, nonce, tag, ciphertext, MA));
        for (auto& value : values) {
            auto reconstructed = materialized_locked(value.hash);
            if (!reconstructed) {
                // The certificate is durable evidence of acceptance and any
                // peer can supply the immutable record: keep it, flag the head
                // for the accepted_heads() cooldown, and let MetadataManager::
                // repair_unreconstructable_heads() re-anchor it live.
                const auto reason = diagnose_unreconstructable_locked(value.hash);
                Log::error("metadata accepted head is not reconstructible locally; keeping it "
                           "for live repair from peers hash=" +
                           hex(value.hash.bytes) + " generation=" +
                           std::to_string(value.generation) + " reason=" + reason);
                unreconstructable_head_retry_at_[value.hash] =
                    Clock::now() + unreconstructable_retry_cooldown;
                accepted_heads_.emplace(value.hash, std::move(value));
                continue;
            }
            // The hash binds the generation: a mismatch is a corrupt certificate.
            if (reconstructed->record.generation != value.generation)
                throw std::runtime_error("accepted metadata head certificate generation does "
                                         "not match its record");
            if (!acceptance_matches_record_policy_locked(value, *reconstructed))
                throw std::runtime_error("accepted metadata head policy does not match commit");
            accepted_heads_.emplace(value.hash, std::move(value));
        }
        if (prune_accepted_heads_locked())
            persist_heads_locked();
    } catch (const std::exception& error) {
        throw std::runtime_error("metadata accepted-head file " + heads_p_.string() + ": " +
                                 error.what());
    }
}

void MetadataReplica::persist_heads_locked() {
    std::vector<MetadataAcceptance> values;
    values.reserve(accepted_heads_.size());
    for (const auto& [_, value] : accepted_heads_)
        values.push_back(value);
    auto plaintext = encode_metadata_acceptance_set(values);
    auto sealed = aes_gcm_seal(key_, plaintext, MA);
    Writer writer;
    writer.raw(MA);
    writer.fixed(sealed.nonce);
    writer.fixed(sealed.tag);
    writer.bytes(sealed.ciphertext);
    const auto bytes = writer.data().size();
    try {
        writefile(heads_p_, writer.data());
        accepted_head_persistence_writes_.fetch_add(1, std::memory_order_relaxed);
        accepted_head_persistence_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    } catch (...) {
        accepted_head_persistence_failures_.fetch_add(1, std::memory_order_relaxed);
        throw;
    }
}

bool MetadataReplica::prune_accepted_heads_locked() {
    const HeadsRevisionBump heads_bump(heads_revision_);
    bool changed = false;
    for (auto it = accepted_heads_.begin(); it != accepted_heads_.end();) {
        bool ancestor = false;
        for (const auto& [other, _] : accepted_heads_) {
            if (other == it->first)
                continue;
            if (accepted_head_is_ancestor_locked(it->first, other)) {
                ancestor = true;
                break;
            }
        }
        if (ancestor) {
            it = accepted_heads_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

bool MetadataReplica::accepted_head_is_ancestor_locked(const Hash256& ancestor,
                                                        const Hash256& descendant) const {
    if (history_is_ancestor_locked(ancestor, descendant))
        return true;

    // Canonical genesis is subsumed by any post-genesis record even when
    // compaction removed the edge, so a pristine replica can adopt a cluster
    // head and a joiner's late genesis certificate is ignored.
    const auto genesis = genesis_metadata();
    if (ancestor == genesis.hash) {
        auto materialized = materialized_locked(descendant);
        return materialized && materialized->record.generation > genesis.generation;
    }

    // History is truncated by each node on its own, so the edge between two
    // heads may be gone. The clocks still say it: a head whose clock covers
    // another's and goes beyond it has incorporated every mutation the other
    // holds.
    const auto older = materialized_locked(ancestor);
    const auto newer = materialized_locked(descendant);
    if (!older || !newer)
        return false;
    const auto& behind = older->snapshot->mutation_sequences;
    const auto& ahead = newer->snapshot->mutation_sequences;
    // A head with no clock says nothing about what it holds.
    return !behind.empty() && behind != ahead &&
           std::all_of(behind.begin(), behind.end(), [&](const auto& item) {
               const auto found = ahead.find(item.first);
               return found != ahead.end() && found->second >= item.second;
           });
}

void MetadataReplica::migrate_legacy_head_locked() {
    const HeadsRevisionBump heads_bump(heads_revision_);
    ensure_history_root(committed_);
    const auto genesis = genesis_metadata();
    if (!accept_pristine_genesis_authority_ && committed_.hash == genesis.hash) {
        // A joiner (bootstrap peers configured) never holds genesis as an
        // accepted head; it stays local, non-authoritative material.
        if (accepted_heads_.erase(genesis.hash))
            persist_heads_locked();
        return;
    }
    if (accepted_heads_.contains(committed_.hash))
        return;

    const auto snapshot = decode_snapshot(committed_.payload);
    if (snapshot.metadata_write_replicas_required != 0) {
        // Without a certificate a write-floor checkpoint is not authority:
        // keep it and force peer recovery rather than invent a legacy head.
        recovery_required_ = true;
        durable_replace_file(
            recovery_p_, "protocol-20 metadata checkpoint has no durable acceptance certificate");
        return;
    }

    if (!accepted_heads_.empty()) {
        // Accepted descendants subsume the checkpoint. A JOURNAL_COMMIT newer
        // than heads.meta (crash between the two writes) replaces them.
        bool committed_is_ancestor = false;
        bool all_heads_are_ancestors = true;
        for (const auto& [head, _] : accepted_heads_) {
            committed_is_ancestor =
                committed_is_ancestor || history_is_ancestor_locked(committed_.hash, head);
            all_heads_are_ancestors =
                all_heads_are_ancestors && history_is_ancestor_locked(head, committed_.hash);
        }
        if (committed_is_ancestor)
            return;
        if (!all_heads_are_ancestors)
            return;
        accepted_heads_.clear();
    }

    MetadataAcceptance legacy;
    legacy.generation = committed_.generation;
    legacy.hash = committed_.hash;
    legacy.required = 0;
    accepted_heads_.emplace(legacy.hash, legacy);
    persist_heads_locked();
}

bool MetadataReplica::acceptance_matches_record_policy_locked(
    const MetadataAcceptance& acceptance, const MetadataMaterialization& materialized) const {
    const auto& record = materialized.record;
    const auto& snapshot = *materialized.snapshot;
    if (!snapshot.metadata_write_replicas_required) {
        // required=0 marks legacy authority; metadata_voters set the floor
        // for leaving it but are not in the certificate.
        if (acceptance.required != 0 || !acceptance.replicas.empty())
            return false;
        // A child of a write-floor parent may not revert to legacy authority.
        for (const auto& parent_hash : metadata_record_parents(record)) {
            auto parent = materialized_locked(parent_hash);
            if (parent && parent->snapshot->metadata_write_replicas_required)
                return false;
        }
        return true;
    }

    // `required` records how many nodes held the commit at acceptance; one,
    // the author, suffices.
    if (!acceptance.required)
        return false;
    for (const auto& witness : acceptance.replicas)
        if (witness == NodeId{})
            return false;
    return true;
}

bool MetadataReplica::legacy_write_api_allowed_locked() const {
    try {
        return decode_snapshot(committed_.payload).metadata_write_replicas_required == 0;
    } catch (...) {
        return false;
    }
}

void MetadataReplica::set_legacy_committed_head_locked(const MetadataRecord& record) {
    const HeadsRevisionBump heads_bump(heads_revision_);
    MetadataAcceptance legacy;
    legacy.generation = record.generation;
    legacy.hash = record.hash;
    legacy.required = 0;
    accepted_heads_.clear();
    accepted_heads_.emplace(legacy.hash, legacy);
    persist_heads_locked();
}

bool MetadataReplica::refresh_materialized_head_in_memory_locked() {
    const HeadsRevisionBump heads_bump(heads_revision_);
    if (accepted_heads_.empty())
        return false;
    // Cooldown bounds how often a broken head re-throws; this runs on every
    // read (see unreconstructable_head_retry_at_).
    const auto now = Clock::now();
    std::erase_if(unreconstructable_head_retry_at_, [&](const auto& item) MACHA_REQUIRES(m_) {
        return !accepted_heads_.contains(item.first);
    });
    std::erase_if(set_aside_, [&](const auto& item) MACHA_REQUIRES(m_) {
        return !accepted_heads_.contains(item.first);
    });
    if (set_aside_.size() >= accepted_heads_.size())
        set_aside_.clear();
    note_set_aside_locked();
    std::optional<MetadataRecord> selected;
    for (const auto& [hash, _] : accepted_heads_) {
        if (set_aside_.contains(hash))
            continue;
        if (auto found = unreconstructable_head_retry_at_.find(hash);
            found != unreconstructable_head_retry_at_.end() && now < found->second)
            continue;
        auto record = historical_locked(hash);
        if (!record) {
            const auto reason = flag_unreconstructable_locked(hash, now, "materialized head refresh");
            throw std::runtime_error("accepted metadata head cannot be reconstructed hash=" +
                                     hex(hash.bytes) + " reason=" + reason);
        }
        unreconstructable_head_retry_at_.erase(hash);
        if (!selected || record->generation > selected->generation ||
            (record->generation == selected->generation && record->hash > selected->hash))
            selected = std::move(record);
    }
    if (!selected || selected->hash == committed_.hash)
        return false;

    // `committed_` is only the preferred materialised head; authority is the
    // accepted-head set, and moving it deletes no branch from history.
    set_committed_locked(*selected);
    cur_ = committed_;
    pending_history_.reset();
    pending_recovered_ = false;
    return true;
}

void MetadataReplica::note_set_aside_locked() {
    uint64_t highest = 0;
    for (const auto& [_, generation] : set_aside_)
        highest = std::max(highest, generation);
    set_aside_generation_.store(highest, std::memory_order_release);
}

void MetadataReplica::load_set_aside_since_locked() {
    if (set_aside_since_loaded_)
        return;
    set_aside_since_loaded_ = true;
    std::ifstream stream(set_aside_p_, std::ios::binary);
    if (!stream)
        return;
    const Bytes bytes(std::istreambuf_iterator<char>(stream), {});
    try {
        Reader reader(bytes);
        const auto count = reader.u32();
        if (count > 4096)
            throw DecodeError("too many set-aside heads");
        for (uint32_t i = 0; i < count; ++i) {
            Hash256 hash{reader.fixed<32>()};
            set_aside_since_.emplace(hash, reader.u64());
        }
        reader.finish();
    } catch (const std::exception& error) {
        // Lost times only restart the wait.
        Log::warn("metadata set-aside record unreadable: " + std::string(error.what()));
        set_aside_since_.clear();
    }
}

void MetadataReplica::persist_set_aside_since_locked() {
    Writer writer;
    writer.u32(static_cast<uint32_t>(set_aside_since_.size()));
    for (const auto& [hash, since] : set_aside_since_) {
        writer.fixed(hash.bytes);
        writer.u64(since);
    }
    writefile(set_aside_p_, writer.data());
}

bool MetadataReplica::set_aside(const Hash256& hash, uint64_t now_unix_ms) {
    const HeadsRevisionBump heads_bump(heads_revision_);
    Lock durable(durable_mutation_m_);
    Lock lock(m_);
    const auto found = accepted_heads_.find(hash);
    if (found == accepted_heads_.end() || set_aside_.size() + 1 >= accepted_heads_.size())
        return false;
    set_aside_.emplace(hash, found->second.generation);
    load_set_aside_since_locked();
    if (set_aside_since_.try_emplace(hash, now_unix_ms).second)
        persist_set_aside_since_locked();
    const auto pending_checkpoint = refresh_materialized_head_deferred_locked();
    lock.unlock();
    if (pending_checkpoint)
        persist(checkpoint_p_, *pending_checkpoint);
    return true;
}

size_t MetadataReplica::expire_set_aside(uint64_t now_unix_ms, std::chrono::milliseconds horizon) {
    const HeadsRevisionBump heads_bump(heads_revision_);
    Lock durable(durable_mutation_m_);
    Lock lock(m_);
    load_set_aside_since_locked();
    if (set_aside_since_.empty())
        return 0;
    size_t dropped = 0;
    bool changed = false;
    for (auto it = set_aside_since_.begin(); it != set_aside_since_.end();) {
        const bool held = accepted_heads_.contains(it->first);
        const bool expired = held && set_aside_.contains(it->first) &&
                             accepted_heads_.size() > 1 && now_unix_ms >= it->second &&
                             now_unix_ms - it->second >= static_cast<uint64_t>(horizon.count());
        if (expired) {
            accepted_heads_.erase(it->first);
            set_aside_.erase(it->first);
            ++dropped;
        }
        if (expired || !held) {
            it = set_aside_since_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed)
        persist_set_aside_since_locked();
    std::optional<MetadataRecord> pending_checkpoint;
    if (dropped) {
        persist_heads_locked();
        pending_checkpoint = refresh_materialized_head_deferred_locked();
    }
    lock.unlock();
    if (pending_checkpoint)
        persist(checkpoint_p_, *pending_checkpoint);
    return dropped;
}

void MetadataReplica::clear_set_aside() {
    const HeadsRevisionBump heads_bump(heads_revision_);
    Lock durable(durable_mutation_m_);
    Lock lock(m_);
    if (set_aside_.empty())
        return;
    set_aside_.clear();
    const auto pending_checkpoint = refresh_materialized_head_deferred_locked();
    lock.unlock();
    if (pending_checkpoint)
        persist(checkpoint_p_, *pending_checkpoint);
}

std::vector<MetadataRecord> MetadataReplica::usable_heads() const {
    auto heads = accepted_heads();
    Lock lock(m_);
    std::erase_if(heads, [&](const MetadataRecord& head) MACHA_REQUIRES(m_) {
        return set_aside_.contains(head.hash);
    });
    return heads;
}

std::vector<MetadataIdentity> MetadataReplica::usable_head_identities() const {
    Lock lock(m_);
    std::vector<MetadataIdentity> out;
    out.reserve(accepted_heads_.size());
    for (const auto& [hash, head] : accepted_heads_)
        if (!set_aside_.contains(hash))
            out.push_back({head.generation, hash});
    return out;
}

void MetadataReplica::refresh_materialized_head_locked() {
    if (refresh_materialized_head_in_memory_locked())
        reset_checkpoint(committed_);
}

std::optional<MetadataRecord> MetadataReplica::refresh_materialized_head_deferred_locked() {
    if (!refresh_materialized_head_in_memory_locked())
        return std::nullopt;
    reset_checkpoint_journal_locked();
    return committed_;
}

void MetadataReplica::ensure_history_root(const MetadataRecord& record) {
    if (history_.contains(record.hash)) {
        cache_materialization_locked(record);
        return;
    }
    MetadataHistoryEntry root;
    root.generation = record.generation;
    root.previous = record.previous;
    root.hash = record.hash;
    // The predecessor material may be absent; the record is a local history root.
    root.previous_known = false;
    root.merge_parents = decode_snapshot(record.payload).merge_parents;
    root.body = MetadataHistoryEntry::Body::full;
    root.payload.assign(record.payload.begin(), record.payload.end());
    append_history(root);
    cache_materialization_locked(record);
}

MetadataHistoryEntry MetadataReplica::history_for_current(std::span<const uint8_t> delta) {
    MetadataHistoryEntry entry_value;
    entry_value.generation = cur_.generation;
    entry_value.previous = cur_.previous;
    entry_value.hash = cur_.hash;
    entry_value.previous_known = cur_.generation > 1 && cur_.previous != Hash256{};
    entry_value.merge_parents = decode_snapshot(cur_.payload).merge_parents;
    constexpr uint64_t full_anchor_interval = 256;
    if (!delta.empty() && cur_.generation % full_anchor_interval != 0) {
        entry_value.body = MetadataHistoryEntry::Body::delta;
        entry_value.payload.assign(delta.begin(), delta.end());
    } else {
        entry_value.body = MetadataHistoryEntry::Body::full;
        entry_value.payload.assign(cur_.payload.begin(), cur_.payload.end());
    }
    return entry_value;
}

std::shared_ptr<const MetadataMaterialization> MetadataReplica::cache_materialization_locked(
    const MetadataRecord& record, std::shared_ptr<const MetadataSnapshot> snapshot,
    uint64_t resident_bytes) const {
    constexpr size_t cache_limit = 64;
    auto found = materialized_history_.find(record.hash);
    if (found != materialized_history_.end()) {
        found->second.last_used = ++materialized_history_clock_;
        return found->second.value;
    }

    if (!snapshot)
        snapshot = std::make_shared<const MetadataSnapshot>(decode_snapshot(record.payload));
    auto value = resident_bytes
                     ? std::make_shared<const MetadataMaterialization>(MetadataMaterialization{
                           record, std::move(snapshot), resident_bytes})
                     : make_materialization(record, std::move(snapshot));
    const uint64_t bytes = value->resident_bytes;
    const bool incoming_pinned = record.hash == cur_.hash || record.hash == committed_.hash;

    while (materialized_history_.size() >= cache_limit ||
           bytes > materialized_history_limit_bytes_ ||
           materialized_history_bytes_ > materialized_history_limit_bytes_ - bytes) {
        auto victim = materialized_history_.end();
        for (auto it = materialized_history_.begin(); it != materialized_history_.end(); ++it) {
            const bool pinned = it->first == cur_.hash || it->first == committed_.hash;
            if (pinned)
                continue;
            if (victim == materialized_history_.end() ||
                it->second.last_used < victim->second.last_used)
                victim = it;
        }
        // Only current/committed are pinned; other heads are reconstructible.
        if (victim == materialized_history_.end())
            break;
        materialized_history_bytes_ -= victim->second.bytes;
        materialized_history_.erase(victim);
        materialization_cache_evictions_.fetch_add(1, std::memory_order_relaxed);
    }

    // Over budget and unpinned: return it uncached.
    if (bytes > materialized_history_limit_bytes_ && !incoming_pinned)
        return value;
    materialized_history_.emplace(record.hash,
                                  MaterializedHistoryEntry{value, ++materialized_history_clock_,
                                                           bytes});
    materialized_history_bytes_ += bytes;
    return value;
}

std::shared_ptr<const MetadataMaterialization>
MetadataReplica::materialized_locked(const Hash256& target) const {
    historical_requests_.fetch_add(1, std::memory_order_relaxed);
    auto found = history_.find(target);
    if (found == history_.end())
        return {};

    if (auto cached = materialized_history_.find(target); cached != materialized_history_.end()) {
        cached->second.last_used = ++materialized_history_clock_;
        materialization_cache_hits_.fetch_add(1, std::memory_order_relaxed);
        return cached->second.value;
    }
    materialization_cache_misses_.fetch_add(1, std::memory_order_relaxed);

    historical_reconstructions_.fetch_add(1, std::memory_order_relaxed);

    // history_ holds only frame indexes; this holds one decoded snapshot and one
    // delta body at a time. Intermediates never enter the cache.
    std::vector<HistoryIndexEntry> deltas;
    std::set<Hash256> seen;
    HistoryIndexEntry cursor = found->second;
    std::shared_ptr<const MetadataMaterialization> materialized_parent;
    while (cursor.body == MetadataHistoryEntry::Body::delta) {
        if (!seen.insert(cursor.hash).second || !cursor.previous_known)
            return {};
        try {
            deltas.push_back(cursor);
        } catch (...) {
            return {};
        }
        auto parent = history_.find(cursor.previous);
        if (parent == history_.end())
            return {};
        cursor = parent->second;
        if (auto cached = materialized_history_.find(cursor.hash);
            cached != materialized_history_.end()) {
            cached->second.last_used = ++materialized_history_clock_;
            materialized_parent = cached->second.value;
            break;
        }
    }

    MetadataRecord working_record;
    MetadataSnapshot working_snapshot;
    if (materialized_parent) {
        working_record = materialized_parent->record;
        working_snapshot = *materialized_parent->snapshot;
        materialized_parent.reset();
    } else {
        MetadataHistoryEntry anchor;
        try {
            anchor = read_history_entry(cursor);
        } catch (...) {
            return {};
        }
        working_record.generation = anchor.generation;
        working_record.previous = anchor.previous;
        working_record.hash = anchor.hash;
        working_record.payload = std::move(anchor.payload);
        if (!valid_metadata_record(working_record))
            return {};
        try {
            working_snapshot = decode_snapshot(working_record.payload);
            if (working_snapshot.merge_parents != anchor.merge_parents)
                return {};
        } catch (...) {
            return {};
        }
    }

    for (auto it = deltas.rbegin(); it != deltas.rend(); ++it) {
        try {
            const auto child = read_history_entry(*it);
            if (child.previous != working_record.hash ||
                !metadata_delta_succession_valid(working_record.generation, child.generation))
                return {};
            const auto delta = decode_metadata_delta(child.payload);
            apply_metadata_delta_in_place(working_snapshot, delta, namespace_applier_);
            historical_deltas_applied_.fetch_add(1, std::memory_order_relaxed);
            MetadataRecord next;
            next.generation = child.generation;
            next.previous = child.previous;
            next.hash = child.hash;
            next.payload = encode_snapshot_for_delta(child.payload, working_snapshot);
            if (!valid_metadata_record(next) ||
                working_snapshot.merge_parents != child.merge_parents)
                return {};
            working_record = std::move(next);
        } catch (...) {
            return {};
        }
    }
    if (working_record.hash != target)
        return {};
    return cache_materialization_locked(
        working_record,
        std::make_shared<const MetadataSnapshot>(std::move(working_snapshot)));
}

std::optional<MetadataRecord> MetadataReplica::historical_locked(const Hash256& target) const {
    auto value = materialized_locked(target);
    if (!value)
        return {};
    return value->record;
}

MetadataReplicaDiagnostics MetadataReplica::diagnostics() const {
    Lock lock(m_);
    return {
        historical_requests_.load(std::memory_order_relaxed),
        historical_reconstructions_.load(std::memory_order_relaxed),
        historical_deltas_applied_.load(std::memory_order_relaxed),
        materialization_cache_hits_.load(std::memory_order_relaxed),
        materialization_cache_misses_.load(std::memory_order_relaxed),
        materialization_cache_evictions_.load(std::memory_order_relaxed),
        materialized_history_.size(),
        materialized_history_bytes_,
        materialized_history_limit_bytes_,
        history_records_,
        history_bytes_,
        0,
        accepted_head_persistence_writes_.load(std::memory_order_relaxed),
        accepted_head_persistence_bytes_.load(std::memory_order_relaxed),
        accepted_head_persistence_failures_.load(std::memory_order_relaxed),
    };
}

bool MetadataReplica::history_is_ancestor_locked(const Hash256& ancestor,
                                                 const Hash256& descendant) const {
    if (ancestor == descendant)
        return history_.contains(ancestor);
    std::vector<Hash256> pending{descendant};
    std::set<Hash256> seen;
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (!seen.insert(current).second)
            continue;
        auto found = history_.find(current);
        if (found == history_.end())
            continue;
        const auto& entry_value = found->second;
        // A compacted root's direct-parent hash is valid ancestry, though not
        // traversable when previous_known=false.
        if (entry_value.previous != Hash256{} && entry_value.previous == ancestor)
            return true;
        if (entry_value.previous_known ||
            (entry_value.previous != Hash256{} && history_.contains(entry_value.previous)))
            pending.push_back(entry_value.previous);
        for (const auto& parent : entry_value.merge_parents) {
            if (parent == ancestor)
                return true;
            pending.push_back(parent);
        }
    }
    return false;
}

std::optional<Hash256> MetadataReplica::history_common_ancestor_locked(const Hash256& left,
                                                                       const Hash256& right) const {
    std::map<Hash256, uint64_t> left_ancestors;
    std::vector<Hash256> pending{left};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (left_ancestors.contains(current))
            continue;
        auto found = history_.find(current);
        const uint64_t generation = found == history_.end() ? 0 : found->second.generation;
        left_ancestors[current] = generation;
        if (found == history_.end())
            continue;
        if (found->second.previous_known ||
            (found->second.previous != Hash256{} &&
             history_.contains(found->second.previous))) {
            pending.push_back(found->second.previous);
        } else if (found->second.previous != Hash256{}) {
            // The boundary parent may be the common ancestor; do not walk
            // into compacted history.
            const auto parent = history_.find(found->second.previous);
            const uint64_t parent_generation =
                parent == history_.end() ? 0 : parent->second.generation;
            left_ancestors.emplace(found->second.previous, parent_generation);
        }
        for (const auto& parent : found->second.merge_parents)
            pending.push_back(parent);
    }

    std::optional<Hash256> best;
    uint64_t best_generation = 0;
    std::set<Hash256> seen;
    pending = {right};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (!seen.insert(current).second)
            continue;
        auto consider = [&](const Hash256& candidate, uint64_t generation) {
            auto intersection = left_ancestors.find(candidate);
            if (intersection == left_ancestors.end())
                return;
            generation = std::max(generation, intersection->second);
            if (!best || generation > best_generation ||
                (generation == best_generation && *best < candidate)) {
                best = candidate;
                best_generation = generation;
            }
        };
        auto found = history_.find(current);
        consider(current, found == history_.end() ? 0 : found->second.generation);
        if (found == history_.end())
            continue;
        if (found->second.previous_known ||
            (found->second.previous != Hash256{} &&
             history_.contains(found->second.previous))) {
            pending.push_back(found->second.previous);
        } else if (found->second.previous != Hash256{}) {
            // As above: matched, not traversed.
            const auto parent = history_.find(found->second.previous);
            consider(found->second.previous,
                     parent == history_.end() ? 0 : parent->second.generation);
        }
        for (const auto& parent : found->second.merge_parents)
            pending.push_back(parent);
    }
    return best;
}

std::optional<MetadataHistoryEntry> MetadataReplica::history_entry(const Hash256& hash) const {
    HistoryIndexEntry index;
    {
        Lock lock(m_);
        auto found = history_.find(hash);
        if (found == history_.end())
            return {};
        index = found->second;
    }
    try {
        return read_history_entry(index);
    } catch (...) {
        return {};
    }
}

std::optional<MetadataHistoryLinks> MetadataReplica::history_links(const Hash256& hash) const {
    Lock lock(m_);
    const auto found = history_.find(hash);
    if (found == history_.end())
        return {};
    return MetadataHistoryLinks{found->second.generation, found->second.previous,
                                found->second.previous_known,
                                found->second.merge_parents, found->second.body};
}

std::optional<MetadataHistoryEntry> MetadataReplica::full_history_record(
    const Hash256& hash) const {
    auto value = materialized(hash);
    if (!value)
        return {};
    MetadataHistoryEntry entry;
    entry.generation = value->record.generation;
    entry.previous = value->record.previous;
    entry.hash = value->record.hash;
    entry.previous_known = entry.generation > 1 && entry.previous != Hash256{} &&
                           history_contains(entry.previous);
    entry.merge_parents = value->snapshot->merge_parents;
    entry.body = MetadataHistoryEntry::Body::full;
    entry.payload.assign(value->record.payload.begin(), value->record.payload.end());
    return entry;
}

std::vector<Hash256> MetadataReplica::unreconstructable_heads() const {
    Lock lock(m_);
    std::vector<Hash256> out;
    for (const auto& [hash, _] : unreconstructable_head_retry_at_)
        if (accepted_heads_.contains(hash))
            out.push_back(hash);
    return out;
}

std::string MetadataReplica::flag_unreconstructable_locked(const Hash256& hash,
                                                            Clock::time_point now,
                                                            std::string_view context) const {
    const HeadsRevisionBump heads_bump(heads_revision_);
    const bool first_failure = !unreconstructable_head_retry_at_.contains(hash);
    unreconstructable_head_retry_at_[hash] = now + unreconstructable_retry_cooldown;
    const auto reason = diagnose_unreconstructable_locked(hash);
    if (first_failure) {
        uint64_t generation = 0;
        if (auto head = accepted_heads_.find(hash); head != accepted_heads_.end())
            generation = head->second.generation;
        Log::warn("metadata accepted head cannot be reconstructed locally; excluded from reads "
                  "pending live repair hash=" +
                  hex(hash.bytes) + " generation=" + std::to_string(generation) +
                  " during=" + std::string(context) + " reason=" + reason);
    }
    return reason;
}

std::string MetadataReplica::diagnose_unreconstructable_locked(const Hash256& target) const {
    const auto describe = [](const HistoryIndexEntry& entry) {
        return "gen=" + std::to_string(entry.generation) + " hash=" + hex(entry.hash.bytes) +
               " body=" +
               std::string(entry.body == MetadataHistoryEntry::Body::delta ? "delta" : "full");
    };
    auto found = history_.find(target);
    if (found == history_.end())
        return "not in local history index";
    std::vector<HistoryIndexEntry> chain;
    std::set<Hash256> seen;
    HistoryIndexEntry cursor = found->second;
    while (cursor.body == MetadataHistoryEntry::Body::delta) {
        if (!seen.insert(cursor.hash).second)
            return "delta chain cycle at " + describe(cursor);
        if (!cursor.previous_known)
            return "delta " + describe(cursor) + " has no known predecessor";
        chain.push_back(cursor);
        auto parent = history_.find(cursor.previous);
        if (parent == history_.end())
            return "delta " + describe(cursor) + " primary parent hash=" +
                   hex(cursor.previous.bytes) + " absent from local history";
        if (!metadata_delta_succession_valid(parent->second.generation, cursor.generation))
            return "delta " + describe(cursor) + " violates succession over parent gen=" +
                   std::to_string(parent->second.generation);
        cursor = parent->second;
    }
    try {
        (void)read_history_entry(cursor);
    } catch (const std::exception& error) {
        return "anchor frame " + describe(cursor) + " unreadable: " + error.what();
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        try {
            (void)read_history_entry(*it);
        } catch (const std::exception& error) {
            return "delta frame " + describe(*it) + " unreadable: " + error.what();
        }
    }
    // The chain was not replayed: report only what was checked.
    return "delta chain from anchor " + describe(cursor) + " over " +
           std::to_string(chain.size()) +
           " frame(s) is structurally sound and every frame is readable; the failure is in "
           "materialisation itself and was not diagnosed here (replay it with "
           "macha-metadata-dump --objects to find the frame)";
}

bool MetadataReplica::reanchor_history(const MetadataHistoryEntry& entry_value) {
    const HeadsRevisionBump heads_bump(heads_revision_);
    if (entry_value.body != MetadataHistoryEntry::Body::full || !entry_value.generation ||
        entry_value.hash == Hash256{} || entry_value.merge_parents.size() > 64)
        return false;
    MetadataRecord record;
    record.generation = entry_value.generation;
    record.previous = entry_value.previous;
    record.hash = entry_value.hash;
    record.payload = entry_value.payload;
    if (!valid_metadata_record(record))
        return false;
    std::shared_ptr<const MetadataSnapshot> snapshot;
    try {
        snapshot = std::make_shared<const MetadataSnapshot>(decode_snapshot(record.payload));
    } catch (...) {
        return false;
    }
    if (snapshot->merge_parents != entry_value.merge_parents)
        return false;

    auto entry = entry_value;
    Lock durable(durable_mutation_m_);
    uint64_t file_offset;
    {
        Lock lock(m_);
        if (auto existing = history_.find(entry.hash); existing != history_.end()) {
            if (existing->second.generation != entry.generation ||
                existing->second.previous != entry.previous)
                return false; // same hash, different identity
            if (materialized_locked(entry.hash)) {
                // Already reconstructible: just lift the exclusion.
                unreconstructable_head_retry_at_.erase(entry.hash);
                return true;
            }
        }
        entry.previous_known = entry.generation > 1 && entry.previous != Hash256{} &&
                               history_.contains(entry.previous);
        file_offset = history_bytes_;
    }
    auto frame = encode_history_frame(entry);
    write_history_frame(frame);

    Lock lock(m_);
    const bool superseded = history_.contains(entry.hash);
    history_[entry.hash] = index_history_entry(entry, file_offset, frame.size());
    ++history_records_;
    history_bytes_ += frame.size();
    materialized_history_.erase(entry.hash);
    auto value = cache_materialization_locked(record, snapshot);
    unreconstructable_head_retry_at_.erase(entry.hash);

    if (auto head = accepted_heads_.find(entry.hash); head != accepted_heads_.end()) {
        // Verify against the record just cached, not a fresh reconstruction.
        if (!acceptance_matches_record_policy_locked(head->second, *value)) {
            // Drop only this certificate.
            Log::error("metadata accepted head certificate does not match its repaired record; "
                       "dropping the certificate hash=" +
                       hex(entry.hash.bytes) + " generation=" +
                       std::to_string(head->second.generation));
            accepted_heads_.erase(head);
            persist_heads_locked();
        }
    }
    Log::info(std::string("metadata history re-anchored ") +
              (superseded ? "superseding unreplayable frame" : "with new record") +
              " hash=" + hex(entry.hash.bytes) + " generation=" +
              std::to_string(entry.generation) + " bytes=" + std::to_string(frame.size()));
    if (prune_accepted_heads_locked())
        persist_heads_locked();
    // Another flagged head may make the refresh throw; this repair still succeeded.
    std::optional<MetadataRecord> pending_checkpoint;
    try {
        pending_checkpoint = refresh_materialized_head_deferred_locked();
    } catch (const std::exception& error) {
        Log::debug("metadata materialized head refresh deferred after re-anchor: " +
                   std::string(error.what()));
    }
    lock.unlock();
    if (pending_checkpoint)
        persist(checkpoint_p_, *pending_checkpoint);
    return true;
}

bool MetadataReplica::import_history(const MetadataHistoryEntry& entry_value) {
    if (!entry_value.generation || entry_value.hash == Hash256{} ||
        entry_value.merge_parents.size() > 64)
        return false;
    auto entry = entry_value;
    std::shared_ptr<const MetadataMaterialization> parent;
    if (entry.body == MetadataHistoryEntry::Body::delta) {
        if (!entry.previous_known)
            return false;
        parent = materialized(entry.previous);
        if (!parent ||
            !metadata_delta_succession_valid(parent->record.generation, entry.generation))
            return false;
    } else if (entry.previous_known && entry.previous != Hash256{} &&
               !history_contains(entry.previous)) {
        // A full record stands alone; do not claim an absent predecessor.
        entry.previous_known = false;
    }

    std::shared_ptr<const MetadataMaterialization> candidate;
    try {
        MetadataRecord record;
        record.generation = entry.generation;
        record.previous = entry.previous;
        record.hash = entry.hash;
        std::shared_ptr<const MetadataSnapshot> snapshot;
        if (entry.body == MetadataHistoryEntry::Body::full) {
            record.payload = entry.payload;
            auto decoded = decode_snapshot(record.payload);
            snapshot = std::make_shared<const MetadataSnapshot>(std::move(decoded));
        } else {
            auto decoded = *parent->snapshot;
            apply_metadata_delta_in_place(decoded, decode_metadata_delta(entry.payload), namespace_applier_);
            record.payload = encode_snapshot_for_delta(entry.payload, decoded);
            snapshot = std::make_shared<const MetadataSnapshot>(std::move(decoded));
        }
        if (!valid_metadata_record(record) || snapshot->merge_parents != entry.merge_parents)
            return false;
        candidate = make_materialization(std::move(record), std::move(snapshot));
    } catch (...) {
        return false;
    }
    auto frame = encode_history_frame(entry);

    Lock durable(durable_mutation_m_);
    {
        Lock lock(m_);
        if (history_.contains(entry.hash))
            return true;
        if (entry.body == MetadataHistoryEntry::Body::delta &&
            !history_.contains(entry.previous))
            return false;
    }
    uint64_t file_offset;
    {
        Lock lock(m_);
        file_offset = history_bytes_;
    }
    write_history_frame(frame);
    Lock lock(m_);
    history_.emplace(entry.hash,
                     index_history_entry(entry, file_offset, frame.size()));
    ++history_records_;
    history_bytes_ += frame.size();
    cache_materialization_locked(candidate->record, candidate->snapshot,
                                 candidate->resident_bytes);
    std::optional<MetadataRecord> pending_checkpoint;
    if (prune_accepted_heads_locked()) {
        persist_heads_locked();
        pending_checkpoint = refresh_materialized_head_deferred_locked();
    }
    lock.unlock();
    if (pending_checkpoint)
        persist(checkpoint_p_, *pending_checkpoint);
    return true;
}

bool MetadataReplica::history_contains(const Hash256& hash) const {
    Lock lock(m_);
    return history_.contains(hash);
}

bool MetadataReplica::store_commit(const MetadataRecord& record,
                                   std::span<const uint8_t> encoded_delta) {
    if (!valid_metadata_record(record))
        return false;
    MetadataHistoryEntry entry_value;
    entry_value.generation = record.generation;
    entry_value.previous = record.previous;
    entry_value.hash = record.hash;
    entry_value.previous_known = record.generation > 1 && record.previous != Hash256{} &&
                                 history_contains(record.previous);
    try {
        entry_value.merge_parents = decode_snapshot(record.payload).merge_parents;
    } catch (...) {
        return false;
    }

    if (!encoded_delta.empty() && entry_value.previous_known &&
        materialized(entry_value.previous)) {
        entry_value.body = MetadataHistoryEntry::Body::delta;
        entry_value.payload.assign(encoded_delta.begin(), encoded_delta.end());
    } else {
        entry_value.body = MetadataHistoryEntry::Body::full;
        entry_value.payload.assign(record.payload.begin(), record.payload.end());
    }

    std::shared_ptr<const MetadataMaterialization> reconstructed;
    try {
        if (entry_value.body == MetadataHistoryEntry::Body::full) {
            reconstructed = make_materialization(
                record, std::make_shared<const MetadataSnapshot>(decode_snapshot(record.payload)));
        } else {
            auto parent = materialized(entry_value.previous);
            if (!parent ||
                !metadata_delta_succession_valid(parent->record.generation, record.generation)) {
                Log::debug("metadata delta body rejected generation=" +
                           std::to_string(record.generation) +
                           (parent ? " reason=succession" : " reason=parent-not-materialized"));
                return false;
            }
            auto snapshot = *parent->snapshot;
            apply_metadata_delta_in_place(snapshot, decode_metadata_delta(entry_value.payload),
                                          namespace_applier_);
            MetadataRecord value;
            value.generation = entry_value.generation;
            value.previous = entry_value.previous;
            value.hash = entry_value.hash;
            value.payload = encode_snapshot_for_delta(entry_value.payload, snapshot);
            reconstructed = make_materialization(
                std::move(value), std::make_shared<const MetadataSnapshot>(std::move(snapshot)));
        }
    } catch (...) {
        return false;
    }
    if (!reconstructed || reconstructed->record.generation != record.generation ||
        reconstructed->record.previous != record.previous ||
        reconstructed->record.hash != record.hash ||
        reconstructed->record.payload != record.payload) {
        // The caller falls back to a full body; this log is the delta's only trace.
        if (entry_value.body == MetadataHistoryEntry::Body::delta)
            Log::debug("metadata delta body rejected generation=" +
                       std::to_string(record.generation) + " reason=" +
                       (!reconstructed ? "no-reconstruction"
                        : reconstructed->record.payload != record.payload ? "payload-mismatch"
                                                                           : "identity-mismatch"));
        return false;
    }
    auto frame = encode_history_frame(entry_value);

    Lock durable(durable_mutation_m_);
    {
        Lock lock(m_);
        if (history_.contains(record.hash))
            return true;
        if (entry_value.body == MetadataHistoryEntry::Body::delta &&
            !history_.contains(entry_value.previous))
            return false;
    }
    uint64_t file_offset;
    {
        Lock lock(m_);
        file_offset = history_bytes_;
    }
    write_history_frame(frame);
    Lock lock(m_);
    history_.emplace(entry_value.hash,
                     index_history_entry(entry_value, file_offset, frame.size()));
    ++history_records_;
    history_bytes_ += frame.size();
    cache_materialization_locked(reconstructed->record, reconstructed->snapshot,
                                 reconstructed->resident_bytes);
    return true;
}

bool MetadataReplica::accept_commit(const MetadataAcceptance& input, bool* heads_changed) {
    const HeadsRevisionBump heads_bump(heads_revision_);
    if (heads_changed)
        *heads_changed = false;
    MetadataAcceptance value = input;
    std::sort(value.replicas.begin(), value.replicas.end());
    value.replicas.erase(std::unique(value.replicas.begin(), value.replicas.end()),
                         value.replicas.end());
    if (!value.generation || value.hash == Hash256{})
        return false;
    if (std::any_of(value.replicas.begin(), value.replicas.end(),
                    [](const NodeId& replica) { return replica == NodeId{}; }))
        return false;
    if (value.required) {
        if (value.replicas.size() < value.required)
            return false;
    } else if (!value.replicas.empty()) {
        return false;
    }

    // Warm the cache off-lock so the locked validation below hits it.
    auto prepared = materialized(value.hash);
    if (!prepared || prepared->record.generation != value.generation)
        return false;
    for (const auto& parent : metadata_record_parents(prepared->record))
        (void)materialized(parent);

    Lock durable(durable_mutation_m_);
    // A head change rewrites the checkpoint (possibly hundreds of MB): write it
    // after releasing `m_`, under `durable_mutation_m_` only, so readers on the
    // RPC path do not stall behind the fsync.
    std::optional<MetadataRecord> pending_checkpoint;
    {
        Lock lock(m_);
        auto materialized = materialized_locked(value.hash);
        if (!materialized || materialized->record.generation != value.generation)
            return false;
        if (!acceptance_matches_record_policy_locked(value, *materialized))
            return false;

        bool changed = false;
        // An accepted ancestor is not a head: remove it if already present.
        bool incoming_is_ancestor = false;
        for (const auto& [head, _] : accepted_heads_) {
            if (head != value.hash && accepted_head_is_ancestor_locked(value.hash, head)) {
                incoming_is_ancestor = true;
                break;
            }
        }
        if (incoming_is_ancestor) {
            if (accepted_heads_.erase(value.hash)) {
                if (heads_changed)
                    *heads_changed = true;
                persist_heads_locked();
                if (refresh_materialized_head_in_memory_locked()) {
                    reset_checkpoint_journal_locked();
                    pending_checkpoint = committed_;
                }
            }
        } else {
            for (auto it = accepted_heads_.begin(); it != accepted_heads_.end();) {
                if (it->first != value.hash &&
                    accepted_head_is_ancestor_locked(it->first, value.hash)) {
                    it = accepted_heads_.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }

            auto found = accepted_heads_.find(value.hash);
            if (found == accepted_heads_.end()) {
                accepted_heads_.emplace(value.hash, value);
                changed = true;
            } else {
                auto merged = found->second;
                if (!merged.required && value.required) {
                    merged = value;
                } else if (merged.required && value.required) {
                    merged.required = std::min(merged.required, value.required);
                    merged.replicas.insert(merged.replicas.end(), value.replicas.begin(),
                                           value.replicas.end());
                    std::sort(merged.replicas.begin(), merged.replicas.end());
                    merged.replicas.erase(
                        std::unique(merged.replicas.begin(), merged.replicas.end()),
                        merged.replicas.end());
                }
                if (merged != found->second) {
                    found->second = std::move(merged);
                    changed = true;
                }
            }

            changed = prune_accepted_heads_locked() || changed;
            if (heads_changed)
                *heads_changed = changed;
            if (changed) {
                persist_heads_locked();
                if (refresh_materialized_head_in_memory_locked()) {
                    reset_checkpoint_journal_locked();
                    pending_checkpoint = committed_;
                }
            }
        }
    }
    if (pending_checkpoint) persist(checkpoint_p_, *pending_checkpoint);
    return true;
}

std::vector<MetadataAcceptance> MetadataReplica::accepted_head_certificates() const {
    Lock lock(m_);
    std::vector<MetadataAcceptance> out;
    out.reserve(accepted_heads_.size());
    for (const auto& [_, value] : accepted_heads_)
        out.push_back(value);
    return out;
}

std::vector<MetadataRecord> MetadataReplica::accepted_heads() const {
    std::vector<Hash256> hashes;
    {
        Lock lock(m_);
        hashes.reserve(accepted_heads_.size());
        for (const auto& [hash, _] : accepted_heads_)
            hashes.push_back(hash);
    }
    // Hot path: a broken head is excluded for its cooldown rather than thrown
    // (see unreconstructable_head_retry_at_). Callers read the set's size as
    // not-ready/converged/needs-reconciliation, so progress continues.
    const auto now = Clock::now();
    std::vector<MetadataRecord> out;
    out.reserve(hashes.size());
    for (const auto& hash : hashes) {
        {
            Lock lock(m_);
            auto found = unreconstructable_head_retry_at_.find(hash);
            if (found != unreconstructable_head_retry_at_.end() && now < found->second)
                continue;
        }
        auto value = materialized(hash);
        Lock lock(m_);
        if (!value) {
            (void)flag_unreconstructable_locked(hash, now, "accepted head enumeration");
            continue;
        }
        unreconstructable_head_retry_at_.erase(hash);
        out.push_back(value->record);
    }
    return out;
}

std::optional<MetadataAcceptance> MetadataReplica::acceptance(const Hash256& hash) const {
    Lock lock(m_);
    auto found = accepted_heads_.find(hash);
    if (found == accepted_heads_.end())
        return {};
    return found->second;
}

bool MetadataReplica::history_is_ancestor(const Hash256& ancestor,
                                          const Hash256& descendant) const {
    Lock lock(m_);
    return history_is_ancestor_locked(ancestor, descendant);
}

std::optional<Hash256> MetadataReplica::history_common_ancestor(const Hash256& left,
                                                                const Hash256& right) const {
    Lock lock(m_);
    return history_common_ancestor_locked(left, right);
}

std::optional<MetadataRecord> MetadataReplica::historical(const Hash256& hash) const {
    auto value = materialized(hash);
    if (!value)
        return {};
    return value->record;
}

std::shared_ptr<const MetadataMaterialization>
MetadataReplica::materialized(const Hash256& hash) const {
    historical_requests_.fetch_add(1, std::memory_order_relaxed);
    Lock computation(materialization_compute_m_);

    std::vector<HistoryIndexEntry> delta_indexes;
    HistoryIndexEntry anchor_index;
    std::shared_ptr<const MetadataMaterialization> base;
    {
        Lock lock(m_);
        auto found = history_.find(hash);
        if (found == history_.end())
            return {};
        if (auto cached = materialized_history_.find(hash); cached != materialized_history_.end()) {
            cached->second.last_used = ++materialized_history_clock_;
            materialization_cache_hits_.fetch_add(1, std::memory_order_relaxed);
            return cached->second.value;
        }
        materialization_cache_misses_.fetch_add(1, std::memory_order_relaxed);
        historical_reconstructions_.fetch_add(1, std::memory_order_relaxed);

        std::set<Hash256> seen;
        auto cursor = found;
        while (cursor->second.body == MetadataHistoryEntry::Body::delta) {
            if (!seen.insert(cursor->first).second || !cursor->second.previous_known)
                return {};
            delta_indexes.push_back(cursor->second);
            cursor = history_.find(cursor->second.previous);
            if (cursor == history_.end())
                return {};
            if (auto cached = materialized_history_.find(cursor->first);
                cached != materialized_history_.end()) {
                cached->second.last_used = ++materialized_history_clock_;
                base = cached->second.value;
                break;
            }
        }
        if (!base)
            anchor_index = cursor->second;
    }

    // Decode, apply, encode and hash run without `m_`, holding exactly one
    // mutable reconstruction.
    MetadataRecord working_record;
    MetadataSnapshot working_snapshot;
    try {
        if (base) {
            working_record = base->record;
            working_snapshot = *base->snapshot;
            base.reset();
        } else {
            auto anchor = read_history_entry(anchor_index);
            working_record.generation = anchor.generation;
            working_record.previous = anchor.previous;
            working_record.hash = anchor.hash;
            working_record.payload = std::move(anchor.payload);
            if (!valid_metadata_record(working_record))
                return {};
            working_snapshot = decode_snapshot(working_record.payload);
            if (working_snapshot.merge_parents != anchor.merge_parents)
                return {};
        }

        for (auto it = delta_indexes.rbegin(); it != delta_indexes.rend(); ++it) {
            const auto child = read_history_entry(*it);
            if (child.previous != working_record.hash ||
                !metadata_delta_succession_valid(working_record.generation, child.generation))
                return {};
            const auto delta = decode_metadata_delta(child.payload);
            apply_metadata_delta_in_place(working_snapshot, delta, namespace_applier_);
            historical_deltas_applied_.fetch_add(1, std::memory_order_relaxed);
            MetadataRecord record;
            record.generation = child.generation;
            record.previous = child.previous;
            record.hash = child.hash;
            record.payload = encode_snapshot_for_delta(child.payload, working_snapshot);
            if (!valid_metadata_record(record) ||
                working_snapshot.merge_parents != child.merge_parents)
                return {};
            working_record = std::move(record);
        }
    } catch (...) {
        return {};
    }

    auto value = make_materialization(
        std::move(working_record),
        std::make_shared<const MetadataSnapshot>(std::move(working_snapshot)));

    Lock lock(m_);
    // Cache only the requested result, and only if compaction has not removed it.
    if (history_.contains(value->record.hash))
        value = cache_materialization_locked(value->record, value->snapshot,
                                             value->resident_bytes);
    if (auto cached = materialized_history_.find(hash); cached != materialized_history_.end())
        return cached->second.value;
    return value && value->record.hash == hash ? value
                                               : std::shared_ptr<const MetadataMaterialization>{};
}

void MetadataReplica::reset_checkpoint_journal_locked() {
    writefile(journal_p_, {});
    journal_records_ = 0;
    journal_bytes_ = 0;
}

void MetadataReplica::reset_checkpoint(const MetadataRecord& record) {
    persist(checkpoint_p_, record);
    reset_checkpoint_journal_locked();
}

void MetadataReplica::compact() {
    constexpr size_t record_threshold = 128;
    constexpr uint64_t byte_threshold = 8ULL * 1024 * 1024;
    // Every journal append holds durable_mutation_m_, so with it held the
    // journal and the committed head stay as read here while the files are
    // written without m_: no reader waits on the checkpoint's sync.
    Lock durable(durable_mutation_m_);
    MetadataRecord checkpoint;
    size_t records = 0;
    uint64_t bytes = 0;
    {
        Lock lock(m_);
        if (cur_.generation != committed_.generation || cur_.hash != committed_.hash)
            return;
        if (journal_records_ < record_threshold && journal_bytes_ < byte_threshold)
            return;
        checkpoint = committed_;
        records = journal_records_;
        bytes = journal_bytes_;
    }
    persist(checkpoint_p_, checkpoint);
    writefile(journal_p_, {});
    {
        Lock lock(m_);
        journal_records_ = 0;
        journal_bytes_ = 0;
    }
    const auto generation = checkpoint.generation;
    const auto snapshot_bytes = checkpoint.payload.size();
    Log::debug("metadata journal compacted generation=" + std::to_string(generation) +
               " records=" + std::to_string(records) + " journal_bytes=" + std::to_string(bytes) +
               " snapshot_bytes=" + std::to_string(snapshot_bytes));
}

bool MetadataReplica::cas(uint64_t generation, const Hash256& hash,
                          std::span<const uint8_t> payload, MetadataRecord* out) {
    MetadataRecord next;
    next.generation = generation + 1;
    next.previous = hash;
    next.payload.assign(payload.begin(), payload.end());
    (void)decode_snapshot(next.payload);
    next.hash = metadata_hash(next.generation, next.previous, next.payload);

    Lock durable(durable_mutation_m_);
    Lock lock(m_);
    if (!legacy_write_api_allowed_locked())
        return false;
    if (cur_.generation != generation || cur_.hash != hash) {
        if (committed_.generation != generation || committed_.hash != hash) {
            if (out)
                *out = cur_;
            return false;
        }

        // Racing coordinators: if each overwrote the PREPARE, each could collect
        // acks for a proposal gone by COMMIT time. The lowest-hash direct child
        // wins; others help it commit, then retry on top.
        const bool competing_direct_child = cur_.generation == generation + 1 &&
                                            cur_.previous == hash && cur_.hash != committed_.hash;
        if (competing_direct_child && pending_recovered_) {
            // A PREPARE recovered at restart has no coordinator; any proposal replaces it.
        } else if (competing_direct_child) {
            if (cur_.hash == next.hash) {
                if (out)
                    *out = cur_;
                return true;
            }
            if (cur_.hash < next.hash) {
                if (out)
                    *out = cur_;
                return false;
            }
        } else {
            if (out)
                *out = cur_;
            return false;
        }

        // Journal the rollback first so replay sees the same replacement.
        append_journal(JOURNAL_SEED_FULL, committed_, committed_.payload);
        cur_ = committed_;
        pending_history_.reset();
        pending_recovered_ = false;
    }

    append_journal(JOURNAL_PREPARE_FULL, next, next.payload);
    cur_ = next;
    pending_history_ = history_for_current();
    pending_recovered_ = false;
    if (out)
        *out = next;
    return true;
}

bool MetadataReplica::cas_delta(uint64_t generation, const Hash256& hash,
                                std::span<const uint8_t> encoded_delta, MetadataRecord* out) {
    Lock durable(durable_mutation_m_);
    Lock lock(m_);
    if (!legacy_write_api_allowed_locked())
        return false;

    const MetadataRecord* base = nullptr;
    if (cur_.generation == generation && cur_.hash == hash) {
        base = &cur_;
    } else if (committed_.generation == generation && committed_.hash == hash) {
        base = &committed_;
    } else {
        if (out)
            *out = cur_;
        return false;
    }

    auto after = decode_snapshot(base->payload);
    auto delta = decode_metadata_delta(encoded_delta);
    apply_metadata_delta_in_place(after, delta, namespace_applier_);

    MetadataRecord next;
    next.generation = generation + 1;
    next.previous = hash;
    next.payload = encode_snapshot_for_delta(encoded_delta, after);
    next.hash = metadata_hash(next.generation, next.previous, next.payload);

    if (cur_.generation != generation || cur_.hash != hash) {
        const bool competing_direct_child = cur_.generation == generation + 1 &&
                                            cur_.previous == hash && cur_.hash != committed_.hash;
        if (competing_direct_child && pending_recovered_) {
            // A PREPARE recovered at restart has no coordinator; any proposal replaces it.
        } else if (competing_direct_child) {
            if (cur_.hash == next.hash) {
                if (out)
                    *out = cur_;
                return true;
            }
            if (cur_.hash < next.hash) {
                if (out)
                    *out = cur_;
                return false;
            }
        } else {
            if (out)
                *out = cur_;
            return false;
        }

        append_journal(JOURNAL_SEED_FULL, committed_, committed_.payload);
        cur_ = committed_;
        pending_history_.reset();
        pending_recovered_ = false;
    }

    append_journal(JOURNAL_PREPARE_DELTA, next, encoded_delta);
    cur_ = next;
    pending_history_ = history_for_current(encoded_delta);
    pending_recovered_ = false;
    if (out)
        *out = next;
    return true;
}

bool MetadataReplica::install_committed_delta(uint64_t generation, const Hash256& hash,
                                              std::span<const uint8_t> encoded_delta,
                                              const MetadataRecord& committed) {
    if (!valid_metadata_record(committed))
        return false;
    Lock durable(durable_mutation_m_);
    Lock lock(m_);
    if (!legacy_write_api_allowed_locked())
        return false;

    if (cur_.generation == committed.generation && cur_.hash == committed.hash) {
        if (committed_.generation == committed.generation && committed_.hash == committed.hash)
            return true;
        if (!history_.contains(cur_.hash)) {
            if (pending_history_ && pending_history_->hash == cur_.hash)
                append_history(*pending_history_);
            else
                append_history(history_for_current());
        }
        append_journal(JOURNAL_COMMIT, cur_);
        set_committed_locked(cur_);
        pending_history_.reset();
        set_legacy_committed_head_locked(committed_);
        return true;
    }
    if (cur_.generation != generation || cur_.hash != hash)
        return false;

    auto after = decode_snapshot(cur_.payload);
    auto delta = decode_metadata_delta(encoded_delta);
    apply_metadata_delta_in_place(after, delta, namespace_applier_);
    MetadataRecord next;
    next.generation = generation + 1;
    next.previous = hash;
    next.payload = encode_snapshot_for_delta(encoded_delta, after);
    next.hash = metadata_hash(next.generation, next.previous, next.payload);
    if (next.generation != committed.generation || next.previous != committed.previous ||
        next.hash != committed.hash || next.payload != committed.payload)
        return false;

    append_journal(JOURNAL_PREPARE_DELTA, next, encoded_delta);
    cur_ = next;
    pending_history_ = history_for_current(encoded_delta);
    append_history(*pending_history_);
    append_journal(JOURNAL_COMMIT, cur_);
    set_committed_locked(cur_);
    pending_history_.reset();
    set_legacy_committed_head_locked(committed_);
    return true;
}

bool MetadataReplica::install_migrated_head(const MetadataRecord& record,
                                            const std::vector<NodeId>& witnesses,
                                            const std::string& reason) {
    const HeadsRevisionBump heads_bump(heads_revision_);
    if (!valid_metadata_record(record))
        return false;
    Lock durable(durable_mutation_m_);
    Lock lock(m_);

    const auto stamp = ".pre-migration." + std::to_string(wall_time_ns());
    durable_replace_file(author_chain_broken_p_, reason);
    std::vector<std::filesystem::path> quarantined;
    for (const auto& path :
         {checkpoint_p_, journal_p_, history_p_, heads_p_, p_, committed_p_}) {
        if (auto moved = quarantine_metadata_file(path, stamp))
            quarantined.push_back(*moved);
    }

    cur_ = record;
    set_committed_locked(record);
    reset_checkpoint(record);
    load_history();
    ensure_history_root(record);
    load_heads();

    // The record must be an accepted head or the node refuses to serve. The
    // certificate names the operator-named nodes being re-rooted onto this
    // record: the record is a pure function of the converged head, and
    // `--expect-hash` proves each node computed the same one. It needs at
    // least one witness (a required=0 certificate would be refused by
    // acceptance_matches_record_policy_locked).
    accepted_heads_.clear();
    MetadataAcceptance accepted;
    accepted.generation = record.generation;
    accepted.hash = record.hash;
    accepted.replicas = witnesses;
    std::sort(accepted.replicas.begin(), accepted.replicas.end());
    accepted.replicas.erase(std::unique(accepted.replicas.begin(), accepted.replicas.end()),
                            accepted.replicas.end());
    if (std::any_of(accepted.replicas.begin(), accepted.replicas.end(),
                    [](const NodeId& id) { return id == NodeId{}; })) {
        Log::error("metadata migration refused: a witness is the empty node id");
        return false;
    }
    if (accepted.replicas.empty()) {
        Log::error("metadata migration refused: no witness named");
        return false;
    }
    accepted.required = static_cast<uint32_t>(accepted.replicas.size());
    accepted_heads_.emplace(accepted.hash, accepted);
    persist_heads_locked();

    refresh_materialized_head_locked();
    // A deliberate re-root, not recovery: the record is authoritative.
    recovery_required_ = false;
    pending_recovered_ = false;

    std::string preserved;
    for (const auto& path : quarantined) {
        if (!preserved.empty())
            preserved += ",";
        preserved += path.string();
    }
    Log::warn("metadata namespace migrated to a new root generation=" +
              std::to_string(record.generation) + " hash=" + to_string(record.hash) +
              " state=" + checkpoint_p_.parent_path().string() + " preserved=" + preserved +
              " reason=" + reason);
    return true;
}

bool MetadataReplica::seed(const MetadataRecord& record) {
    if (!valid_metadata_record(record))
        return false;
    const auto parents = metadata_record_parents(record);
    Lock durable(durable_mutation_m_);
    Lock lock(m_);
    if (!legacy_write_api_allowed_locked())
        return false;
    if (record.hash == cur_.hash)
        return true;
    if (record.hash == committed_.hash) {
        append_journal(JOURNAL_SEED_FULL, committed_, committed_.payload);
        cur_ = committed_;
        pending_history_.reset();
        pending_recovered_ = false;
        return true;
    }
    if (record.generation < committed_.generation)
        return false;

    const bool fresh = committed_.generation <= 1;
    const bool direct_parent =
        std::find(parents.begin(), parents.end(), committed_.hash) != parents.end();
    const bool known_descendant =
        history_.contains(record.hash) && history_is_ancestor_locked(committed_.hash, record.hash);
    const bool parent_descends_from_committed =
        std::any_of(parents.begin(), parents.end(), [&](const Hash256& parent) MACHA_REQUIRES(m_) {
            return history_.contains(parent) && history_is_ancestor_locked(committed_.hash, parent);
        });
    if (!fresh && !direct_parent && !known_descendant && !parent_descends_from_committed)
        return false;

    // A prepare, not a commit: the committed branch stays in history and
    // reconciliation names it as a parent.
    append_journal(JOURNAL_SEED_FULL, record, record.payload);
    cur_ = record;
    pending_history_ = history_for_current();
    pending_recovered_ = false;
    return true;
}

bool MetadataReplica::remember_committed(const MetadataRecord& record) {
    if (!valid_metadata_record(record))
        return false;
    const auto parents = metadata_record_parents(record);
    Lock durable(durable_mutation_m_);
    Lock lock(m_);
    if (!legacy_write_api_allowed_locked())
        return false;
    if (record.hash == committed_.hash)
        return true;
    if (record.generation < committed_.generation)
        return false;

    if (cur_.hash != record.hash) {
        const bool fresh = committed_.generation <= 1;
        const bool direct_parent =
            std::find(parents.begin(), parents.end(), committed_.hash) != parents.end();
        const bool known_descendant = history_.contains(record.hash) &&
                                      history_is_ancestor_locked(committed_.hash, record.hash);
        const bool parent_descends_from_committed = std::any_of(
            parents.begin(), parents.end(), [&](const Hash256& parent) MACHA_REQUIRES(m_) {
                return history_.contains(parent) &&
                       history_is_ancestor_locked(committed_.hash, parent);
            });
        if (!fresh && !direct_parent && !known_descendant && !parent_descends_from_committed)
            return false;
        append_journal(JOURNAL_SEED_FULL, record, record.payload);
        cur_ = record;
        pending_history_ = history_for_current();
    }

    if (!history_.contains(cur_.hash)) {
        if (pending_history_ && pending_history_->hash == cur_.hash)
            append_history(*pending_history_);
        else
            append_history(history_for_current());
    }
    append_journal(JOURNAL_COMMIT, cur_);
    set_committed_locked(cur_);
    pending_history_.reset();
    pending_recovered_ = false;
    set_legacy_committed_head_locked(committed_);
    return true;
}

bool MetadataReplica::remember_current_committed(uint64_t generation, const Hash256& hash) {
    Lock durable(durable_mutation_m_);
    Lock lock(m_);
    if (!legacy_write_api_allowed_locked())
        return false;
    if (cur_.generation != generation || cur_.hash != hash)
        return false;
    if (cur_.generation < committed_.generation)
        return false;
    if (cur_.generation == committed_.generation && cur_.hash != committed_.hash)
        return false;
    if (cur_.hash == committed_.hash)
        return true;
    if (!history_.contains(cur_.hash)) {
        if (pending_history_ && pending_history_->hash == cur_.hash)
            append_history(*pending_history_);
        else
            append_history(history_for_current());
    }
    append_journal(JOURNAL_COMMIT, cur_);
    set_committed_locked(cur_);
    pending_history_.reset();
    pending_recovered_ = false;
    set_legacy_committed_head_locked(committed_);
    return true;
}

bool MetadataReplica::compact_history_if_safe(size_t record_threshold, uint64_t byte_threshold) {
    Lock durable(durable_mutation_m_);
    Lock lock(m_);
    if ((history_records_ < record_threshold && history_bytes_ < byte_threshold) ||
        cur_.generation != committed_.generation || cur_.hash != committed_.hash ||
        pending_history_ || accepted_heads_.size() != 1 ||
        !accepted_heads_.contains(committed_.hash))
        return false;

    // The sole accepted head becomes a full entry with no known previous:
    // the root of this node's history.
    MetadataHistoryEntry root;
    root.generation = committed_.generation;
    root.previous = committed_.previous;
    root.hash = committed_.hash;
    root.previous_known = false;
    root.merge_parents = decode_snapshot(committed_.payload).merge_parents;
    root.body = MetadataHistoryEntry::Body::full;
    root.payload.assign(committed_.payload.begin(), committed_.payload.end());

    auto frame = encode_history_frame(root);
    durable_replace_file(
        history_p_, std::string_view(reinterpret_cast<const char*>(frame.data()), frame.size()));

    history_.clear();
    history_.emplace(root.hash, index_history_entry(root, 0, frame.size()));
    materialized_history_.clear();
    materialized_history_clock_ = 0;
    materialized_history_bytes_ = 0;
    cache_materialization_locked(committed_);
    history_records_ = 1;
    history_bytes_ = frame.size();
    return true;
}

void MetadataReplica::persist(const std::filesystem::path& path, const MetadataRecord& record) {
    auto encoded = encode_metadata_record(record);
    auto sealed = aes_gcm_seal(key_, encoded, DM);
    Writer writer;
    writer.raw(DM);
    writer.fixed(sealed.nonce);
    writer.fixed(sealed.tag);
    writer.bytes(sealed.ciphertext);
    writefile(path, writer.data());
}

std::optional<MetadataRecord> MetadataReplica::load(const std::filesystem::path& path) const {
    if (!std::filesystem::exists(path))
        return {};
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            throw std::runtime_error("cannot open");
        Bytes bytes(std::istreambuf_iterator<char>(stream), {});
        Reader reader(bytes);
        auto magic = reader.raw(8);
        if (!std::equal(magic.begin(), magic.end(), DM.begin()))
            throw std::runtime_error("bad metadata file header");
        auto nonce = reader.fixed<12>();
        auto tag = reader.fixed<16>();
        auto ciphertext = reader.bytes();
        reader.finish();
        return decode_metadata_record(aes_gcm_open(key_, nonce, tag, ciphertext, DM));
    } catch (const std::exception& error) {
        throw std::runtime_error("metadata file " + path.string() + ": " + error.what());
    }
}

std::set<ObjectId> metadata_conflict_extent_roots(const MetadataSnapshot& snapshot) {
    std::set<ObjectId> out;
    for (const auto& [_, conflict] : snapshot.conflicts) {
        if (conflict.kind != MetadataConflictKind::namespace_entry)
            continue;
        for (const auto* candidate :
             {&conflict.base_entry, &conflict.left_entry, &conflict.right_entry}) {
            if (!*candidate)
                continue;
            for (const auto& extent : (**candidate).extents) {
                if (!extent.hole)
                    out.insert(extent.id);
            }
        }
    }
    return out;
}

std::set<ObjectId> metadata_catalogue_root_set(const MetadataSnapshot& snapshot) {
    std::set<ObjectId> out;
    if (snapshot.catalogue_root)
        out.insert(*snapshot.catalogue_root);
    for (const auto& [_, conflict] : snapshot.conflicts) {
        if (conflict.kind != MetadataConflictKind::catalogue_root)
            continue;
        for (const auto* candidate : {&conflict.base_catalogue_root, &conflict.left_catalogue_root,
                                      &conflict.right_catalogue_root}) {
            if (*candidate)
                out.insert(**candidate);
        }
    }
    return out;
}

Hash256 metadata_namespace_signature(const MetadataSnapshot& snapshot) {
    // Streamed into SHA-256: no namespace-sized buffer.
    Sha256Hasher hash;
    // A tree's root already hashes this content; the empty entry map would
    // give every tree-backed namespace the same signature.
    if (snapshot.namespace_root) {
        hash.update(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>("macha/namespace-signature/tree/v1"), 33));
        hash.update(snapshot.namespace_root->bytes);
        return Hash256{hash.finish().bytes};
    }
    hash_u64(hash, snapshot.entries.size());
    for (const auto& [path, entry] : snapshot.entries) {
        hash_string(hash, path);
        hash_u8(hash, static_cast<uint8_t>(entry.type));
        if (entry.type != EntryType::file)
            continue;
        hash_u64(hash, entry.size);
        if (entry.extents.size() > UINT32_MAX)
            throw std::runtime_error("too many extents for namespace signature");
        hash_u32(hash, static_cast<uint32_t>(entry.extents.size()));
        for (const auto& extent : entry.extents) {
            hash_u64(hash, extent.offset);
            hash_u64(hash, extent.length);
            hash_u8(hash, extent.hole);
            if (!extent.hole)
                hash.update(extent.id.bytes);
        }
    }
    return hash.finish();
}

std::string normalize_path(const std::string& p) {
    std::vector<std::string> v;
    size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p[i] == '/')
            ++i;
        auto s = i;
        while (i < p.size() && p[i] != '/')
            ++i;
        if (s == i)
            break;
        auto x = p.substr(s, i - s);
        if (x == ".")
            continue;
        if (x == "..") {
            if (!v.empty())
                v.pop_back();
            continue;
        }
        v.push_back(x);
    }
    if (v.empty())
        return "/";
    std::string o;
    for (auto& x : v)
        o += "/" + x;
    return o;
}
std::string parent_path(const std::string& p) {
    auto n = normalize_path(p);
    if (n == "/")
        return "/";
    auto i = n.rfind('/');
    return i ? n.substr(0, i) : "/";
}
std::string base_name(const std::string& p) {
    auto n = normalize_path(p);
    return n == "/" ? "/" : n.substr(n.rfind('/') + 1);
}
int64_t wall_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace macha
