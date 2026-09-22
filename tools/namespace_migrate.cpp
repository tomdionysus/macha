// SPDX-License-Identifier: GPL-3.0-or-later
//
// Re-roots a node's namespace onto the content-addressed Merkle tree (SM14).
//
//   macha-namespace-migrate <state_path> <cluster.key> [options]
//
// The record stops carrying the namespace and starts pointing at it. Today a
// metadata record IS the serialised namespace and its identity is a SHA-256
// over those bytes, so every commit re-serialises and re-hashes the whole
// library; after this, a commit rewrites the leaf holding the changed path and
// the branches above it.
//
// **This is destructive and offline.** It quarantines the node's checkpoint,
// journal, history, heads and acceptance proof under a timestamped suffix --
// they stay on the disk -- and installs a new head with no ancestry. What is
// discarded is the ability to roll back past this point; what is not touched,
// at all, is a single extent. Paths, stat fields and the ObjectIds naming
// content are copied across unchanged, and those ObjectIds address objects in
// the data backends that no metadata format change rewrites or moves.
//
// **Run it on every node, with every node stopped, after they have converged.**
// The new record is a pure function of the namespace the node already holds:
// the tree's shape is determined by the entry set alone, so every node
// computes the same nodes, the same root and the same record hash
// independently. Nothing is shipped between nodes and nothing needs to be.
// That is also the check: `--expect-hash` makes a node refuse to install
// anything other than the record another node already produced.
//
// Which is why converged matters. Two nodes at different heads produce two
// different records and the cluster comes back split, each node authoritative
// for a namespace nobody else has.
#include "codec.hpp"
#include "crypto.hpp"
#include "local_store.hpp"
#include "metadata.hpp"
#include "namespace_control_store.hpp"
#include "namespace_tree.hpp"

#include <filesystem>
#include <set>
#include <iostream>
#include <string>
#include <string_view>

using namespace macha;

namespace {

struct Options {
    std::filesystem::path state;
    std::filesystem::path key;
    std::filesystem::path objects;
    std::optional<Hash256> expect;
    std::vector<NodeId> witnesses;
    bool dry_run{false};
};

[[noreturn]] void usage(std::string_view problem) {
    std::cerr << "macha-namespace-migrate: " << problem << "\n\n"
              << "  macha-namespace-migrate <state_path> <cluster.key> [options]\n\n"
              << "  --objects <path>     control object store (default:\n"
              << "                       <state_path>/../metadata-objects, then\n"
              << "                       <state_path>/metadata-objects)\n"
              << "  --expect-hash <hex>  refuse to install any other record\n"
              << "  --witness <node-id>  a node being re-rooted onto this record; repeat\n"
              << "                       once per node. Defaults to the nodes this one\n"
              << "                       has seen. At least as many as the write floor.\n"
              << "  --dry-run            build and verify, install nothing\n\n"
              << "Stop every node and let them converge first. Run this on each\n"
              << "node; they all compute the same record independently.\n";
    std::exit(2);
}

Hash256 parse_hash(const std::string& text) {
    const auto bytes = unhex(text);
    if (!bytes || bytes->size() != Hash256{}.bytes.size())
        throw std::runtime_error("invalid metadata hash: " + text);
    Hash256 out;
    std::copy(bytes->begin(), bytes->end(), out.bytes.begin());
    return out;
}

Options parse(int argc, char** argv) {
    if (argc < 3)
        usage("state path and key file are required");
    Options options;
    options.state = argv[1];
    options.key = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--dry-run") {
            options.dry_run = true;
        } else if (arg == "--objects") {
            if (++i >= argc)
                usage("--objects needs a path");
            options.objects = argv[i];
        } else if (arg == "--expect-hash") {
            if (++i >= argc)
                usage("--expect-hash needs a hash");
            options.expect = parse_hash(argv[i]);
        } else if (arg == "--witness") {
            if (++i >= argc)
                usage("--witness needs a node id");
            const auto bytes = unhex(argv[i]);
            if (!bytes || bytes->size() != NodeId{}.bytes.size())
                throw std::runtime_error("invalid witness node id: " + std::string(argv[i]));
            NodeId witness;
            std::copy(bytes->begin(), bytes->end(), witness.bytes.begin());
            options.witnesses.push_back(witness);
        } else {
            usage("unknown option: " + std::string(arg));
        }
    }
    if (options.objects.empty()) {
        // Where the daemon puts it in the shipped configuration, then the
        // documented default. Guessing wrong writes tree nodes the node will
        // not find, so a missing store is an error rather than a fresh one.
        const auto beside = options.state.parent_path() / "metadata-objects";
        const auto inside = options.state / "metadata-objects";
        if (std::filesystem::exists(beside))
            options.objects = beside;
        else if (std::filesystem::exists(inside))
            options.objects = inside;
        else
            throw std::runtime_error(
                "cannot find the control object store; pass --objects (looked in " +
                beside.string() + " and " + inside.string() + ")");
    }
    return options;
}

