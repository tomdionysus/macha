// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "retry_policy.hpp"
#include "subsystem/subsystem.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace macha {

struct SubsystemStatus {
    std::string name;
    SubsystemState state{SubsystemState::unavailable};
    size_t restart_count{};
    std::string last_fault;
};

using SubsystemRetryPolicy = RetryPolicy;

// Same contract as a plugin's entry factory (subsystem_abi.hpp).
using SubsystemFactory = std::function<std::unique_ptr<Subsystem>(const SubsystemContext&)>;

// Owns every subsystem's lifecycle: dlopens plugins from `plugin_dir`, checks
// each build identity before calling in, and runs create/start (and faults
// reported through the fault sink) under a backed-off retry loop that
// disables the subsystem after too many failures in a window. A failure
// degrades that subsystem to `faulted`/`disabled`; the process stays up.
class SubsystemSupervisor {
  public:
    explicit SubsystemSupervisor(std::filesystem::path plugin_dir,
                                 SubsystemRetryPolicy policy = {});
    ~SubsystemSupervisor();

    SubsystemSupervisor(const SubsystemSupervisor&) = delete;
    SubsystemSupervisor& operator=(const SubsystemSupervisor&) = delete;

    // Discovers plugins (a missing directory loads none) and starts every
    // lifecycle thread. The context arrives here, not at construction, because
    // its references (IngestManager) are built after Service.
    void start(SubsystemContext context);

    // Supervises a subsystem linked into this binary, with the same lifecycle
    // as a plugin. Call before start(); `name` is what Status reports.
    void add_builtin(std::string name, SubsystemFactory factory);

    // Stops every running subsystem and joins their lifecycle threads.
    void stop();

    std::vector<SubsystemStatus> statuses() const;

  private:
    struct Entry;
    void discover_plugins();
    void run_entry(Entry&, std::stop_token);

    std::filesystem::path plugin_dir_;
    SubsystemContext context_;
    SubsystemRetryPolicy policy_;
    std::vector<std::unique_ptr<Entry>> entries_;
};

} // namespace macha
