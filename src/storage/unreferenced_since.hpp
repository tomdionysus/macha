// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"
#include "types.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>

// When this node first saw each object it holds unreferenced, on its own
// clock. The physical sweeps delete an object only once it has stayed
// unreferenced here for the deletion grace, whatever any other node's clock
// or tombstone says, so a node back from a long absence removes nothing
// before its own grace has run.
//
// ThreadSafety: any thread. Waits: none; save() writes the file.
namespace macha {

Bytes encode_unreferenced_since(const std::map<ObjectId, uint64_t>&);
// Throws DecodeError.
std::map<ObjectId, uint64_t> decode_unreferenced_since(std::span<const uint8_t>);

class UnreferencedSince {
  public:
    // Loads `path` when it exists; an unreadable file is ignored, which only
    // delays deletion.
    explicit UnreferencedSince(std::filesystem::path path);

    // Records `now_unix_ms` as the first sighting when there is none, and
    // answers whether the object has been unreferenced for `grace`.
    bool matured(const ObjectId&, uint64_t now_unix_ms, std::chrono::milliseconds grace);
    // The object is referenced again, or gone.
    void forget(const ObjectId&);
    // A sweep visited every object it holds: sightings it did not renew
    // since the previous call belong to objects no longer held.
    void pass_complete();
    // Writes the table when it changed since the last save.
    void save();

    size_t size() const;

  private:
    struct Sighting {
        uint64_t first_unix_ms{};
        bool renewed{};
    };
    const std::filesystem::path path_;
    mutable Mutex mutex_;
    std::map<ObjectId, Sighting> seen_ MACHA_GUARDED_BY(mutex_);
    bool dirty_ MACHA_GUARDED_BY(mutex_){};
};

} // namespace macha
