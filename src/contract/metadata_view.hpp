// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/walk.hpp"
#include "contract/work.hpp"
#include "metadata/metadata.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Edges by referrer (object ledger spec, B2): the replicated metadata as the
// components above it see it. Implemented by MetadataManager.
namespace macha {

// One decoded snapshot and the head it is at.
struct MetadataSnapshotView {
    uint64_t generation{};
    uint64_t namespace_revision{};
    Hash256 hash{};
    std::shared_ptr<const MetadataSnapshot> snapshot;
};

enum class MetadataAvailability : uint8_t {
    unavailable = 0,
    read_only = 1,
    writable = 2,
};

// The cluster's metadata as this node sees it.
struct MetadataClusterStatus {
    uint64_t generation{};
    uint64_t observed_unix_ms{};
    uint32_t replicas{};
    uint32_t replicas_online{};
    uint32_t write_replicas_required{};
    MetadataAvailability availability{MetadataAvailability::unavailable};
    bool stable{};
    bool write_available{};
};

// See MetadataManager::mutate_delta(). `origin` is any NodeId-shaped key the
// caller owns (a node's own id, or one derived from it for a sub-system);
// `sequence` must increase across that caller's mutations.
struct MetadataMutationIdentity {
    NodeId origin{};
    uint64_t sequence{};
};

// Mutation wall time since start: the pre-publication retention barrier and
// the commit fan-out, totals and maxima in ms.
struct MetadataMutationTiming {
    uint64_t mutations{};
    uint64_t retention_ms_total{};
    uint64_t retention_ms_max{};
    uint64_t publish_ms_total{};
    uint64_t publish_ms_max{};
};

class MetadataView {
  public:
    virtual ~MetadataView() = default;

    // The newest decoded snapshot this node holds, or none before the first.
    // Never reads: thread-safe, waits on nothing.
    static constexpr Waits current_waits = Waits::none;
    virtual std::optional<MetadataSnapshotView> current() const = 0;

    // The snapshot at the newest generation known: the cache when current,
    // else read from the replicas. With a work context, the wait guard
    // refuses work that may not wait on the network.
    static constexpr Waits converged_waits = Waits::state_device | Waits::network;
    virtual MetadataSnapshotView converged() = 0;
    virtual MetadataSnapshotView converged(const WorkContext&) = 0;

    // The generation and namespace revision of current(), without taking the
    // view. Waits on nothing.
    virtual uint64_t current_generation() const noexcept = 0;
    virtual uint64_t current_namespace_revision() const noexcept = 0;

    // The committed record at the newest generation known: the cache, else
    // read from the replicas (the catalogue's cold start and record reads).
    static constexpr Waits record_waits = converged_waits;
    virtual MetadataRecord record() = 0;

    // A page of `view`'s namespace entries after `from`, in path order, one
    // budget operation per entry (namespace_entries over this node's control
    // store). Waits on the state device for tree nodes.
    static constexpr Waits entries_waits = Waits::state_device;
    virtual Page<std::pair<std::string, FsEntry>, std::string>
    entries(const MetadataSnapshotView& view, Cursor<std::string> from, Budget& budget) = 0;

    // The snapshot at the sole accepted head, which retention release reads;
    // none while heads diverge. Waits on nothing.
    static constexpr Waits release_head_waits = Waits::none;
    virtual std::optional<MetadataSnapshotView> release_head() const = 0;

    // Whether the cluster's metadata is stable, available and writable.
    static constexpr Waits status_waits = Waits::none;
    virtual MetadataClusterStatus status() const noexcept = 0;

    // A metadata commit through the replicas, retried on a concurrent one.
    static constexpr Waits mutate_waits = Waits::state_device | Waits::network;
    virtual MetadataRecord mutate(const std::function<void(MetadataSnapshot&)>&,
                                  size_t retries = 8) = 0;
    virtual MetadataRecord mutate_delta(
        const std::function<void(MetadataSnapshot&, MetadataDelta&)>&, size_t retries = 8,
        std::optional<MetadataMutationIdentity> identity = {}) = 0;

    // Resolves a standing conflict by choosing one side; a commit.
    static constexpr Waits resolve_conflict_waits = mutate_waits;
    virtual bool resolve_conflict(const std::string& id, std::string_view choice) = 0;

    // Diagnostics for Status: conflicts superseded and resolved since start,
    // and where mutation time goes. Waits on nothing.
    virtual uint64_t conflicts_superseded() const noexcept = 0;
    virtual uint64_t conflicts_resolved() const noexcept = 0;
    virtual MetadataMutationTiming mutation_timing() const noexcept = 0;

};

// The metadata component's upkeep of its own replicas, kept off the view so
// no reader sees it. Single owner (the maintenance pass's thread).
// Implemented by MetadataManager.
class MetadataMaintenance {
  public:
    virtual ~MetadataMaintenance() = default;

    // One repair step: converge the replicas toward the accepted heads.
    static constexpr Waits repair_step_waits = Waits::state_device | Waits::network;
    virtual void repair_step() = 0;
    // Records whether the last replica-set validation succeeded, and why
    // not; Status reads it.
    static constexpr Waits note_validation_waits = Waits::none;
    virtual void note_replica_validation(bool available, std::string_view reason = {}) = 0;
    // Repairs accepted heads the local replica cannot reconstruct, from
    // peers; returns how many.
    static constexpr Waits repair_heads_waits = Waits::state_device | Waits::network;
    virtual size_t repair_unreconstructable_heads(FrameType frame_type = FrameType::control) = 0;
    // One round toward re-rooting local history at a checkpoint, gated on
    // its thresholds.
    static constexpr Waits checkpoint_waits = Waits::state_device | Waits::network;
    virtual void attempt_history_checkpoint(size_t record_threshold = 256,
                                            uint64_t byte_threshold = 64ULL * 1024 * 1024) = 0;
};

} // namespace macha
