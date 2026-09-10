// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "filesystem.hpp"
#include "hydration.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace macha {

class SpoolRetirementRateEstimator {
  public:
    using TimePoint = std::chrono::steady_clock::time_point;
    struct Sample {
        uint64_t bytes{};
        std::chrono::milliseconds elapsed{};
        double bytes_per_second{};
    };

  private:
    std::optional<TimePoint> started_;
    uint64_t bytes_{};

  public:
    void start(TimePoint now) {
        if (!started_)
            started_ = now;
    }
    std::optional<Sample> retire(uint64_t bytes, TimePoint now) {
        if (!bytes)
            return {};
        if (!started_)
            started_ = now;
        bytes_ = bytes > std::numeric_limits<uint64_t>::max() - bytes_
                     ? std::numeric_limits<uint64_t>::max()
                     : bytes_ + bytes;
        const auto elapsed = now - *started_;
        const auto seconds = std::chrono::duration<double>(elapsed).count();
        if (seconds <= 0.0)
            return {};
        return Sample{bytes_, std::chrono::duration_cast<std::chrono::milliseconds>(elapsed),
                      static_cast<double>(bytes_) / seconds};
    }
    void reset() {
        started_.reset();
        bytes_ = 0;
    }
};

// Event-driven duty-cycle gate for loader publication while genuine viewer
// traffic is active. A bounded loader burst is followed by a proportional
// cooldown. The ratio is relative active time, not a static bandwidth cap;
// when viewer traffic is absent the loader is always admitted.
class WeightedLoaderService {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

  private:
    mutable std::mutex mutex_;
    size_t viewer_weight_;
    size_t loader_weight_;
    std::chrono::milliseconds slice_;
    std::optional<TimePoint> burst_started_;
    TimePoint slice_deadline_{};
    TimePoint not_before_{};
    size_t active_loaders_{};

    void reset_locked() {
        burst_started_.reset();
        slice_deadline_ = {};
        not_before_ = {};
    }

  public:
    explicit WeightedLoaderService(size_t viewer_weight = 95, size_t loader_weight = 5,
                                   std::chrono::milliseconds slice =
                                       std::chrono::milliseconds(25))
        : viewer_weight_(viewer_weight), loader_weight_(loader_weight), slice_(slice) {}

    bool can_start(TimePoint now, bool viewer_active) {
        std::lock_guard lock(mutex_);
        if (!viewer_active) {
            reset_locked();
            return true;
        }
        if (!loader_weight_)
            return false;
        if (now < not_before_)
            return false;
        return !burst_started_ || now < slice_deadline_;
    }

    void started(TimePoint now, bool viewer_active, bool begin_service = true) {
        std::lock_guard lock(mutex_);
        ++active_loaders_;
        if (!viewer_active) {
            reset_locked();
            return;
        }
        if (!loader_weight_ || !begin_service)
            return;
        if (!burst_started_) {
            burst_started_ = now;
            slice_deadline_ = now + slice_;
        }
    }

    // A cold loader may be admitted before its distributed writer is ready.
    // Account it as active immediately, but begin its proportional service
    // slice only when it can perform useful bounded work.
    void service_started(TimePoint now, bool viewer_active) {
        std::lock_guard lock(mutex_);
        if (!viewer_active) {
            reset_locked();
            return;
        }
        if (!loader_weight_)
            return;
        if (!burst_started_) {
            burst_started_ = now;
            slice_deadline_ = now + slice_;
        }
    }

    bool should_yield(TimePoint now, bool viewer_active) {
        std::lock_guard lock(mutex_);
        if (!viewer_active) {
            reset_locked();
            return false;
        }
        if (!loader_weight_)
            return true;
        // Viewer arrival during an unrestricted loader quantum yields at the
        // next bounded chunk. Its pipeline drain is accounted as the first
        // contended loader burst before the proportional cooldown.
        if (!burst_started_) {
            burst_started_ = now;
            slice_deadline_ = now;
            return true;
        }
        return now >= slice_deadline_;
    }

