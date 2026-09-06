// SPDX-License-Identifier: GPL-3.0-or-later
//
// A subsystem plugin that deliberately reports a build identity that cannot
// match any real core build, used only by
// foundations/test_subsystem_supervisor_refuses_a_mismatched_plugin to prove
// a partial-deploy-style ABI skew is refused at load time rather than run.
#include "subsystem.hpp"
#include "subsystem_abi.hpp"

namespace {

class MismatchedSubsystem final : public macha::Subsystem {
  public:
    std::string_view name() const noexcept override { return "test-plugin-mismatched-abi"; }
    void start() override {}
    void stop() override {}
};

std::unique_ptr<macha::Subsystem> create(const macha::SubsystemContext&) {
    return std::make_unique<MismatchedSubsystem>();
}

constexpr std::string_view kBogusBuildIdentity = "0.0.0+mismatched-build-for-testing";
const macha::SubsystemPluginEntry kEntry{kBogusBuildIdentity, &create};

} // namespace

extern "C" const macha::SubsystemPluginEntry* macha_subsystem_entry() {
    return &kEntry;
}
