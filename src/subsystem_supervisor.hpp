// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "retry_policy.hpp"
#include "subsystem.hpp"

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

// How aggressively a failing subsystem is retried before being disabled: the
// one RetryPolicy every retried work item shares (retry_policy.hpp). The
// defaults here are the production ones; tests override them to keep a
// fault-injection run fast and deterministic.
using SubsystemRetryPolicy = RetryPolicy;

// Constructs one subsystem instance, with the same contract as a plugin's
// entry factory (subsystem_abi.hpp): returning no instance means "this node
// is configured not to run this capability" and throwing means the attempt
// failed and is subject to the retry/disable policy.
using SubsystemFactory = std::function<std::unique_ptr<Subsystem>(const SubsystemContext&)>;

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
// `disabled` state instead.
//
// A fault the subsystem discovers on its own thread *after* a successful
// start reaches the same machinery through Subsystem::attach_fault_sink.
// That half was deliberately left until a subsystem needed it; FUSE does
// (a lost kernel mount is exactly this shape), so it is designed against a
// real one rather than guessed at.
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

    // Supervise a subsystem that is linked into this binary rather than
    // loaded from a plugin file. It gets the identical lifecycle: the same
    // create/start attempt, the same fault sink, the same backoff/disable
    // policy, and the same `subsystems` entry in Status.
    //
    // This is what lets FUSE move behind the supervisor (Stage A) before its
    // libfuse adapter moves into libmacha-fuse (Stage B) -- the crash
    // isolation is the valuable half and does not need the dlopen. See
    // TODO/2026-09-14-fuse-supervised-subsystem-plan.md.
    //
    // Must be called before start(); builtins registered afterwards are not
    // run. `name` is what Status reports, alongside the plugin-derived names.
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