    std::chrono::milliseconds finished(TimePoint now, bool viewer_active) {
        std::lock_guard lock(mutex_);
        if (active_loaders_)
            --active_loaders_;
        if (!viewer_active) {
            reset_locked();
            return {};
        }
        if (!loader_weight_ || active_loaders_ || !burst_started_)
            return {};
        auto active = std::chrono::duration_cast<std::chrono::milliseconds>(now - *burst_started_);
        active = std::max(active, std::chrono::milliseconds(1));
        const auto ratio = static_cast<long double>(viewer_weight_) /
                           static_cast<long double>(loader_weight_);
        const auto raw = static_cast<long double>(active.count()) * ratio;
        const auto capped = std::min<long double>(
            raw, static_cast<long double>(std::chrono::hours(24).count()) * 60.0L * 60.0L *
                     1000.0L);
        const auto cooldown = std::chrono::milliseconds(static_cast<int64_t>(capped));
        not_before_ = now + cooldown;
        burst_started_.reset();
        slice_deadline_ = {};
        return cooldown;
    }

    std::chrono::milliseconds wait_for(TimePoint now, bool viewer_active) {
        std::lock_guard lock(mutex_);
        if (!viewer_active) {
            reset_locked();
            return {};
        }
        if (!loader_weight_)
            return std::chrono::hours(24);
        if (now >= not_before_)
            return {};
        return std::chrono::duration_cast<std::chrono::milliseconds>(not_before_ - now);
    }
};

enum class FuseOperationClass : uint8_t {
    lookup,
    namespace_mutation,
    read,
    write,
    sync,
    lifecycle,
};

enum class FuseRequestState : uint8_t {
    queued,
    running,
    cancelled,
    complete,
};

struct FuseEntryAttributes {
    EntryType type{EntryType::file};
    uint32_t mode{0644}, uid{}, gid{};
    uint64_t size{};
    int64_t ctime_ns{}, mtime_ns{};
    uint64_t version{1};
};

class FuseReadSession;

struct FuseOpenHandle {
    uint64_t inode{};
    bool readable{};
    bool writable{};
    bool append{};
    std::shared_ptr<FuseReadSession> read_session;
};

struct FuseFrontendStatus {
    size_t broker_pending{};
    size_t pending_namespace{};
    size_t pending_data{};
    size_t pending_recovery_data{};
    size_t active_data{};
    size_t active_recovery_data{};
    uint64_t timed_out_requests{};
    uint64_t merged_publications{};
    uint64_t data_publication_requests{};
    uint64_t data_publication_notifications_suppressed{};
    uint64_t spool_pressure_publication_sweeps{};
    uint64_t data_publication_coalesced_queued{};
    uint64_t data_publication_coalesced_running{};
    uint64_t data_publication_coalesced_unconfirmed{};
    uint64_t data_publications_started{};
    uint64_t data_publications_completed{};
    uint64_t data_publication_peak_active{};
    uint64_t data_publication_quanta{};
    uint64_t data_publication_yields{};
    uint64_t data_publication_peak_inflight_bytes{};
    uint64_t data_publication_pipeline_limit_bytes{};
    uint64_t data_publication_peak_pipeline_extents{};
    uint64_t data_closed_priority_selections{};
    uint64_t data_retirement_priority_selections{};
    uint64_t open_publications{};
    uint64_t peak_open_publications{};
    uint64_t publication_max_open_writers{};
    uint64_t data_publication_selections_under_writer_cap{};
    // Events that RELEASED publication-owned retained memory: a pipelined
    // extent retiring into the manifest, and a handle committing. This is the
    // counter a writer blocked on admission watches, and the one to read when
    // asking "is this pipeline moving at all". Deliberately not the admitted-
    // quantum count: quanta rise on a wedged node too, because a failure frees
    // a slot that admits the next file.
    uint64_t data_publication_progress_events{};
    uint64_t data_publication_bytes_read{};
    uint64_t data_publication_bytes_committed{};
    uint64_t data_publication_bytes_confirmed{};
    uint64_t data_publication_completed_spool_bytes_read{};
    uint64_t data_publication_completed_source_bytes_read{};
    uint64_t data_publication_completed_reused_extents{};
    uint64_t data_publication_completed_put_extents{};
    uint64_t data_overlay_read_queries{};
    uint64_t data_overlay_ranges_examined{};
    uint64_t data_overlay_descriptors_copied{};
    uint64_t retained_data_operations{};
    uint64_t retained_data_operation_bytes{};
    uint64_t retained_overlay_ranges{};
    uint64_t retained_overlay_bytes{};
    uint64_t retained_publication_operations{};
    uint64_t retained_publication_operation_bytes{};
    uint64_t operation_metadata_bytes{};
    uint64_t peak_operation_metadata_bytes{};
    uint64_t operation_metadata_limit_bytes{};
    uint64_t operation_metadata_waits{};
    uint64_t retained_durability_tickets{};
    uint64_t data_publication_inflight_bytes{};
    uint64_t backend_failures{};
    uint64_t durability_batches{};
    uint64_t durability_writes{};
    uint64_t namespace_operations_admitted{};
    uint64_t namespace_operations_recovered{};
    uint64_t namespace_publication_attempts{};
    uint64_t namespace_publication_batches{};
    uint64_t namespace_operations_batched{};
    uint64_t namespace_operations_published{};
    uint64_t namespace_operations_confirmed{};
    uint64_t journal_append_batches{};
    uint64_t journal_records_appended{};
    uint64_t journal_durability_barriers{};
    uint64_t spool_bytes{};
    uint64_t spool_limit_bytes{};
    uint64_t spool_publish_rate_bytes_per_second{};
    uint64_t spool_publish_rate_window_bytes{};
    uint64_t spool_publish_rate_window_ms{};
    uint64_t spool_throttle_waits{};
    uint64_t spool_throttle_wait_ms{};
    uint64_t pending_write_request_bytes{};
    uint64_t peak_pending_write_request_bytes{};
    uint64_t pending_write_request_limit_bytes{};
    uint64_t extent_executor_workers{};
    uint64_t extent_executor_queued{};
    uint64_t extent_executor_active{};
    uint64_t extent_executor_peak_queued{};
    uint64_t extent_executor_peak_active{};
    uint64_t extent_executor_submitted{};
    // Inode ownership is explicit: the table owns namespace-visible inodes and
    // detached inodes only while a handle, durable namespace operation, data
    // operation, publication or recovery activity still refers to them.
    uint64_t inode_count{};
    uint64_t detached_inode_count{};
    uint64_t peak_inode_count{};
    uint64_t reclaimed_inode_count{};
};

