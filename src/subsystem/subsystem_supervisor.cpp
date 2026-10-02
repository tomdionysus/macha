// SPDX-License-Identifier: GPL-3.0-or-later
#include "subsystem/subsystem_supervisor.hpp"
#include "log.hpp"
#include "macha_version.hpp"
#include "subsystem/subsystem_abi.hpp"
#include "supervised.hpp"
#include "types.hpp"

#include <condition_variable>
#include <deque>
#include <dlfcn.h>
#include <mutex>
#include <system_error>
#include <thread>

namespace macha {
namespace {

bool has_plugin_extension(const std::filesystem::path& path) {
    const auto ext = path.extension().string();
    return ext == ".so" || ext == ".dylib";
}

} // namespace

struct SubsystemSupervisor::Entry {
    std::string name;
    // Empty for a builtin (add_builtin): it came from this binary, not a file.
    std::filesystem::path plugin_path;
    void* handle{};
    const SubsystemPluginEntry* plugin{};
    SubsystemFactory factory;

    mutable std::mutex mutex;
    std::condition_variable_any cv;
    // Not yet tried until the first attempt decides.
    SubsystemState state{SubsystemState::starting};
    size_t restart_count{};
    std::string last_fault;
    std::unique_ptr<Subsystem> instance;
    // Set by the instance's fault sink; run_entry tears down and retries.
    bool fault_requested{};
    std::string pending_fault;

    std::jthread lifecycle;

    std::string origin() const {
        return plugin_path.empty() ? std::string("<builtin>") : plugin_path.string();
    }

