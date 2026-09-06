// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal real subsystem plugin used only by
// foundations/test_subsystem_supervisor_loads_and_stops_a_real_plugin. Starts
// and stops cleanly, proving the real dlopen -> version-check -> construct ->
// start -> stop path works end to end, not just in-process mocks.
#include "subsystem.hpp"
#include "subsystem_abi.hpp"
#include "macha_version.hpp"

namespace {

class OkSubsystem final : public macha::Subsystem {
  public:
    std::string_view name() const noexcept override { return "test-plugin-ok"; }
    void start() override {}
    void stop() override {}
};

std::unique_ptr<macha::Subsystem> create(const macha::SubsystemContext&) {
    return std::make_unique<OkSubsystem>();
}

const macha::SubsystemPluginEntry kEntry{macha::kBuildIdentity, &create};

} // namespace

extern "C" const macha::SubsystemPluginEntry* macha_subsystem_entry() {
    return &kEntry;
}
