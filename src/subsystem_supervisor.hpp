// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "retry_policy.hpp"
#include "subsystem.hpp"

#include <chrono>
#include <filesystem>
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

// How aggressively a failing subsystem is retried before being disabled: the
// one RetryPolicy every retried work item shares (retry_policy.hpp). The
// defaults here are the production ones; tests override them to keep a
// fault-injection run fast and deterministic.
using SubsystemRetryPolicy = RetryPolicy;

// Owns the lifecycle of every subsystem loaded as a plugin: discovers
// .so/.dylib files in `plugin_dir`, dlopens each, checks its build-identity
// stamp against this process's own (see subsystem_abi.hpp) before ever
// calling into it, and keeps the resulting Subsystem's construction/start
// attempt behind a supervised, backed-off retry loop that disables itself
// after too many failures in a window rather than crash-looping forever.
//
// This directly targets the failure mode that motivated it: FuseFrontend's
// constructor throwing during journal replay crashed the whole corvus-es-1
// process 49 times (see TODO/2026-09-05-subsystem-plugin-isolation-plan.md).
// A construction/start failure here degrades to a per-subsystem `faulted`/
// `disabled` state instead. Detecting a subsystem's own background thread
// dying *after* a successful start is intentionally not attempted yet -- no
// subsystem has migrated onto this interface (Phase 1/2), and it is better
// designed against a real one than guessed at now.
class SubsystemSupervisor {
  public:
    explicit SubsystemSupervisor(std::filesystem::path plugin_dir,
                                 SubsystemRetryPolicy policy = {});
    ~SubsystemSupervisor();

    SubsystemSupervisor(const SubsystemSupervisor&) = delete;
    SubsystemSupervisor& operator=(const SubsystemSupervisor&) = delete;

    // Discovers every plugin in the configured directory and starts each
    // one's supervised lifecycle thread. Safe to call when the directory
    // does not exist or holds no plugins -- that just means nothing loads.
    //
    // The context is supplied here rather than at construction because the
    // references in it (IngestManager in particular) do not exist until
    // Service::initialise_services has run, long after Service itself is
    // constructed.
    void start(SubsystemContext context);

    // Stops every running subsystem and joins their lifecycle threads.
    void stop();

    std::vector<SubsystemStatus> statuses() const;

  private:
    struct Entry;
    void run_entry(Entry&, std::stop_token);

    std::filesystem::path plugin_dir_;
    SubsystemContext context_;
    SubsystemRetryPolicy policy_;
    std::vector<std::unique_ptr<Entry>> entries_;
};

} // namespace macha
