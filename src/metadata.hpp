// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "crypto.hpp"
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
namespace macha {
enum class EntryType : uint8_t { directory = 1, file = 2 };

enum class MetadataConflictKind : uint8_t {
    namespace_entry = 1,
    catalogue_root = 2,
};
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

struct MetadataConflict {
    MetadataConflictKind kind{MetadataConflictKind::namespace_entry};
    std::string key;
    Hash256 left_head{}, right_head{};
    std::optional<FsEntry> base_entry, left_entry, right_entry;
    std::optional<ObjectId> base_catalogue_root, left_catalogue_root, right_catalogue_root;
    auto operator<=>(const MetadataConflict&) const = default;
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
    // Legacy 0.18 serialized voter list. Protocol 20 ignores this as authority
    // and clears it on the first metadata policy transition. Keep the field in
    // SM10/DLT4 encoding so existing 0.18 state remains readable.
    std::vector<NodeId> metadata_voters;
    uint32_t data_replication{};
    uint64_t extent_size{};
    // Protocol-20 cluster safety policy. Zero means a legacy pre-0.19 snapshot
    // which has not yet been transitioned; every newly accepted 0.19 commit
    // carries the exact durability floor under which it is valid.
    uint32_t metadata_write_replicas_required{};
    // Protocol-20 migration roster used only while establishing retention claims
    // for an upgraded pre-0.19 namespace. It is never an authority/eligibility
    // list and is cleared once the migration baseline is complete.
    // Removal requires an explicit future retirement protocol; silently
    // forgetting an offline participant would make destructive GC unsafe.
    std::set<NodeId> metadata_participants;
    // Causal stability horizon for destructive retention release. This names an
    // accepted commit which every registered participant was synchronously
    // brought to (or beyond) before the horizon-advance commit was published.
    // No participant may subsequently author from an ancestor/sibling behind
    // this floor. Zero means no protocol-20 retirement horizon has yet formed.
    Hash256 metadata_branch_floor{};
    // True once every object reachable from the reconciled protocol-20
    // namespace/catalogue has acquired durable retention claims. Legacy/SM12
    // upgrades start false and cannot perform destructive reachability GC until
    // the whole participant roster has converged and this baseline is accepted.
    bool retention_baseline_complete{};
    // Highest metadata mutation sequence incorporated from each node. A node
    // serialises its own mutations, so this is a compact idempotency clock for
    // recognising an already accepted logical mutation after publication retry
    // or branch reconciliation.
    std::map<NodeId, uint64_t> mutation_sequences;
    std::optional<ObjectId> catalogue_root;
    std::map<std::string, FsEntry> entries;
    std::vector<GarbageRef> garbage;
    // Low-frequency, cluster-persisted last-known node observations. This is
    // deliberately not a metrics history: each node coalesces its current
    // status into one record and ephemeral telemetry remains outside metadata publication.
    std::map<NodeId, PersistedNodeStatus> node_status;
    // Durable endpoint->NodeId invalidations. These are tombstones, not node
    // deletion: they suppress a stale association while allowing the endpoint
    // to authenticate later as a different NodeId.
    std::map<std::string, IdentityAssociationReset, std::less<>> identity_resets;
    // Additional parents are present only on a reconciliation commit. The
    // MetadataRecord::previous field remains the primary parent so ordinary
    // delta replay stays compact and compatible with the existing record shape.
    std::vector<Hash256> merge_parents;
    // Durable semantic conflicts preserve all alternatives while the effective
    // namespace/catalogue remains at its common-ancestor value. Unrelated work
    // may continue; explicit conflict resolution is a later metadata mutation.
    std::map<std::string, MetadataConflict, std::less<>> conflicts;
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

// Durable evidence that an immutable metadata commit reached the configured
// write floor. `replicas` names the distinct nodes that durably acknowledged
// storing the commit before the certificate was issued. `required == 0` is
// reserved for migration of a pre-0.19 committed checkpoint: the encrypted
// checkpoint itself was already authoritative under the previous protocol.
struct MetadataAcceptance {
    uint64_t generation{};
    Hash256 hash{};
    uint32_t required{};
    std::vector<NodeId> replicas;
    auto operator<=>(const MetadataAcceptance&) const = default;
};

struct MetadataHistoryEntry {
    enum class Body : uint8_t { full = 1, delta = 2 };

