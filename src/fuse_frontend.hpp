// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "config.hpp"
#include "filesystem.hpp"
#include "hydration.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace macha {

enum class FuseOperationClass : uint8_t {
    lookup,
    namespace_mutation,
    read,
    write,
    sync,
    lifecycle,
};

struct FuseOpenHandle {
    uint64_t inode{};
    bool readable{};
    bool writable{};
    bool append{};
};

struct FuseFrontendStatus {
    size_t broker_pending{};
    size_t pending_namespace{};
    size_t pending_data{};
    size_t active_data{};
    uint64_t timed_out_requests{};
    uint64_t merged_publications{};
    uint64_t backend_failures{};
};

struct FuseDirtyRange {
    uint64_t offset{};
    uint64_t length{};
    auto operator<=>(const FuseDirtyRange&) const = default;
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

    template <class Fn>
    auto dispatch(FuseOperationClass operation, Fn&& fn)
        -> std::invoke_result_t<Fn, Clock::time_point, std::atomic_bool&> {
        using Result = std::invoke_result_t<Fn, Clock::time_point, std::atomic_bool&>;
        auto timeout = std::min(timeout_for(operation), absolute_timeout());
        const auto deadline = Clock::now() + timeout;
        auto cancelled = std::make_shared<std::atomic_bool>(false);
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();

        auto task = [promise, cancelled, fn = std::forward<Fn>(fn)](
                        Clock::time_point task_deadline, std::atomic_bool& task_cancelled) mutable {
            try {
                if (task_cancelled.load(std::memory_order_relaxed) ||
                    Clock::now() >= task_deadline)
                    throw FsError(ETIMEDOUT, "FUSE request deadline exceeded before execution");
                if constexpr (std::is_void_v<Result>) {
                    fn(task_deadline, task_cancelled);
                    promise->set_value();
                } else {
                    promise->set_value(fn(task_deadline, task_cancelled));
                }
            } catch (...) {
                try {
                    promise->set_exception(std::current_exception());
                } catch (...) {
                }
            }
        };

        if (!submit_task(operation, deadline, cancelled, std::move(task)))
            throw FsError(EAGAIN, "FUSE request broker saturated");

        if (future.wait_until(deadline) != std::future_status::ready) {
            cancelled->store(true, std::memory_order_relaxed);
            note_timeout();
            throw FsError(ETIMEDOUT, "FUSE request deadline exceeded");
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

    std::string_view name() const override { return "fuse"; }
    std::vector<HydrationHint> hints() override;

    std::chrono::milliseconds absolute_timeout() const;
    const FuseConfig& config() const;

    FsEntry getattr(std::string_view path);
    std::vector<std::pair<std::string, FsEntry>> readdir(std::string_view path);
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
    size_t write(uint64_t inode, uint64_t offset, std::span<const uint8_t>, bool append = false);
    void truncate(uint64_t inode, uint64_t size);
    void truncate(std::string_view path, uint64_t size);
    void flush(uint64_t inode);
    void fsync(uint64_t inode);
    void release(uint64_t inode, bool writable);

    std::pair<uint64_t, uint64_t> logical_capacity() const;
    void note_interactive_activity(uint64_t bytes = 0);
    std::string path_for_inode(uint64_t inode) const;
    std::optional<uint64_t> inode_for_path(std::string_view path) const;
    std::vector<FuseDirtyRange> dirty_ranges(uint64_t inode) const;
    FuseFrontendStatus status() const;
    bool wait_for_idle(std::chrono::milliseconds timeout = std::chrono::seconds(10));
    void stop();
};

} // namespace macha
