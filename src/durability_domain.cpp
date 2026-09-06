// SPDX-License-Identifier: GPL-3.0-or-later
#include "durability_domain.hpp"
#include "diagnostics.hpp"
#include "supervised.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace macha {
namespace {

#if !defined(__linux__)
void sync_file(const std::filesystem::path& path) {
    if (path.empty())
        return;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT)
            return;
        throw std::runtime_error("cannot open durability file: " + std::string(strerror(errno)));
    }
    int rc;
    do { rc = ::fsync(fd); } while (rc != 0 && errno == EINTR);
    const int saved = errno;
    ::close(fd);
    if (rc != 0)
        throw std::runtime_error("cannot sync durability file: " + std::string(strerror(saved)));
}

void sync_directory(const std::filesystem::path& path) {
    if (path.empty())
        return;
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        if (errno == ENOENT)
            return;
        throw std::runtime_error("cannot open durability directory: " +
                                 std::string(strerror(errno)));
    }
    int rc;
    do { rc = ::fsync(fd); } while (rc != 0 && errno == EINTR);
    const int saved = errno;
    ::close(fd);
    if (rc != 0)
        throw std::runtime_error("cannot sync durability directory: " +
                                 std::string(strerror(saved)));
}
#endif

} // namespace

DurabilityDomain::DurabilityDomain(uint64_t id, std::filesystem::path representative,
                                   std::chrono::milliseconds batch_window)
    : id_(id), batch_window_(batch_window) {
    if (!id_)
        throw std::runtime_error("durability domain id must be non-zero");
    if (batch_window_ < std::chrono::milliseconds::zero())
        throw std::runtime_error("durability batch window cannot be negative");
    add_representative(std::move(representative));
    worker_ = std::jthread([this](std::stop_token stop) {
        run_supervised("durability-domain", [this, stop] { loop(stop); });
    });
}

