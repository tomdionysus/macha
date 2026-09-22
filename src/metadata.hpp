// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "crypto.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
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
    // Advertised API endpoint, mirrored from NodeTelemetry for Status's
    // last-known fallback. Deliberately local-only: NOT part of the metadata
    // wire codec (encode_node_status/decode_node_status in metadata.cpp), so
    // it never enters the replicated snapshot/checkpoint format.
    std::string api_endpoint;
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
    // SM14 only: the root of the content-addressed namespace tree, carried
    // *instead of* `entries` rather than alongside them. A snapshot holds one
    // or the other and never both -- `encode_snapshot` refuses a root it has
    // no field for and `encode_snapshot_v14` refuses entries it would silently
    // drop -- so there is no encoding in which the two can disagree about what
    // the namespace is. See namespace_tree.hpp and
    // TODO/2026-09-17-namespace-merkle-root-plan.md.
    std::optional<ObjectId> namespace_root;
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

    size_t size() const noexcept {
        return bytes_->size();
    }
    bool empty() const noexcept {
        return bytes_->empty();
    }
    const uint8_t* data() const noexcept {
        return bytes_->data();
    }
    Bytes::const_iterator begin() const noexcept {
        return bytes_->begin();
    }
    Bytes::const_iterator end() const noexcept {
        return bytes_->end();
    }
    operator std::span<const uint8_t>() const noexcept {
        return *bytes_;
    }

    friend bool operator==(const SharedBytes& a, const SharedBytes& b) {
        return a.bytes_ == b.bytes_ || *a.bytes_ == *b.bytes_;
    }
    friend bool operator==(const SharedBytes& a, const Bytes& b) {
        return *a.bytes_ == b;
    }
    friend bool operator==(const Bytes& a, const SharedBytes& b) {
        return a == *b.bytes_;
    }
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

