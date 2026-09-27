// SPDX-License-Identifier: GPL-3.0-or-later
//
// A subsystem that writes into the store from its own stop(), used by
// subsystem_supervisor/test_service_stops_plugins_before_the_store. A plugin
// such as the torrent subsystem publishes into the store right up to the
// moment it is stopped, so the node must still accept its writes then.
//
// The outcome is written to the file named by MACHA_TEST_STOP_WRITE_RESULT:
// "started" once start() has run, then "ok" or "failed: <why>" from stop().
#include "filesystem/filesystem.hpp"
#include "subsystem/subsystem.hpp"
#include "subsystem/subsystem_abi.hpp"
#include "macha_version.hpp"

#include <cstdlib>
#include <fstream>
#include <random>

namespace {

void record(const std::string& outcome) {
    const char* path = std::getenv("MACHA_TEST_STOP_WRITE_RESULT");
    if (!path) return;
    std::ofstream(path, std::ios::trunc) << outcome;
}

class WritesOnStopSubsystem final : public macha::Subsystem {
    macha::FileSystem* filesystem_;

  public:
    explicit WritesOnStopSubsystem(macha::FileSystem* filesystem) : filesystem_(filesystem) {}
    std::string_view name() const noexcept override { return "test-plugin-writes-on-stop"; }
    void start() override { record("started"); }
    void stop() override {
        std::vector<uint8_t> bytes(4096);
        std::random_device random;
        for (auto& byte : bytes) byte = static_cast<uint8_t>(random());
        try {
            (void)filesystem_->store().put(bytes, macha::FrameType::loader);
            record("ok");
        } catch (const std::exception& error) {
            record(std::string("failed: ") + error.what());
        }
    }
};

std::unique_ptr<macha::Subsystem> create(const macha::SubsystemContext& context) {
    if (!context.filesystem) return {};
    return std::make_unique<WritesOnStopSubsystem>(context.filesystem);
}

const macha::SubsystemPluginEntry kEntry{macha::kBuildIdentity, &create};

} // namespace

extern "C" const macha::SubsystemPluginEntry* macha_subsystem_entry() {
    return &kEntry;
}
