// SPDX-License-Identifier: GPL-3.0-or-later
#include "crypto.hpp"
#include "metadata.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace macha;

namespace {
NodeId parse_node_id(const std::string& text) {
    const auto bytes = unhex(text);
    if (!bytes || bytes->size() != NodeId{}.bytes.size())
        throw std::runtime_error("invalid witness NodeId: " + text);
    NodeId out;
    std::copy(bytes->begin(), bytes->end(), out.bytes.begin());
    return out;
}

Hash256 parse_hash(const std::string& text) {
    const auto bytes = unhex(text);
    if (!bytes || bytes->size() != Hash256{}.bytes.size())
        throw std::runtime_error("invalid metadata hash: " + text);
    Hash256 out;
    std::copy(bytes->begin(), bytes->end(), out.bytes.begin());
    return out;
}

Bytes read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open acceptance file: " + path.string());
    return Bytes(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void write_bytes(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create acceptance file: " + path.string());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("cannot write acceptance file: " + path.string());
}

MetadataManualRepairPlan repair_plan(MetadataReplica& replica) {
    auto heads = replica.accepted_heads();
    if (heads.size() != 2)
        throw std::runtime_error("manual causal repair requires exactly two accepted heads");
    if (replica.history_common_ancestor(heads[0].hash, heads[1].hash))
        throw std::runtime_error("heads have a common ancestor; use ordinary reconciliation");
    const auto left = decode_snapshot(heads[0].payload);
    const auto right = decode_snapshot(heads[1].payload);
    auto plan = plan_causally_dominant_metadata_repair(heads[0], left, heads[1], right);
    if (!plan)
        throw std::runtime_error("neither accepted head strictly causally dominates the other");
    return *plan;
}

void print_plan(const MetadataManualRepairPlan& plan) {
    std::cout << "repair generation=" << plan.record.generation
              << " hash=" << to_string(plan.record.hash)
              << " primary=" << to_string(plan.record.previous)
              << " dominant=" << to_string(plan.dominant_head)
              << " subsumed=" << to_string(plan.subsumed_head) << '\n';
}

MetadataConflictPreservingRepairPlan conflict_repair_plan(MetadataReplica& replica) {
    auto heads = replica.accepted_heads();
    if (heads.size() != 2)
        throw std::runtime_error("manual conflict-preserving repair requires exactly two accepted heads");
    if (replica.history_common_ancestor(heads[0].hash, heads[1].hash))
        throw std::runtime_error("heads have a common ancestor; use ordinary reconciliation");
    const auto left = decode_snapshot(heads[0].payload);
    const auto right = decode_snapshot(heads[1].payload);
    auto plan = plan_conflict_preserving_metadata_repair(heads[0], left, heads[1], right);
    if (!plan)
        throw std::runtime_error(
            "heads have entries absent from one another; conflict-preserving repair only "
            "covers entries changed in place on both sides -- refusing rather than risk "
            "misclassifying an add/remove/rename");
    return *plan;
}

void print_conflict_plan(const MetadataConflictPreservingRepairPlan& plan) {
    std::cout << "conflict_repair generation=" << plan.record.generation
              << " hash=" << to_string(plan.record.hash)
              << " primary=" << to_string(plan.record.previous)
              << " left=" << to_string(plan.left_head) << " right=" << to_string(plan.right_head)
              << " conflicts_created=" << plan.conflicts_created << '\n';
}

void print_entry(std::string_view label, const FsEntry& entry) {
    std::cout << "  " << label << ": type=" << (entry.type == EntryType::directory ? "dir" : "file")
              << " mode=" << std::oct << entry.mode << std::dec << " uid=" << entry.uid
              << " gid=" << entry.gid << " size=" << entry.size << " mtime_ns=" << entry.mtime_ns
              << " version=" << entry.version << " extents=" << entry.extents.size();
    for (const auto& extent : entry.extents)
        std::cout << " [offset=" << extent.offset << " length=" << extent.length
                  << " hole=" << extent.hole << " id=" << to_string(extent.id) << ']';
    std::cout << '\n';
}

// Read-only, additive to the causal-merge machinery above: prints exactly
// which namespace paths differ between two accepted heads, field by field,
// rather than only the aggregate counts the ordinary report prints. Intended
// for a concurrent (neither-dominates) divergence, where the causal-merge
// plan refuses to run and an operator needs to see precisely what is at
// stake before deciding how to proceed by hand.
void diff_heads(MetadataReplica& replica) {
    auto heads = replica.accepted_heads();
    if (heads.size() != 2)
        throw std::runtime_error("--diff-heads requires exactly two accepted heads");
    const auto left_snapshot = decode_snapshot(heads[0].payload);
    const auto right_snapshot = decode_snapshot(heads[1].payload);
    std::cout << "diff left=" << to_string(heads[0].hash) << " right=" << to_string(heads[1].hash)
              << '\n';
    for (const auto& [path, entry] : left_snapshot.entries) {
        const auto found = right_snapshot.entries.find(path);
        if (found == right_snapshot.entries.end()) {
            std::cout << "left_only path=" << path << '\n';
            print_entry("left", entry);
        } else if (found->second != entry) {
            std::cout << "changed path=" << path << '\n';
            print_entry("left", entry);
            print_entry("right", found->second);
        }
    }
    for (const auto& [path, entry] : right_snapshot.entries) {
        if (!left_snapshot.entries.contains(path)) {
            std::cout << "right_only path=" << path << '\n';
            print_entry("right", entry);
        }
    }
    if (left_snapshot.catalogue_root != right_snapshot.catalogue_root)
        std::cout << "catalogue_root differs left="
                  << (left_snapshot.catalogue_root ? to_string(*left_snapshot.catalogue_root) : "none")
                  << " right="
                  << (right_snapshot.catalogue_root ? to_string(*right_snapshot.catalogue_root) : "none")
                  << '\n';
    for (const auto& [origin, sequence] : left_snapshot.mutation_sequences) {
        const auto found = right_snapshot.mutation_sequences.find(origin);
        std::cout << "mutation_origin=" << to_string(origin) << " left_sequence=" << sequence
                  << " right_sequence=" << (found != right_snapshot.mutation_sequences.end()
                                                ? std::to_string(found->second)
                                                : "absent")
                  << '\n';
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: macha-metadata-repair STATE_PATH KEY_FILE\n"
                     "       macha-metadata-repair --plan-causal-merge STATE_PATH KEY_FILE\n"
                     "       macha-metadata-repair --stage-causal-merge STATE_PATH KEY_FILE\n"
                     "       macha-metadata-repair --accept-causal-merge STATE_PATH KEY_FILE WITNESS...\n"
                     "       macha-metadata-repair --export-acceptance STATE_PATH KEY_FILE HASH FILE\n"
                     "       macha-metadata-repair --import-acceptance STATE_PATH KEY_FILE FILE\n"
                     "       macha-metadata-repair --diff-heads STATE_PATH KEY_FILE\n"
                     "       macha-metadata-repair --plan-conflict-merge STATE_PATH KEY_FILE\n"
                     "       macha-metadata-repair --stage-conflict-merge STATE_PATH KEY_FILE\n"
                     "       macha-metadata-repair --accept-conflict-merge STATE_PATH KEY_FILE WITNESS...\n";
        return 2;
    }
    try {
        const bool command = std::string_view(argv[1]).starts_with("--");
        const auto state_index = command ? 2 : 1;
        const auto key_index = command ? 3 : 2;
        if (argc <= key_index) throw std::runtime_error("state path and key file are required");
        const auto keys = load_cluster_keys(argv[key_index]);
        MetadataReplica replica(std::filesystem::path(argv[state_index]), keys.storage);
        if (command) {
            const std::string action = argv[1];
            if (action == "--export-acceptance") {
                if (argc != 6) throw std::runtime_error("hash and output file are required");
                const auto hash = parse_hash(argv[4]);
                const auto acceptance = replica.acceptance(hash);
                if (!acceptance) throw std::runtime_error("metadata head is not accepted here");
                write_bytes(argv[5], encode_metadata_acceptance(*acceptance));
                std::cout << "exported generation=" << acceptance->generation
                          << " hash=" << to_string(acceptance->hash)
                          << " required=" << acceptance->required
                          << " witnesses=" << acceptance->replicas.size() << '\n';
                return 0;
            }
            if (action == "--import-acceptance") {
                if (argc != 5) throw std::runtime_error("acceptance file is required");
                const auto acceptance = decode_metadata_acceptance(read_bytes(argv[4]));
                if (!replica.history_contains(acceptance.hash))
                    throw std::runtime_error("accepted record is absent from local history");
                if (!replica.accept_commit(acceptance))
                    throw std::runtime_error("acceptance certificate failed local policy validation");
                std::cout << "imported generation=" << acceptance.generation
                          << " hash=" << to_string(acceptance.hash)
                          << " required=" << acceptance.required
                          << " witnesses=" << acceptance.replicas.size() << '\n';
                return 0;
            }
            if (action == "--diff-heads") {
                diff_heads(replica);
                return 0;
            }
            if (action == "--plan-conflict-merge" || action == "--stage-conflict-merge" ||
                action == "--accept-conflict-merge") {
                auto plan = conflict_repair_plan(replica);
                print_conflict_plan(plan);
                if (action == "--plan-conflict-merge") return 0;
                if (action == "--stage-conflict-merge") {
                    if (!replica.store_commit(plan.record))
                        throw std::runtime_error("failed to durably stage manual repair commit");
                    std::cout << "staged=true\n";
                    return 0;
                }
                const auto snapshot = decode_snapshot(plan.record.payload);
                const auto required = snapshot.metadata_write_replicas_required;
                std::vector<NodeId> witnesses;
                for (int i = key_index + 1; i < argc; ++i)
                    witnesses.push_back(parse_node_id(argv[i]));
                std::sort(witnesses.begin(), witnesses.end());
                witnesses.erase(std::unique(witnesses.begin(), witnesses.end()), witnesses.end());
                if (!required || witnesses.size() < required)
                    throw std::runtime_error("insufficient distinct witnesses for repair policy");
                if (!replica.history_contains(plan.record.hash))
                    throw std::runtime_error("repair commit has not been staged on this replica");
                MetadataAcceptance acceptance{plan.record.generation, plan.record.hash,
                                              required, std::move(witnesses)};
                if (!replica.accept_commit(acceptance))
                    throw std::runtime_error("failed to accept manual repair commit");
                std::cout << "accepted=true\n";
                return 0;
            }
            auto plan = repair_plan(replica);
            print_plan(plan);
            if (action == "--plan-causal-merge") return 0;
            if (action == "--stage-causal-merge") {
                if (!replica.store_commit(plan.record))
                    throw std::runtime_error("failed to durably stage manual repair commit");
                std::cout << "staged=true\n";
                return 0;
            }
            if (action == "--accept-causal-merge") {
                const auto snapshot = decode_snapshot(plan.record.payload);
                const auto required = snapshot.metadata_write_replicas_required;
                std::vector<NodeId> witnesses;
                for (int i = key_index + 1; i < argc; ++i)
                    witnesses.push_back(parse_node_id(argv[i]));
                std::sort(witnesses.begin(), witnesses.end());
                witnesses.erase(std::unique(witnesses.begin(), witnesses.end()), witnesses.end());
                if (!required || witnesses.size() < required)
                    throw std::runtime_error("insufficient distinct witnesses for repair policy");
                if (!replica.history_contains(plan.record.hash))
                    throw std::runtime_error("repair commit has not been staged on this replica");
                MetadataAcceptance acceptance{plan.record.generation, plan.record.hash,
                                              required, std::move(witnesses)};
                if (!replica.accept_commit(acceptance))
                    throw std::runtime_error("failed to accept manual repair commit");
                std::cout << "accepted=true\n";
                return 0;
            }
            throw std::runtime_error("unknown repair action: " + action);
        }
        const auto committed = replica.committed();
        std::cout << "committed generation=" << committed.generation
                  << " hash=" << to_string(committed.hash)
                  << " previous=" << to_string(committed.previous) << '\n';

        auto heads = replica.accepted_heads();
        std::sort(heads.begin(), heads.end(), [](const auto& left, const auto& right) {
            return left.hash < right.hash;
        });
        std::cout << "accepted_heads=" << heads.size() << '\n';
        for (const auto& head : heads) {
            const auto certificate = replica.acceptance(head.hash);
            const auto snapshot = decode_snapshot(head.payload);
            std::cout << "head generation=" << head.generation
                      << " hash=" << to_string(head.hash)
                      << " previous=" << to_string(head.previous)
                      << " required=" << (certificate ? certificate->required : 0)
                      << " witnesses=" << (certificate ? certificate->replicas.size() : 0)
                      << " entries=" << snapshot.entries.size()
                      << " garbage=" << snapshot.garbage.size()
                      << " mutation_origins=" << snapshot.mutation_sequences.size()
                      << " namespace=" << to_string(metadata_namespace_signature(snapshot))
                      << " catalogue=" << (snapshot.catalogue_root
                                                ? to_string(*snapshot.catalogue_root)
                                                : "none")
                      << '\n';
        }
        for (size_t left = 0; left < heads.size(); ++left) {
            for (size_t right = left + 1; right < heads.size(); ++right) {
                const auto left_snapshot = decode_snapshot(heads[left].payload);
                const auto right_snapshot = decode_snapshot(heads[right].payload);
                auto dominates = [](const auto& candidate, const auto& other) {
                    for (const auto& [origin, sequence] : other.mutation_sequences) {
                        const auto found = candidate.mutation_sequences.find(origin);
                        if (found == candidate.mutation_sequences.end() || found->second < sequence)
                            return false;
                    }
                    return true;
                };
                size_t left_only = 0, right_only = 0, changed = 0;
                for (const auto& [path, entry] : left_snapshot.entries) {
                    const auto found = right_snapshot.entries.find(path);
                    if (found == right_snapshot.entries.end())
                        ++left_only;
                    else if (found->second != entry)
                        ++changed;
                }
                for (const auto& [path, _] : right_snapshot.entries)
                    if (!left_snapshot.entries.contains(path)) ++right_only;
                const auto common = replica.history_common_ancestor(heads[left].hash,
                                                                    heads[right].hash);
                std::cout << "common left=" << to_string(heads[left].hash)
                          << " right=" << to_string(heads[right].hash)
                          << " hash=" << (common ? to_string(*common) : "none")
                          << " left_dominates=" << dominates(left_snapshot, right_snapshot)
                          << " right_dominates=" << dominates(right_snapshot, left_snapshot)
                          << " left_only_entries=" << left_only
                          << " right_only_entries=" << right_only
                          << " changed_entries=" << changed
                          << " catalogue_equal="
                          << (left_snapshot.catalogue_root == right_snapshot.catalogue_root)
                          << '\n';
            }
        }

        std::vector<Hash256> pending;
        for (const auto& head : heads) pending.push_back(head.hash);
        std::set<Hash256> seen;
        while (!pending.empty()) {
            const auto hash = pending.back();
            pending.pop_back();
            if (!seen.insert(hash).second) continue;
            const auto entry = replica.history_entry(hash);
            if (!entry) {
                std::cout << "missing hash=" << to_string(hash) << '\n';
                continue;
            }
            std::cout << "history generation=" << entry->generation
                      << " hash=" << to_string(entry->hash)
                      << " previous=" << to_string(entry->previous)
                      << " previous_known=" << entry->previous_known
                      << " body=" << (entry->body == MetadataHistoryEntry::Body::full ? "full" : "delta")
                      << " merge_parents=" << entry->merge_parents.size() << '\n';
            if (entry->previous != Hash256{}) {
                if (entry->previous_known)
                    pending.push_back(entry->previous);
                else if (!replica.history_entry(entry->previous))
                    std::cout << "boundary hash=" << to_string(entry->previous) << '\n';
            }
            for (const auto& parent : entry->merge_parents)
                pending.push_back(parent);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "metadata inspection failed: " << error.what() << '\n';
        return 1;
    }
}