// Durable proof that every durably-known metadata participant has
// acknowledged the same accepted-head hash as the new history ancestry
// floor. `epoch` fingerprints the participant set the proposal was made
// against (see MetadataManager) -- a membership change invalidates any
// in-flight proposal by changing the epoch, never by trusting a smaller or
// stale participant list. `participants` names exactly who this proof
// covers. Reaching `Status::committed` is the *only* thing that may justify
// calling MetadataReplica::compact_history_if_safe(); a proof that is
// merely `acked`, or whose floor_hash does not match the replica's current
// committed head, must never be used to justify anything.
struct HistoryCheckpointProof {
    enum class Status : uint8_t { acked = 1, committed = 2 };
    Hash256 floor_hash{};
    uint64_t floor_generation{};
    Hash256 epoch{};
    std::vector<NodeId> participants;
    Status status{Status::acked};
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

// Immutable history bodies carry ancestry links for topology and repair, but
// only these links are prerequisites for materializing the body itself.
std::vector<Hash256> metadata_history_materialization_dependencies(
    const MetadataHistoryEntry&);

// The single succession rule for a delta-body history entry over its primary
// parent, shared by every writer (store_commit, import_history, load_history)
// and every reader (both materialization walks). A reconciliation merge is
// numbered max(parents)+1 with the lower-hash parent as its primary, so a
// legitimate delta can sit many generations above its parent; the record hash
// binds the generation, so the walk needs only monotonicity. Writers and
// readers once encoded this rule separately and disagreed (writers accepted
// parent<child, readers demanded parent+1): a merge commit was durably written,
// hash-verified, and unreadable the moment it left the materialization cache --
// the 2026-09-06 "accepted metadata head cannot be reconstructed" outage. Keep
// exactly one implementation.
constexpr bool metadata_delta_succession_valid(uint64_t parent_generation,
                                               uint64_t child_generation) noexcept {
    return parent_generation < child_generation;
}

struct MetadataMergeResult {
    MetadataSnapshot snapshot;
    size_t conflicts_created{};
    // Standing conflicts dropped because a later mutation had already
    // replaced the subject they were about (see prune_superseded_conflicts()).
    size_t conflicts_superseded{};
};

struct MetadataManualRepairPlan {
    MetadataRecord record;
    Hash256 dominant_head{};
    Hash256 subsumed_head{};
};

// Offline/manual recovery only. This is deliberately not part of automatic
// reconciliation: it can join histories whose common ancestor was discarded
// only when one durable causal clock strictly dominates the other.
std::optional<MetadataManualRepairPlan> plan_causally_dominant_metadata_repair(
    const MetadataRecord&, const MetadataSnapshot&,
    const MetadataRecord&, const MetadataSnapshot&);

struct MetadataConflictPreservingRepairPlan {
    MetadataRecord record;
    Hash256 left_head{};
    Hash256 right_head{};
    size_t conflicts_created{};
};

// Offline/manual recovery only, for the concurrent case
// plan_causally_dominant_metadata_repair() refuses (neither head dominates)
// once the common ancestor has been discarded, so the ordinary three-way
// merge in merge_metadata_snapshots() has no base to run against. Merges
// with a deliberately empty base rather than a fabricated one: every path
// with an identical value on both sides reconciles regardless of base
// (equal values short-circuit before base is ever consulted), and every path
// that differs becomes a durable first-class conflict exactly as ordinary
// reconciliation would record it -- both alternatives are preserved for
// explicit resolution, never silently discarded.
// Refuses (returns nullopt) if either head has a path the other lacks: an
// empty base also disables merge_metadata_snapshots()'s rename/move-collision
// detection (which walks the base to tell a rename from two independent
// creates at different paths), so this is only sound when the two heads
// differ solely by entries changed in place, never by adds or removes.
std::optional<MetadataConflictPreservingRepairPlan> plan_conflict_preserving_metadata_repair(
    const MetadataRecord&, const MetadataSnapshot&,
    const MetadataRecord&, const MetadataSnapshot&);

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
    // Whole-set replacements for the branch topology. Ordinary mutations leave
    // both unset and retain the rolling-compatible DLT5 encoding. DLT7 carries
    // a presence flag for each, so a merge or a conflict resolution sends only
    // the set that changed (a conflict-free merge no longer carries the whole
    // standing conflict set -- 336 KB on the cluster, 2026-09-06). DLT6 has no
    // flags: a DLT6 body must replace both or neither, and encode_metadata_delta()
    // emits DLT7 whenever only one is set.
    std::optional<std::vector<Hash256>> replace_merge_parents;
    std::optional<std::map<std::string, MetadataConflict, std::less<>>>
        replace_conflicts;
    // DLT7: after the tombstone edits are applied the tombstone vector is
    // sorted by ObjectId (see garbage_is_canonical()). Reconciliation always
    // produces that order, while historical snapshots carry append order and
    // pre-DLT7 deltas could not express the reordering -- so every merge whose
    // primary parent had an append-ordered vector fell back to a full
    // snapshot frame (5-8 MB per replica, 4 of 5 reconciliations, 2026-09-06).
    bool canonical_garbage{};
    // DLT8: a file whose extent table only grew (sequential publication of a
    // large file, one quantum at a time) is carried as the appended extents
    // plus its new attributes instead of the whole entry. A 13.9 GB file's
    // 32 MB quantum commit was a ~105 KB delta re-sending 3,000+ extents
    // (124 MB of history per node in 35 min, 2026-09-07); now ~500 B.
    struct EntryAppend {
        uint64_t size{};
        int64_t mtime_ns{};
        int64_t ctime_ns{};
        uint64_t version{};
        uint32_t base_extents{};        // extents the entry must already hold
        std::vector<ExtentRef> extents; // appended after them
    };
    std::map<std::string, EntryAppend> append_entries;
    CatalogueDelta catalogue{CatalogueDelta::unchanged};
    std::optional<ObjectId> catalogue_root;
};

