// SPDX-License-Identifier: GPL-3.0-or-later
#include "service/availability_service.hpp"

#include "codec.hpp"
#include "durable_file.hpp"
#include "filesystem/filesystem.hpp"
#include "log.hpp"
#include "metadata/namespace_control_store.hpp"
#include "metadata/namespace_tree.hpp"
#include "observation.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <tuple>

namespace macha {

namespace {

// "/a/b/c" -> "/a/b"; "/a" -> "/"; "/" has no parent.
std::string_view parent_of(std::string_view path) {
    if (path.size() <= 1)
        return {};
    const auto slash = path.rfind('/');
    return slash == 0 ? path.substr(0, 1) : path.substr(0, slash);
}

// A file's own facts against a survey.
PathAvailability file_facts(const FsEntry& entry, const AvailabilitySurvey& survey,
                            const HeldFn& held, const AvailabilitySnapshot* last_known) {
    PathAvailability facts;
    {
        facts.size = entry.size;
        facts.hash = file_media_id(entry);
        for (const auto& extent : entry.extents) {
            if (extent.hole)
                continue;
            ++facts.extents;
            if (held(extent.id))
                ++facts.extents_local;
            else if (survey.is_unavailable(extent.id))
                ++facts.extents_unavailable;
            else if (survey.is_unknown(extent.id))
                ++facts.extents_unknown;
        }
        if (facts.extents_unknown && last_known)
            if (const auto known = last_known->by_hash.find(facts.hash);
                known != last_known->by_hash.end())
                if (const auto was = last_known->paths.find(known->second);
                    was != last_known->paths.end() && was->second.hash == facts.hash &&
                    was->second.extents == facts.extents && !was->second.extents_unknown) {
                    facts.extents_unavailable = std::max(
                        facts.extents_unavailable, std::min(was->second.extents_unavailable,
                                                            facts.extents - facts.extents_local));
                    facts.extents_unknown = 0;
                }
    }
    return facts;
}

void add_file(AvailabilitySnapshot& out, const std::string& path, const PathAvailability& file) {
    out.paths[path] = file;
    out.by_hash.emplace(file.hash, path);
    for (auto parent = parent_of(path); !parent.empty(); parent = parent_of(parent)) {
        auto& above = out.paths[std::string(parent)];
        above.directory = true;
        above.size += file.size;
        above.extents += file.extents;
        above.extents_local += file.extents_local;
        above.extents_unavailable += file.extents_unavailable;
        above.extents_unknown += file.extents_unknown;
    }
}

} // namespace

void fill_path_table(AvailabilitySnapshot& out, const MetadataSnapshot& snapshot,
                     const NamespaceNodeStore& store, const HeldFn& held,
                     const AvailabilitySnapshot* last_known, const std::function<void()>& pause) {
    for_each_namespace_entry(snapshot, &store, [&](const std::string& path, const FsEntry& entry) {
        if (pause)
            pause();
        if (entry.type != EntryType::file)
            out.paths[path].directory = true;
        else
            add_file(out, path, file_facts(entry, out.survey, held, last_known));
    });
}

void update_path_table(AvailabilitySnapshot& out, const AvailabilitySnapshot& previous,
                       const NamespaceDifferences& changes, const MetadataSnapshot& snapshot,
                       const NamespaceNodeStore& store, const HeldFn& held) {
    out.paths = previous.paths;
    out.by_hash = previous.by_hash;
    // Directories that may have lost their reason to be listed.
    std::set<std::string, std::greater<>> emptied;
    for (const auto& [path, change] : changes) {
        if (!change.before)
            continue;
        if (change.before->type != EntryType::file) {
            if (!change.after)
                emptied.insert(path);
            continue;
        }
        const auto found = out.paths.find(path);
        if (found == out.paths.end())
            continue;
        const auto file = found->second;
        out.paths.erase(found);
        for (auto [at, end] = out.by_hash.equal_range(file.hash); at != end; ++at)
            if (at->second == path) {
                out.by_hash.erase(at);
                break;
            }
        for (auto parent = parent_of(path); !parent.empty(); parent = parent_of(parent)) {
            const auto above = out.paths.find(parent);
            if (above == out.paths.end())
                continue;
            above->second.size -= file.size;
            above->second.extents -= file.extents;
            above->second.extents_local -= file.extents_local;
            above->second.extents_unavailable -= file.extents_unavailable;
            above->second.extents_unknown -= file.extents_unknown;
            emptied.insert(std::string(parent));
        }
    }
    for (const auto& [path, change] : changes) {
        if (!change.after)
            continue;
        if (change.after->type != EntryType::file)
            out.paths[path].directory = true;
        else
            add_file(out, path, file_facts(*change.after, out.survey, held, nullptr));
    }
    // Deepest first. A directory is listed for its own entry or for a file
    // beneath it.
    for (const auto& path : emptied) {
        const auto found = out.paths.find(path);
        if (found == out.paths.end() || !found->second.directory ||
            namespace_contains(snapshot, &store, path))
            continue;
        const auto prefix = path == "/" ? path : path + "/";
        bool file_beneath = false;
        for (auto at = std::next(found);
             at != out.paths.end() && at->first.starts_with(prefix) && !file_beneath; ++at)
            file_beneath = !at->second.directory;
        if (!file_beneath)
            out.paths.erase(found);
    }
}

namespace {

constexpr uint32_t persisted_schema = 1;

} // namespace

Bytes encode_availability_paths(const AvailabilitySnapshot& snapshot) {
    Writer out;
    out.u32(persisted_schema);
    out.u64(snapshot.generation);
    out.u64(snapshot.surveyed_unix_ms);
    out.u64(snapshot.paths.size());
    for (const auto& [path, facts] : snapshot.paths) {
        out.string(path);
        out.u8(facts.directory ? 1 : 0);
        out.u64(facts.size);
        out.u64(facts.extents);
        out.u64(facts.extents_local);
        out.u64(facts.extents_unavailable);
        out.u64(facts.extents_unknown);
        out.string(facts.hash);
    }
    return out.take();
}

AvailabilitySnapshot decode_availability_paths(std::span<const uint8_t> bytes) {
    Reader in(bytes);
    if (in.u32() != persisted_schema)
        throw DecodeError("unknown availability schema");
    AvailabilitySnapshot snapshot;
    snapshot.generation = in.u64();
    snapshot.surveyed_unix_ms = in.u64();
    const auto count = in.u64();
    for (uint64_t i = 0; i < count; ++i) {
        auto path = in.string();
        PathAvailability facts;
        const auto directory = in.u8();
        if (directory > 1)
            throw DecodeError("bad availability path kind");
        facts.directory = directory == 1;
        facts.size = in.u64();
        facts.extents = in.u64();
        facts.extents_local = in.u64();
        facts.extents_unavailable = in.u64();
        facts.extents_unknown = in.u64();
        facts.hash = in.string();
        if (facts.extents_local > facts.extents ||
            facts.extents_unavailable > facts.extents - facts.extents_local ||
            facts.extents_unknown > facts.extents - facts.extents_local - facts.extents_unavailable)
            throw DecodeError("availability counts exceed the extents");
        if (!facts.directory)
            snapshot.by_hash.emplace(facts.hash, path);
        if (!snapshot.paths.emplace(std::move(path), std::move(facts)).second)
            throw DecodeError("availability path repeated");
    }
    in.finish();
    return snapshot;
}

AvailabilityService::AvailabilityService(dagda::Context& store,
                                         const dagda::contract::ObjectLedger& ledger,
                                         std::filesystem::path persisted)
    : store_(store), ledger_(ledger), persisted_(std::move(persisted)) {
    if (persisted_.empty())
        return;
    std::ifstream in(persisted_, std::ios::binary);
    if (!in)
        return;
    const Bytes bytes{std::istreambuf_iterator<char>(in), {}};
    try {
        snapshot_.publish(decode_availability_paths(bytes));
    } catch (const DecodeError& error) {
        Log::warn("availability: ignoring " + persisted_.string() + ": " + error.what());
    }
}

bool AvailabilityService::refresh(
    const MetadataSnapshotView& head,
    const std::shared_ptr<const dagda::store::AvailabilitySnapshot>& survey,
    const std::function<void()>& pause) {
    if (!head.snapshot || !survey)
        return false;
    const auto view = ledger_.held_view(RetentionClass::data);
    if (!view.identity.complete || !view.trie)
        return false;
    const auto& root = head.snapshot->namespace_root;
    const bool same_holdings = tabled_ && view.identity.hash == table_holdings_;
    if (same_holdings && survey == table_survey_ && root == table_root_)
        return false;

    const auto started = Clock::now();
    const auto previous = snapshot_.handle();
    AvailabilitySnapshot next;
    next.generation = head.generation;
    next.surveyed_unix_ms = survey->surveyed_unix_ms;
    next.survey = survey->survey;
    // With this node's holdings and the survey's answer unchanged, no
    // unchanged file's facts can differ: the table follows the tree.
    const bool same_answer = previous && previous->survey.unavailable == next.survey.unavailable &&
                             previous->survey.unknown.empty() && next.survey.unknown.empty();
    const auto stored = ControlNamespaceNodeStore::for_reading(store_);
    const HeldFn held = view.held;
    const char* table = "walked";
    if (same_holdings && same_answer && root == table_root_) {
        next.paths = previous->paths;
        next.by_hash = previous->by_hash;
        table = "kept";
    } else if (same_holdings && same_answer && root && table_root_) {
        update_path_table(next, *previous, diff_namespace_trees(*table_root_, *root, stored),
                          *head.snapshot, stored, held);
        table = "followed";
    } else {
        fill_path_table(next, *head.snapshot, stored, held, previous.get(), pause);
    }
    observations().record("availability.table_us", elapsed_us(started));
    Log::debug("availability table generation=" + std::to_string(next.generation) +
               " surveyed_unix_ms=" + std::to_string(next.surveyed_unix_ms) +
               " unavailable=" + std::to_string(next.survey.unavailable.size()) +
               " unknown=" + std::to_string(next.survey.unknown.size()) + " table=" + table);
    if (!persisted_.empty()) {
        try {
            const auto bytes = encode_availability_paths(next);
            durable_replace_file(
                persisted_,
                std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        } catch (const std::exception& error) {
            Log::warn(std::string("availability: cannot keep the table: ") + error.what());
        }
    }
    snapshot_.publish(std::move(next));
    tabled_ = true;
    table_survey_ = survey;
    table_root_ = root;
    table_holdings_ = view.identity.hash;
    return true;
}

} // namespace macha
