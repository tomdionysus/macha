// SPDX-License-Identifier: GPL-3.0-or-later
#include "metadata.hpp"
#include "codec.hpp"
#include "durable_file.hpp"
#include "log.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <tuple>
#include <unistd.h>
namespace macha {
namespace {
constexpr std::array<uint8_t, 8> SM5{'D', 'H', 'T', 'M', 'E', 'T', 'A', '5'},
    SM6{'D', 'H', 'T', 'M', 'E', 'T', 'A', '6'},
    SM7{'D', 'H', 'T', 'M', 'E', 'T', 'A', '7'},
    SM8{'D', 'H', 'T', 'M', 'E', 'T', 'A', '8'},
    SM9{'D', 'H', 'T', 'M', 'E', 'T', 'A', '9'},
    SM10{'D', 'H', 'T', 'M', 'E', 'T', 'B', '0'},
    SM11{'D', 'H', 'T', 'M', 'E', 'T', 'B', '1'},
    SM12{'D', 'H', 'T', 'M', 'E', 'T', 'B', '2'},
    SM13{'D', 'H', 'T', 'M', 'E', 'T', 'B', '3'},
    DM{'D', 'H', 'T', 'M', 'D', 'B', '0', '1'},
    MJ{'D', 'H', 'T', 'M', 'J', 'N', 'L', '1'},
    MH{'D', 'H', 'T', 'M', 'H', 'S', 'T', '1'},
    MA{'D', 'H', 'T', 'M', 'A', 'C', 'C', '1'},
    MS{'D', 'H', 'T', 'M', 'S', 'E', 'Q', '1'};
constexpr uint8_t JOURNAL_PREPARE_FULL = 1, JOURNAL_PREPARE_DELTA = 2,
                  JOURNAL_SEED_FULL = 3, JOURNAL_COMMIT = 4;
void entry(Writer& w, const FsEntry& e) {
    w.u8((uint8_t)e.type);
    w.u32(e.mode);
    w.u32(e.uid);
    w.u32(e.gid);
    w.u64(e.size);
    w.i64(e.ctime_ns);
    w.i64(e.mtime_ns);
    w.u64(e.version);
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
    auto n = r.u32();
    if (n > 10000000)
        throw DecodeError("too many extents");
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

void encode_conflict(Writer& w, const MetadataConflict& conflict) {
    w.u8(static_cast<uint8_t>(conflict.kind));
    w.string(conflict.key);
    w.fixed(conflict.left_head.bytes);
    w.fixed(conflict.right_head.bytes);
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
    const auto kind = r.u8();
    if (kind < static_cast<uint8_t>(MetadataConflictKind::namespace_entry) ||
        kind > static_cast<uint8_t>(MetadataConflictKind::catalogue_root))
        throw DecodeError("bad metadata conflict kind");
    conflict.kind = static_cast<MetadataConflictKind>(kind);
    conflict.key = r.string(8192);
    if (conflict.key.empty())
        throw DecodeError("empty metadata conflict key");
    conflict.left_head.bytes = r.fixed<32>();
    conflict.right_head.bytes = r.fixed<32>();
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
    // Exact legacy snapshot representation. This is used only while
    // replaying DLT1 records from an existing metadata journal: the journal
    // stores the successor hash, so reconstructing the historical SM7 bytes
    // is part of on-disk compatibility. Ordinary new snapshots remain SM8.
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
    // Exact pre-telemetry representation. DLT2 journal successor hashes were
    // computed over SM8 bytes, so rolling forward an existing journal must
    // reproduce that encoding rather than silently upgrading it to SM9.
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
    if (data[7] < '1' || data[7] > '5')
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
        // Preserve SM9 for historical DLT3 journal successors.
        if (!snapshot.identity_resets.empty())
            throw DecodeError("DLT3 cannot contain identity resets");
        return encode_snapshot(snapshot);
    case 4:
    case 5:
        return encode_snapshot(snapshot);
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
    durable_replace_file(
        p, std::string_view(reinterpret_cast<const char*>(d.data()), d.size()));
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
        throw std::runtime_error("cannot sync truncated metadata journal " + path.string() +
                                 ": " + strerror(error));
    }
    if (close(fd) != 0)
        throw std::runtime_error("cannot close truncated metadata journal " + path.string() +
                                 ": " + strerror(errno));
}

std::filesystem::path quarantine_journal_tail(const std::filesystem::path& path,
                                              std::span<const uint8_t> tail) {
    const auto quarantine = path.string() + ".corrupt." + std::to_string(wall_time_ns());
    durable_replace_file(
        quarantine,
        std::string_view(reinterpret_cast<const char*>(tail.data()), tail.size()));
    return quarantine;
}

void sync_directory(const std::filesystem::path& path) {
    const auto directory = path.parent_path().empty() ? std::filesystem::path(".")
                                                      : path.parent_path();
    int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        throw std::runtime_error("cannot open metadata directory " + directory.string() +
                                 ": " + strerror(errno));
    if (fsync(fd) != 0) {
        const auto error = errno;
        close(fd);
        throw std::runtime_error("cannot sync metadata directory " + directory.string() +
                                 ": " + strerror(error));
    }
    if (close(fd) != 0)
        throw std::runtime_error("cannot close metadata directory " + directory.string() +
                                 ": " + strerror(errno));
}

std::optional<std::filesystem::path> quarantine_metadata_file(
    const std::filesystem::path& path, const std::string& suffix) {
    if (!std::filesystem::exists(path))
        return {};
    const auto quarantine = std::filesystem::path(path.string() + suffix);
    if (rename(path.c_str(), quarantine.c_str()) != 0)
        throw std::runtime_error("cannot quarantine metadata file " + path.string() +
                                 ": " + strerror(errno));
    sync_directory(path);
    return quarantine;
}
} // namespace
Bytes encode_snapshot(const MetadataSnapshot& s) {
    Writer w;
    // SM11 introduced branch topology/conflicts. SM12 additionally persists
    // the metadata write floor as cluster policy. SM13 adds the durable
    // branch-capable participant roster and causal GC/branch floor. Legacy
    // snapshots retain their exact historical encodings until the first
    // protocol-20 policy transition.
    const bool branch_state = !s.merge_parents.empty() || !s.conflicts.empty();
    const bool policy_state = s.metadata_write_replicas_required != 0;
    const bool governance_state = !s.metadata_participants.empty() ||
                                  s.metadata_branch_floor != Hash256{} ||
                                  s.retention_baseline_complete;
    const bool include_node_status = policy_state || branch_state || !s.node_status.empty() || !s.identity_resets.empty();
    if (governance_state)
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
    return w.take();
}
MetadataSnapshot decode_snapshot(std::span<const uint8_t> d) {
    Reader r(d);
    auto m = r.raw(8);
    const bool v5 = std::equal(m.begin(), m.end(), SM5.begin());
    const bool v6 = std::equal(m.begin(), m.end(), SM6.begin());
    const bool v7 = std::equal(m.begin(), m.end(), SM7.begin());
    const bool v8 = std::equal(m.begin(), m.end(), SM8.begin());
    const bool v9 = std::equal(m.begin(), m.end(), SM9.begin());
    const bool v10 = std::equal(m.begin(), m.end(), SM10.begin());
    const bool v11 = std::equal(m.begin(), m.end(), SM11.begin());
    const bool v12 = std::equal(m.begin(), m.end(), SM12.begin());
    const bool v13 = std::equal(m.begin(), m.end(), SM13.begin());
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
        if (!s.metadata_write_replicas_required)
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
    r.finish();
    auto x = s.entries.find("/");
    if (x == s.entries.end() || x->second.type != EntryType::directory)
        throw DecodeError("missing root");
    return s;
}

Bytes encode_metadata_delta(const MetadataDelta& delta) {
    // DLT1-DLT4 are historical replay formats whose successor hashes are tied
    // to the snapshot encoder current when they were written. Protocol 20 must
    // never choose one of those formats merely because a mutation happens not
    // to touch a later snapshot field: doing so can reconstruct an SM8/SM9
    // successor and silently drop write-floor/governance state. DLT5 always
    // reconstructs with the current canonical snapshot encoder.
    static constexpr std::array<uint8_t, 8> magic_v5{'D', 'H', 'T', 'M', 'D', 'L', 'T', '5'};
    Writer w;
    w.raw(magic_v5);
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
    return w.take();
}

MetadataDelta decode_metadata_delta(std::span<const uint8_t> data) {
    static constexpr std::array<uint8_t, 8> magic_v1{'D', 'H', 'T', 'M', 'D', 'L', 'T', '1'};
    static constexpr std::array<uint8_t, 8> magic_v2{'D', 'H', 'T', 'M', 'D', 'L', 'T', '2'};
    static constexpr std::array<uint8_t, 8> magic_v3{'D', 'H', 'T', 'M', 'D', 'L', 'T', '3'};
    static constexpr std::array<uint8_t, 8> magic_v4{'D', 'H', 'T', 'M', 'D', 'L', 'T', '4'};
    static constexpr std::array<uint8_t, 8> magic_v5{'D', 'H', 'T', 'M', 'D', 'L', 'T', '5'};
    Reader r(data);
    auto got = r.raw(magic_v1.size());
    const bool v1 = std::equal(got.begin(), got.end(), magic_v1.begin());
    const bool v2 = std::equal(got.begin(), got.end(), magic_v2.begin());
    const bool v3 = std::equal(got.begin(), got.end(), magic_v3.begin());
    const bool v4 = std::equal(got.begin(), got.end(), magic_v4.begin());
    const bool v5 = std::equal(got.begin(), got.end(), magic_v5.begin());
    if (!v1 && !v2 && !v3 && !v4 && !v5)
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
        // DLT1 is retained only for replaying metadata journals written by
        // legacy nodes. New network mutations are always DLT3.
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
    if (v3 || v4 || v5) {
        const auto count = r.u32();
        if (count > 65536)
            throw DecodeError("too many metadata delta node status records");
        for (uint32_t i = 0; i < count; ++i) {
            NodeId node{r.fixed<16>()};
            if (!delta.upsert_node_status.emplace(node, decode_node_status(r)).second)
                throw DecodeError("duplicate metadata delta node status");
        }
    }
    if (v4 || v5) {
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
        before.retention_baseline_complete != after.retention_baseline_complete ||
        before.merge_parents != after.merge_parents ||
        before.conflicts != after.conflicts)
        return {};

    MetadataDelta delta;
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

    for (const auto& [path, value] : before.entries) {
        auto it = after.entries.find(path);
        if (it == after.entries.end())
            delta.erase_entries.push_back(path);
    }
    for (const auto& [path, value] : after.entries) {
        auto it = before.entries.find(path);
        if (it == before.entries.end() || it->second != value)
            delta.upsert_entries.emplace(path, value);
    }

    if (before.catalogue_root != after.catalogue_root) {
        if (after.catalogue_root) {
            delta.catalogue = CatalogueDelta::set;
            delta.catalogue_root = after.catalogue_root;
        } else {
            delta.catalogue = CatalogueDelta::clear;
        }
    }

    // Garbage can contain millions of tombstones on a long-lived media node.
    // Do not materialise two std::map copies merely to diff them: tree-node
    // overhead alone can consume gigabytes. Sort compact pointer indexes and
    // merge them instead. The snapshots remain immutable throughout the diff.
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
    // Once a snapshot has crossed into SM9, every delta successor must remain
    // SM9. An unchanged witness makes the wire version explicit without adding
    // another format flag to MetadataDelta.
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

    return delta;
}

void apply_metadata_delta_in_place(MetadataSnapshot& out, const MetadataDelta& delta) {
    for (const auto& [node, sequence] : delta.mutation_sequences) {
        auto it = out.mutation_sequences.find(node);
        if (it != out.mutation_sequences.end() && sequence < it->second)
            throw DecodeError("metadata delta sequence regressed");
        out.mutation_sequences[node] = sequence;
    }
    for (const auto& path : delta.erase_entries) {
        auto normalized = normalize_path(path);
        if (normalized == "/")
            throw DecodeError("metadata delta removed root");
        out.entries.erase(normalized);
    }
    for (const auto& [path, value] : delta.upsert_entries)
        out.entries[normalize_path(path)] = value;
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
    for (const auto& id : delta.erase_garbage) {
        std::erase_if(out.garbage, [&](const GarbageRef& garbage) { return garbage.id == id; });
    }
    for (const auto& garbage : delta.upsert_garbage) {
        auto it = std::find_if(out.garbage.begin(), out.garbage.end(),
                               [&](const GarbageRef& value) { return value.id == garbage.id; });
        if (it == out.garbage.end())
            out.garbage.push_back(garbage);
        else
            *it = garbage;
    }
    for (const auto& [node, status] : delta.upsert_node_status)
        out.node_status[node] = status;
    for (const auto& [key, reset] : delta.upsert_identity_resets) {
        auto found = out.identity_resets.find(key);
        if (found == out.identity_resets.end() || found->second.epoch < reset.epoch)
            out.identity_resets[key] = reset;
    }
    auto root = out.entries.find("/");
    if (root == out.entries.end() || root->second.type != EntryType::directory)
        throw DecodeError("metadata delta lost root");
}

MetadataSnapshot apply_metadata_delta(const MetadataSnapshot& before, const MetadataDelta& delta) {
    MetadataSnapshot out = before;
    apply_metadata_delta_in_place(out, delta);
    return out;
}

Hash256 metadata_hash(uint64_t g, const Hash256& p, std::span<const uint8_t> d) {
    // Preserve the exact canonical encoding without allocating a second copy of
    // the (potentially hundreds-of-megabytes) snapshot payload merely to hash it.
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
        std::adjacent_find(value.replicas.begin(), value.replicas.end()) !=
            value.replicas.end())
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

MetadataMergeResult merge_metadata_snapshots(const MetadataSnapshot& base,
                                             const MetadataSnapshot& left,
                                             const MetadataSnapshot& right,
                                             const Hash256& left_head,
                                             const Hash256& right_head) {
    if (left_head == right_head)
        return {left, 0};
    if (right_head < left_head)
        return merge_metadata_snapshots(base, right, left, right_head, left_head);

    MetadataMergeResult result;
    auto& out = result.snapshot;
    out.metadata_voters.clear();

    auto scalar_merge = [](auto base_value, auto left_value, auto right_value,
                           std::string_view name) {
        if (left_value == right_value)
            return left_value;
        if (left_value == base_value)
            return right_value;
        if (right_value == base_value)
            return left_value;
        throw std::runtime_error("metadata cluster policy diverged: " + std::string(name));
    };
    out.data_replication =
        scalar_merge(base.data_replication, left.data_replication, right.data_replication,
                     "data_replication");
    out.extent_size = scalar_merge(base.extent_size, left.extent_size, right.extent_size,
                                   "extent_size");
    out.metadata_write_replicas_required = scalar_merge(
        base.metadata_write_replicas_required, left.metadata_write_replicas_required,
        right.metadata_write_replicas_required, "metadata_write_replicas_required");
    // Participation is monotonic until an explicit branch-retirement protocol
    // says otherwise. A merge must therefore preserve every branch-capable node.
    // Participant rosters are migration bookkeeping only, never branch authority.
    out.metadata_participants = base.metadata_participants;
    out.metadata_participants.insert(left.metadata_participants.begin(), left.metadata_participants.end());
    out.metadata_participants.insert(right.metadata_participants.begin(), right.metadata_participants.end());
    // A branch floor is safe only when both branches independently carry the
    // same advancement. If they differ, fall back to the common-ancestor floor;
    // maintenance will advance it again after all merged participants converge.
    out.metadata_branch_floor = {};
    out.retention_baseline_complete = left.retention_baseline_complete &&
                                      right.retention_baseline_complete;
    if (out.retention_baseline_complete)
        out.metadata_participants.clear();

    // Per-origin sequences are monotonic idempotency clocks; max is the join.
    out.mutation_sequences = base.mutation_sequences;
    for (const auto* source : {&left.mutation_sequences, &right.mutation_sequences}) {
        for (const auto& [node, sequence] : *source) {
            auto& current = out.mutation_sequences[node];
            current = std::max(current, sequence);
        }
    }

    // Conflict presence is itself three-way metadata. If one branch explicitly
    // resolved a base conflict while the other branch left it untouched, honour
    // the resolution; a later unrelated branch merge must never resurrect it.
    std::set<std::string> conflict_ids;
    for (const auto* source : {&base.conflicts, &left.conflicts, &right.conflicts})
        for (const auto& [id, _] : *source)
            conflict_ids.insert(id);
    auto find_conflict = [](const auto& conflicts, const std::string& id)
        -> std::optional<MetadataConflict> {
        auto found = conflicts.find(id);
        if (found == conflicts.end())
            return {};
        return found->second;
    };
    for (const auto& id : conflict_ids) {
        const auto b = find_conflict(base.conflicts, id);
        const auto l = find_conflict(left.conflicts, id);
        const auto r = find_conflict(right.conflicts, id);
        std::optional<MetadataConflict> merged;
        if (l == r)
            merged = l;
        else if (l == b)
            merged = r;
        else if (r == b)
            merged = l;
        else
            throw std::runtime_error("metadata conflict state diverged for id " + id);
        if (merged)
            out.conflicts.emplace(id, std::move(*merged));
    }

    auto find_entry = [](const std::map<std::string, FsEntry>& entries,
                         const std::string& path) -> std::optional<FsEntry> {
        auto it = entries.find(path);
        if (it == entries.end())
            return {};
        return it->second;
    };
    auto install_entry = [&](const std::string& path, const std::optional<FsEntry>& value) {
        if (value)
            out.entries[path] = *value;
        else
            out.entries.erase(path);
    };
    auto add_namespace_conflict = [&](const std::string& path,
                                      const std::optional<FsEntry>& b,
                                      const std::optional<FsEntry>& l,
                                      const std::optional<FsEntry>& r) {
        MetadataConflict conflict;
        conflict.kind = MetadataConflictKind::namespace_entry;
        conflict.key = path;
        conflict.left_head = left_head;
        conflict.right_head = right_head;
        conflict.base_entry = b;
        conflict.left_entry = l;
        conflict.right_entry = r;
        const auto id = metadata_conflict_id(conflict);
        if (out.conflicts.emplace(id, std::move(conflict)).second)
            ++result.conflicts_created;
    };

    std::set<std::string> paths;
    for (const auto* entries : {&base.entries, &left.entries, &right.entries})
        for (const auto& [path, _] : *entries)
            paths.insert(path);

    // Path-wise three-way merge is insufficient for rename semantics: two
    // branches can remove the same source and create different destinations,
    // which would otherwise look like compatible independent creates. Detect
    // exact-entry moves across paths and force the source plus candidate
    // destinations into the conflict set when the move/delete intent diverges.
    std::set<std::string> forced_conflict_paths;
    auto moved_destinations = [&](const MetadataSnapshot& branch, const std::string& source,
                                  const FsEntry& entry) {
        std::set<std::string> destinations;
        for (const auto& [path, candidate] : branch.entries) {
            if (path == source || candidate != entry)
                continue;
            auto base_candidate = base.entries.find(path);
            if (base_candidate == base.entries.end() || base_candidate->second != entry)
                destinations.insert(path);
        }
        return destinations;
    };
    for (const auto& [source, base_entry] : base.entries) {
        if (source == "/")
            continue;
        const auto l = find_entry(left.entries, source);
        const auto r = find_entry(right.entries, source);
        const auto ld = moved_destinations(left, source, base_entry);
        const auto rd = moved_destinations(right, source, base_entry);
        const bool left_removed = !l;
        const bool right_removed = !r;
        bool semantic_collision = false;
        if (left_removed && right_removed) {
            semantic_collision = ld != rd && (!ld.empty() || !rd.empty());
        } else if (left_removed && r != std::optional<FsEntry>(base_entry)) {
            semantic_collision = true;
        } else if (right_removed && l != std::optional<FsEntry>(base_entry)) {
            semantic_collision = true;
        }
        if (!semantic_collision)
            continue;
        forced_conflict_paths.insert(source);
        forced_conflict_paths.insert(ld.begin(), ld.end());
        forced_conflict_paths.insert(rd.begin(), rd.end());
    }

    for (const auto& path : paths) {
        const auto b = find_entry(base.entries, path);
        const auto l = find_entry(left.entries, path);
        const auto r = find_entry(right.entries, path);
        if (forced_conflict_paths.contains(path)) {
            install_entry(path, b);
            add_namespace_conflict(path, b, l, r);
            continue;
        }
        if (l == r) {
            install_entry(path, l);
            continue;
        }
        if (l == b) {
            install_entry(path, r);
            continue;
        }
        if (r == b) {
            install_entry(path, l);
            continue;
        }

        // Independently creating the same directory is semantically compatible
        // even though wall-clock ctime/mtime differ. Use the deterministic lesser
        // representation so every reconciler produces the same commit hash.
        if (!b && l && r && l->type == EntryType::directory && r->type == EntryType::directory &&
            l->mode == r->mode && l->uid == r->uid && l->gid == r->gid &&
            l->size == r->size && l->extents == r->extents) {
            install_entry(path, (*l < *r) ? l : r);
            continue;
        }

        // Keep the common-ancestor value visible until explicit resolution.
        install_entry(path, b);
        add_namespace_conflict(path, b, l, r);
    }

    // A conflict on a newly-created parent may otherwise leave an auto-merged
    // descendant without a directory. Preserve the ancestor view for any such
    // path and record the descendant alternatives as conflicts too.
    for (const auto& path : paths) {
        if (path == "/")
            continue;
        auto it = out.entries.find(path);
        if (it == out.entries.end())
            continue;
        const auto parent = parent_path(path);
        auto parent_it = out.entries.find(parent);
        if (parent_it != out.entries.end() && parent_it->second.type == EntryType::directory)
            continue;
        const auto b = find_entry(base.entries, path);
        const auto l = find_entry(left.entries, path);
        const auto r = find_entry(right.entries, path);
        install_entry(path, b);
        if (l != r || l != b)
            add_namespace_conflict(path, b, l, r);
    }

    if (!out.entries.contains("/") || out.entries.at("/").type != EntryType::directory)
        throw std::runtime_error("metadata reconciliation lost filesystem root");

    // Catalogue roots are immutable. If only one branch changed the root, take
    // it. If both changed differently, retain the common-ancestor catalogue and
    // persist both alternatives as a first-class conflict. Catalogue-object
    // semantic merging can subsequently resolve this without re-querying a provider.
    if (left.catalogue_root == right.catalogue_root) {
        out.catalogue_root = left.catalogue_root;
    } else if (left.catalogue_root == base.catalogue_root) {
        out.catalogue_root = right.catalogue_root;
    } else if (right.catalogue_root == base.catalogue_root) {
        out.catalogue_root = left.catalogue_root;
    } else {
        out.catalogue_root = base.catalogue_root;
        MetadataConflict conflict;
        conflict.kind = MetadataConflictKind::catalogue_root;
        conflict.key = "catalogue_root";
        conflict.left_head = left_head;
        conflict.right_head = right_head;
        conflict.base_catalogue_root = base.catalogue_root;
        conflict.left_catalogue_root = left.catalogue_root;
        conflict.right_catalogue_root = right.catalogue_root;
        const auto id = metadata_conflict_id(conflict);
        if (out.conflicts.emplace(id, std::move(conflict)).second)
            ++result.conflicts_created;
    }

    // Garbage is maintenance state, not user namespace state. Union branch
    // retirements conservatively; keeping an extra tombstone is safe and lets a
    // later branch-aware reachability pass decide when physical deletion is valid.
    std::map<ObjectId, GarbageRef> garbage;
    for (const auto* source : {&left.garbage, &right.garbage}) {
        for (const auto& value : *source) {
            auto found = garbage.find(value.id);
            if (found == garbage.end() ||
                std::tie(found->second.retired_at_ns, found->second.retirement_id) <
                    std::tie(value.retired_at_ns, value.retirement_id))
                garbage[value.id] = value;
        }
    }
    out.garbage.reserve(garbage.size());
    for (const auto& [_, value] : garbage)
        out.garbage.push_back(value);

    // Persisted node observations and identity resets are monotonic operational
    // metadata and have deterministic joins.
    out.node_status = base.node_status;
    for (const auto* source : {&left.node_status, &right.node_status}) {
        for (const auto& [node, status] : *source) {
            auto found = out.node_status.find(node);
            if (found == out.node_status.end() ||
                found->second.observed_unix_ms < status.observed_unix_ms ||
                (found->second.observed_unix_ms == status.observed_unix_ms &&
                 found->second < status))
                out.node_status[node] = status;
        }
    }

    out.identity_resets = base.identity_resets;
    for (const auto* source : {&left.identity_resets, &right.identity_resets}) {
        for (const auto& [key, reset] : *source) {
            auto found = out.identity_resets.find(key);
            if (found == out.identity_resets.end() || found->second.epoch < reset.epoch ||
                (found->second.epoch == reset.epoch && found->second < reset))
                out.identity_resets[key] = reset;
        }
    }

    out.merge_parents.clear();
    return result;
}
MetadataReplica::MetadataReplica(std::filesystem::path r, std::array<uint8_t, 32> k,
                                 std::optional<MetadataRecord> recovery_seed)
    : p_(r / "metadata" / "current.meta"),
      committed_p_(r / "metadata" / "committed.meta"),
      checkpoint_p_(r / "metadata" / "checkpoint.meta"),
      journal_p_(r / "metadata" / "journal.log"),
      history_p_(r / "metadata" / "history.log"),
      heads_p_(r / "metadata" / "heads.meta"),
      mutation_sequence_p_(r / "metadata" / "mutation-sequence.meta"),
      recovery_p_(r / "metadata" / "recovery.required"), key_(k) {
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

    // If a previous startup already entered recovery, never silently promote
    // its fallback checkpoint to authoritative merely because this restart can
    // decrypt it. It remains read-only/stale until replica checkpointing clears
    // the durable recovery marker.
    if (std::filesystem::exists(recovery_p_)) {
        recovery_required_ = true;
        try {
            if (auto checkpoint = load(checkpoint_p_)) {
                cur_ = *checkpoint;
                committed_ = *checkpoint;
                load_history();
                ensure_history_root(committed_);
                load_journal();
                pending_recovered_ = cur_.hash != committed_.hash;
                ensure_history_root(committed_);
                load_heads();
                // A checkpoint loaded while recovery.required exists may have
                // originated from the persistent metadata cache. Never manufacture
                // legacy acceptance for it. Only heads.meta carried from a later
                // successful peer recovery may provide authority here.
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
                                     "cache seed is available at " + recovery_p_.string());
        recover_from_seed(*seed, "continuing previously marked metadata recovery");
        return;
    }

    try {
        if (auto checkpoint = load(checkpoint_p_)) {
            cur_ = *checkpoint;
            committed_ = *checkpoint;
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

        // 0.8.x migration. The old format kept a fully materialised current and
        // committed snapshot. Preserve the committed record as the journal base and
        // represent a newer accepted-but-not-committed current record as a trusted
        // full seed entry. After the new files are durable, rename the old files so
        // accidentally starting a pre-0.9 binary fails instead of silently rolling
        // the namespace backwards.
        auto current = load(p_);
        auto committed = load(committed_p_);
        if (current.has_value() != committed.has_value())
            throw std::runtime_error(
                "incomplete metadata state: current.meta and committed.meta are both required");

        if (!current) {
            cur_ = genesis_metadata();
            committed_ = cur_;
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
        committed_ = *committed;
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

    std::vector<std::filesystem::path> quarantined;
    for (const auto& path : {checkpoint_p_, journal_p_, history_p_, heads_p_, p_, committed_p_}) {
        if (auto moved = quarantine_metadata_file(path, stamp))
            quarantined.push_back(*moved);
    }

    cur_ = seed;
    committed_ = seed;
    reset_checkpoint(seed);
    load_history();
    ensure_history_root(seed);
    load_heads();
    // The cache seed is deliberately *not* accepted. It is useful material for
    // read-only diagnosis/reconstruction, but cannot stand in for the durable
    // acceptance evidence which was lost with the primary metadata state.
    refresh_materialized_head_locked();
    recovery_required_ = true;

    std::string preserved;
    for (const auto& path : quarantined) {
        if (!preserved.empty())
            preserved += ",";
        preserved += path.string();
    }
    Log::error("metadata primary state failed authentication/validation; using persistent cache "
               "seed pending replica recovery generation=" + std::to_string(seed.generation) +
               " state=" + checkpoint_p_.parent_path().string() +
               " preserved=" + preserved + " reason=" + reason);
}

MetadataRecord MetadataReplica::current() const {
    std::lock_guard g(m_);
    return cur_;
}

MetadataRecord MetadataReplica::committed() const {
    std::lock_guard g(m_);
    return committed_;
}

MetadataIdentity MetadataReplica::current_identity() const {
    std::lock_guard g(m_);
    return {cur_.generation, cur_.hash};
}

MetadataIdentity MetadataReplica::committed_identity() const {
    std::lock_guard g(m_);
    return {committed_.generation, committed_.hash};
}

uint64_t MetadataReplica::generation() const {
    std::lock_guard g(m_);
    return cur_.generation;
}

uint64_t MetadataReplica::committed_generation() const {
    std::lock_guard g(m_);
    return committed_.generation;
}

bool MetadataReplica::recovery_required() const {
    std::lock_guard g(m_);
    return recovery_required_;
}

void MetadataReplica::mark_recovered() {
    std::lock_guard g(m_);
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

uint64_t MetadataReplica::reserve_mutation_sequence(uint64_t observed_floor) {
    std::lock_guard lock(m_);
    if (!mutation_sequence_loaded_) {
        mutation_sequence_ = 0;
        if (std::filesystem::exists(mutation_sequence_p_)) {
            try {
                std::ifstream stream(mutation_sequence_p_, std::ios::binary);
                if (!stream)
                    throw std::runtime_error("cannot open");
                Bytes bytes(std::istreambuf_iterator<char>(stream), {});
                Reader reader(bytes);
                auto magic = reader.raw(MS.size());
                if (!std::equal(magic.begin(), magic.end(), MS.begin()))
                    throw std::runtime_error("bad metadata mutation-sequence file header");
                auto nonce = reader.fixed<12>();
                auto tag = reader.fixed<16>();
                auto ciphertext = reader.bytes();
                reader.finish();
                auto plaintext = aes_gcm_open(key_, nonce, tag, ciphertext, MS);
                Reader plain(plaintext);
                mutation_sequence_ = plain.u64();
                plain.finish();
            } catch (const std::exception& error) {
                throw std::runtime_error("metadata mutation-sequence file " +
                                         mutation_sequence_p_.string() + ": " + error.what());
            }
        }
        mutation_sequence_loaded_ = true;
    }

    const auto floor = std::max(mutation_sequence_, observed_floor);
    if (floor == std::numeric_limits<uint64_t>::max())
        throw std::runtime_error("metadata mutation sequence exhausted");
    const auto next = floor + 1;

    Writer plain;
    plain.u64(next);
    auto sealed = aes_gcm_seal(key_, plain.data(), MS);
    Writer file;
    file.raw(MS);
    file.fixed(sealed.nonce);
    file.fixed(sealed.tag);
    file.bytes(sealed.ciphertext);
    writefile(mutation_sequence_p_, file.data());
    mutation_sequence_ = next;
    return next;
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
    if (fsync(fd)) {
        auto error = errno;
        close(fd);
        throw std::runtime_error("cannot sync metadata journal " + journal_p_.string() + ": " +
                                 strerror(error));
    }
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
                                     " offset=" + std::to_string(offset) +
                                     ": record too large");
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
            throw std::runtime_error("metadata journal " + journal_p_.string() +
                                     " offset=" + std::to_string(offset) +
                                     ": malformed encrypted frame: " + error.what());
        }

        Bytes plaintext;
        try {
            plaintext = aes_gcm_open(key_, nonce, tag, ciphertext, MJ);
        } catch (const std::exception& error) {
            // A journal append writes one complete authenticated frame and then
            // fsyncs it. A host/storage failure can nevertheless leave the file
            // length extended while the final ciphertext/tag is only partially
            // durable. No later frame can depend on an unauthenticated final
            // frame. Preserve those bytes for diagnosis and replay only the
            // authenticated prefix. Authentication failure anywhere except EOF
            // remains fatal: silently skipping a middle frame would break the
            // metadata chain and could roll the namespace backwards.
            if (final_frame && std::string_view(error.what()) ==
                                   "AES-GCM authentication failed") {
                trailing_problem = "final frame failed AES-GCM authentication";
                break;
            }
            throw std::runtime_error("metadata journal " + journal_p_.string() +
                                     " offset=" + std::to_string(offset) + ": " +
                                     error.what());
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

            // Compaction writes the new checkpoint before truncating the old
            // journal. A crash in that small window legitimately leaves journal
            // frames already represented by the checkpoint; ignore only those
            // exact/older generations and replay anything newer.
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
                apply_metadata_delta_in_place(replayed, delta);
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

                // A seed is an explicitly journaled replacement of the current
                // *uncommitted* proposal.  It may roll current back to the
                // durable head, advance from that head, or install a descendant
                // whose ancestry was imported into history before the seed.
                // None of those transitions rewrites committed history.
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
                const bool known_descendant = history_.contains(record.hash) &&
                                              history_is_ancestor_locked(committed_.hash,
                                                                         record.hash);
                const bool parent_descends_from_committed = std::any_of(
                    parents.begin(), parents.end(), [&](const Hash256& parent) {
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
                committed_ = cur_;
                pending_history_.reset();
                break;
            default:
                throw std::runtime_error("unknown record type " + std::to_string(kind));
            }
        } catch (const std::exception& error) {
            throw std::runtime_error("metadata journal " + journal_p_.string() +
                                     " offset=" + std::to_string(offset) + ": " +
                                     error.what());
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
        Log::warn("metadata journal recovered path=" + journal_p_.string() +
                  " offset=" + std::to_string(valid) +
                  " discarded_bytes=" + std::to_string(tail.size()) +
                  " quarantine=" + quarantine.string() +
                  " reason=" + trailing_problem);
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

void MetadataReplica::append_history(const MetadataHistoryEntry& entry_value) {
    if (history_.contains(entry_value.hash))
        return;

    auto bytes = encode_history_frame(entry_value);
    write_history_frame(bytes);
    history_.emplace(entry_value.hash, entry_value);
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
            throw std::runtime_error("cannot write metadata history " + history_p_.string() +
                                     ": " + strerror(error));
        }
        offset += static_cast<size_t>(count);
    }
    if (fsync(fd) != 0) {
        const auto error = errno;
        close(fd);
        throw std::runtime_error("cannot sync metadata history " + history_p_.string() + ": " +
                                 strerror(error));
    }
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

    // Recovery is deliberately streaming: history can span many namespace
    // generations, so startup RSS is bounded by one history frame rather than
    // the lifetime size of history.log.
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

            // Cold-start history loading must be O(history bytes), not O(history^2).
            // Validate each frame locally here; full reconstruction is deferred to
            // accepted/current heads which can actually become authority.
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
                auto snapshot = decode_snapshot(record.payload);
                if (snapshot.merge_parents != entry_value.merge_parents)
                    throw DecodeError("metadata history merge parents mismatch");
            } else if (entry_value.body == MetadataHistoryEntry::Body::delta) {
                if (!entry_value.previous_known || entry_value.generation <= 1)
                    throw DecodeError("metadata delta history has no predecessor");
                auto parent = history_.find(entry_value.previous);
                if (parent == history_.end() || parent->second.generation + 1 != entry_value.generation)
                    throw DecodeError("metadata delta history predecessor missing or non-adjacent");
                (void)decode_metadata_delta(entry_value.payload);
            } else {
                throw DecodeError("unknown metadata history body");
            }

            if (!history_.emplace(entry_value.hash, std::move(entry_value)).second)
                throw DecodeError("duplicate metadata history record");
        } catch (const std::exception& error) {
            if (final_frame && std::string_view(error.what()) == "AES-GCM authentication failed") {
                trailing_problem = "final frame failed AES-GCM authentication";
                break;
            }
            throw std::runtime_error("metadata history " + history_p_.string() +
                                     " offset=" + std::to_string(offset) + ": " + error.what());
        }

        offset += 4 + length;
        valid = offset;
        ++history_records_;
    }

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
        Log::warn("metadata history recovered path=" + history_p_.string() +
                  " offset=" + std::to_string(valid) +
                  " discarded_bytes=" + std::to_string(tail_size) +
                  " quarantine=" + quarantine + " reason=" + trailing_problem);
    }
    history_bytes_ = valid;
}

void MetadataReplica::load_heads() {
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
        auto values = decode_metadata_acceptance_set(aes_gcm_open(key_, nonce, tag, ciphertext, MA));
        for (auto& value : values) {
            auto reconstructed = materialized_locked(value.hash);
            if (!reconstructed || reconstructed->record.generation != value.generation)
                throw std::runtime_error("accepted metadata head is not reconstructible");
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
    writefile(heads_p_, writer.data());
}

bool MetadataReplica::prune_accepted_heads_locked() {
    bool changed = false;
    for (auto it = accepted_heads_.begin(); it != accepted_heads_.end();) {
        bool ancestor = false;
        for (const auto& [other, _] : accepted_heads_) {
            if (other == it->first)
                continue;
            if (history_is_ancestor_locked(it->first, other)) {
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

void MetadataReplica::migrate_legacy_head_locked() {
    ensure_history_root(committed_);
    if (accepted_heads_.contains(committed_.hash))
        return;

    const auto snapshot = decode_snapshot(committed_.payload);
    if (snapshot.metadata_write_replicas_required != 0) {
        // A protocol-20 checkpoint without heads.meta is useful recovery material
        // but is not authority: only an acceptance certificate proves that the
        // commit reached the cluster write floor. Preserve the checkpoint and
        // force peer recovery rather than manufacturing a legacy accepted head.
        recovery_required_ = true;
        durable_replace_file(recovery_p_,
            "protocol-20 metadata checkpoint has no durable acceptance certificate");
        return;
    }

    if (!accepted_heads_.empty()) {
        // New-protocol accepted descendants already subsume the materialised
        // checkpoint. Conversely, a legacy JOURNAL_COMMIT can be newer than an
        // older heads.meta if the process crashed between those two durable
        // writes; in that case preserve the committed linear successor.
        bool committed_is_ancestor = false;
        bool all_heads_are_ancestors = true;
        for (const auto& [head, _] : accepted_heads_) {
            committed_is_ancestor = committed_is_ancestor ||
                                    history_is_ancestor_locked(committed_.hash, head);
            all_heads_are_ancestors = all_heads_are_ancestors &&
                                      history_is_ancestor_locked(head, committed_.hash);
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
    const auto policy_floor = [](const MetadataSnapshot& snapshot) -> uint32_t {
        if (snapshot.metadata_write_replicas_required)
            return snapshot.metadata_write_replicas_required;
        if (!snapshot.metadata_voters.empty())
            return static_cast<uint32_t>(snapshot.metadata_voters.size() / 2 + 1);
        return 0;
    };

    const auto& record = materialized.record;
    const auto& snapshot = *materialized.snapshot;
    if (!snapshot.metadata_write_replicas_required) {
        // required=0 is the durable legacy authority marker. A pre-0.19
        // snapshot may still contain metadata_voters; those voters define the
        // stronger floor required to transition out of legacy authority, but
        // they were never encoded into the legacy acceptance certificate.
        if (acceptance.required != 0 || !acceptance.replicas.empty())
            return false;
        // Once a branch has crossed into protocol 20 it may not manufacture a
        // legacy-authority child and thereby discard the accepted write floor.
        for (const auto& parent_hash : metadata_record_parents(record)) {
            auto parent = materialized_locked(parent_hash);
            if (parent && parent->snapshot->metadata_write_replicas_required)
                return false;
        }
        return true;
    }

    const auto current = policy_floor(snapshot);
    if (!acceptance.required)
        return false;
    for (const auto& witness : acceptance.replicas)
        if (witness == NodeId{})
            return false;

    uint32_t required = current;
    for (const auto& parent_hash : metadata_record_parents(record)) {
        auto parent = materialized_locked(parent_hash);
        if (!parent) {
            // An ordinary same-policy certificate is self-describing enough to
            // retain branch evidence while ancestry is still being imported. A
            // stronger transition certificate, however, cannot be validated
            // without the parent policy which required that stronger floor.
            if (acceptance.required != current)
                return false;
            continue;
        }
        required = std::max(required, policy_floor(*parent->snapshot));
    }
    return acceptance.required == required;
}

bool MetadataReplica::legacy_write_api_allowed_locked() const {
    try {
        return decode_snapshot(committed_.payload).metadata_write_replicas_required == 0;
    } catch (...) {
        return false;
    }
}

void MetadataReplica::set_legacy_committed_head_locked(const MetadataRecord& record) {
    MetadataAcceptance legacy;
    legacy.generation = record.generation;
    legacy.hash = record.hash;
    legacy.required = 0;
    accepted_heads_.clear();
    accepted_heads_.emplace(legacy.hash, legacy);
    persist_heads_locked();
}

void MetadataReplica::refresh_materialized_head_locked() {
    if (accepted_heads_.empty())
        return;
    std::optional<MetadataRecord> selected;
    for (const auto& [hash, _] : accepted_heads_) {
        auto record = historical_locked(hash);
        if (!record)
            throw std::runtime_error("accepted metadata head cannot be reconstructed");
        if (!selected || record->generation > selected->generation ||
            (record->generation == selected->generation && record->hash > selected->hash))
            selected = std::move(record);
    }
    if (!selected || selected->hash == committed_.hash)
        return;

    // `committed_` is now only the locally materialised preferred accepted head
    // used by legacy callers and the fast read cache. Authority lives in the
    // accepted-head set above. Moving this materialisation never deletes another
    // accepted branch from history.
    committed_ = *selected;
    cur_ = committed_;
    pending_history_.reset();
    pending_recovered_ = false;
    reset_checkpoint(committed_);
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
    // An upgraded 0.18 checkpoint has a predecessor hash but not necessarily the
    // predecessor material. Treat the checkpoint as the local 0.19 history root.
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
    const MetadataRecord& record, std::shared_ptr<const MetadataSnapshot> snapshot) const {
    constexpr size_t cache_limit = 64;
    auto found = materialized_history_.find(record.hash);
    if (found != materialized_history_.end()) {
        found->second.last_used = ++materialized_history_clock_;
        return found->second.value;
    }

    while (materialized_history_.size() >= cache_limit) {
        auto victim = materialized_history_.end();
        for (auto it = materialized_history_.begin(); it != materialized_history_.end(); ++it) {
            const bool pinned = it->first == cur_.hash || it->first == committed_.hash ||
                                accepted_heads_.contains(it->first);
            if (pinned)
                continue;
            if (victim == materialized_history_.end() ||
                it->second.last_used < victim->second.last_used)
                victim = it;
        }
        // Accepted/current heads are correctness-relevant hot materializations.
        // A pathological number of concurrent accepted heads may temporarily
        // exceed the soft bound rather than evicting one of those entries.
        if (victim == materialized_history_.end())
            break;
        materialized_history_.erase(victim);
        materialization_cache_evictions_.fetch_add(1, std::memory_order_relaxed);
    }

    if (!snapshot)
        snapshot = std::make_shared<const MetadataSnapshot>(decode_snapshot(record.payload));
    auto value = std::make_shared<const MetadataMaterialization>(
        MetadataMaterialization{record, std::move(snapshot)});
    materialized_history_.emplace(
        record.hash, MaterializedHistoryEntry{value, ++materialized_history_clock_});
    return value;
}

std::shared_ptr<const MetadataMaterialization> MetadataReplica::materialized_locked(
    const Hash256& target) const {
    historical_requests_.fetch_add(1, std::memory_order_relaxed);
    auto found = history_.find(target);
    if (found == history_.end())
        return {};

    if (auto cached = materialized_history_.find(target);
        cached != materialized_history_.end()) {
        cached->second.last_used = ++materialized_history_clock_;
        materialization_cache_hits_.fetch_add(1, std::memory_order_relaxed);
        return cached->second.value;
    }
    materialization_cache_misses_.fetch_add(1, std::memory_order_relaxed);

    historical_reconstructions_.fetch_add(1, std::memory_order_relaxed);

    std::vector<const MetadataHistoryEntry*> deltas;
    std::set<Hash256> seen;
    const MetadataHistoryEntry* cursor = &found->second;
    std::shared_ptr<const MetadataMaterialization> materialized_parent;
    while (cursor->body == MetadataHistoryEntry::Body::delta) {
        if (!seen.insert(cursor->hash).second || !cursor->previous_known)
            return {};
        deltas.push_back(cursor);
        auto parent = history_.find(cursor->previous);
        if (parent == history_.end())
            return {};
        cursor = &parent->second;
        if (auto cached = materialized_history_.find(cursor->hash);
            cached != materialized_history_.end()) {
            cached->second.last_used = ++materialized_history_clock_;
            materialized_parent = cached->second.value;
            break;
        }
    }

    std::shared_ptr<const MetadataMaterialization> materialized;
    if (materialized_parent) {
        materialized = std::move(materialized_parent);
    } else {
        MetadataRecord record;
        record.generation = cursor->generation;
        record.previous = cursor->previous;
        record.hash = cursor->hash;
        record.payload = cursor->payload;
        if (!valid_metadata_record(record))
            return {};
        try {
            auto snapshot = decode_snapshot(record.payload);
            if (snapshot.merge_parents != cursor->merge_parents)
                return {};
            materialized = cache_materialization_locked(
                record, std::make_shared<const MetadataSnapshot>(std::move(snapshot)));
        } catch (...) {
            return {};
        }
    }

    for (auto it = deltas.rbegin(); it != deltas.rend(); ++it) {
        const auto& child = **it;
        const auto& parent = materialized->record;
        if (child.previous != parent.hash || child.generation != parent.generation + 1)
            return {};
        try {
            auto snapshot = *materialized->snapshot;
            const auto delta = decode_metadata_delta(child.payload);
            apply_metadata_delta_in_place(snapshot, delta);
            historical_deltas_applied_.fetch_add(1, std::memory_order_relaxed);
            MetadataRecord next;
            next.generation = child.generation;
            next.previous = child.previous;
            next.hash = child.hash;
            next.payload = encode_snapshot_for_delta(child.payload, snapshot);
            if (!valid_metadata_record(next) || snapshot.merge_parents != child.merge_parents)
                return {};
            materialized = cache_materialization_locked(
                next, std::make_shared<const MetadataSnapshot>(std::move(snapshot)));
        } catch (...) {
            return {};
        }
    }
    return materialized;
}

std::optional<MetadataRecord> MetadataReplica::historical_locked(const Hash256& target) const {
    auto value = materialized_locked(target);
    if (!value)
        return {};
    return value->record;
}

MetadataReplicaDiagnostics MetadataReplica::diagnostics() const {
    std::lock_guard lock(m_);
    return {
        historical_requests_.load(std::memory_order_relaxed),
        historical_reconstructions_.load(std::memory_order_relaxed),
        historical_deltas_applied_.load(std::memory_order_relaxed),
        materialization_cache_hits_.load(std::memory_order_relaxed),
        materialization_cache_misses_.load(std::memory_order_relaxed),
        materialization_cache_evictions_.load(std::memory_order_relaxed),
        materialized_history_.size(),
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
        // A compacted full root retains its authenticated direct-parent hash,
        // even though previous_known=false prevents reconstruction or traversal
        // beyond that boundary.  The direct edge is still valid ancestry.
        if (entry_value.previous != Hash256{} && entry_value.previous == ancestor)
            return true;
        if (entry_value.previous_known)
            pending.push_back(entry_value.previous);
        for (const auto& parent : entry_value.merge_parents) {
            if (parent == ancestor)
                return true;
            pending.push_back(parent);
        }
    }
    return false;
}

std::optional<Hash256> MetadataReplica::history_common_ancestor_locked(
    const Hash256& left, const Hash256& right) const {
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
        if (found->second.previous_known) {
            pending.push_back(found->second.previous);
        } else if (found->second.previous != Hash256{}) {
            // Record the authenticated boundary parent as a possible common
            // ancestor, but do not walk into history deliberately discarded by
            // compaction.
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
        if (found->second.previous_known) {
            pending.push_back(found->second.previous);
        } else if (found->second.previous != Hash256{}) {
            // As above, the boundary parent participates in ancestry matching
            // without becoming a traversal edge.
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
    std::lock_guard lock(m_);
    auto found = history_.find(hash);
    if (found == history_.end())
        return {};
    return found->second;
}

bool MetadataReplica::import_history(const MetadataHistoryEntry& entry_value) {
    if (!entry_value.generation || entry_value.hash == Hash256{} ||
        entry_value.merge_parents.size() > 64)
        return false;
    std::shared_ptr<const MetadataMaterialization> parent;
    if (entry_value.body == MetadataHistoryEntry::Body::delta) {
        if (!entry_value.previous_known)
            return false;
        parent = materialized(entry_value.previous);
        if (!parent)
            return false;
    }

    std::shared_ptr<const MetadataMaterialization> candidate;
    try {
        MetadataRecord record;
        record.generation = entry_value.generation;
        record.previous = entry_value.previous;
        record.hash = entry_value.hash;
        std::shared_ptr<const MetadataSnapshot> snapshot;
        if (entry_value.body == MetadataHistoryEntry::Body::full) {
            record.payload = entry_value.payload;
            auto decoded = decode_snapshot(record.payload);
            snapshot = std::make_shared<const MetadataSnapshot>(std::move(decoded));
        } else {
            auto decoded = *parent->snapshot;
            apply_metadata_delta_in_place(decoded, decode_metadata_delta(entry_value.payload));
            record.payload = encode_snapshot_for_delta(entry_value.payload, decoded);
            snapshot = std::make_shared<const MetadataSnapshot>(std::move(decoded));
        }
        if (!valid_metadata_record(record) || snapshot->merge_parents != entry_value.merge_parents)
            return false;
        candidate = std::make_shared<const MetadataMaterialization>(
            MetadataMaterialization{std::move(record), std::move(snapshot)});
    } catch (...) {
        return false;
    }
    auto frame = encode_history_frame(entry_value);

    std::lock_guard durable(durable_mutation_m_);
    {
        std::lock_guard lock(m_);
        if (history_.contains(entry_value.hash))
            return true;
        if (entry_value.body == MetadataHistoryEntry::Body::delta &&
            !history_.contains(entry_value.previous))
            return false;
    }
    write_history_frame(frame);
    std::lock_guard lock(m_);
    history_.emplace(entry_value.hash, entry_value);
    ++history_records_;
    history_bytes_ += frame.size();
    cache_materialization_locked(candidate->record, candidate->snapshot);
    if (prune_accepted_heads_locked()) {
        persist_heads_locked();
        refresh_materialized_head_locked();
    }
    return true;
}

bool MetadataReplica::history_contains(const Hash256& hash) const {
    std::lock_guard lock(m_);
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
    entry_value.previous_known = record.generation > 1 && record.previous != Hash256{};
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
            reconstructed = std::make_shared<const MetadataMaterialization>(
                MetadataMaterialization{record,
                    std::make_shared<const MetadataSnapshot>(decode_snapshot(record.payload))});
        } else {
            auto parent = materialized(entry_value.previous);
            if (!parent)
                return false;
            auto snapshot = *parent->snapshot;
            apply_metadata_delta_in_place(snapshot, decode_metadata_delta(entry_value.payload));
            MetadataRecord value;
            value.generation = entry_value.generation;
            value.previous = entry_value.previous;
            value.hash = entry_value.hash;
            value.payload = encode_snapshot_for_delta(entry_value.payload, snapshot);
            reconstructed = std::make_shared<const MetadataMaterialization>(
                MetadataMaterialization{std::move(value),
                    std::make_shared<const MetadataSnapshot>(std::move(snapshot))});
        }
    } catch (...) {
        return false;
    }
    if (!reconstructed || reconstructed->record.generation != record.generation ||
        reconstructed->record.previous != record.previous ||
        reconstructed->record.hash != record.hash ||
        reconstructed->record.payload != record.payload)
        return false;
    auto frame = encode_history_frame(entry_value);

    std::lock_guard durable(durable_mutation_m_);
    {
        std::lock_guard lock(m_);
        if (history_.contains(record.hash))
            return true;
        if (entry_value.body == MetadataHistoryEntry::Body::delta &&
            !history_.contains(entry_value.previous))
            return false;
    }
    write_history_frame(frame);
    std::lock_guard lock(m_);
    history_.emplace(entry_value.hash, entry_value);
    ++history_records_;
    history_bytes_ += frame.size();
    cache_materialization_locked(reconstructed->record, reconstructed->snapshot);
    return true;
}

bool MetadataReplica::accept_commit(const MetadataAcceptance& input) {
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

    // Populate the target and policy-parent materializations through the
    // off-lock cache-miss path. The locked validation below then consists of
    // immutable-history checks and cache hits rather than chain reconstruction.
    auto prepared = materialized(value.hash);
    if (!prepared || prepared->record.generation != value.generation)
        return false;
    for (const auto& parent : metadata_record_parents(prepared->record))
        (void)materialized(parent);

    std::lock_guard durable(durable_mutation_m_);
    std::lock_guard lock(m_);
    auto materialized = materialized_locked(value.hash);
    if (!materialized || materialized->record.generation != value.generation)
        return false;
    if (!acceptance_matches_record_policy_locked(value, *materialized))
        return false;

    bool changed = false;
    // An accepted ancestor remains valid evidence, but it is no longer a head.
    // If it was already present before ancestry arrived, remove it now instead
    // of returning early and leaving a non-maximal accepted head behind.
    bool incoming_is_ancestor = false;
    for (const auto& [head, _] : accepted_heads_) {
        if (head != value.hash && history_is_ancestor_locked(value.hash, head)) {
            incoming_is_ancestor = true;
            break;
        }
    }
    if (incoming_is_ancestor) {
        if (accepted_heads_.erase(value.hash)) {
            persist_heads_locked();
            refresh_materialized_head_locked();
        }
        return true;
    }

    for (auto it = accepted_heads_.begin(); it != accepted_heads_.end();) {
        if (it->first != value.hash && history_is_ancestor_locked(it->first, value.hash)) {
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
            merged.replicas.erase(std::unique(merged.replicas.begin(), merged.replicas.end()),
                                  merged.replicas.end());
        }
        if (merged != found->second) {
            found->second = std::move(merged);
            changed = true;
        }
    }

    changed = prune_accepted_heads_locked() || changed;
    if (changed) {
        persist_heads_locked();
        refresh_materialized_head_locked();
    }
    return true;
}

std::vector<MetadataAcceptance> MetadataReplica::accepted_head_certificates() const {
    std::lock_guard lock(m_);
    std::vector<MetadataAcceptance> out;
    out.reserve(accepted_heads_.size());
    for (const auto& [_, value] : accepted_heads_)
        out.push_back(value);
    return out;
}

std::vector<MetadataRecord> MetadataReplica::accepted_heads() const {
    std::vector<Hash256> hashes;
    {
        std::lock_guard lock(m_);
        hashes.reserve(accepted_heads_.size());
        for (const auto& [hash, _] : accepted_heads_)
            hashes.push_back(hash);
    }
    std::vector<MetadataRecord> out;
    out.reserve(hashes.size());
    for (const auto& hash : hashes) {
        auto value = materialized(hash);
        if (!value)
            throw std::runtime_error("accepted metadata head cannot be reconstructed");
        out.push_back(value->record);
    }
    return out;
}

std::optional<MetadataAcceptance> MetadataReplica::acceptance(const Hash256& hash) const {
    std::lock_guard lock(m_);
    auto found = accepted_heads_.find(hash);
    if (found == accepted_heads_.end())
        return {};
    return found->second;
}

bool MetadataReplica::history_is_ancestor(const Hash256& ancestor,
                                          const Hash256& descendant) const {
    std::lock_guard lock(m_);
    return history_is_ancestor_locked(ancestor, descendant);
}

std::optional<Hash256> MetadataReplica::history_common_ancestor(const Hash256& left,
                                                                const Hash256& right) const {
    std::lock_guard lock(m_);
    return history_common_ancestor_locked(left, right);
}

std::optional<MetadataRecord> MetadataReplica::historical(const Hash256& hash) const {
    auto value = materialized(hash);
    if (!value)
        return {};
    return value->record;
}

std::shared_ptr<const MetadataMaterialization> MetadataReplica::materialized(
    const Hash256& hash) const {
    historical_requests_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard computation(materialization_compute_m_);

    std::vector<MetadataHistoryEntry> deltas;
    MetadataHistoryEntry anchor;
    std::shared_ptr<const MetadataMaterialization> base;
    {
        std::lock_guard lock(m_);
        auto found = history_.find(hash);
        if (found == history_.end())
            return {};
        if (auto cached = materialized_history_.find(hash);
            cached != materialized_history_.end()) {
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
            deltas.push_back(cursor->second);
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
            anchor = cursor->second;
    }

    // Snapshot decoding, delta application, encoding, and hashing can dominate
    // recovery time. They deliberately run without the global replica mutex.
    std::vector<std::shared_ptr<const MetadataMaterialization>> calculated;
    std::shared_ptr<const MetadataMaterialization> value = std::move(base);
    try {
        if (!value) {
            MetadataRecord record;
            record.generation = anchor.generation;
            record.previous = anchor.previous;
            record.hash = anchor.hash;
            record.payload = anchor.payload;
            if (!valid_metadata_record(record))
                return {};
            auto snapshot = decode_snapshot(record.payload);
            if (snapshot.merge_parents != anchor.merge_parents)
                return {};
            value = std::make_shared<const MetadataMaterialization>(
                MetadataMaterialization{record,
                    std::make_shared<const MetadataSnapshot>(std::move(snapshot))});
            calculated.push_back(value);
        }

        for (auto it = deltas.rbegin(); it != deltas.rend(); ++it) {
            const auto& child = *it;
            if (child.previous != value->record.hash ||
                child.generation != value->record.generation + 1)
                return {};
            auto snapshot = *value->snapshot;
            const auto delta = decode_metadata_delta(child.payload);
            apply_metadata_delta_in_place(snapshot, delta);
            historical_deltas_applied_.fetch_add(1, std::memory_order_relaxed);
            MetadataRecord record;
            record.generation = child.generation;
            record.previous = child.previous;
            record.hash = child.hash;
            record.payload = encode_snapshot_for_delta(child.payload, snapshot);
            if (!valid_metadata_record(record) || snapshot.merge_parents != child.merge_parents)
                return {};
            value = std::make_shared<const MetadataMaterialization>(
                MetadataMaterialization{std::move(record),
                    std::make_shared<const MetadataSnapshot>(std::move(snapshot))});
            calculated.push_back(value);
        }
    } catch (...) {
        return {};
    }

    std::lock_guard lock(m_);
    // History is append-only except for explicit compaction. Install only values
    // whose immutable source entry still exists after the off-lock calculation.
    for (const auto& item : calculated) {
        if (history_.contains(item->record.hash))
            value = cache_materialization_locked(item->record, item->snapshot);
    }
    if (auto cached = materialized_history_.find(hash);
        cached != materialized_history_.end())
        return cached->second.value;
    return value && value->record.hash == hash ? value : std::shared_ptr<const MetadataMaterialization>{};
}

void MetadataReplica::reset_checkpoint(const MetadataRecord& record) {
    persist(checkpoint_p_, record);
    writefile(journal_p_, {});
    journal_records_ = 0;
    journal_bytes_ = 0;
}

void MetadataReplica::compact_if_needed() {
    constexpr size_t record_threshold = 128;
    constexpr uint64_t byte_threshold = 8ULL * 1024 * 1024;
    if (cur_.generation != committed_.generation || cur_.hash != committed_.hash)
        return;
    if (journal_records_ < record_threshold && journal_bytes_ < byte_threshold)
        return;
    const auto records = journal_records_;
    const auto bytes = journal_bytes_;
    const auto generation = committed_.generation;
    const auto snapshot_bytes = committed_.payload.size();
    reset_checkpoint(committed_);
    Log::debug("metadata journal compacted generation=" + std::to_string(generation) +
               " records=" + std::to_string(records) +
               " journal_bytes=" + std::to_string(bytes) +
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

    std::lock_guard durable(durable_mutation_m_);
    std::lock_guard lock(m_);
    if (!legacy_write_api_allowed_locked())
        return false;
    if (cur_.generation != generation || cur_.hash != hash) {
        if (committed_.generation != generation || committed_.hash != hash) {
            if (out)
                *out = cur_;
            return false;
        }

        // Multiple coordinators may legitimately race from the same accepted
        // head. A PREPARE is not authoritative yet, but allowing every caller to
        // overwrite it creates a symmetric swap: each coordinator can collect a
        // write-floor of acknowledgements for a proposal that no longer exists by
        // COMMIT time. Keep the lexicographically lowest direct-child proposal as
        // the deterministic contender. Other coordinators can help that contender
        // commit and then retry their own mutation on top of it.
        const bool competing_direct_child =
            cur_.generation == generation + 1 && cur_.previous == hash &&
            cur_.hash != committed_.hash;
        if (competing_direct_child && pending_recovered_) {
            // PREPARE state recovered after process restart has no live coordinator
            // and therefore no authority.  A later proposal based on the durable
            // committed head may replace it regardless of contender hash.
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

        // The new proposal wins deterministic contention. Journal the rollback
        // first so crash replay sees the same PREPARE replacement sequence.
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
    std::lock_guard durable(durable_mutation_m_);
    std::lock_guard lock(m_);
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
    apply_metadata_delta_in_place(after, delta);

    MetadataRecord next;
    next.generation = generation + 1;
    next.previous = hash;
    next.payload = encode_snapshot_for_delta(encoded_delta, after);
    next.hash = metadata_hash(next.generation, next.previous, next.payload);

    if (cur_.generation != generation || cur_.hash != hash) {
        const bool competing_direct_child =
            cur_.generation == generation + 1 && cur_.previous == hash &&
            cur_.hash != committed_.hash;
        if (competing_direct_child && pending_recovered_) {
            // PREPARE state recovered after process restart has no live coordinator
            // and therefore no authority.  A later proposal based on the durable
            // committed head may replace it regardless of contender hash.
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
    std::lock_guard durable(durable_mutation_m_);
    std::lock_guard lock(m_);
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
        committed_ = cur_;
        pending_history_.reset();
        set_legacy_committed_head_locked(committed_);
        return true;
    }
    if (cur_.generation != generation || cur_.hash != hash)
        return false;

    auto after = decode_snapshot(cur_.payload);
    auto delta = decode_metadata_delta(encoded_delta);
    apply_metadata_delta_in_place(after, delta);
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
    committed_ = cur_;
    pending_history_.reset();
    set_legacy_committed_head_locked(committed_);
    return true;
}

bool MetadataReplica::seed(const MetadataRecord& record) {
    if (!valid_metadata_record(record))
        return false;
    const auto parents = metadata_record_parents(record);
    std::lock_guard durable(durable_mutation_m_);
    std::lock_guard lock(m_);
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
    const bool known_descendant = history_.contains(record.hash) &&
                                  history_is_ancestor_locked(committed_.hash, record.hash);
    const bool parent_descends_from_committed = std::any_of(
        parents.begin(), parents.end(), [&](const Hash256& parent) {
            return history_.contains(parent) &&
                   history_is_ancestor_locked(committed_.hash, parent);
        });
    if (!fresh && !direct_parent && !known_descendant && !parent_descends_from_committed)
        return false;

    // This is a prepare, not a commit. Replacing an uncommitted local proposal
    // is safe; the previously committed branch remains durable in history and a
    // reconciliation record explicitly names that branch as a parent.
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
    std::lock_guard durable(durable_mutation_m_);
    std::lock_guard lock(m_);
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
            parents.begin(), parents.end(), [&](const Hash256& parent) {
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
    committed_ = cur_;
    pending_history_.reset();
    pending_recovered_ = false;
    set_legacy_committed_head_locked(committed_);
    return true;
}

bool MetadataReplica::remember_current_committed(uint64_t generation, const Hash256& hash) {
    std::lock_guard durable(durable_mutation_m_);
    std::lock_guard lock(m_);
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
    committed_ = cur_;
    pending_history_.reset();
    pending_recovered_ = false;
    set_legacy_committed_head_locked(committed_);
    return true;
}

void MetadataReplica::compact() {
    std::lock_guard durable(durable_mutation_m_);
    std::lock_guard lock(m_);
    compact_if_needed();
}

bool MetadataReplica::compact_history_if_safe(size_t record_threshold,
                                              uint64_t byte_threshold) {
    std::lock_guard durable(durable_mutation_m_);
    std::lock_guard lock(m_);
    if ((history_records_ < record_threshold && history_bytes_ < byte_threshold) ||
        cur_.generation != committed_.generation || cur_.hash != committed_.hash ||
        pending_history_ || accepted_heads_.size() != 1 ||
        !accepted_heads_.contains(committed_.hash))
        return false;

    // Re-root the sole converged accepted head as a full entry. Its predecessor
    // and merge-parent hashes remain part of the immutable record/snapshot, but
    // previous_known=false establishes a deliberate local ancestry floor: an old
    // branch is no longer reconstructable from this node after the cluster has
    // proven that every known participant has converged beyond it.
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
        history_p_,
        std::string_view(reinterpret_cast<const char*>(frame.data()), frame.size()));

    history_.clear();
    history_.emplace(root.hash, std::move(root));
    materialized_history_.clear();
    materialized_history_clock_ = 0;
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
        for (const auto* candidate : {&conflict.base_entry, &conflict.left_entry,
                                      &conflict.right_entry}) {
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
        for (const auto* candidate : {&conflict.base_catalogue_root,
                                      &conflict.left_catalogue_root,
                                      &conflict.right_catalogue_root}) {
            if (*candidate)
                out.insert(**candidate);
        }
    }
    return out;
}

Hash256 metadata_namespace_signature(const MetadataSnapshot& snapshot) {
    // This signature is consulted by catalogue/maintenance paths. Stream the
    // canonical representation into SHA-256 so a namespace containing millions
    // of extents never requires a second namespace-sized byte buffer.
    Sha256Hasher hash;
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