// Record `after` for `path` in `delta` as an append (when `before` exists
// and its extents are a strict prefix of after's, attributes aside) or as a
// whole-entry upsert otherwise.
void record_entry_change(MetadataDelta& delta, const std::string& path, const FsEntry* before,
                         const FsEntry& after);

// True when `garbage` is strictly ordered by ObjectId (no duplicates): the
// canonical order every DLT7-era snapshot keeps.
bool garbage_is_canonical(const std::vector<GarbageRef>&);
// Sort into canonical order (stable; equal ids keep their relative order).
void canonicalise_garbage(std::vector<GarbageRef>&);
// Same bytes at the same path: type, mode, ownership, size and extents equal
// (mtime/ctime/version may differ). Two writers publishing the same media are
// not in conflict.
bool same_content(const FsEntry&, const FsEntry&);
// Drop every standing conflict whose subject has since been mutated: a
// namespace_entry conflict whose path no longer holds the common-ancestor
// value, or a catalogue_root conflict once the root moved on. The later
// mutation *is* the resolution; carrying the record (~3 KB each) in every
// snapshot and every merge delta afterwards is habit D. Returns the count.
size_t prune_superseded_conflicts(MetadataSnapshot&);
Bytes encode_snapshot(const MetadataSnapshot&);
MetadataSnapshot decode_snapshot(std::span<const uint8_t>);
// SM14: the record as a pointer to the namespace rather than the namespace.
// The payload is the non-entry fields plus `namespace_root`, so its size is a
// property of the cluster rather than of the library -- a few hundred bytes
// where SM13 is the whole serialised namespace, which on es-1 today is
// 22,525,100 of them.
//
// Not authoritative. `encode_snapshot` never emits SM14, nothing in the commit
// path constructs one, and a peer that does not know the magic refuses it as
// `bad snapshot` -- so this deploys as dead code and becomes reachable in
// Stage C. Requires `namespace_root` set and `entries` empty; `detach_namespace`
// (namespace_tree.hpp) is how an ordinary snapshot gets there.
Bytes encode_snapshot_v14(const MetadataSnapshot&);
Bytes encode_metadata_record(const MetadataRecord&);
Bytes encode_metadata_acceptance(const MetadataAcceptance&);
MetadataAcceptance decode_metadata_acceptance(std::span<const uint8_t>);
Bytes encode_metadata_acceptance_set(const std::vector<MetadataAcceptance>&);
std::vector<MetadataAcceptance> decode_metadata_acceptance_set(std::span<const uint8_t>);
Bytes encode_history_checkpoint_proof(const HistoryCheckpointProof&);
HistoryCheckpointProof decode_history_checkpoint_proof(std::span<const uint8_t>);
Bytes encode_metadata_history_entry(const MetadataHistoryEntry&);
MetadataHistoryEntry decode_metadata_history_entry(std::span<const uint8_t>);
Bytes encode_metadata_delta(const MetadataDelta&);
MetadataDelta decode_metadata_delta(std::span<const uint8_t>);
std::optional<MetadataDelta> metadata_delta(const MetadataSnapshot&, const MetadataSnapshot&);
// How a tree-backed namespace applies a delta: given the current root and the
// delta, return the new root. Supplied by whoever holds a node store, because
// this layer deliberately has none -- MetadataReplica is a state directory and
// a codec, not a cluster client.
//
// Unset means a tree-backed delta cannot be applied, and it is refused rather
// than applied to the entry map, where the edits would land in an empty map
// nothing reads while the root went on addressing the namespace as it was.
using NamespaceDeltaApplier = std::function<ObjectId(const ObjectId& root, const MetadataDelta&)>;

void apply_metadata_delta_in_place(MetadataSnapshot&, const MetadataDelta&,
                                   const NamespaceDeltaApplier& = {});
MetadataSnapshot apply_metadata_delta(const MetadataSnapshot&, const MetadataDelta&,
                                      const NamespaceDeltaApplier& = {});
