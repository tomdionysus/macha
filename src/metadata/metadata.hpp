// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "contract/thread_safety.hpp"
#include "crypto.hpp"
#include "torrent/torrent_request.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
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
class Writer;
class Reader;

// One mutation of one author. Zero is no dot.
struct MetadataDot {
    NodeId author{};
    uint64_t sequence{};
    explicit operator bool() const noexcept { return sequence != 0; }
    auto operator<=>(const MetadataDot&) const = default;
};
// Whether a head with this clock has incorporated the mutation.
inline bool clock_covers(const std::map<NodeId, uint64_t>& clock, const MetadataDot& dot) {
    const auto found = clock.find(dot.author);
    return found != clock.end() && found->second >= dot.sequence;
}

// Where an entry came from, which is what lets two heads be merged without
// their common ancestor. An entry written before provenance was kept has
// none (all three empty) and is encoded exactly as it always was.
struct EntryProvenance {
    // Set when the file or directory is created and carried by a rename.
    NodeId file_id{};
    // Its last change of content or attributes.
    MetadataDot content;
    // When it came to be at this path, by creation or rename.
    MetadataDot name;
    bool empty() const noexcept { return file_id == NodeId{} && !content && !name; }
    auto operator<=>(const EntryProvenance&) const = default;
};

struct FsEntry {
    EntryType type{EntryType::file};
    uint32_t mode{0644}, uid{}, gid{};
    uint64_t size{};
    int64_t ctime_ns{}, mtime_ns{};
    uint64_t version{1};
    std::vector<ExtentRef> extents;
    EntryProvenance provenance;
    auto operator<=>(const FsEntry&) const = default;
};
// The identity of an entry that has none, from the path it sits at: two
// branches that first touch the same such file agree on it.
NodeId legacy_file_id(std::string_view path);
// The wire form of an entry's type byte carries this bit when provenance
// follows; a reader that does not know it refuses the entry.
inline constexpr uint8_t entry_type_with_provenance = 0x80;
void encode_entry_provenance(Writer&, const EntryProvenance&);
EntryProvenance decode_entry_provenance(Reader&);

struct GarbageRef {
    ObjectId id{};
    // Wall-clock retirement time. Zero (legacy formats) is stamped by
    // maintenance before the tombstone becomes collectable.
    int64_t retired_at_ns{};
    // Stops a stale maintenance decision pruning a later retirement of the
    // same object. Empty for legacy tombstones.
    NodeId retirement_id{};
    auto operator<=>(const GarbageRef&) const = default;
};

struct MetadataConflict {
    MetadataConflictKind kind{MetadataConflictKind::namespace_entry};
    // Which value the merge left in place. Set by a merge of two heads: the
    // left alternative, which is the later of the two (or the directory that
    // had to stay). Unset in a record made over a base: the base.
    bool later_installed{};
    // With `later_installed`: the dot of the value left in place, which is
    // how a later decision that keeps the same value is still seen.
    MetadataDot installed_dot;
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
    // Local-only mirrors of NodeTelemetry for Status's last-known fallback;
    // not in the wire codec, so never replicated.
    std::string api_endpoint;
    std::string node_name;
    auto operator<=>(const PersistedNodeStatus&) const = default;
};

