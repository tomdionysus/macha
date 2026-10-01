// SPDX-License-Identifier: GPL-3.0-or-later
#include "component/composition_root.hpp"

#include "log.hpp"

#include <map>
#include <stdexcept>

namespace macha {

CompositionRoot::CompositionRoot(LifecycleHook lifecycle, FaultHandler faults)
    : lifecycle_(std::move(lifecycle)), faults_(std::move(faults)) {
    if (!faults_)
        faults_ = [](std::string_view component, const std::string& reason) {
            Log::error("component " + std::string(component) + " fault: " + reason);
        };
}

CompositionRoot::~CompositionRoot() {
    try {
        stop();
    } catch (const std::exception& error) {
        Log::error("composition root: stop during destruction failed: " + std::string(error.what()));
    }
}

void CompositionRoot::external(std::string contract) {
    std::lock_guard lock(mutex_);
    if (start_called_)
        throw std::logic_error("composition root: external contract declared after start");
    externals_.insert(std::move(contract));
}

void CompositionRoot::add_component(std::unique_ptr<Component> component) {
    std::lock_guard lock(mutex_);
    if (start_called_)
        throw std::logic_error("composition root: component added after start");
    for (const auto& entry : entries_)
        if (entry.component->name() == component->name())
            throw std::invalid_argument("composition root: two components named " +
                                        std::string(component->name()));
    entries_.push_back({std::move(component), false});
}

std::vector<size_t> CompositionRoot::ordered_indices() const {
    std::map<std::string, size_t> provider;
    for (size_t i = 0; i < entries_.size(); ++i)
        for (const auto& contract : entries_[i].component->provided()) {
            if (externals_.contains(contract) || !provider.emplace(contract, i).second)
                throw std::invalid_argument("composition root: contract " + contract +
                                            " has two providers");
        }
    // waiting[i]: how many providers component i still waits for.
    std::vector<size_t> waiting(entries_.size());
    std::vector<std::vector<size_t>> dependants(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i)
        for (const auto& contract : entries_[i].component->required()) {
            if (externals_.contains(contract))
                continue;
            const auto found = provider.find(contract);
            if (found == provider.end())
                throw std::invalid_argument("composition root: " +
                                            std::string(entries_[i].component->name()) +
                                            " requires " + contract + ", which nothing provides");
            ++waiting[i];
            dependants[found->second].push_back(i);
        }
    // Repeatedly take the earliest-added component whose providers have all
    // been taken.
    std::vector<size_t> order;
    std::vector<bool> taken(entries_.size());
    while (order.size() < entries_.size()) {
        size_t next = entries_.size();
        for (size_t i = 0; i < entries_.size(); ++i)
            if (!taken[i] && waiting[i] == 0) {
                next = i;
                break;
            }
        if (next == entries_.size()) {
            std::string cycle;
            for (size_t i = 0; i < entries_.size(); ++i)
                if (!taken[i])
                    cycle += (cycle.empty() ? "" : ", ") + std::string(entries_[i].component->name());
            throw std::invalid_argument("composition root: requirements form a cycle among " + cycle);
        }
        taken[next] = true;
        order.push_back(next);
        for (auto dependant : dependants[next])
            --waiting[dependant];
    }
    return order;
}

std::vector<std::string> CompositionRoot::order() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> names;
    for (auto i : ordered_indices())
        names.emplace_back(entries_[i].component->name());
    return names;
}

void CompositionRoot::note(std::string_view step, const Entry& entry) const {
    if (lifecycle_)
        lifecycle_(std::string(step) + " " + std::string(entry.component->name()));
}

void CompositionRoot::start() {
    std::lock_guard lock(mutex_);
    if (start_called_)
        throw std::logic_error("composition root: started twice");
    const auto order = ordered_indices();
    start_called_ = true;
    for (auto i : order) {
        auto& entry = entries_[i];
        entry.component->attach_fault_sink(
            [this, name = std::string(entry.component->name())](std::string reason) {
                faults_(name, reason);
            });
        note("start", entry);
        try {
            entry.component->start();
        } catch (...) {
            stop_locked();
            throw;
        }
        entry.running = true;
        started_.push_back(i);
    }
}

void CompositionRoot::request_stop() {
    std::lock_guard lock(mutex_);
    for (auto it = started_.rbegin(); it != started_.rend(); ++it) {
        auto& entry = entries_[*it];
        if (!entry.running)
            continue;
        note("request_stop", entry);
        entry.component->request_stop();
    }
}

void CompositionRoot::stop() {
    std::lock_guard lock(mutex_);
    stop_locked();
}

void CompositionRoot::stop_locked() {
    for (auto it = started_.rbegin(); it != started_.rend(); ++it) {
        auto& entry = entries_[*it];
        if (!entry.running)
            continue;
        note("stop", entry);
        entry.running = false;
        entry.component->stop();
    }
}

} // namespace macha
