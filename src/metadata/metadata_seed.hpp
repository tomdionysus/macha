// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/thread_safety.hpp"
#include "metadata/metadata.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace macha {

// The replica's recovery seed: the last committed head, kept so a node whose
// replica state fails can start from it. Not authority (MetadataReplica checks
// it as any other candidate). One sealed file under the state path, written
// whole by a durable replace, sealed under the domain's storage key with the
// domain bound into the seal, as the store seals what it keeps at rest.
class MetadataSeed {
  public:
    MetadataSeed(std::filesystem::path state_path, std::array<uint8_t, 32> key);

    // Keeps `record` unless it is the head already kept. A failure to write is
    // a debug line: the seed is an aid, never a reason to fail a commit.
    void remember(const MetadataRecord& record);
    // The head kept, if one is and it opens.
    std::optional<MetadataRecord> read() const;

  private:
    const std::filesystem::path path_;
    const std::array<uint8_t, 32> key_;
    // Held across the file's write and read.
    mutable IoMutex mutex_;
    mutable std::optional<Hash256> kept_ MACHA_GUARDED_BY(mutex_);
};

} // namespace macha