struct MetadataSnapshot {
    // Legacy voter list: not authority, cleared on the first policy
    // transition; kept in the SM10/DLT4 encoding so old state stays readable.
    std::vector<NodeId> metadata_voters;
    uint32_t data_replication{};
    uint64_t extent_size{};
    // Durability floor under which this commit is valid. Zero means a legacy
    // snapshot not yet transitioned.
    uint32_t metadata_write_replicas_required{};
    // Roster used only while establishing retention claims for a legacy
    // namespace; never an authority list, cleared once the baseline completes.
    // Silently forgetting an offline participant would make destructive GC unsafe.
    std::set<NodeId> metadata_participants;
    // Causal stability horizon for destructive retention release: an accepted
    // commit every participant reached before the horizon advanced. No
    // participant may author behind it. Zero means no horizon yet.
    Hash256 metadata_branch_floor{};
    // Every reachable object holds a durable retention claim. Destructive
    // reachability GC is refused until this is true.
    bool retention_baseline_complete{};
    // Highest mutation sequence incorporated per node: an idempotency clock
    // recognising an accepted mutation after retry or reconciliation.
    std::map<NodeId, uint64_t> mutation_sequences;
    // `mutation_sequences` as it stood when this lineage was first written
    // by code that keeps entry provenance: every mutation that could have
    // created an entry without provenance lies within it. Unset until then;
    // a merge joins by max.
    std::optional<std::map<NodeId, uint64_t>> legacy_clock;
    // The mutation that last set or cleared `catalogue_root`. Carried with
    // the legacy clock; none on a root set before that.
    MetadataDot catalogue_dot;
    std::optional<ObjectId> catalogue_root;
    // SM14: the namespace tree root, carried instead of `entries`, never
    // alongside; both encoders refuse the mix. See namespace_tree.hpp.
    std::optional<ObjectId> namespace_root;
    std::map<std::string, FsEntry> entries;
    std::vector<GarbageRef> garbage;
    // One coalesced last-known record per node; not a metrics history.
    std::map<NodeId, PersistedNodeStatus> node_status;
    // Endpoint->NodeId tombstones: suppress a stale association; the endpoint
    // may later authenticate as a different NodeId.
    std::map<std::string, IdentityAssociationReset, std::less<>> identity_resets;
    // Extra parents of a reconciliation commit; MetadataRecord::previous stays
    // the primary parent.
    std::vector<Hash256> merge_parents;
    // Each conflict keeps its alternatives while the one its merge installed
    // stays in place; resolution is a later mutation.
    std::map<std::string, MetadataConflict, std::less<>> conflicts;
    // Torrents requested for download, by request id (SM15/SM16).
    std::map<std::string, TorrentRequest, std::less<>> torrent_requests;
};
// Immutable bytes with value semantics and shared storage, so copying a record
// with a payload of hundreds of MB is cheap. Assignment allocates new storage;
// aliases never observe mutation.
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

// Evidence a commit reached the write floor: `replicas` durably acknowledged
// it. `required == 0` marks a migrated legacy checkpoint, authoritative itself.
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

// The ancestry links a body needs in order to be materialized (a subset of
// all its links).
std::vector<Hash256> metadata_history_materialization_dependencies(
    const MetadataHistoryEntry&);

// The one succession rule for a delta body over its primary parent, shared by
// every writer and reader; writers and readers must agree or a written commit
// becomes unreadable. A merge is numbered max(parents)+1, so a delta may sit
// many generations above its primary; the hash binds the generation, so
// monotonicity suffices.
constexpr bool metadata_delta_succession_valid(uint64_t parent_generation,
                                               uint64_t child_generation) noexcept {
    return parent_generation < child_generation;
}

struct MetadataMergeResult {
    MetadataSnapshot snapshot;
    size_t conflicts_created{};
    // Dropped by prune_superseded_conflicts().
    size_t conflicts_superseded{};
};

enum class CatalogueDelta : uint8_t { unchanged = 0, clear = 1, set = 2 };

