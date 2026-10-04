// SPDX-License-Identifier: GPL-3.0-or-later
//
// Re-roots a node's namespace onto the content-addressed Merkle tree (SM14).
//
//   macha-namespace-migrate <state_path> <cluster.key> [options]
//
// The migrated record points at the tree root instead of carrying the
// serialised namespace, so a commit rewrites only the changed leaf and the
// branches above it.
//
// Destructive and offline. It quarantines the checkpoint, journal, history,
// heads and acceptance proof under a timestamped suffix and installs a head
// with no ancestry. No extent is touched: paths, stat fields and ObjectIds are
// copied unchanged. The head carries no retention baseline, so the cluster's
// first repair claims the tree before destructive GC resumes.
//
// Run it on every node, all stopped and converged. The tree's shape depends on
// the entry set alone, so every node computes the same root independently;
// `--expect-hash` refuses any record but the one another node produced. Nodes
// at different heads would come back split.
#include "codec.hpp"
#include "crypto.hpp"
#include "storage/local_store.hpp"
#include "metadata/metadata.hpp"
#include "metadata/namespace_control_store.hpp"
#include "metadata/namespace_tree.hpp"

#include <filesystem>
#include <set>
#include <fstream>
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
    std::filesystem::path export_record;
    std::filesystem::path adopt_record;
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
              << "  --export-record <p>  write the computed record to a file\n"
              << "  --adopt <p>           install THAT record instead of this node's own,\n"
              << "                       after proving this node's namespace produces the\n"
              << "                       same tree root. For a node that stopped a few\n"
              << "                       commits behind the one you migrated first.\n"
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
        } else if (arg == "--export-record") {
            if (++i >= argc)
                usage("--export-record needs a path");
            options.export_record = argv[i];
        } else if (arg == "--adopt") {
            if (++i >= argc)
                usage("--adopt needs a path");
            options.adopt_record = argv[i];
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
        // The shipped configuration's location, then the documented default. A
        // missing store is an error: a fresh one would hold nodes the daemon never finds.
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

        // Builds the tree, verifies it reads back as the source namespace, and
        // returns the replacement record. Installs nothing: the content-addressed
        // tree nodes written so far are unreferenced.
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

        // Defaults to the nodes this one knows (participant roster and node status).
        // Naming them is better: the list is a claim about the other machines this
        // node cannot check.
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

        if (!options.export_record.empty()) {
            const auto encoded = encode_metadata_record(record);
            std::ofstream out(options.export_record, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(encoded.data()),
                      static_cast<std::streamsize>(encoded.size()));
            if (!out)
                throw std::runtime_error("cannot write " + options.export_record.string());
            std::cout << "exported record to " << options.export_record.string() << '\n';
        }

        // Adopting another node's record. Nodes stopped back to back differ in
        // `previous`, `generation` and catalogue root because the head moves
        // constantly, so only the namespace root must match; a node whose namespace
        // diverged is refused and must catch up first.
        auto installing = record;
        if (!options.adopt_record.empty()) {
            std::ifstream in(options.adopt_record, std::ios::binary);
            if (!in)
                throw std::runtime_error("cannot read " + options.adopt_record.string());
            const std::string raw{std::istreambuf_iterator<char>(in),
                                  std::istreambuf_iterator<char>{}};
            const Bytes encoded(raw.begin(), raw.end());
            const auto adopted = decode_metadata_record(std::span<const uint8_t>(encoded));
            if (!valid_metadata_record(adopted))
                throw std::runtime_error("the record to adopt does not verify");
            const auto adopted_snapshot = decode_snapshot(adopted.payload);
            if (!adopted_snapshot.namespace_root)
                throw std::runtime_error("the record to adopt is not tree-backed");
            if (*adopted_snapshot.namespace_root != migration.root)
                throw std::runtime_error(
                    "this node's namespace produces root " + to_string(migration.root) +
                    " but the record to adopt names " + to_string(*adopted_snapshot.namespace_root) +
                    "; this node's namespace differs, so start it alongside the migrated node, let "
                    "it catch up, stop it and try again");
            installing = adopted;
            std::cout << "adopting record generation=" << installing.generation
                      << " hash=" << to_string(installing.hash)
                      << " -- same namespace root, computed independently here\n";
        }

        if (options.dry_run) {
            std::cout << "dry run: nothing installed. The tree nodes were written and are "
                         "harmless -- they are content-addressed objects nothing points at.\n";
            return 0;
        }

        if (!replica.install_migrated_head(installing, witnesses,
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
