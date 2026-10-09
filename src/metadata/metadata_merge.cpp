// SPDX-License-Identifier: GPL-3.0-or-later
//
// The merge of two heads from what the two heads say, with no common
// ancestor: each entry's provenance and each head's clock tell "removed
// there" from "never seen there" and "newer" from "concurrent".
#include "metadata/metadata.hpp"

#include "codec.hpp"

#include <algorithm>
#include <iterator>
#include <set>
#include <stdexcept>

namespace macha {

namespace {

using VersionClock = std::map<NodeId, uint64_t>;

bool clock_covers_clock(const VersionClock& clock, const VersionClock& other) {
    return std::all_of(other.begin(), other.end(), [&](const auto& item) {
        const auto found = clock.find(item.first);
        return found != clock.end() && found->second >= item.second;
    });
}

// One head as the merge reads it.
struct Side {
    const MetadataSnapshot& snapshot;
    const NamespaceEntries& entries;
    const VersionClock& clock;
    // Every mutation that could have written an entry without provenance.
    const VersionClock& legacy;

    Side(const MetadataSnapshot& s, const NamespaceEntries& e)
        : snapshot(s), entries(e), clock(s.mutation_sequences),
          legacy(s.legacy_clock ? *s.legacy_clock : s.mutation_sequences) {}