// Lock-free, process-lifetime operational totals suitable for Status. This is
// deliberately separate from FuseFrontendStatus: obtaining queue state may
// inspect live inode state, while observational diagnostics must stay O(1).
struct FuseFrontendDiagnostics {
    uint64_t timed_out_requests{};
    uint64_t merged_publications{};
    uint64_t data_publication_requests{};
    uint64_t data_publication_notifications_suppressed{};
    uint64_t spool_pressure_publication_sweeps{};
    uint64_t data_publication_coalesced_queued{};
    uint64_t data_publication_coalesced_running{};
    uint64_t data_publication_coalesced_unconfirmed{};
    uint64_t data_publications_started{};
    uint64_t data_publications_completed{};
    uint64_t data_publication_peak_active{};
    uint64_t data_publication_quanta{};
    uint64_t data_publication_yields{};
    uint64_t data_publication_peak_inflight_bytes{};
    uint64_t data_publication_pipeline_limit_bytes{};
    uint64_t data_publication_peak_pipeline_extents{};
    uint64_t data_closed_priority_selections{};
    uint64_t data_retirement_priority_selections{};
    // Inodes holding a provisional publication writer, its high-water mark, the
    // bound on it, and how many queue selections happened while that bound was
    // in effect. Each open writer holds up to one extent buffer plus the
    // pipeline in retained memory, so peak_open_publications x (extent_size +
    // pipeline limit) is publication's worst-case claim on the ledger.
    // Selections under the bound rising while completions move is the bound
    // working; rising while completions stay at zero means the open set itself
    // is stuck.
    uint64_t open_publications{};
    uint64_t peak_open_publications{};
    uint64_t publication_max_open_writers{};
    uint64_t data_publication_selections_under_writer_cap{};
    // Events that RELEASED publication-owned retained memory: a pipelined
    // extent retiring into the manifest, and a handle committing. This is the
    // counter a writer blocked on admission watches, and the one to read when
    // asking "is this pipeline moving at all". Deliberately not the admitted-
    // quantum count: quanta rise on a wedged node too, because a failure frees
    // a slot that admits the next file.
    uint64_t data_publication_progress_events{};
    uint64_t data_publication_bytes_read{};
    uint64_t data_publication_bytes_committed{};
    uint64_t data_publication_bytes_confirmed{};
    uint64_t data_publication_completed_spool_bytes_read{};
    uint64_t data_publication_completed_source_bytes_read{};
    uint64_t data_publication_completed_reused_extents{};
    uint64_t data_publication_completed_put_extents{};
    uint64_t data_overlay_read_queries{};
    uint64_t data_overlay_ranges_examined{};
    uint64_t data_overlay_descriptors_copied{};
    uint64_t retained_data_operations{};
    uint64_t retained_data_operation_bytes{};
    uint64_t retained_overlay_ranges{};
    uint64_t retained_overlay_bytes{};
    uint64_t retained_publication_operations{};
    uint64_t retained_publication_operation_bytes{};
    uint64_t operation_metadata_bytes{};
    uint64_t peak_operation_metadata_bytes{};
    uint64_t operation_metadata_limit_bytes{};
    uint64_t operation_metadata_waits{};
    uint64_t retained_durability_tickets{};
    uint64_t data_publication_inflight_bytes{};
    uint64_t backend_failures{};
    uint64_t durability_batches{};
    uint64_t durability_writes{};
    uint64_t namespace_operations_admitted{};
    uint64_t namespace_operations_recovered{};
    uint64_t namespace_publication_attempts{};
    uint64_t namespace_publication_batches{};
    uint64_t namespace_operations_batched{};
    uint64_t namespace_operations_published{};
    uint64_t namespace_operations_confirmed{};
    uint64_t journal_append_batches{};
    uint64_t journal_records_appended{};
    uint64_t journal_durability_barriers{};
    uint64_t spool_bytes{};
    uint64_t spool_limit_bytes{};
    uint64_t spool_publish_rate_bytes_per_second{};
    uint64_t spool_publish_rate_window_bytes{};
    uint64_t spool_publish_rate_window_ms{};
    uint64_t spool_throttle_waits{};
    uint64_t spool_throttle_wait_ms{};
    uint64_t pending_write_request_bytes{};
    uint64_t peak_pending_write_request_bytes{};
    uint64_t pending_write_request_limit_bytes{};
    uint64_t extent_executor_workers{};
    uint64_t extent_executor_queued{};
    uint64_t extent_executor_active{};
    uint64_t extent_executor_peak_queued{};
    uint64_t extent_executor_peak_active{};
    uint64_t extent_executor_submitted{};
    uint64_t inode_count{};
    uint64_t peak_inode_count{};
    uint64_t reclaimed_inode_count{};
    // Namespace revision the mount last adopted versus the newest one the
    // MetadataManager has decoded. refreshed < available means the mount is
    // showing an older namespace than this node already holds.
    uint64_t namespace_refreshed_revision{};
    uint64_t namespace_available_revision{};
    // Files parked after exhausting their publication retry budget; details
    // via FuseFrontend::parked_publications().
    uint64_t parked_publications{};
    uint64_t publication_retries_backed_off{};
    // Failure runs that crossed the escalation threshold and were reported at
    // WARN. Non-zero means a file is failing repeatedly but has not (yet)
    // exhausted its budget -- the state that used to be invisible.
    uint64_t publications_retrying_persistently{};
    // Discipline 3: what recovery resolved rather than refused. Frames the
    // journal loader skipped, bytes quarantined after mid-journal corruption,
    // operations dropped for an inode with no descriptor, and publications
    // abandoned because their file left the namespace.
    uint64_t journal_recovery_skipped_frames{};
    uint64_t journal_recovery_quarantined_bytes{};
    uint64_t recovery_dropped_operations{};
    uint64_t publications_abandoned{};
};

