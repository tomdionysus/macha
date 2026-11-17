// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "contract/work.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace macha {

// Immutable provenance and bounds for DATA-plane work. CONTROL stays outside
// this pool on its own reserved transport and executors.
class DataWorkContext : public WorkContext {
    uint64_t quantum_bytes_{};
    // No-progress budget, distinct from the absolute deadline: slow work is
    // not cancelled, but work not moving at all fails. The counter is shared
    // by a pipeline's workers, so progress anywhere re-arms the window.
    // Co-owned: a store write keeps it for as long as its work runs.
    std::shared_ptr<const std::atomic_uint64_t> progress_;
    std::chrono::milliseconds no_progress_budget_{};

  public:
    explicit DataWorkContext(FrameType frame_type = FrameType::loader, uint64_t quantum_bytes = 0,
                             Clock::time_point deadline = {}, std::atomic_bool* cancelled = nullptr,
                             std::shared_ptr<const std::atomic_uint64_t> progress = {},
                             std::chrono::milliseconds no_progress_budget = {})
        : WorkContext(frame_type, deadline, cancelled), quantum_bytes_(quantum_bytes),
          progress_(std::move(progress)), no_progress_budget_(no_progress_budget) {
        if (work_class(frame_type) == WorkClass::control)
            throw std::invalid_argument("control is not a DATA work class");
    }

    // Present only when the caller supplied both a counter and a budget.
    const std::atomic_uint64_t* progress() const noexcept {
        return no_progress_budget_.count() ? progress_.get() : nullptr;
    }
    std::shared_ptr<const std::atomic_uint64_t> shared_progress() const noexcept {
        return no_progress_budget_.count() ? progress_ : nullptr;
    }
    std::chrono::milliseconds no_progress_budget() const noexcept {
        return no_progress_budget_;
    }

    uint64_t quantum_bytes() const noexcept {
        return quantum_bytes_;
    }
};

} // namespace macha