    std::optional<FsEntry> at(const std::string& path) const {
        const auto found = entries.find(path);
        if (found == entries.end())
            return {};
        return found->second;
    }
};

// Whether `viewer` has incorporated the mutation that gave `owner`'s entry
// its content, or its name. An entry (or a name) from before provenance is
// seen once the viewer has every mutation that could have written it.
bool content_seen(const Side& viewer, const Side& owner, const FsEntry& entry) {
    return entry.provenance.content ? clock_covers(viewer.clock, entry.provenance.content)
                                    : clock_covers_clock(viewer.clock, owner.legacy);
}
bool name_seen(const Side& viewer, const Side& owner, const FsEntry& entry) {
    return entry.provenance.name ? clock_covers(viewer.clock, entry.provenance.name)
                                 : clock_covers_clock(viewer.clock, owner.legacy);
}

FsEntry without_provenance(FsEntry entry) {
    entry.provenance = {};
    return entry;
}

enum class Origin : uint8_t { left, right, both };

} // namespace

MetadataMergeResult merge_metadata_heads_over(const MetadataSnapshot& left,
                                              const MetadataSnapshot& right,
                                              const NamespaceEntries& left_entries,
                                              const NamespaceEntries& right_entries,
                                              const Hash256& left_head,
                                              const Hash256& right_head) {
    if (left_head == right_head) {
        MetadataMergeResult same{left, 0};
        same.snapshot.namespace_root.reset();
        same.snapshot.entries = left_entries;
        return same;
    }
    // One order for every reconciler.
    if (right_head < left_head)
        return merge_metadata_heads_over(right, left, right_entries, left_entries, right_head,
                                         left_head);

    const Side l(left, left_entries);
    const Side r(right, right_entries);
    MetadataMergeResult result;
    auto& out = result.snapshot;

    // Values of the cluster rather than of an entry: the greater where the
    // heads differ.
    out.data_replication = std::max(left.data_replication, right.data_replication);
    out.extent_size = std::max(left.extent_size, right.extent_size);
    out.metadata_write_replicas_required = std::max(left.metadata_write_replicas_required,
                                                    right.metadata_write_replicas_required);
    out.metadata_participants = left.metadata_participants;
    out.metadata_participants.insert(right.metadata_participants.begin(),
                                     right.metadata_participants.end());
    out.retention_baseline_complete =
        left.retention_baseline_complete && right.retention_baseline_complete;
    if (out.retention_baseline_complete)
        out.metadata_participants.clear();

    out.mutation_sequences = left.mutation_sequences;
    for (const auto& [node, sequence] : right.mutation_sequences) {
        auto& current = out.mutation_sequences[node];
        current = std::max(current, sequence);
    }
    if (left.legacy_clock || right.legacy_clock) {
        auto joined = l.legacy;
        for (const auto& [node, sequence] : r.legacy) {
            auto& current = joined[node];
            current = std::max(current, sequence);
        }
        out.legacy_clock = std::move(joined);
    }

    // `installed` is the value the merge leaves at the path, `other` the
    // alternative it keeps for whoever decides.
    const auto record_conflict = [&](const std::string& path, const FsEntry& installed,
                                     const FsEntry& other) {
        MetadataConflict conflict;
        conflict.kind = MetadataConflictKind::namespace_entry;
        conflict.later_installed = true;
        conflict.key = path;
        // No heads: the same pair of alternatives is the same conflict
        // whichever merge finds it.
        conflict.installed_dot = installed.provenance.content;
        conflict.left_entry = installed;
        conflict.right_entry = other;
        const auto id = metadata_conflict_id(conflict);
        if (out.conflicts.emplace(id, std::move(conflict)).second)
            ++result.conflicts_created;
    };

    // Two differing values for what is one entry: the newer when one head
    // has seen the other's, otherwise a concurrent change.
    // A head has seen the other's entry at a path when it has seen both what
    // it holds and how it came to be there: a different file moved onto the
    // path is new there though its content is old. `one_file` compares two
    // entries known to be the same file at different paths, where only the
    // content is in question.
    const auto settle = [&](const std::string& path, const FsEntry& a, const FsEntry& b,
                            bool one_file = false) {
        if (without_provenance(a) == without_provenance(b))
            return a.provenance < b.provenance ? b : a;
        const bool left_seen = content_seen(r, l, a) && (one_file || name_seen(r, l, a));
        const bool right_seen = content_seen(l, r, b) && (one_file || name_seen(l, r, b));
        if (left_seen && !right_seen)
            return b;
        if (right_seen && !left_seen)
            return a;
        // Times and versions alone are no disagreement.
        if (same_content(a, b))
            return a < b ? a : b;
        const auto& installed = later_entry(a, b);
        record_conflict(path, installed, &installed == &a ? b : a);
        return installed;
    };

    std::map<std::string, Origin> origin;
    std::set<std::string> paths;
    for (const auto* entries : {&left_entries, &right_entries})
        for (const auto& [path, _] : *entries)
            paths.insert(path);

    for (const auto& path : paths) {
        const auto a = l.at(path);
        const auto b = r.at(path);
        if (a && b) {
            out.entries[path] = *a == *b ? *a : settle(path, *a, *b);
            origin[path] = Origin::both;
            continue;
        }
        // On one side only: removed by the other, or never seen by it.
        const auto& present = a ? *a : *b;
        const auto& owner = a ? l : r;
        const auto& other = a ? r : l;
        const bool removed = present.provenance.empty()
                                 ? clock_covers_clock(other.clock, owner.legacy)
                                 : name_seen(other, owner, present) &&
                                       content_seen(other, owner, present);
        if (removed)
            continue;
        out.entries[path] = present;
        origin[path] = a ? Origin::left : Origin::right;
    }

    // Identity: one file at two paths is a rename on one side of an entry
    // the other kept at its old path because it changed it.
    std::map<NodeId, std::vector<std::string>> by_identity;
    for (const auto& [path, entry] : out.entries)
        if (entry.provenance.file_id != NodeId{})
            by_identity[entry.provenance.file_id].push_back(path);
    for (const auto& [_, at] : by_identity) {
        if (at.size() != 2 || origin[at[0]] == Origin::both || origin[at[1]] == Origin::both ||
            origin[at[0]] == origin[at[1]])
            continue;
        const auto& x = origin[at[0]] == Origin::left ? at[0] : at[1];
        const auto& y = origin[at[0]] == Origin::left ? at[1] : at[0];
        const auto from_left = out.entries.at(x);
        const auto from_right = out.entries.at(y);
        if (from_left.type != from_right.type)
            continue;
        // The newer name is the one the other head has not seen.
        const bool left_name_seen = name_seen(r, l, from_left);
        const bool right_name_seen = name_seen(l, r, from_right);
        bool keep_left = false;
        if (left_name_seen != right_name_seen)
            keep_left = right_name_seen;
        else
            // Renamed on both sides, to different names: the later rename's
            // name stands.
            keep_left = from_right.provenance.name < from_left.provenance.name;
        const auto& kept = keep_left ? x : y;
        const auto& dropped = keep_left ? y : x;
        auto merged = settle(kept, from_left, from_right, true);
        merged.provenance.name = (keep_left ? from_left : from_right).provenance.name;
        out.entries.erase(dropped);
        out.entries[kept] = std::move(merged);
    }

    // Parents: every surviving entry needs its directories. One that was
    // removed while a child was added comes back from the side that has it.
    std::function<void(const std::string&)> ensure_directory = [&](const std::string& path) {
        const auto found = out.entries.find(path);
        if (found != out.entries.end() && found->second.type == EntryType::directory)
            return;
        if (path != "/")
            ensure_directory(parent_path(path));
        const auto a = l.at(path);
        const auto b = r.at(path);
        const bool left_directory = a && a->type == EntryType::directory;
        const bool right_directory = b && b->type == EntryType::directory;
        if (!left_directory && !right_directory)
            throw std::runtime_error("metadata reconciliation lost the directory " + path);
        const auto& directory = left_directory && right_directory ? settle(path, *a, *b)
                                : left_directory                  ? *a
                                                                  : *b;
        // A file that won the path gives way to the directory with entries
        // under it, and is kept as the conflict's other alternative.
        if (found != out.entries.end())
            record_conflict(path, directory, found->second);
        out.entries[path] = directory;
    };
    for (const auto& path : std::vector<std::string>(paths.begin(), paths.end()))
        if (path != "/" && out.entries.contains(path))
            ensure_directory(parent_path(path));
    if (paths.contains("/") &&
        (!out.entries.contains("/") || out.entries.at("/").type != EntryType::directory))
        throw std::runtime_error("metadata reconciliation lost filesystem root");

    // The catalogue root is one value with one dot.
    const auto seen = [](const Side& viewer, const Side& owner) {
        return owner.snapshot.catalogue_dot
                   ? clock_covers(viewer.clock, owner.snapshot.catalogue_dot)
                   : clock_covers_clock(viewer.clock, owner.legacy);
    };
    const bool left_seen = seen(r, l);
    const bool right_seen = seen(l, r);
    if (left.catalogue_root == right.catalogue_root) {
        out.catalogue_root = left.catalogue_root;
        // The same root set twice: the dot is the later setting's, so a
        // conflict a later commit decided in favour of the root in place is
        // not taken for standing. Concurrent, the greater.
        if (left_seen != right_seen)
            out.catalogue_dot = right_seen ? left.catalogue_dot : right.catalogue_dot;
        else
            out.catalogue_dot = std::max(left.catalogue_dot, right.catalogue_dot);
    } else {
        bool take_left = false;
        if (left_seen != right_seen) {
            take_left = right_seen;
        } else {
            // Concurrent: the greater dot stands, the other is kept as the
            // conflict's alternative for the catalogue to merge in.
            take_left = right.catalogue_dot < left.catalogue_dot ||
                        (right.catalogue_dot == left.catalogue_dot &&
                         right.catalogue_root < left.catalogue_root);
            MetadataConflict conflict;
            conflict.kind = MetadataConflictKind::catalogue_root;
            conflict.later_installed = true;
            conflict.key = "catalogue_root";
            conflict.left_head = left_head;
            conflict.right_head = right_head;
            conflict.installed_dot = take_left ? left.catalogue_dot : right.catalogue_dot;
            conflict.left_catalogue_root = take_left ? left.catalogue_root : right.catalogue_root;
            conflict.right_catalogue_root =
                take_left ? right.catalogue_root : left.catalogue_root;
            const auto id = metadata_conflict_id(conflict);
            if (out.conflicts.emplace(id, std::move(conflict)).second)
                ++result.conflicts_created;
        }
        out.catalogue_root = take_left ? left.catalogue_root : right.catalogue_root;
        out.catalogue_dot = take_left ? left.catalogue_dot : right.catalogue_dot;
    }

    // Standing conflicts from either head; those since decided are dropped
    // below, by the value their subject now holds.
    for (const auto* source : {&left.conflicts, &right.conflicts})
        for (const auto& [id, conflict] : *source)
            out.conflicts.emplace(id, conflict);

    // Union garbage: an extra tombstone is safe, and reachability later decides
    // when physical deletion is valid.
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
    // Union the tombstone batches: each is immutable and content-addressed,
    // so one named by both heads is the same batch.
    out.tombstone_batches.clear();
    std::set_union(left.tombstone_batches.begin(), left.tombstone_batches.end(),
                   right.tombstone_batches.begin(), right.tombstone_batches.end(),
                   std::back_inserter(out.tombstone_batches),
                   [](const TombstoneBatch& a, const TombstoneBatch& b) { return a.id < b.id; });

    // Node status and identity resets are monotonic with deterministic joins.
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
    for (const auto* source : {&left.identity_resets, &right.identity_resets}) {
        for (const auto& [key, reset] : *source) {
            auto found = out.identity_resets.find(key);
            if (found == out.identity_resets.end() || found->second.epoch < reset.epoch ||
                (found->second.epoch == reset.epoch && found->second < reset))
                out.identity_resets[key] = reset;
        }
    }
    // A request on one side only is new there; one both hold joins.
    out.torrent_requests = merge_torrent_requests({}, left.torrent_requests,
                                                  right.torrent_requests);

    result.conflicts_superseded = prune_superseded_conflicts(out);
    out.merge_parents.clear();
    return result;
}

MetadataMergeResult merge_metadata_heads(const MetadataSnapshot& left,
                                         const MetadataSnapshot& right, const Hash256& left_head,
                                         const Hash256& right_head) {
    // A tree-backed snapshot's empty map would merge to an empty namespace.
    for (const auto* branch : {&left, &right})
        if (branch->namespace_root)
            throw std::logic_error("metadata merge requires materialised namespaces; a branch is "
                                   "still a tree");
    return merge_metadata_heads_over(left, right, left.entries, right.entries, left_head,
                                     right_head);
}

} // namespace macha