struct FuseDirtyRange {
    uint64_t offset{};
    uint64_t length{};
    auto operator<=>(const FuseDirtyRange&) const = default;
};

// A durable namespace operation stuck on a non-retryable backend error. The
// worker never abandons one of these automatically (see fuse_frontend.cpp's
// namespace_loop() for why); an operator who has independently confirmed it
// is safe to drop can do so explicitly via
// FuseFrontend::skip_blocked_namespace_operation(sequence).
struct BlockedNamespaceOperation {
    uint64_t sequence{};
    std::string kind;
    std::string path;
    std::string secondary_path; // rename's destination, otherwise empty
    int error_code{};
    std::string error_message;
    std::chrono::milliseconds blocked_for{};
};

// A file whose data publication exhausted its retry budget (config
// fuse.publication_retry). The bytes stay in the durable spool; nothing is
// lost and nothing retries until an operator either retries it (the budget
// is reset) or abandons the generation (the spool is retired).
struct ParkedPublication {
    uint64_t inode{};
    std::string path;
    int error_code{};
    std::string error_message;
    size_t attempts{};
    std::chrono::milliseconds failing_for{};
    std::chrono::milliseconds parked_for{};
    uint64_t pending_bytes{};
};

// Bounded local frontend for the kernel-facing filesystem. FUSE callbacks enter
// this object, never MetadataManager/DistributedStore directly. Distributed
// namespace/data publication is queued behind the local inode/namespace state.
class FuseFrontend final : public HydrationHintProvider {
    struct State;
    std::unique_ptr<State> state_;

