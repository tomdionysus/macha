// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "crypto.hpp"
#include <filesystem>
#include <map>
#include <mutex>
namespace macha {
enum class EntryType : uint8_t { directory = 1, file = 2 };
struct ExtentRef {
    uint64_t offset{}, length{};
    ObjectId id{};
    bool hole{};
    auto operator<=>(const ExtentRef&) const = default;
};
struct FsEntry {
    EntryType type{EntryType::file};
    uint32_t mode{0644}, uid{}, gid{};
    uint64_t size{};
    int64_t ctime_ns{}, mtime_ns{};
    uint64_t version{1};
    std::vector<ExtentRef> extents;
    auto operator<=>(const FsEntry&) const = default;
};
struct GarbageRef {
    ObjectId id{};
    // Wall-clock retirement time written into committed metadata. Zero denotes
    // a tombstone written by metadata formats before 0.10.0; maintenance
    // conservatively stamps those before making them eligible for collection.
    int64_t retired_at_ns{};
    // Unique retirement identity prevents an old maintenance decision from
    // pruning a later retirement of the same content-addressed object. Empty
    // means the tombstone came from a pre-0.10.0 snapshot/journal.
    NodeId retirement_id{};
    auto operator<=>(const GarbageRef&) const = default;
};

struct MetadataSnapshot {
    std::vector<NodeId> metadata_voters;
    uint32_t data_replication{};
    uint64_t extent_size{};
    // Highest metadata mutation sequence incorporated from each node. A node
    // serialises its own mutations, so this is a compact idempotency clock for
    // recognising successful proposals after quorum-CAS conflict/retry.
    std::map<NodeId, uint64_t> mutation_sequences;
    std::optional<ObjectId> catalogue_root;
    std::map<std::string, FsEntry> entries;
    std::vector<GarbageRef> garbage;
};
struct MetadataRecord {
    uint64_t generation{};
    Hash256 previous{}, hash{};
    Bytes payload;
};

struct MetadataIdentity {
    uint64_t generation{};
    Hash256 hash{};
    auto operator<=>(const MetadataIdentity&) const = default;
};

enum class CatalogueDelta : uint8_t { unchanged = 0, clear = 1, set = 2 };

// Compact deterministic mutation from one canonical metadata snapshot to the
// next. Ordinary namespace/catalogue mutation uses this on the wire and in the
// local encrypted journal; full MetadataRecord payloads remain the repair and
// reconfiguration primitive.
struct MetadataDelta {
    std::map<NodeId, uint64_t> mutation_sequences;
    std::map<std::string, FsEntry> upsert_entries;
    std::vector<std::string> erase_entries;
    std::vector<ObjectId> erase_garbage;
    std::vector<GarbageRef> upsert_garbage;
    CatalogueDelta catalogue{CatalogueDelta::unchanged};
    std::optional<ObjectId> catalogue_root;
};
Bytes encode_snapshot(const MetadataSnapshot&);
MetadataSnapshot decode_snapshot(std::span<const uint8_t>);
Bytes encode_metadata_record(const MetadataRecord&);
Bytes encode_metadata_delta(const MetadataDelta&);
MetadataDelta decode_metadata_delta(std::span<const uint8_t>);
std::optional<MetadataDelta> metadata_delta(const MetadataSnapshot&, const MetadataSnapshot&);
MetadataSnapshot apply_metadata_delta(const MetadataSnapshot&, const MetadataDelta&);
MetadataRecord decode_metadata_record(std::span<const uint8_t>);
Hash256 metadata_hash(uint64_t, const Hash256&, std::span<const uint8_t>);
MetadataRecord genesis_metadata();
bool valid_metadata_record(const MetadataRecord&);
// Deterministic identity of namespace paths and immutable file content. Deliberately
// excludes catalogue/garbage/voter state and non-content stat metadata.
Hash256 metadata_namespace_signature(const MetadataSnapshot&);
class MetadataReplica {
    std::filesystem::path p_;
    std::filesystem::path committed_p_;
    std::filesystem::path checkpoint_p_;
    std::filesystem::path journal_p_;
    std::array<uint8_t, 32> key_;
    mutable std::mutex m_;
    MetadataRecord cur_;
    MetadataRecord committed_;
    size_t journal_records_{};
    uint64_t journal_bytes_{};
    void persist(const std::filesystem::path&, const MetadataRecord&);
    std::optional<MetadataRecord> load(const std::filesystem::path&) const;
    void append_journal(uint8_t, const MetadataRecord&, std::span<const uint8_t> = {});
    void load_journal();
    void compact_if_needed();
    void reset_checkpoint(const MetadataRecord&);

  public:
    MetadataReplica(std::filesystem::path, std::array<uint8_t, 32>);
    MetadataRecord current() const;
    MetadataRecord committed() const;
    MetadataIdentity current_identity() const;
    MetadataIdentity committed_identity() const;
    uint64_t generation() const;
    uint64_t committed_generation() const;
    bool cas(uint64_t, const Hash256&, std::span<const uint8_t>, MetadataRecord*);
    bool cas_delta(uint64_t, const Hash256&, std::span<const uint8_t>, MetadataRecord*);
    bool install_committed_delta(uint64_t, const Hash256&, std::span<const uint8_t>,
                                 const MetadataRecord&);
    bool seed(const MetadataRecord&);
    bool remember_committed(const MetadataRecord&);
    bool remember_current_committed(uint64_t, const Hash256&);
    void compact();
};
std::string normalize_path(const std::string&);
std::string parent_path(const std::string&);
std::string base_name(const std::string&);
int64_t wall_time_ns();
} // namespace macha
