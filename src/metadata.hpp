// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "crypto.hpp"
#include <filesystem>
#include <map>
#include <memory>
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
    // a tombstone written by legacy metadata formats; maintenance
    // conservatively stamps those before making them eligible for collection.
    int64_t retired_at_ns{};
    // Unique retirement identity prevents an old maintenance decision from
    // pruning a later retirement of the same content-addressed object. Empty
    // means the tombstone came from a legacy snapshot/journal.
    NodeId retirement_id{};
    auto operator<=>(const GarbageRef&) const = default;
};

struct PersistedNodeStatus {
    NodeId boot_id{};
    uint64_t observed_unix_ms{};
    std::string version;
    std::string host;
    std::string failure_domain;
    uint16_t port{};
    uint64_t storage_capacity{};
    uint64_t storage_used{};
    uint64_t cache_capacity{};
    uint64_t cache_used{};
    uint64_t metadata_generation{};
    uint32_t storage_backends_online{};
    auto operator<=>(const PersistedNodeStatus&) const = default;
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
    // Low-frequency, cluster-persisted last-known node observations. This is
    // deliberately not a metrics history: each node coalesces its current
    // status into one record and ephemeral telemetry remains off-quorum.
    std::map<NodeId, PersistedNodeStatus> node_status;
    // Durable endpoint->NodeId invalidations. These are tombstones, not node
    // deletion: they suppress a stale association while allowing the endpoint
    // to authenticate later as a different NodeId.
    std::map<std::string, IdentityAssociationReset, std::less<>> identity_resets;
};
// Metadata records are immutable once constructed. Their canonical snapshot payload can
// be hundreds of megabytes on large media namespaces, so copying a MetadataRecord must
// not duplicate the complete byte vector. SharedBytes retains value semantics while
// making record copies share immutable backing storage. Any assignment/assign creates a
// new backing vector, so aliases can never observe mutation.
class SharedBytes {
    std::shared_ptr<const Bytes> bytes_{std::make_shared<const Bytes>()};

  public:
    SharedBytes() = default;
    SharedBytes(Bytes value) : bytes_(std::make_shared<const Bytes>(std::move(value))) {}

    SharedBytes& operator=(Bytes value) {
        bytes_ = std::make_shared<const Bytes>(std::move(value));
        return *this;
    }

    template <class Iterator> void assign(Iterator first, Iterator last) {
        bytes_ = std::make_shared<const Bytes>(first, last);
    }

    size_t size() const noexcept { return bytes_->size(); }
    bool empty() const noexcept { return bytes_->empty(); }
    const uint8_t* data() const noexcept { return bytes_->data(); }
    Bytes::const_iterator begin() const noexcept { return bytes_->begin(); }
    Bytes::const_iterator end() const noexcept { return bytes_->end(); }
    operator std::span<const uint8_t>() const noexcept { return *bytes_; }

    friend bool operator==(const SharedBytes& a, const SharedBytes& b) {
        return a.bytes_ == b.bytes_ || *a.bytes_ == *b.bytes_;
    }
    friend bool operator==(const SharedBytes& a, const Bytes& b) { return *a.bytes_ == b; }
    friend bool operator==(const Bytes& a, const SharedBytes& b) { return a == *b.bytes_; }
};

struct MetadataRecord {
    uint64_t generation{};
    Hash256 previous{}, hash{};
    SharedBytes payload;
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
    std::map<NodeId, PersistedNodeStatus> upsert_node_status;
    std::map<std::string, IdentityAssociationReset, std::less<>> upsert_identity_resets;
    CatalogueDelta catalogue{CatalogueDelta::unchanged};
    std::optional<ObjectId> catalogue_root;
};
Bytes encode_snapshot(const MetadataSnapshot&);
MetadataSnapshot decode_snapshot(std::span<const uint8_t>);
Bytes encode_metadata_record(const MetadataRecord&);
Bytes encode_metadata_delta(const MetadataDelta&);
MetadataDelta decode_metadata_delta(std::span<const uint8_t>);
std::optional<MetadataDelta> metadata_delta(const MetadataSnapshot&, const MetadataSnapshot&);
void apply_metadata_delta_in_place(MetadataSnapshot&, const MetadataDelta&);
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
    std::filesystem::path recovery_p_;
    std::array<uint8_t, 32> key_;
    mutable std::mutex m_;
    MetadataRecord cur_;
    MetadataRecord committed_;
    size_t journal_records_{};
    uint64_t journal_bytes_{};
    bool recovery_required_{};
    void persist(const std::filesystem::path&, const MetadataRecord&);
    std::optional<MetadataRecord> load(const std::filesystem::path&) const;
    void append_journal(uint8_t, const MetadataRecord&, std::span<const uint8_t> = {});
    void load_journal();
    void compact_if_needed();
    void reset_checkpoint(const MetadataRecord&);
    void recover_from_seed(const MetadataRecord&, const std::string&);

  public:
    MetadataReplica(std::filesystem::path, std::array<uint8_t, 32>,
                    std::optional<MetadataRecord> recovery_seed = {});
    MetadataRecord current() const;
    MetadataRecord committed() const;
    MetadataIdentity current_identity() const;
    MetadataIdentity committed_identity() const;
    uint64_t generation() const;
    uint64_t committed_generation() const;
    bool recovery_required() const;
    void mark_recovered();
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