// Deterministic mutation from one canonical snapshot to the next, used on the
// wire and in the journal; full records remain the repair primitive.
struct MetadataDelta {
    std::map<NodeId, uint64_t> mutation_sequences;
    std::map<std::string, FsEntry> upsert_entries;
    std::vector<std::string> erase_entries;
    std::vector<ObjectId> erase_garbage;
    std::vector<GarbageRef> upsert_garbage;
    std::map<NodeId, PersistedNodeStatus> upsert_node_status;
    std::map<std::string, IdentityAssociationReset, std::less<>> upsert_identity_resets;
    // DLT9: torrent requests written whole; tombstones erased after their grace.
    std::map<std::string, TorrentRequest, std::less<>> upsert_torrent_requests;
    std::vector<std::string> erase_torrent_requests;
    // Whole-set replacements of the branch topology; both unset encodes as
    // DLT5. DLT6 replaces both or neither; DLT7 flags each, so only the set
    // that changed is sent.
    std::optional<std::vector<Hash256>> replace_merge_parents;
    std::optional<std::map<std::string, MetadataConflict, std::less<>>>
        replace_conflicts;
    // DLT7: sort tombstones by ObjectId after the edits, so a merge over an
    // append-ordered parent still encodes as a delta.
    bool canonical_garbage{};
    // DLT8: an entry whose extent table only grew is carried as the appended
    // extents plus new attributes, keeping per-quantum publication deltas small.
    struct EntryAppend {
        uint64_t size{};
        int64_t mtime_ns{};
        int64_t ctime_ns{};
        uint64_t version{};
        uint32_t base_extents{};        // extents the entry must already hold
        std::vector<ExtentRef> extents; // appended after them
        // DLT10: the appending mutation, which becomes the entry's content
        // dot. Empty in a delta written without provenance.
        MetadataDot content;
    };
    std::map<std::string, EntryAppend> append_entries;
    CatalogueDelta catalogue{CatalogueDelta::unchanged};
    std::optional<ObjectId> catalogue_root;
    // DLT10: sets the snapshot's legacy clock, once.
    std::optional<std::map<NodeId, uint64_t>> set_legacy_clock;
    // DLT10: the catalogue root's dot, when the mutation moves it.
    std::optional<MetadataDot> set_catalogue_dot;
};
// Applies one append to the entry at `path`, as both namespace forms do.
// Throws DecodeError when the entry is not the file the append was made for.
void apply_entry_append(FsEntry&, std::string_view path, const MetadataDelta::EntryAppend&);
// Gives an entry written by a mutation its provenance. `prior` is the entry
// the parent head holds at `path`; `dot` is the mutation. The entry keeps
// the identity and name of a file it replaces in place, takes a new name
// dot when it is new at the path, and a new content dot when it differs
// from what was there or arrives with none.
void stamp_entry_provenance(FsEntry&, std::string_view path, const FsEntry* prior,
                            const MetadataDot& dot);

// Record `after` as an append when `before`'s extents are a strict prefix of
// its own, else as a whole-entry upsert.
void record_entry_change(MetadataDelta& delta, const std::string& path, const FsEntry* before,
                         const FsEntry& after);

// Strictly ordered by ObjectId, no duplicates.
bool garbage_is_canonical(const std::vector<GarbageRef>&);
// Stable sort into canonical order.
void canonicalise_garbage(std::vector<GarbageRef>&);
// Type, mode, ownership, size and extents equal; times and version may
// differ. Two writers publishing the same media are not in conflict.
bool same_content(const FsEntry&, const FsEntry&);
// The alternative a conflict's merge left in place: the left one when
// `later_installed`, the base otherwise.
std::optional<FsEntry> conflict_installed_entry(const MetadataConflict&);
std::optional<ObjectId> conflict_installed_catalogue_root(const MetadataConflict&);
// Of two concurrent values, the one a merge leaves in place: the later
// modification time, then the greater entry.
const FsEntry& later_entry(const FsEntry&, const FsEntry&);
// Drop every conflict whose subject no longer holds the value its merge left
// in place: the later mutation is the resolution. Returns the count.
// `lookup` reads the snapshot's namespace when it is not held as a map.
using NamespaceLookup = std::function<std::optional<FsEntry>(const std::string& path)>;
size_t prune_superseded_conflicts(MetadataSnapshot&, const NamespaceLookup& lookup = {});
Bytes encode_snapshot(const MetadataSnapshot&);
MetadataSnapshot decode_snapshot(std::span<const uint8_t>);
// SM14: the non-entry fields plus `namespace_root`, so the payload size is
// independent of the library. Requires `namespace_root` set and `entries`
// empty; `detach_namespace` (namespace_tree.hpp) produces that form.
Bytes encode_snapshot_v14(const MetadataSnapshot&);
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
// Applies a delta to a tree-backed namespace, returning the new root. Supplied
// by the store holder; this layer has no store. Unset, a tree-backed delta is
// refused rather than applied to the (unread) entry map.
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
using NamespaceEntries = std::map<std::string, FsEntry>;
// The merge of two heads from the heads alone: each entry's provenance and
// each head's clock tell "removed there" from "never seen there" and "newer"
// from "concurrent". A pure function of the two heads, so every reconciler
// computes the same record. `merge_metadata_heads` takes materialised
// namespaces; `..._over` takes each head's entries at the paths to consider
// (every path that differs, each standing conflict's subject and the
// directories above them) and returns the merged entries at those paths. The
// snapshots' own entry maps and roots are not read.
MetadataMergeResult merge_metadata_heads(const MetadataSnapshot& left,
                                         const MetadataSnapshot& right, const Hash256& left_head,
                                         const Hash256& right_head);
