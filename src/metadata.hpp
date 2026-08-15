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
};
struct GarbageRef {
    ObjectId id{};
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
Bytes encode_snapshot(const MetadataSnapshot&);
MetadataSnapshot decode_snapshot(std::span<const uint8_t>);
Bytes encode_metadata_record(const MetadataRecord&);
MetadataRecord decode_metadata_record(std::span<const uint8_t>);
Hash256 metadata_hash(uint64_t, const Hash256&, std::span<const uint8_t>);
MetadataRecord genesis_metadata();
bool valid_metadata_record(const MetadataRecord&);
class MetadataReplica {
    std::filesystem::path p_;
    std::filesystem::path committed_p_;
    std::array<uint8_t, 32> key_;
    mutable std::mutex m_;
    MetadataRecord cur_;
    MetadataRecord committed_;
    void persist(const std::filesystem::path&, const MetadataRecord&);
    std::optional<MetadataRecord> load(const std::filesystem::path&) const;

  public:
    MetadataReplica(std::filesystem::path, std::array<uint8_t, 32>);
    MetadataRecord current() const;
    MetadataRecord committed() const;
    bool cas(uint64_t, const Hash256&, std::span<const uint8_t>, MetadataRecord*);
    bool seed(const MetadataRecord&);
    bool remember_committed(const MetadataRecord&);
};
std::string normalize_path(const std::string&);
std::string parent_path(const std::string&);
std::string base_name(const std::string&);
int64_t wall_time_ns();
} // namespace macha