    std::chrono::milliseconds timeout_for(FuseOperationClass) const;
    bool submit_task(FuseOperationClass, Clock::time_point, std::shared_ptr<std::atomic_bool>,
                     std::function<void(Clock::time_point, std::atomic_bool&)>);
    size_t read_impl(uint64_t inode, const std::shared_ptr<FuseReadSession>&, uint64_t offset,
                     std::span<uint8_t>);

    template <class Fn>
    auto dispatch(FuseOperationClass operation, Fn&& fn)
        -> std::invoke_result_t<Fn, Clock::time_point, std::atomic_bool&> {
        using Result = std::invoke_result_t<Fn, Clock::time_point, std::atomic_bool&>;
        const auto timeout = std::min(timeout_for(operation), absolute_timeout());
        const auto deadline = Clock::now() + timeout;
        const bool complete_once_started =
            operation != FuseOperationClass::lookup && operation != FuseOperationClass::read;
        auto cancelled = std::make_shared<std::atomic_bool>(false);
        auto request_state =
            std::make_shared<std::atomic<FuseRequestState>>(FuseRequestState::queued);
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();

        auto task = [promise, cancelled, request_state, complete_once_started,
                     fn = std::forward<Fn>(fn)](Clock::time_point task_deadline,
                                                std::atomic_bool& task_cancelled) mutable {
            // A mutating FUSE request may be rejected while it is still queued,
            // but once it starts it must have exactly one observable outcome.
            // Returning ETIMEDOUT while a pwrite/truncate/namespace mutation is
            // already executing leaves the kernel unable to know whether the
            // mutation happened. Read-only requests remain cooperatively
            // cancellable after they begin.
            if (Clock::now() >= task_deadline) {
                auto expected = FuseRequestState::queued;
                if (request_state->compare_exchange_strong(expected, FuseRequestState::cancelled,
                                                           std::memory_order_acq_rel)) {
                    task_cancelled.store(true, std::memory_order_relaxed);
                    try {
                        promise->set_exception(std::make_exception_ptr(
                            FsError(ETIMEDOUT, "FUSE request deadline exceeded before execution")));
                    } catch (...) {
                    }
                }
                return;
            }

            auto expected = FuseRequestState::queued;
            if (!request_state->compare_exchange_strong(expected, FuseRequestState::running,
                                                        std::memory_order_acq_rel))
                return;

            try {
                const auto effective_deadline =
                    complete_once_started ? Clock::time_point::max() : task_deadline;
                if constexpr (std::is_void_v<Result>) {
                    fn(effective_deadline, task_cancelled);
                    request_state->store(FuseRequestState::complete, std::memory_order_release);
                    promise->set_value();
                } else {
                    auto result = fn(effective_deadline, task_cancelled);
                    request_state->store(FuseRequestState::complete, std::memory_order_release);
                    promise->set_value(std::move(result));
                }
            } catch (...) {
                request_state->store(FuseRequestState::complete, std::memory_order_release);
                try {
                    promise->set_exception(std::current_exception());
                } catch (...) {
                }
            }
        };

        if (!submit_task(operation, deadline, cancelled, std::move(task)))
            throw FsError(EAGAIN, "FUSE request broker saturated");

        if (future.wait_until(deadline) != std::future_status::ready) {
            auto expected = FuseRequestState::queued;
            if (request_state->compare_exchange_strong(expected, FuseRequestState::cancelled,
                                                       std::memory_order_acq_rel)) {
                cancelled->store(true, std::memory_order_relaxed);
                note_timeout();
                throw FsError(ETIMEDOUT, "FUSE request deadline exceeded before execution");
            }

            if (!complete_once_started && expected == FuseRequestState::running) {
                cancelled->store(true, std::memory_order_relaxed);
                note_timeout();
                throw FsError(ETIMEDOUT, "FUSE request deadline exceeded");
            }

            // A mutation has already started. Wait for its actual result rather
            // than manufacture a timeout while its side effects continue.
            future.wait();
        }
        if constexpr (std::is_void_v<Result>) {
            future.get();
            return;
        } else {
            return future.get();
        }
    }

