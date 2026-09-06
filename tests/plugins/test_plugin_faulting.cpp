// SPDX-License-Identifier: GPL-3.0-or-later
//
// A subsystem whose start() always throws, used only by
// foundations/test_subsystem_supervisor_disables_a_repeatedly_faulting_plugin
// to prove the exact failure mode that crash-looped corvus-es-1 (a
// subsystem's construction/start throwing) degrades to a per-subsystem
// faulted/disabled state instead of taking the whole process down.
#include "subsystem.hpp"
#include "subsystem_abi.hpp"
#include "macha_version.hpp"

#include <stdexcept>

namespace {

class FaultingSubsystem final : public macha::Subsystem {
  public:
    std::string_view name() const noexcept override { return "test-plugin-faulting"; }
    void start() override {
        throw std::runtime_error("test-plugin-faulting always fails to start");
    }
    void stop() override {}
};

std::unique_ptr<macha::Subsystem> create(const macha::SubsystemContext&) {
    return std::make_unique<FaultingSubsystem>();
}

const macha::SubsystemPluginEntry kEntry{macha::kBuildIdentity, &create};

} // namespace

extern "C" const macha::SubsystemPluginEntry* macha_subsystem_entry() {
    return &kEntry;
}
