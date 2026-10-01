// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "component/component.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The composition root (the object ledger spec, A5): owns the components
// added to it and runs their lifecycle in the order their declarations
// give. A component starts after every component providing what it
// requires; where the graph leaves the order free, the order of adding
// decides. Stopping runs the start order backwards.
//
// At stage 0 most of the node is still built by Service, outside the root;
// what Service hands in is declared external, so every requirement is
// accounted for.
//
// Thread-safe: each call is one step under the root's lock, so a stop
// requested from another thread while start() runs waits for the start in
// progress to return (its components are then asked to stop).
namespace macha {

class CompositionRoot {
  public:
    // Each lifecycle step, before it is taken: "start <name>",
    // "request_stop <name>", "stop <name>".
    using LifecycleHook = std::function<void(std::string_view event)>;
    // A fault a component's own threads reported after it started.
    using FaultHandler = std::function<void(std::string_view component, const std::string& reason)>;

    explicit CompositionRoot(LifecycleHook lifecycle = {}, FaultHandler faults = {});
    // Stops whatever is still running.
    ~CompositionRoot();
    CompositionRoot(const CompositionRoot&) = delete;
    CompositionRoot& operator=(const CompositionRoot&) = delete;

    // A contract supplied from outside the root. Before start().
    void external(std::string contract);

    // Takes ownership. Before start(); a component's name is unique.
    template <class T> T& add(std::unique_ptr<T> component) {
        auto& added = *component;
        add_component(std::unique_ptr<Component>(std::move(component)));
        return added;
    }

    // The start order the declarations give. Throws std::invalid_argument,
    // naming the contracts, when a requirement has no provider, a contract
    // has two, or the requirements form a cycle.
    std::vector<std::string> order() const;

    // Starts every component in order(). Once only. When a start throws, the
    // components already started are stopped, in reverse, and the exception
    // is rethrown.
    void start();
    // Asks every running component to stop, in reverse order. Blocks only
    // for a start() in progress on another thread.
    void request_stop();
    // Stops every running component in reverse order. A stopped component is
    // not stopped again, and is no longer asked to stop.
    void stop();

  private:
    struct Entry {
        std::unique_ptr<Component> component;
        bool running{};
    };
    void add_component(std::unique_ptr<Component>);
    std::vector<size_t> ordered_indices() const;
    void stop_locked();
    void note(std::string_view step, const Entry&) const;

    mutable std::mutex mutex_;

    LifecycleHook lifecycle_;
    FaultHandler faults_;
    std::set<std::string> externals_;
    std::vector<Entry> entries_;
    // Indices into entries_, in the order they were started.
    std::vector<size_t> started_;
    bool start_called_{};
};

} // namespace macha