    uint64_t generation{};
    Hash256 previous{}, hash{};
    bool previous_known{};
    std::vector<Hash256> merge_parents;
    Body body{Body::full};
    Bytes payload;
    auto operator<=>(const MetadataHistoryEntry&) const = default;
};

struct MetadataMergeResult {
    MetadataSnapshot snapshot;
    size_t conflicts_created{};
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
Bytes encode_metadata_acceptance(const MetadataAcceptance&);
MetadataAcceptance decode_metadata_acceptance(std::span<const uint8_t>);
Bytes encode_metadata_acceptance_set(const std::vector<MetadataAcceptance>&);
std::vector<MetadataAcceptance> decode_metadata_acceptance_set(std::span<const uint8_t>);
Bytes encode_metadata_history_entry(const MetadataHistoryEntry&);
MetadataHistoryEntry decode_metadata_history_entry(std::span<const uint8_t>);
Bytes encode_metadata_delta(const MetadataDelta&);
MetadataDelta decode_metadata_delta(std::span<const uint8_t>);
std::optional<MetadataDelta> metadata_delta(const MetadataSnapshot&, const MetadataSnapshot&);
void apply_metadata_delta_in_place(MetadataSnapshot&, const MetadataDelta&);
MetadataSnapshot apply_metadata_delta(const MetadataSnapshot&, const MetadataDelta&);
MetadataRecord decode_metadata_record(std::span<const uint8_t>);
Hash256 metadata_hash(uint64_t, const Hash256&, std::span<const uint8_t>);
MetadataRecord genesis_metadata();
bool valid_metadata_record(const MetadataRecord&);
std::vector<Hash256> metadata_record_parents(const MetadataRecord&);
std::string metadata_conflict_id(const MetadataConflict&);
MetadataMergeResult merge_metadata_snapshots(const MetadataSnapshot& base,
                                             const MetadataSnapshot& left,
                                             const MetadataSnapshot& right,
                                             const Hash256& left_head,
                                             const Hash256& right_head);
// Reachability roots held only by unresolved metadata conflicts. These helpers
// keep GC policy close to the conflict representation so every maintenance
// consumer protects the alternatives required for later explicit resolution.
std::set<ObjectId> metadata_conflict_extent_roots(const MetadataSnapshot&);
std::set<ObjectId> metadata_catalogue_root_set(const MetadataSnapshot&);
// Deterministic identity of namespace paths and immutable file content. Deliberately
// excludes catalogue/garbage/voter state and non-content stat metadata.
Hash256 metadata_namespace_signature(const MetadataSnapshot&);
class MetadataReplica {
    std::filesystem::path p_;
    std::filesystem::path committed_p_;
    std::filesystem::path checkpoint_p_;
    std::filesystem::path journal_p_;
    std::filesystem::path history_p_;
    std::filesystem::path heads_p_;
    std::filesystem::path mutation_sequence_p_;
    std::filesystem::path recovery_p_;
    std::array<uint8_t, 32> key_;
    mutable std::mutex m_;
    MetadataRecord cur_;
    MetadataRecord committed_;
    std::map<Hash256, MetadataHistoryEntry> history_;
    std::map<Hash256, MetadataAcceptance> accepted_heads_;
    std::optional<MetadataHistoryEntry> pending_history_;
    bool pending_recovered_{};
    bool mutation_sequence_loaded_{};
    uint64_t mutation_sequence_{};
    size_t journal_records_{};
    uint64_t journal_bytes_{};
    bool recovery_required_{};
    void persist(const std::filesystem::path&, const MetadataRecord&);
    std::optional<MetadataRecord> load(const std::filesystem::path&) const;
    void append_journal(uint8_t, const MetadataRecord&, std::span<const uint8_t> = {});
    void load_journal();
    void append_history(const MetadataHistoryEntry&);
    void load_history();
    void load_heads();
    void persist_heads_locked();
    void ensure_history_root(const MetadataRecord&);
    void migrate_legacy_head_locked();
    bool prune_accepted_heads_locked();
    bool acceptance_matches_record_policy_locked(const MetadataAcceptance&,
                                                 const MetadataRecord&) const;
    bool legacy_write_api_allowed_locked() const;
    void set_legacy_committed_head_locked(const MetadataRecord&);
    void refresh_materialized_head_locked();
    MetadataHistoryEntry history_for_current(std::span<const uint8_t> delta = {});
    std::optional<MetadataRecord> historical_locked(const Hash256&) const;
    bool history_is_ancestor_locked(const Hash256&, const Hash256&) const;
    std::optional<Hash256> history_common_ancestor_locked(const Hash256&, const Hash256&) const;
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
    uint64_t reserve_mutation_sequence(uint64_t observed_floor);
    bool cas(uint64_t, const Hash256&, std::span<const uint8_t>, MetadataRecord*);
    bool cas_delta(uint64_t, const Hash256&, std::span<const uint8_t>, MetadataRecord*);
    bool install_committed_delta(uint64_t, const Hash256&, std::span<const uint8_t>,
                                 const MetadataRecord&);
    bool seed(const MetadataRecord&);
    bool remember_committed(const MetadataRecord&);
    bool remember_current_committed(uint64_t, const Hash256&);
    std::optional<MetadataHistoryEntry> history_entry(const Hash256&) const;
    bool import_history(const MetadataHistoryEntry&);
    bool history_contains(const Hash256&) const;
    bool store_commit(const MetadataRecord&, std::span<const uint8_t> delta = {});
    bool accept_commit(const MetadataAcceptance&);
    std::vector<MetadataAcceptance> accepted_head_certificates() const;
    std::vector<MetadataRecord> accepted_heads() const;
    std::optional<MetadataAcceptance> acceptance(const Hash256&) const;
    bool history_is_ancestor(const Hash256&, const Hash256&) const;
    std::optional<Hash256> history_common_ancestor(const Hash256&, const Hash256&) const;
    std::optional<MetadataRecord> historical(const Hash256&) const;
    void compact();
};
std::string normalize_path(const std::string&);
std::string parent_path(const std::string&);
std::string base_name(const std::string&);
int64_t wall_time_ns();
} // namespace macha