MetadataRecord decode_metadata_record(std::span<const uint8_t>);
Hash256 metadata_hash(uint64_t, const Hash256&, std::span<const uint8_t>);
MetadataRecord genesis_metadata();
bool valid_metadata_record(const MetadataRecord&);
std::vector<Hash256> metadata_record_parents(const MetadataRecord&);
std::string metadata_conflict_id(const MetadataConflict&);
MetadataMergeResult merge_metadata_snapshots(const MetadataSnapshot& base,
                                             const MetadataSnapshot& left,
                                             const MetadataSnapshot& right,
                                             const Hash256& left_head, const Hash256& right_head);
// Reachability roots held only by unresolved metadata conflicts. These helpers
// keep GC policy close to the conflict representation so every maintenance
// consumer protects the alternatives required for later explicit resolution.
std::set<ObjectId> metadata_conflict_extent_roots(const MetadataSnapshot&);
std::set<ObjectId> metadata_catalogue_root_set(const MetadataSnapshot&);
// Deterministic identity of namespace paths and immutable file content. Deliberately
// excludes catalogue/garbage/voter state and non-content stat metadata.
Hash256 metadata_namespace_signature(const MetadataSnapshot&);

// Decoded in-memory cost of one snapshot, by the same accounting the
// materialization cache charges against its budget. Exposed so an operator can
// measure a real library's residency from a state directory rather than
// inferring it from struct declarations.
uint64_t snapshot_resident_bytes(const MetadataSnapshot&);

struct MetadataReplicaDiagnostics {
    uint64_t historical_requests{};
    uint64_t historical_reconstructions{};
    uint64_t historical_deltas_applied{};
    uint64_t materialization_cache_hits{};
    uint64_t materialization_cache_misses{};
    uint64_t materialization_cache_evictions{};
    size_t materialization_cache_entries{};
    uint64_t materialization_cache_bytes{};
    uint64_t materialization_cache_limit_bytes{};
    uint64_t history_records{};
    uint64_t history_file_bytes{};
    uint64_t history_resident_payload_bytes{};
    uint64_t accepted_head_persistence_writes{};
    uint64_t accepted_head_persistence_bytes{};
    uint64_t accepted_head_persistence_failures{};
};

struct MetadataMaterialization {
    MetadataRecord record;
    std::shared_ptr<const MetadataSnapshot> snapshot;
    // Conservative deep weight of the immutable record and decoded object
    // graph. Computed once, off the replica-state mutex, when materialized.
    uint64_t resident_bytes{};
};

struct MetadataHistoryLinks {
    uint64_t generation{};
    Hash256 previous{};
    bool previous_known{};
    std::vector<Hash256> merge_parents;
    MetadataHistoryEntry::Body body{MetadataHistoryEntry::Body::full};
};

class MetadataReplica {
    struct HistoryIndexEntry {
        uint64_t generation{};
        Hash256 previous{};
        Hash256 hash{};
        bool previous_known{};
        std::vector<Hash256> merge_parents;
        MetadataHistoryEntry::Body body{MetadataHistoryEntry::Body::full};
        uint64_t file_offset{};
        uint64_t frame_bytes{};
    };

    struct MaterializedHistoryEntry {
        std::shared_ptr<const MetadataMaterialization> value;
        uint64_t last_used{};
        uint64_t bytes{};
    };

