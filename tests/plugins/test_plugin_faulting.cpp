// SPDX-License-Identifier: GPL-3.0-or-later
//
// A plugin whose start() always throws: the supervisor must mark it faulted
// and disabled without taking the process down.
#include "subsystem/subsystem.hpp"
#include "subsystem/subsystem_abi.hpp"
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
