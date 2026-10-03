// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "storage/local_store.hpp"

#include <functional>
#include <mutex>

namespace macha::test_support {

// The device with a step the test sets, run before each operation: it may
// block the caller, throw as a failing device would, or record what was asked.
// Outlives the store it is handed to.
class InterposedLocalStoreFiles final : public LocalStoreFiles {
  public:
    enum class Op { install, read, read_at, append, truncate, list };
    using Step = std::function<void(Op, const std::filesystem::path&)>;

    void before(Step step) {
        std::lock_guard lock(mutex_);
        step_ = std::move(step);
    }

    void install(const std::filesystem::path& path, std::span<const uint8_t> head,
                 std::span<const uint8_t> body) override {
        run(Op::install, path);
        device_.install(path, head, body);
    }
    Bytes read(const std::filesystem::path& path) override {
        run(Op::read, path);
        return device_.read(path);
    }
    std::optional<Bytes> read_at(const std::filesystem::path& path, uint64_t offset,
                                 size_t size) override {
        run(Op::read_at, path);
        return device_.read_at(path, offset, size);
    }
    void append(const std::filesystem::path& path, std::span<const uint8_t> head,
                std::span<const uint8_t> body) override {
        run(Op::append, path);
        device_.append(path, head, body);
    }
    bool truncate(const std::filesystem::path& path, uint64_t size) override {
        run(Op::truncate, path);
        return device_.truncate(path, size);
    }
    std::vector<std::pair<std::filesystem::path, uint64_t>>
    list(const std::filesystem::path& dir) override {
        run(Op::list, dir);
        return device_.list(dir);
    }

  private:
    void run(Op op, const std::filesystem::path& path) {
        Step step;
        {
            std::lock_guard lock(mutex_);
            step = step_;
        }
        if (step)
            step(op, path);
    }

    std::mutex mutex_;
    Step step_;
    PosixLocalStoreFiles device_;
};

} // namespace macha::test_support
