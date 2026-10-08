// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "storage/sealed_journal.hpp"
#include "types.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace macha {

// One class's object ledger on disk: every object id this node records, each
// with a value of up to `value_max` bytes, in a radix-256 trie keyed by the
// id's bytes.
//
// Its shape depends only on the ids: a node holding more than `leaf_max`
// records below it is interior, with one child per next byte; otherwise it
// is a leaf of sorted records. Each node's hash covers what is below it, so
// two tries over the same records hash the same, whatever order they were
// written in.
//
// Changes are written to a sealed journal and fsynced before they apply, and
// folded into the node file (copy-on-write: changed nodes are appended, then
// a new root is published) once the journal reaches `checkpoint_bytes`.
// Opening reads the published root and replays the journal: its cost is the
// journal, never the number of records. Saved nodes are read from disk as
// lookups reach them and kept in a cache of `cache_bytes`; the root and
// unsaved nodes stay in memory. When the node file holds more superseded
// nodes than live ones it is rewritten.
//
// Not thread-safe: its owner serialises every call, const ones included.
// Snapshots are read from any thread, alongside the owner.
class ObjectTrie {
    struct Node;
    using NodePtr = std::shared_ptr<const Node>;
    static constexpr uint64_t unsaved = ~0ULL;
    // A slot in an interior node, or the root: where the subtree is saved,
    // what it holds, and the subtree itself while it is unsaved.
    struct Child {
        uint64_t offset{unsaved};
        uint32_t size{};
        uint64_t count{};
        Hash256 hash{};
        NodePtr node;
        bool empty() const noexcept { return count == 0; }
    };

  public:
    using Record = std::pair<ObjectId, Bytes>;

  private:
    struct Node {
        uint8_t depth{};
        bool leaf{true};
        std::vector<Record> records;
        std::vector<Child> children; // 256 for an interior node
        uint64_t count{};
        Hash256 hash{};
    };

  public:
    struct Options {
        size_t cache_bytes = 64ULL * 1024 * 1024;
        uint64_t checkpoint_bytes = 1ULL * 1024 * 1024;
        // The node file is rewritten once its superseded bytes exceed both
        // its live bytes and this.
        uint64_t rewrite_floor_bytes = 16ULL * 1024 * 1024;
    };
    static constexpr size_t leaf_max = 128;
    static constexpr size_t value_max = 16 * 1024;

    // A record set to `value`, or erased when it has none.
    struct Change {
        ObjectId id;
        std::optional<Bytes> value;
    };

    // Saved nodes read from disk, shared by the trie and its snapshots,
    // least recently used out first. Thread safe.
    class NodeCache;

    // Every record as they were when it was taken: later changes,
    // checkpoints and rewrites leave it as it is (it reads its node file
    // generation through its own handle). Thread safe; cheap to copy.
    class Snapshot {
      public:
        Snapshot() = default;
        std::optional<Bytes> get(const ObjectId&) const;
        std::vector<Record> next(const std::optional<ObjectId>& after, size_t limit) const;
        uint64_t size() const noexcept { return root_.count; }
        Hash256 root_hash() const noexcept { return root_.hash; }

      private:
        friend class ObjectTrie;
        struct Source;
        std::shared_ptr<const Source> source_;
        Child root_;
    };

    // Opens the trie in `dir`, creating it if empty. Throws if its published
    // root cannot be read.
    ObjectTrie(std::filesystem::path dir, std::array<uint8_t, 32> key, Options options);
    ~ObjectTrie();
    ObjectTrie(const ObjectTrie&) = delete;
    ObjectTrie& operator=(const ObjectTrie&) = delete;

    std::optional<Bytes> get(const ObjectId&) const;
    // Journals the changes as one frame, then applies them in order (a later
    // change to the same id wins).
    void apply(std::span<const Change>);
    // apply() in two steps, for an owner that keeps lookups going while the
    // journal syncs: write() journals changes already sorted by id and
    // unique; install() applies them (and checkpoints when the journal is
    // due). The owner serialises writes and installs, in the same order.
    void write(std::span<const Change> sorted_unique);
    void install(std::span<const Change> sorted_unique);
    // Replaces every record with `records` (sorted by id, unique) and
    // checkpoints at once: what was there before is superseded.
    void replace_all(std::vector<Record> records);
    // Folds the journal into the node file now.
    void checkpoint();
    // Records after `after` (from the first when none), in id order, at most
    // `limit` of them.
    std::vector<Record> next(const std::optional<ObjectId>& after, size_t limit) const;
    uint64_t size() const noexcept;
    // Of every record; zero for none.
    Hash256 root_hash() const noexcept;
    Snapshot snapshot() const;

    struct Stats {
        uint64_t journal_bytes{};
        uint64_t node_file_bytes{};
        // Bytes of saved nodes the published root reaches.
        uint64_t live_bytes{};
        uint64_t cache_bytes{};
        uint64_t cached_nodes{};
        // Nodes read from disk since open.
        uint64_t loads{};
        uint64_t checkpoints{};
        uint64_t rewrites{};
    };
    Stats stats() const noexcept;

  private:
    // Reads a saved node's bytes.
    using Read = std::function<Bytes(uint64_t offset)>;

    std::filesystem::path dir_;
    std::array<uint8_t, 32> key_;
    Options options_;
    std::unique_ptr<SealedJournal> journal_;
    std::unique_ptr<SealedJournal> nodes_;
    uint64_t generation_{};
    // Opened in format 1; rewritten in the current format before the
    // constructor returns.
    bool format1_{};
    Child root_;
    uint64_t live_bytes_{};
    uint64_t superseded_bytes_{};
    std::shared_ptr<NodeCache> cache_;
    uint64_t checkpoints_{};
    uint64_t rewrites_{};

    std::filesystem::path nodes_path(uint64_t generation) const;
    void publish_root() const;
    NodePtr load(const Child&) const;
    static NodePtr load_from(const Child&, uint64_t generation, const Read&, NodeCache&,
                             bool format1);
    static NodePtr make_leaf(uint8_t depth, std::vector<Record> records);
    static NodePtr make_interior(uint8_t depth, std::vector<Child> children);
    static Child child_of(NodePtr node);
    Child build(uint8_t depth, std::vector<Record> records) const;
    void gather(const Child&, std::vector<Record>& out, bool superseding);
    Child mutate(const Child&, uint8_t depth, std::span<const Change>);
    Child save(const Child&, SealedJournal& into);
    void apply_unjournaled(std::span<const Change>);
    static std::optional<Bytes> find(const Child& root, const ObjectId&,
                                     const std::function<NodePtr(const Child&)>& load);
    static void collect(const Child&, const std::optional<ObjectId>& after, bool bounded,
                        size_t limit, std::vector<Record>& out,
                        const std::function<NodePtr(const Child&)>& load);
    Bytes encode(const Node&) const;
    static NodePtr decode(std::span<const uint8_t>, bool format1);
    static size_t footprint(const Node&) noexcept;
};

} // namespace macha