    std::filesystem::path p_;
    std::filesystem::path committed_p_;
    std::filesystem::path checkpoint_p_;
    std::filesystem::path journal_p_;
    std::filesystem::path history_p_;
    std::filesystem::path heads_p_;
    std::filesystem::path checkpoint_proof_p_;
    std::filesystem::path mutation_sequence_p_;
    std::filesystem::path recovery_p_;
    std::array<uint8_t, 32> key_;
    mutable std::mutex m_;
    // Serialises durable metadata mutations while their slow file operations
    // execute without holding m_. Always acquired before m_ by public writers.
    mutable std::mutex durable_mutation_m_;
    // Serialises cache-miss computation without blocking short replica-state
    // readers or durable mutation bookkeeping on the main mutex.
    mutable std::mutex materialization_compute_m_;
    MetadataRecord cur_;
    MetadataRecord committed_;
    // Durable history payloads live in history.log. Memory retains only the
    // ancestry and frame-location index needed to find them on demand.
    std::map<Hash256, HistoryIndexEntry> history_;
    std::map<Hash256, MetadataAcceptance> accepted_heads_;
    // An accepted head that fails to reconstruct is exceptional -- normally a
    // narrow, transient race -- and every caller up the stack (checkpoint
    // maintenance, catalogue publication, conflict reconciliation, ordinary
    // reads) treats it as such by catching and logging. Without a cooldown,
    // every one of those callers re-attempts and re-throws immediately, and
    // under sustained concurrent-write load that call volume turns one
    // recoverable glitch into an unbounded tight retry loop pinning a thread
    // indefinitely (see the 2026-09-06 concurrent-write stress test incident).
    // This bounds how often reconstruction is actually retried (and the
    // exception actually re-raised) per hash; between attempts the hash is
    // treated as temporarily absent from the accepted set rather than as a
    // hard failure, so the replica keeps serving its last good committed
    // state instead of wedging.
    mutable std::map<Hash256, Clock::time_point> unreconstructable_head_retry_at_;
    static constexpr std::chrono::seconds unreconstructable_retry_cooldown{30};
    // Record a confirmed reconstruction failure for `hash`: arms the cooldown
    // and, on the first failure of an episode, logs one WARN naming the exact
    // break in the local chain (see diagnose_unreconstructable_locked()).
    // Returns that diagnosis so the caller's exception can carry it.
    std::string flag_unreconstructable_locked(const Hash256& hash, Clock::time_point now,
                                              std::string_view context) const;
    // Walk `hash`'s delta chain exactly as materialized_locked() does and name
    // the first thing that stops it: not indexed, parent absent, succession
    // rule violated, cycle, unreadable frame, or replay hash mismatch.
    std::string diagnose_unreconstructable_locked(const Hash256& hash) const;
    // Test-only: force historical_locked()/materialized_locked() to report a
    // specific hash as unreconstructable without needing to fabricate genuine
    // on-disk corruption or a dependency-chain hole, so the retry-cooldown
    // behaviour above can be exercised directly and deterministically.
    mutable std::function<bool(const Hash256&)> force_unreconstructable_for_tests_;
    // Only ever populated with a proof that has already validated against
    // committed_ at load time (see load_checkpoint_proof()) -- nullopt means
    // "no valid proof," never "assume the worst possible one."
    std::optional<HistoryCheckpointProof> checkpoint_proof_;
    std::optional<MetadataHistoryEntry> pending_history_;
    bool pending_recovered_{};
    bool mutation_sequence_loaded_{};
    uint64_t mutation_sequence_{};
    size_t journal_records_{};
    uint64_t journal_bytes_{};
    size_t history_records_{};
    uint64_t history_bytes_{};
    bool recovery_required_{};
    NamespaceDeltaApplier namespace_applier_;
    bool accept_pristine_genesis_authority_{true};
    mutable std::map<Hash256, MaterializedHistoryEntry> materialized_history_;
    mutable uint64_t materialized_history_clock_{};
    mutable uint64_t materialized_history_bytes_{};
    uint64_t materialized_history_limit_bytes_{};
    mutable std::atomic_uint64_t historical_requests_{};
    mutable std::atomic_uint64_t historical_reconstructions_{};
    mutable std::atomic_uint64_t historical_deltas_applied_{};
    mutable std::atomic_uint64_t materialization_cache_hits_{};
    mutable std::atomic_uint64_t materialization_cache_misses_{};
    mutable std::atomic_uint64_t materialization_cache_evictions_{};
    std::atomic_uint64_t accepted_head_persistence_writes_{};
    std::atomic_uint64_t accepted_head_persistence_bytes_{};
    std::atomic_uint64_t accepted_head_persistence_failures_{};
    void persist(const std::filesystem::path&, const MetadataRecord&);
    std::optional<MetadataRecord> load(const std::filesystem::path&) const;
    void append_journal(uint8_t, const MetadataRecord&, std::span<const uint8_t> = {});
    void load_journal();
    Bytes encode_history_frame(const MetadataHistoryEntry&) const;
    MetadataHistoryEntry read_history_entry(const HistoryIndexEntry&) const;
    static HistoryIndexEntry index_history_entry(const MetadataHistoryEntry&, uint64_t, uint64_t);
    void write_history_frame(std::span<const uint8_t>) const;
    void append_history(const MetadataHistoryEntry&);
    void load_history();
    void load_heads();
    void persist_heads_locked();
    void load_checkpoint_proof();
    void persist_checkpoint_proof_locked();
    void ensure_history_root(const MetadataRecord&);
    void migrate_legacy_head_locked();
    bool prune_accepted_heads_locked();
    bool accepted_head_is_ancestor_locked(const Hash256&, const Hash256&) const;
    bool acceptance_matches_record_policy_locked(const MetadataAcceptance&,
                                                 const MetadataMaterialization&) const;
    bool legacy_write_api_allowed_locked() const;
    void set_legacy_committed_head_locked(const MetadataRecord&);
    void refresh_materialized_head_locked();
    bool refresh_materialized_head_in_memory_locked();
    MetadataHistoryEntry history_for_current(std::span<const uint8_t> delta = {});
    std::shared_ptr<const MetadataMaterialization>
    cache_materialization_locked(const MetadataRecord&,
                                 std::shared_ptr<const MetadataSnapshot> = {},
                                 uint64_t resident_bytes = 0) const;
    std::shared_ptr<const MetadataMaterialization> materialized_locked(const Hash256&) const;
    std::optional<MetadataRecord> historical_locked(const Hash256&) const;
    bool history_is_ancestor_locked(const Hash256&, const Hash256&) const;
    std::optional<Hash256> history_common_ancestor_locked(const Hash256&, const Hash256&) const;
    void compact_if_needed();
    void reset_checkpoint(const MetadataRecord&);
    void reset_checkpoint_journal_locked();
    void recover_from_seed(const MetadataRecord&, const std::string&);

