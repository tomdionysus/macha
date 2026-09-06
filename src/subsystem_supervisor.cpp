// SPDX-License-Identifier: GPL-3.0-or-later
#include "subsystem_supervisor.hpp"
#include "log.hpp"
#include "macha_version.hpp"
#include "subsystem_abi.hpp"
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
    std::filesystem::path plugin_path;
    void* handle{};
    const SubsystemPluginEntry* plugin{};

    mutable std::mutex mutex;
    std::condition_variable_any cv;
    SubsystemState state{SubsystemState::unavailable};
    size_t restart_count{};
    std::string last_fault;
    std::unique_ptr<Subsystem> instance;

    std::jthread lifecycle;

    // Deliberately no dlclose. Unmapping a plugin's code invalidates anything
    // of it that outlives the Subsystem instance -- a shared_ptr's deleter
    // and control block, a vtable, a std::function -- and core legitimately
    // holds such references (SubsystemRegistry hands out
    // std::shared_ptr<TorrentService>, whose destructor lives in the plugin).
    // Closing the library on supervisor stop crashed
    // rpc_cluster/test_ingest_torrent_jobs_visible_and_actionable_from_non_owning_node
    // in exactly that way: the last shared_ptr released after the unmap.
    // Restarting a faulted subsystem re-creates the instance from the library
    // that is still loaded, so nothing needs the unload; loading a *new build*
    // of a plugin without restarting the process is explicitly out of scope
    // (see the plan's "Risks and open questions").
};

SubsystemSupervisor::SubsystemSupervisor(std::filesystem::path plugin_dir,
                                         SubsystemRetryPolicy policy)
    : plugin_dir_(std::move(plugin_dir)), policy_(policy) {}

SubsystemSupervisor::~SubsystemSupervisor() {
    stop();
}

void SubsystemSupervisor::start(SubsystemContext context) {
    context_ = context;

    std::error_code discovery_error;
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

        ::dlerror(); // clear any pending error before dlsym, per dlsym(3).
        auto* raw_symbol = ::dlsym(entry->handle, kSubsystemEntrySymbol);
        if (const char* symbol_error = ::dlerror(); symbol_error || !raw_symbol) {
            // Not a real plugin at all, most likely: `plugin_path` defaults
            // to the executable's own directory, which on a real install can
            // be a shared system bindir holding unrelated .so files (e.g.
            // /usr/bin/ld.so on Debian/RPi OS) that happen to match the
            // extension filter. That is an expected, harmless occurrence,
            // not a broken deployment -- log it quietly and don't report a
            // fake "subsystem" for it at all. Unloading is safe here, unlike
            // for a real plugin (see Entry): nothing of this library was ever
            // called, so nothing can hold a reference into it.
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

        entries_.push_back(std::move(entry));
    }

    for (auto& entry : entries_) {
        if (entry->state == SubsystemState::disabled)
            continue; // failed to load above; nothing to run.
        Entry* raw = entry.get();
        raw->lifecycle = std::jthread([this, raw](std::stop_token stop) {
            run_supervised(raw->name, [this, raw, stop] { run_entry(*raw, stop); });
        });
    }
}

void SubsystemSupervisor::run_entry(Entry& entry, std::stop_token stop) {
    RetryState retry;

    while (!stop.stop_requested()) {
        {
            std::lock_guard lock(entry.mutex);
            entry.state = SubsystemState::starting;
        }

        std::unique_ptr<Subsystem> instance;
        bool declined = false;
        std::string fault;
        try {
            instance = entry.plugin->create(context_);
            if (instance)
                instance->start();
            else
                declined = true;
        } catch (const std::exception& e) {
            fault = e.what();
        } catch (...) {
            fault = "unknown exception";
        }

        if (declined) {
            // The plugin loaded and its factory ran, but this node is
            // configured not to run the capability (e.g. torrent.enabled is
            // false). That is a deliberate operator choice, not a fault: no
            // instance, no retry, no backoff, and Status says `unavailable`
            // rather than `disabled`.
            Log::info("subsystem plugin '" + entry.name +
                      "' loaded but its capability is not enabled on this node path=" +
                      entry.plugin_path.string());
            std::lock_guard lock(entry.mutex);
            entry.state = SubsystemState::unavailable;
            return;
        }

        if (fault.empty()) {
            // Every terminal outcome of a load says so at INFO. Without this
            // the successful case was the only silent one, so confirming that
            // a deployed plugin was actually picked up meant inspecting
            // /proc/<pid>/maps -- see docs/operations.md, "Subsystem plugins".
            Log::info("subsystem plugin '" + entry.name + "' loaded and running path=" +
                      entry.plugin_path.string());
            {
                std::lock_guard lock(entry.mutex);
                entry.state = SubsystemState::running;
                entry.instance = std::move(instance);
            }
            retry.succeeded(); // a clean start resets backoff.

            std::unique_lock lock(entry.mutex);
            entry.cv.wait(lock, stop, [] { return false; });
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
            return;
        }

        Log::error("subsystem '" + entry.name + "' failed to start: " + fault);
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
                      " failed start attempts; needs an operator");
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