DurabilityDomain::~DurabilityDomain() {
    worker_.request_stop();
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void DurabilityDomain::add_representative(std::filesystem::path path) {
    if (path.empty())
        return;
    path = path.lexically_normal();
    std::lock_guard lock(mutex_);
    if (std::find(representatives_.begin(), representatives_.end(), path) ==
        representatives_.end())
        representatives_.push_back(std::move(path));
}

DurabilityDomain::Generation DurabilityDomain::complete_mutation(
    std::filesystem::path file, std::filesystem::path directory) {
    std::lock_guard lock(mutex_);
    if (failure_)
        std::rethrow_exception(failure_);
    const auto generation = ++mutation_generation_;
#if !defined(__linux__)
    portable_mutations_.push_back({generation, std::move(file), std::move(directory)});
#else
    (void)file;
    (void)directory;
#endif
    return generation;
}

void DurabilityDomain::await_durable(Generation generation, DurabilityUrgency urgency) {
    if (!generation)
        return;
    std::unique_lock lock(mutex_);
    if (generation > mutation_generation_)
        throw std::runtime_error("durability generation was never admitted");
    if (failure_)
        std::rethrow_exception(failure_);
    if (generation <= durable_generation_)
        return;

    const bool had_request = requested_generation_ > durable_generation_;
    requested_generation_ = std::max(requested_generation_, generation);
    if (urgency == DurabilityUrgency::immediate) {
        immediate_requested_ = true;
    } else if (!had_request || !batch_deadline_) {
        batch_deadline_ = std::chrono::steady_clock::now() + batch_window_;
    }
    cv_.notify_all();

    cv_.wait(lock, [&] {
        return durable_generation_ >= generation || failure_ != nullptr;
    });
    if (failure_)
        std::rethrow_exception(failure_);
}

void DurabilityDomain::flush() {
    Generation generation;
    {
        std::lock_guard lock(mutex_);
        generation = mutation_generation_;
    }
    await_durable(generation, DurabilityUrgency::immediate);
}

DurabilityDomain::Generation DurabilityDomain::current_generation() const {
    std::lock_guard lock(mutex_);
    return mutation_generation_;
}

DurabilityDomain::Generation DurabilityDomain::durable_generation() const {
    std::lock_guard lock(mutex_);
    return durable_generation_;
}

bool DurabilityDomain::failed() const {
    std::lock_guard lock(mutex_);
    return failure_ != nullptr;
}

void DurabilityDomain::perform_barrier(Generation cut, std::vector<PortableMutation> portable) {
#if defined(__linux__)
    (void)cut;
    (void)portable;
    std::vector<std::filesystem::path> representatives;
    {
        std::lock_guard lock(mutex_);
        representatives = representatives_;
    }
    int last_error = ENOENT;
    bool synced = false;
    for (const auto& representative : representatives) {
        int fd = ::open(representative.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd < 0) {
            last_error = errno;
            continue;
        }
        int rc;
        do { rc = ::syncfs(fd); } while (rc != 0 && errno == EINTR);
        last_error = errno;
        ::close(fd);
        if (rc == 0) {
            synced = true;
            break;
        }
    }
    if (!synced)
        throw std::runtime_error("cannot sync durability domain: " +
                                 std::string(strerror(last_error)));
#else
    std::vector<std::filesystem::path> files;
    std::vector<std::filesystem::path> directories;
    for (const auto& mutation : portable) {
        if (mutation.generation > cut)
            continue;
        if (!mutation.file.empty())
            files.push_back(mutation.file);
        if (!mutation.directory.empty())
            directories.push_back(mutation.directory);
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    for (const auto& file : files)
        sync_file(file);
    std::sort(directories.begin(), directories.end());
    directories.erase(std::unique(directories.begin(), directories.end()), directories.end());
    for (const auto& directory : directories)
        sync_directory(directory);
#endif
    physical_barriers_.fetch_add(1, std::memory_order_relaxed);
}

void DurabilityDomain::loop(std::stop_token stop) {
    set_thread_name("macha-durable");
    while (true) {
        Generation cut = 0;
        std::vector<PortableMutation> portable;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] {
                return stop.stop_requested() || failure_ ||
                       requested_generation_ > durable_generation_;
            });
            if (failure_)
                return;
            if (stop.stop_requested() && requested_generation_ <= durable_generation_)
                return;

            if (!immediate_requested_ && !stop.stop_requested()) {
                if (!batch_deadline_)
                    batch_deadline_ = std::chrono::steady_clock::now() + batch_window_;
                const auto deadline = *batch_deadline_;
                cv_.wait_until(lock, deadline, [&] {
                    return stop.stop_requested() || failure_ || immediate_requested_;
                });
                if (failure_)
                    return;
            }

            // current_generation contains only mutations which completed their
            // filesystem syscalls and then registered. Writers may continue
            // while syncfs runs; any mutation registered afterwards receives a
            // later generation and is conservatively excluded from this cut.
            cut = mutation_generation_;
            if (cut <= durable_generation_) {
                requested_generation_ = std::max(requested_generation_, durable_generation_);
                immediate_requested_ = false;
                batch_deadline_.reset();
                continue;
            }
#if !defined(__linux__)
            for (const auto& mutation : portable_mutations_) {
                if (mutation.generation <= cut)
                    portable.push_back(mutation);
            }
#endif
            immediate_requested_ = false;
            batch_deadline_.reset();
        }

        try {
            perform_barrier(cut, std::move(portable));
        } catch (...) {
            std::lock_guard lock(mutex_);
            failure_ = std::current_exception();
            cv_.notify_all();
            return;
        }

        {
            std::lock_guard lock(mutex_);
            durable_generation_ = std::max(durable_generation_, cut);
#if !defined(__linux__)
            portable_mutations_.erase(
                portable_mutations_.begin(),
                std::find_if(portable_mutations_.begin(), portable_mutations_.end(),
                             [&](const PortableMutation& mutation) {
                                 return mutation.generation > durable_generation_;
                             }));
#endif
            if (requested_generation_ <= durable_generation_)
                requested_generation_ = durable_generation_;
            cv_.notify_all();
        }
    }
}

} // namespace macha