  public:
    MetadataReplica(std::filesystem::path, std::array<uint8_t, 32>,
                    std::optional<MetadataRecord> recovery_seed = {},
                    bool accept_pristine_genesis_authority = true,
                    uint64_t materialization_cache_limit_bytes = 128ULL * 1024ULL * 1024ULL);
    MetadataRecord current() const;
    MetadataRecord committed() const;
    MetadataIdentity current_identity() const;
    MetadataIdentity committed_identity() const;
    uint64_t generation() const;
    uint64_t committed_generation() const;
    bool recovery_required() const;
    void mark_recovered();

    // How this replica replays a delta over a tree-backed namespace. Set by
    // MetadataManager once it has a store; until then a tree-backed history
    // entry is refused rather than misapplied. Set once during construction of
    // the node, before any replay can run.
    void set_namespace_delta_applier(NamespaceDeltaApplier applier) {
        namespace_applier_ = std::move(applier);
    }
    uint64_t reserve_mutation_sequence(uint64_t observed_floor);
    bool cas(uint64_t, const Hash256&, std::span<const uint8_t>, MetadataRecord*);
    bool cas_delta(uint64_t, const Hash256&, std::span<const uint8_t>, MetadataRecord*);
    bool install_committed_delta(uint64_t, const Hash256&, std::span<const uint8_t>,
                                 const MetadataRecord&);
    bool seed(const MetadataRecord&);
    bool remember_committed(const MetadataRecord&);
    bool remember_current_committed(uint64_t, const Hash256&);
    std::optional<MetadataHistoryEntry> history_entry(const Hash256&) const;
    std::optional<MetadataHistoryLinks> history_links(const Hash256&) const;
    // The immutable record for `hash` as a self-contained full-body history
    // entry, materialized locally -- what a peer needs to re-anchor a head it
    // has indexed but can no longer reconstruct (its own stored frame may be
    // the very delta it cannot replay). nullopt if this replica cannot
    // materialize it either.
    std::optional<MetadataHistoryEntry> full_history_record(const Hash256&) const;
    // Live repair of an accepted head this replica cannot reconstruct: append
    // `entry` (a full body whose hash/generation/previous match the local
    // index entry, or a brand-new record) as a new full anchor and point the
    // index at it, superseding the unreplayable frame in place. History is
    // append-only, so the old frame stays on disk; load_history() prefers the
    // full frame when it meets both. Clears the head's cooldown, re-verifies
    // its acceptance certificate against the repaired record, and refreshes
    // the materialized head. Returns true when the head is reconstructible
    // afterwards (including when it already was), false when `entry` is not
    // a valid full record for that hash.
    bool reanchor_history(const MetadataHistoryEntry& entry);
    // Accepted heads currently flagged as unreconstructable (in cooldown, or
    // carried over from load_heads()). The live-repair driver pulls these.
    std::vector<Hash256> unreconstructable_heads() const;
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
    std::shared_ptr<const MetadataMaterialization> materialized(const Hash256&) const;
    MetadataReplicaDiagnostics diagnostics() const;
    void compact();
    // Once every durably-known node is directly reachable and metadata repair
    // has converged the cluster, old ancestry is no longer needed to reconcile
    // an unseen branch. Re-root history at the sole committed accepted head so
    // disk and startup RSS do not grow with lifetime mutation count.
    bool compact_history_if_safe(size_t record_threshold = 256,
                                 uint64_t byte_threshold = 64ULL * 1024 * 1024);

