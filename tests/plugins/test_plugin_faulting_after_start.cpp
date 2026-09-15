// SPDX-License-Identifier: GPL-3.0-or-later
//
// A subsystem that starts cleanly and then faults from its own thread, which
// is the shape of a lost FUSE mount and the one SubsystemSupervisor could not
// see until Subsystem::attach_fault_sink existed (see
// TODO/2026-09-14-fuse-supervised-subsystem-plan.md, Stage A). Before that the
// supervisor parked on a condition variable until asked to stop, so a
// subsystem whose background work died after start() stayed `running` forever.
//
// Each instance faults once, shortly after start(), so the supervisor is
// driven through faulted -> restarting -> running for as many cycles as the
// retry policy allows.
#include "subsystem.hpp"
#include "subsystem_abi.hpp"
#include "macha_version.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

class FaultingAfterStartSubsystem final : public macha::Subsystem {
    FaultSink sink_;
    std::atomic_bool stopping_{};
    std::thread worker_;

  public:
    ~FaultingAfterStartSubsystem() override { stop(); }

    std::string_view name() const noexcept override { return "test-plugin-faulting-after-start"; }

    void attach_fault_sink(FaultSink sink) override { sink_ = std::move(sink); }

    void start() override {
        worker_ = std::thread([this] {
            // Long enough that the supervisor has published `running` before
            // the fault arrives, short enough to keep the test quick.
            for (int i = 0; i < 20 && !stopping_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (stopping_.load())
                return;
            if (sink_)
                sink_("test-plugin-faulting-after-start lost its work");
        });
    }

    void stop() override {
        stopping_.store(true);
        if (worker_.joinable())
            worker_.join();
    }
};

std::unique_ptr<macha::Subsystem> create(const macha::SubsystemContext&) {
    return std::make_unique<FaultingAfterStartSubsystem>();
}

const macha::SubsystemPluginEntry kEntry{macha::kBuildIdentity, &create};

} // namespace

extern "C" const macha::SubsystemPluginEntry* macha_subsystem_entry() {
    return &kEntry;
}