MetadataMergeResult merge_metadata_heads_over(const MetadataSnapshot& left,
                                              const MetadataSnapshot& right,
                                              const NamespaceEntries& left_entries,
                                              const NamespaceEntries& right_entries,
                                              const Hash256& left_head,
                                              const Hash256& right_head);
// Reachability roots held by unresolved conflicts, which GC must protect.
std::set<ObjectId> metadata_conflict_extent_roots(const MetadataSnapshot&);
std::set<ObjectId> metadata_catalogue_root_set(const MetadataSnapshot&);
// Identity of namespace paths and file content; excludes catalogue, garbage,
// voter state and non-content stat fields.
Hash256 metadata_namespace_signature(const MetadataSnapshot&);

// Decoded in-memory cost of a snapshot, as the materialization cache charges it.
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
    // Conservative deep weight of record and snapshot, computed off m_.
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
    std::filesystem::path mutation_sequence_p_;
    std::filesystem::path author_p_;
    // Present from the moment the head set is replaced until the next
    // mutation takes a new author id.
    std::filesystem::path author_chain_broken_p_;
    std::filesystem::path set_aside_p_;
    std::filesystem::path recovery_p_;
    std::array<uint8_t, 32> key_;
    // Held across journal appends, heads and checkpoint writes and their
    // fsyncs, and history reads.
    mutable IoMutex m_;
    // Serialises durable mutations whose history append and fsync run without
    // m_; held across them.
    mutable IoMutex durable_mutation_m_ MACHA_ACQUIRED_BEFORE(m_);
    // Serialises cache-miss computation without blocking m_; held across
    // history reads.
    mutable IoMutex materialization_compute_m_ MACHA_ACQUIRED_BEFORE(m_);
    MetadataRecord cur_ MACHA_GUARDED_BY(m_);
    MetadataRecord committed_ MACHA_GUARDED_BY(m_);
    // committed_.generation, readable without m_; set_committed_locked keeps
    // the two together.
    std::atomic_uint64_t committed_generation_{};
    void set_committed_locked(MetadataRecord record) MACHA_REQUIRES(m_) {
        committed_ = std::move(record);
        committed_generation_.store(committed_.generation, std::memory_order_release);
    }
    // Payloads live in history.log; memory holds ancestry and frame offsets.
    std::map<Hash256, HistoryIndexEntry> history_ MACHA_GUARDED_BY(m_);
    std::map<Hash256, MetadataAcceptance> accepted_heads_ MACHA_GUARDED_BY(m_);
    // Accepted heads this node cannot merge with its own now, with their
    // generations. They stay accepted and are still offered to peers, but
    // never become the committed record. Not persisted: a restart tries the
    // merge again.
    std::map<Hash256, uint64_t> set_aside_ MACHA_GUARDED_BY(m_);
    // When each head was first set aside, on this node's wall clock; kept
    // across restarts so a head whose content never arrives is dropped after
    // the absence horizon rather than held for ever.
    std::map<Hash256, uint64_t> set_aside_since_ MACHA_GUARDED_BY(m_);
    bool set_aside_since_loaded_ MACHA_GUARDED_BY(m_){};
    void load_set_aside_since_locked() MACHA_REQUIRES(m_);
    void persist_set_aside_since_locked() MACHA_REQUIRES(m_);
    std::atomic_uint64_t set_aside_generation_{};
    void note_set_aside_locked() MACHA_REQUIRES(m_);
    // Per-hash retry cooldown for an accepted head that fails to reconstruct,
    // so callers that catch and retry cannot spin. Between attempts the head
    // is treated as absent and the replica serves its last good state.
    mutable std::map<Hash256, Clock::time_point>
        unreconstructable_head_retry_at_ MACHA_GUARDED_BY(m_);
    static constexpr std::chrono::seconds unreconstructable_retry_cooldown{30};
    // Arms the cooldown, logs one WARN per episode, and returns the diagnosis
    // for the caller's exception.
    std::string flag_unreconstructable_locked(const Hash256& hash, Clock::time_point now,
                                              std::string_view context) const MACHA_REQUIRES(m_);
    // Walk the delta chain as materialized_locked() does and name the first
    // break: not indexed, parent absent, succession, cycle, unreadable frame,
    // or replay hash mismatch.
    std::string diagnose_unreconstructable_locked(const Hash256& hash) const MACHA_REQUIRES(m_);
    std::optional<MetadataHistoryEntry> pending_history_ MACHA_GUARDED_BY(m_);
    bool pending_recovered_ MACHA_GUARDED_BY(m_){};
    // This node as an author of mutations. One author's commits form a
    // chain: each extends a head that already carries the one before. When
    // that cannot be shown (state restored from an older copy, a recovery
    // from a seed, a re-root) the node takes a new author id and starts
    // again at 1, so a head's clock entry for an author always means "every
    // mutation of that author up to here".
    struct Author {
        NodeId id{};
        // Highest sequence handed out, and highest this node has had accepted.
        uint64_t reserved{};
        uint64_t accepted{};
        // Earlier ids of this node, newest first.
        std::vector<NodeId> past;
    };
    bool author_loaded_ MACHA_GUARDED_BY(m_){};
    Author author_ MACHA_GUARDED_BY(m_);
    void load_author_locked(const NodeId& node_id, const std::map<NodeId, uint64_t>& head_clock)
        MACHA_REQUIRES(m_);
    void persist_author_locked() MACHA_REQUIRES(m_);
    void rotate_author_locked(std::string_view why) MACHA_REQUIRES(m_);
    size_t journal_records_ MACHA_GUARDED_BY(m_){};
    uint64_t journal_bytes_ MACHA_GUARDED_BY(m_){};
    size_t history_records_ MACHA_GUARDED_BY(m_){};
    uint64_t history_bytes_ MACHA_GUARDED_BY(m_){};
    bool recovery_required_ MACHA_GUARDED_BY(m_){};
    // Edits a tree-backed namespace; fixed for the replica's lifetime.
    const NamespaceDeltaApplier namespace_applier_;
    const bool accept_pristine_genesis_authority_{true};
    mutable std::map<Hash256, MaterializedHistoryEntry> materialized_history_ MACHA_GUARDED_BY(m_);
    mutable uint64_t materialized_history_clock_ MACHA_GUARDED_BY(m_){};
    mutable uint64_t materialized_history_bytes_ MACHA_GUARDED_BY(m_){};
    const uint64_t materialized_history_limit_bytes_{};
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
    // Construction calls the load_* functions and recover_from_seed() before the
    // replica is shared; the analysis does not check constructors.
    void append_journal(uint8_t, const MetadataRecord&, std::span<const uint8_t> = {})
        MACHA_REQUIRES(m_);
    void load_journal() MACHA_REQUIRES(m_);
    Bytes encode_history_frame(const MetadataHistoryEntry&) const;
    MetadataHistoryEntry read_history_entry(const HistoryIndexEntry&) const;
    static HistoryIndexEntry index_history_entry(const MetadataHistoryEntry&, uint64_t, uint64_t);
    void write_history_frame(std::span<const uint8_t>) const;
    void append_history(const MetadataHistoryEntry&) MACHA_REQUIRES(m_);
    void load_history() MACHA_REQUIRES(m_);
    void load_heads() MACHA_REQUIRES(m_);
    void persist_heads_locked() MACHA_REQUIRES(m_);
    void ensure_history_root(const MetadataRecord&) MACHA_REQUIRES(m_);
    void migrate_legacy_head_locked() MACHA_REQUIRES(m_);
    bool prune_accepted_heads_locked() MACHA_REQUIRES(m_);
    bool accepted_head_is_ancestor_locked(const Hash256&, const Hash256&) const MACHA_REQUIRES(m_);
    bool acceptance_matches_record_policy_locked(const MetadataAcceptance&,
                                                 const MetadataMaterialization&) const
        MACHA_REQUIRES(m_);
    bool legacy_write_api_allowed_locked() const MACHA_REQUIRES(m_);
    void set_legacy_committed_head_locked(const MetadataRecord&) MACHA_REQUIRES(m_);
    void refresh_materialized_head_locked() MACHA_REQUIRES(m_);
    bool refresh_materialized_head_in_memory_locked() MACHA_REQUIRES(m_);
    MetadataHistoryEntry history_for_current(std::span<const uint8_t> delta = {})
        MACHA_REQUIRES(m_);
    std::shared_ptr<const MetadataMaterialization>
    cache_materialization_locked(const MetadataRecord&,
                                 std::shared_ptr<const MetadataSnapshot> = {},
                                 uint64_t resident_bytes = 0) const MACHA_REQUIRES(m_);
    std::shared_ptr<const MetadataMaterialization> materialized_locked(const Hash256&) const
        MACHA_REQUIRES(m_);
    std::optional<MetadataRecord> historical_locked(const Hash256&) const MACHA_REQUIRES(m_);
    bool history_is_ancestor_locked(const Hash256&, const Hash256&) const MACHA_REQUIRES(m_);
    std::optional<Hash256> history_common_ancestor_locked(const Hash256&, const Hash256&) const
        MACHA_REQUIRES(m_);
    void compact_if_needed() MACHA_REQUIRES(m_);
    void reset_checkpoint(const MetadataRecord&) MACHA_REQUIRES(m_);
    void reset_checkpoint_journal_locked() MACHA_REQUIRES(m_);
    void recover_from_seed(const MetadataRecord&, const std::string&) MACHA_REQUIRES(m_);

  public:
    MetadataReplica(std::filesystem::path, std::array<uint8_t, 32>,
                    std::optional<MetadataRecord> recovery_seed = {},
                    bool accept_pristine_genesis_authority = true,
                    uint64_t materialization_cache_limit_bytes = 128ULL * 1024ULL * 1024ULL,
                    NamespaceDeltaApplier namespace_applier = {});
    MetadataRecord current() const;
    MetadataRecord committed() const;
    MetadataIdentity current_identity() const;
    MetadataIdentity committed_identity() const;
    uint64_t generation() const;
    // Lock-free; any thread.
    uint64_t committed_generation() const noexcept;
    bool recovery_required() const;
    void mark_recovered();

    // The dot for this node's next mutation on a head with `head_clock`:
    // its author id (the node id until the chain is first broken) and the
    // next sequence, durable before it returns.
    MetadataDot reserve_mutation_dot(const NodeId& node_id,
                                     const std::map<NodeId, uint64_t>& head_clock);
    // A commit carrying this dot was accepted here.
    void note_author_accepted(const MetadataDot&);
    // This node's author ids, the current one first.
    std::vector<NodeId> author_ids(const NodeId& node_id,
                                   const std::map<NodeId, uint64_t>& head_clock);
    bool cas(uint64_t, const Hash256&, std::span<const uint8_t>, MetadataRecord*);
    bool cas_delta(uint64_t, const Hash256&, std::span<const uint8_t>, MetadataRecord*);
    bool install_committed_delta(uint64_t, const Hash256&, std::span<const uint8_t>,
                                 const MetadataRecord&);
    // Installs `record` as the sole accepted committed head, quarantining the
    // prior state as recover_from_seed does. Unlike a seed, this grants
    // authority, which comes from the operator re-rooting every node onto the
    // same converged record. Only correct offline, cluster stopped, on every
    // node; `macha-namespace-migrate` is the only caller. `witnesses` becomes
    // the certificate's replica list and must cover the write floor.
    bool install_migrated_head(const MetadataRecord&, const std::vector<NodeId>& witnesses,
                               const std::string& reason);
    bool seed(const MetadataRecord&);
    bool remember_committed(const MetadataRecord&);
    bool remember_current_committed(uint64_t, const Hash256&);
    std::optional<MetadataHistoryEntry> history_entry(const Hash256&) const;
    std::optional<MetadataHistoryLinks> history_links(const Hash256&) const;
    // `hash` as a full-body entry, for a peer to re-anchor a head it cannot
    // reconstruct. nullopt if this replica cannot materialize it either.
    std::optional<MetadataHistoryEntry> full_history_record(const Hash256&) const;
    // Live repair: append full-body `entry` as a new anchor and index it over
    // the unreplayable frame (load_history() prefers the full frame). Clears
    // the cooldown and re-verifies the certificate. True if the head is
    // reconstructible afterwards; false if `entry` is not a valid full record.
    bool reanchor_history(const MetadataHistoryEntry& entry);
    // Accepted heads flagged unreconstructable, for the live-repair driver.
    std::vector<Hash256> unreconstructable_heads() const;
    // Sets an accepted head aside; false when it is the only head not set
    // aside, since a node always keeps a head to work on.
    bool set_aside(const Hash256&, uint64_t now_unix_ms);
    // Drops every head that has been set aside for `horizon` and returns how
    // many went: whatever held its content is not coming back.
    size_t expire_set_aside(uint64_t now_unix_ms, std::chrono::milliseconds horizon);
    void clear_set_aside();
    // The highest generation among heads set aside, zero when none is. An
    // atomic read.
    uint64_t set_aside_generation() const noexcept {
        return set_aside_generation_.load(std::memory_order_acquire);
    }
    // accepted_heads() without those set aside.
    std::vector<MetadataRecord> usable_heads() const;
    bool import_history(const MetadataHistoryEntry&);
    bool history_contains(const Hash256&) const;
    bool store_commit(const MetadataRecord&, std::span<const uint8_t> delta = {});
    // `heads_changed` is decided under the lock; announce head changes from it,
    // not by diffing copies taken around the call, which races.
    bool accept_commit(const MetadataAcceptance&, bool* heads_changed = nullptr);
    std::vector<MetadataAcceptance> accepted_head_certificates() const;
    std::vector<MetadataRecord> accepted_heads() const;
    std::optional<MetadataAcceptance> acceptance(const Hash256&) const;
    bool history_is_ancestor(const Hash256&, const Hash256&) const;
    std::optional<Hash256> history_common_ancestor(const Hash256&, const Hash256&) const;
    std::optional<MetadataRecord> historical(const Hash256&) const;
    std::shared_ptr<const MetadataMaterialization> materialized(const Hash256&) const;
    MetadataReplicaDiagnostics diagnostics() const;
    void compact();
    // Re-roots history at the sole accepted head once it is the committed
    // record and a threshold is met, so disk and startup RSS do not grow
    // with mutation count.
    bool compact_history_if_safe(size_t record_threshold = 256,
                                 uint64_t byte_threshold = 64ULL * 1024 * 1024);
};
std::string normalize_path(const std::string&);
std::string parent_path(const std::string&);
std::string base_name(const std::string&);
int64_t wall_time_ns();
} // namespace macha