    // The current durable checkpoint proof, whatever its status. A freshly
    // constructed replica only ever loads one that already validated against
    // its committed head at load time (status == committed AND floor_hash ==
    // committed().hash -- see load_checkpoint_proof()); record_checkpoint_ack()
    // can subsequently populate this with a merely-acked, not-yet-authoritative
    // proposal. This accessor does not filter for authority: a caller that
    // means to use the result to justify anything (e.g. the
    // accepted_head_is_ancestor_locked() checkpoint-floor trust rule) must
    // itself check status == committed AND floor_hash == committed().hash --
    // never guess or fall back to a weaker check.
    std::optional<HistoryCheckpointProof> checkpoint_proof() const;
    // Durably record this replica's agreement to a proposal, before an RPC
    // reply is sent. Returns false (and records nothing) if this replica's own
    // accepted-head set is not exactly {floor_hash} right now -- e.g. it has
    // already advanced past the proposed floor via an ordinary peer commit
    // received concurrently with the round. Acking a stale floor unconditionally
    // let a proposer that had itself fallen behind convince every other replica
    // to durably agree to a floor those replicas had already superseded, so the
    // proposer alone compacted its own history to that stale point -- silently
    // discarding the only remaining shared ancestry other replicas needed for
    // ordinary two-parent reconciliation, with no automatic recovery possible
    // afterward. On success, always status == acked regardless of what the
    // caller passes; supersedes any prior record for a different (floor_hash, epoch).
    bool record_checkpoint_ack(HistoryCheckpointProof proposal);
    // Promote a matching acked record to committed. Returns false (no-op,
    // nothing promoted) if there is no acked record for exactly this
    // (floor_hash, epoch) -- e.g. it was never proposed here, or a different
    // proposal superseded it.
    bool record_checkpoint_commit(const Hash256& floor_hash, const Hash256& epoch);
    void set_force_unreconstructable_for_tests(std::function<bool(const Hash256&)> hook) {
        std::lock_guard lock(m_);
        force_unreconstructable_for_tests_ = std::move(hook);
    }
};
std::string normalize_path(const std::string&);
std::string parent_path(const std::string&);
std::string base_name(const std::string&);
int64_t wall_time_ns();
} // namespace macha