std::string bytes_human(uint64_t bytes) {
    char out[64];
    if (bytes >= 1024ULL * 1024 * 1024)
        std::snprintf(out, sizeof(out), "%.2f GiB", double(bytes) / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        std::snprintf(out, sizeof(out), "%.2f MiB", double(bytes) / (1024.0 * 1024));
    else if (bytes >= 1024)
        std::snprintf(out, sizeof(out), "%.2f KiB", double(bytes) / 1024.0);
    else
        std::snprintf(out, sizeof(out), "%llu B", static_cast<unsigned long long>(bytes));
    return out;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        const auto keys = load_cluster_keys(options.key);

        MetadataReplica replica(options.state, keys.storage);
        const auto head = replica.committed();
        if (!valid_metadata_record(head))
            throw std::runtime_error("this node has no valid committed metadata head");
        if (replica.recovery_required())
            throw std::runtime_error(
                "this node is marked as requiring metadata recovery; recover it before migrating");

        const auto current = replica.current();
        if (current.hash != head.hash)
            throw std::runtime_error(
                "this node has an uncommitted head (current generation " +
                std::to_string(current.generation) + ", committed " +
                std::to_string(head.generation) +
                "); start it, let it settle, stop it again, then migrate");

        auto snapshot = decode_snapshot(head.payload);
        if (snapshot.namespace_root) {
            std::cout << "already migrated: generation=" << head.generation
                      << " root=" << to_string(*snapshot.namespace_root) << '\n';
            return 0;
        }

        std::cout << "head generation=" << head.generation << " hash=" << to_string(head.hash)
                  << " entries=" << snapshot.entries.size()
                  << " payload=" << bytes_human(head.payload.size())
                  << "\nobjects=" << options.objects.string() << '\n';

        LocalStore objects(options.objects,
                           LocalStoreOptions{std::numeric_limits<uint64_t>::max(), 0, 0, 0},
                           keys.storage);
        LocalNamespaceNodeStore nodes(objects);

        // Builds the tree, verifies it reads back as the namespace it came
        // from, and returns the record that would replace the head. Installs
        // nothing: everything up to here is additive, and the tree nodes it
        // wrote are content-addressed objects nothing points at yet.
        const auto migration = plan_namespace_migration(head, nodes);
        const auto& stats = migration.stats;
        std::cout << "tree root=" << to_string(migration.root)
                  << " nodes=" << stats.leaves + stats.branches << " leaves=" << stats.leaves
                  << " branches=" << stats.branches << " extent_nodes=" << stats.extent_nodes
                  << " depth=" << stats.depth << " bytes=" << bytes_human(stats.bytes)
                  << " largest_node=" << bytes_human(stats.largest_node_bytes) << '\n'
                  << "verified entries=" << migration.entries << " against the namespace\n"
                  << "record generation=" << migration.record.generation
                  << " hash=" << to_string(migration.record.hash)
                  << " payload=" << bytes_human(migration.record.payload.size()) << " (was "
                  << bytes_human(migration.previous_payload_bytes) << ")\n";
        const auto& record = migration.record;

        // Who is being re-rooted. The default is what this node knows: the
        // durable participant roster, and the nodes it has status for. Naming
        // them explicitly is better, because this list is a claim about what
        // the operator is about to do on the other machines, not a fact this
        // node can check.
        auto witnesses = options.witnesses;
        if (witnesses.empty()) {
            std::set<NodeId> known(snapshot.metadata_participants.begin(),
                                   snapshot.metadata_participants.end());
            for (const auto& [id, _] : snapshot.node_status)
                known.insert(id);
            witnesses.assign(known.begin(), known.end());
        }
        std::cout << "witnesses=" << witnesses.size();
        for (const auto& witness : witnesses)
            std::cout << ' ' << to_string(witness).substr(0, 12);
        std::cout << (options.witnesses.empty() ? " (inferred; --witness to name them)" : "")
                  << '\n';

        if (options.expect && *options.expect != record.hash)
            throw std::runtime_error(
                "this node computed " + to_string(record.hash) + " but was told to expect " +
                to_string(*options.expect) +
                "; the nodes had not converged, so migrating would split the cluster");

        if (options.dry_run) {
            std::cout << "dry run: nothing installed. The tree nodes were written and are "
                         "harmless -- they are content-addressed objects nothing points at.\n";
            return 0;
        }

        if (!replica.install_migrated_head(record, witnesses,
                                           "namespace migrated to SM14 by "
                                           "macha-namespace-migrate"))
            throw std::runtime_error("the replica refused the migrated head");

        std::cout << "installed. The previous checkpoint, journal, history, heads and acceptance\n"
                     "proof are quarantined beside them with a .pre-migration.<ns> suffix.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "macha-namespace-migrate: " << error.what() << '\n';
        return 1;
    }
}