    void note_timeout();

  public:
    FuseFrontend(FileSystem&, FuseConfig);
    ~FuseFrontend() override;
    FuseFrontend(const FuseFrontend&) = delete;
    FuseFrontend& operator=(const FuseFrontend&) = delete;

    std::string_view name() const override {
        return "fuse";
    }
    std::vector<HydrationHint> hints() override;
    void set_wake_callback(std::function<void()> callback) override;

    std::chrono::milliseconds absolute_timeout() const;
    const FuseConfig& config() const;

    FuseEntryAttributes getattr(std::string_view path);
    std::vector<std::pair<std::string, FuseEntryAttributes>> readdir(std::string_view path);
    void mkdir(std::string_view path, uint32_t mode, uint32_t uid, uint32_t gid);
    void rmdir(std::string_view path);
    void unlink(std::string_view path);
    void rename(std::string_view from, std::string_view to, bool noreplace = false);
    void chmod(std::string_view path, uint32_t mode);
    void chown(std::string_view path, uint32_t uid, uint32_t gid, bool set_uid, bool set_gid);
    void utimens(std::string_view path, int64_t mtime_ns);

    FuseOpenHandle open(std::string_view path, bool readable, bool writable, bool append,
                        bool truncate);
    FuseOpenHandle create(std::string_view path, uint32_t mode, uint32_t uid, uint32_t gid,
                          bool readable, bool writable, bool append);
    size_t read(uint64_t inode, uint64_t offset, std::span<uint8_t>);
    size_t read(const FuseOpenHandle&, uint64_t offset, std::span<uint8_t>);
    size_t write(uint64_t inode, uint64_t offset, std::span<const uint8_t>, bool append = false);
    void truncate(uint64_t inode, uint64_t size);
    void truncate(std::string_view path, uint64_t size);
    void flush(uint64_t inode);
    void fsync(uint64_t inode);
    void release(uint64_t inode, bool writable);

    std::pair<uint64_t, uint64_t> logical_capacity() const;
    // Called by the kernel adapter before viewer-critical open/read callbacks.
    // This must drive the same foreground clock used to gate loader publication.
    void note_viewer_activity(uint64_t bytes = 0);
    std::string path_for_inode(uint64_t inode) const;
    std::optional<uint64_t> inode_for_path(std::string_view path);
    std::vector<FuseDirtyRange> dirty_ranges(uint64_t inode) const;
    FuseFrontendStatus status() const;
    FuseFrontendDiagnostics diagnostics() const noexcept;
    // The namespace operation the publication worker is currently wedged on,
    // if any (see BlockedNamespaceOperation).
    std::optional<BlockedNamespaceOperation> blocked_namespace_operation() const;
    // Operator escape hatch: abandon the operation the worker is currently
    // blocked on. The caller must name the exact sequence number (from
    // blocked_namespace_operation()) to guard against skipping the wrong one
    // if it changed between query and action. Returns false if there is no
    // matching blocked operation right now.
    bool skip_blocked_namespace_operation(uint64_t sequence);
    // Files whose data publication is parked after exhausting its retry
    // budget (see ParkedPublication), and the two operator actions on them.
    std::vector<ParkedPublication> parked_publications() const;
    bool retry_parked_publication(uint64_t inode);
    bool abandon_parked_publication(uint64_t inode);
    bool wait_for_idle(std::chrono::milliseconds timeout = std::chrono::seconds(10));
    void stop();
};

} // namespace macha