    // Never dlclose: core holds plugin code beyond the instance's life (e.g.
    // a shared_ptr<TorrentService> whose deleter lives in the plugin).
    // Restarts reuse the loaded library; hot-loading a new build is out of scope.
};

SubsystemSupervisor::SubsystemSupervisor(std::filesystem::path plugin_dir,
                                         SubsystemRetryPolicy policy)
    : plugin_dir_(std::move(plugin_dir)), policy_(policy) {}

void SubsystemSupervisor::add_builtin(std::string name, SubsystemFactory factory) {
    if (!factory)
        return;
    auto entry = std::make_unique<Entry>();
    entry->name = std::move(name);
    entry->factory = std::move(factory);
    entries_.push_back(std::move(entry));
}

SubsystemSupervisor::~SubsystemSupervisor() {
    stop();
}

void SubsystemSupervisor::start(SubsystemContext context) {
    context_ = context;
    discover_plugins();

    for (auto& entry : entries_) {
        if (entry->state == SubsystemState::disabled)
            continue; // failed to load
        Entry* raw = entry.get();
        raw->lifecycle = std::jthread([this, raw](std::stop_token stop) {
            run_supervised_loop(raw->name, stop, [this, raw, stop] { run_entry(*raw, stop); });
        });
    }
}

void SubsystemSupervisor::discover_plugins() {
    std::error_code discovery_error;
    // No plugin directory is ordinary: builtins are already in entries_.
    if (!std::filesystem::is_directory(plugin_dir_, discovery_error))
        return;

    for (const auto& candidate : std::filesystem::directory_iterator(plugin_dir_, discovery_error)) {
        if (discovery_error) {
            Log::warn("subsystem plugin discovery failed dir=" + plugin_dir_.string() +
                      " error=" + discovery_error.message());
            break;
        }
        if (!candidate.is_regular_file() || !has_plugin_extension(candidate.path()))
            continue;

        auto entry = std::make_unique<Entry>();
        entry->name = candidate.path().stem().string();
        entry->plugin_path = candidate.path();

        entry->handle = ::dlopen(candidate.path().c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!entry->handle) {
            Log::error("subsystem plugin '" + entry->name +
                      "' failed to load: " + std::string(::dlerror()));
            entry->state = SubsystemState::disabled;
            entries_.push_back(std::move(entry));
            continue;
        }

        ::dlerror(); // clear any pending error, per dlsym(3)
        auto* raw_symbol = ::dlsym(entry->handle, kSubsystemEntrySymbol);
        if (const char* symbol_error = ::dlerror(); symbol_error || !raw_symbol) {
            // Not a plugin: the default plugin_path is the executable's
            // directory, which may hold unrelated .so files (/usr/bin/ld.so).
            // Log quietly and report nothing. Unloading is safe: none of its
            // code was called.
            Log::debug("subsystem plugin candidate '" + entry->name +
                      "' has no entry symbol '" + kSubsystemEntrySymbol +
                      "', skipping as not a macha plugin: " +
                      (symbol_error ? symbol_error : "symbol resolved to null"));
            ::dlclose(entry->handle);
            continue;
        }

        const auto entry_function = reinterpret_cast<SubsystemEntryFunction>(raw_symbol);
        entry->plugin = entry_function();
        if (!entry->plugin) {
            Log::error("subsystem plugin '" + entry->name + "' entry function returned null");
            entry->state = SubsystemState::disabled;
            entries_.push_back(std::move(entry));
            continue;
        }
        if (entry->plugin->build_identity != kBuildIdentity) {
            Log::error("subsystem plugin '" + entry->name +
                      "' build identity mismatch: plugin=" +
                      std::string(entry->plugin->build_identity) +
                      " core=" + std::string(kBuildIdentity) +
                      "; refusing to load (partial deploy?)");
            entry->state = SubsystemState::disabled;
            entries_.push_back(std::move(entry));
            continue;
        }

        entry->factory = [plugin = entry->plugin](const SubsystemContext& context) {
            return plugin->create(context);
        };
        entries_.push_back(std::move(entry));
    }
}

void SubsystemSupervisor::run_entry(Entry& entry, std::stop_token stop) {
    RetryState retry;

    while (!stop.stop_requested()) {
        {
            std::lock_guard lock(entry.mutex);
            // Status tells a first start from a rebuild.
            entry.state = entry.restart_count ? SubsystemState::restarting
                                              : SubsystemState::starting;
            entry.fault_requested = false;
            entry.pending_fault.clear();
        }

        std::unique_ptr<Subsystem> instance;
        bool declined = false;
        std::string fault;
        try {
            // Lets stop() cancel a blocking factory (FuseFrontend waits for
            // the initial namespace).
            SubsystemContext attempt = context_;
            attempt.startup_stop = stop;
            instance = entry.factory(attempt);
            if (instance) {
                // Before start(): threads started inside start() can report.
                instance->attach_fault_sink([&entry](std::string reason) {
                    std::lock_guard lock(entry.mutex);
                    if (entry.fault_requested)
                        return; // first reason wins
                    entry.fault_requested = true;
                    entry.pending_fault =
                        reason.empty() ? std::string("subsystem reported a fault")
                                       : std::move(reason);
                    entry.cv.notify_all();
                });
                instance->start();
            } else {
                declined = true;
            }
        } catch (const std::exception& e) {
            fault = e.what();
        } catch (...) {
            fault = "unknown exception";
        }

        if (declined) {
            // Configured off on this node (e.g. torrent.enabled false): not a
            // fault, so no retry and Status says `unavailable`.
            Log::info("subsystem plugin '" + entry.name +
                      "' loaded but its capability is not enabled on this node path=" +
                      entry.origin());
            std::lock_guard lock(entry.mutex);
            entry.state = SubsystemState::unavailable;
            return;
        }

        if (fault.empty()) {
            Log::info("subsystem plugin '" + entry.name + "' loaded and running path=" +
                      entry.origin());
            {
                std::lock_guard lock(entry.mutex);
                entry.state = SubsystemState::running;
                entry.instance = std::move(instance);
            }
            retry.succeeded(); // resets backoff, keeps the failure window

            // Park until stopping or a reported fault. The kept failure window
            // drives a flapping subsystem into `disabled`.
            std::unique_lock lock(entry.mutex);
            entry.cv.wait(lock, stop, [&entry] { return entry.fault_requested; });
            const bool post_start_fault = entry.fault_requested;
            if (post_start_fault)
                fault = std::move(entry.pending_fault);
            entry.fault_requested = false;
            entry.pending_fault.clear();
            auto owned = std::move(entry.instance);
            lock.unlock();

            if (owned) {
                try {
                    owned->stop();
                } catch (const std::exception& e) {
                    Log::warn("subsystem '" + entry.name + "' stop() threw: " + e.what());
                } catch (...) {
                    Log::warn("subsystem '" + entry.name + "' stop() threw an unknown exception");
                }
            }
            // Destroy before building the replacement; its fault sink points
            // at this entry until then.
            owned.reset();

            if (!post_start_fault)
                return; // ordinary shutdown

            Log::error("subsystem '" + entry.name + "' faulted while running: " + fault);
        } else {
            // Destroy before the retry delay, not after the next attempt.
            instance.reset();
            Log::error("subsystem '" + entry.name + "' failed to start: " + fault);
        }

        {
            std::lock_guard lock(entry.mutex);
            entry.last_fault = fault;
            ++entry.restart_count;
        }

        const auto delay = retry.failed(policy_);
        if (!delay) {
            std::lock_guard lock(entry.mutex);
            entry.state = SubsystemState::disabled;
            Log::error("subsystem '" + entry.name + "' disabled after " +
                      std::to_string(retry.failures_in_window()) +
                      " failed attempts; needs an operator");
            return;
        }

        {
            std::lock_guard lock(entry.mutex);
            entry.state = SubsystemState::faulted;
        }
        std::unique_lock lock(entry.mutex);
        entry.cv.wait_for(lock, stop, *delay, [] { return false; });
    }
}

void SubsystemSupervisor::stop() {
    for (auto& entry : entries_) {
        if (entry->lifecycle.joinable()) {
            entry->lifecycle.request_stop();
            entry->cv.notify_all();
        }
    }
    for (auto& entry : entries_) {
        if (entry->lifecycle.joinable())
            entry->lifecycle.join();
    }
    entries_.clear();
}

std::vector<SubsystemStatus> SubsystemSupervisor::statuses() const {
    std::vector<SubsystemStatus> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) {
        std::lock_guard lock(entry->mutex);
        result.push_back(SubsystemStatus{entry->name, entry->state, entry->restart_count,
                                         entry->last_fault});
    }
    return result;
}

} // namespace macha
