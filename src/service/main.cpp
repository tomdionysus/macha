// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.hpp"
#include "crypto.hpp"
#include "media/ffmpeg_log.hpp"
#include "fuse/fuse_mountpoint.hpp"
#include "log.hpp"
#include "process_allocator.hpp"
#include "service/service.hpp"
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <unistd.h>

int main(int argc, char** argv) {
    try {
        auto config = macha::parse_config(argc, argv);
        macha::Log::set_logger(std::make_shared<macha::ConsoleLogger>(config.log_level));
        const auto allocator =
            macha::configure_process_allocator(config.runtime.glibc_arena_max);
        if (allocator.supported)
            macha::Log::debug("glibc allocator arena limit=" +
                              std::to_string(allocator.arena_max));
        macha::configure_ffmpeg_logging(config.ffmpeg_log_level);
        auto keys = macha::load_cluster_keys(config.key_file);
        // Recover a stale mount and close the covered directory before any service
        // starts: the fail-closed guard shuts the window before the mount comes up.
        if (config.fuse.mount_path) {
            macha::prepare_fuse_mountpoint(*config.fuse.mount_path, config.fuse);
            std::filesystem::create_directories(*config.fuse.mount_path);
        }

        // Signals belong to this loop, on mounting nodes too, so SIGHUP reload always
        // works. The mask is installed before Service is constructed so every thread
        // it starts inherits it, libfuse's workers included.
        sigset_t service_signals;
        sigemptyset(&service_signals);
        sigaddset(&service_signals, SIGINT);
        sigaddset(&service_signals, SIGTERM);
#ifdef SIGHUP
        sigaddset(&service_signals, SIGHUP);
#endif
        const int blocked = pthread_sigmask(SIG_BLOCK, &service_signals, nullptr);
        if (blocked != 0)
            throw std::runtime_error("cannot block service signals: " +
                                     std::string(std::strerror(blocked)));

        // How long a DATA commit that asks for durability waits so that others
        // share its physical sync.
        constexpr std::chrono::milliseconds durability_batch_window{500};
        macha::Service service(config, keys, durability_batch_window);
        service.start();

        while (true) {
            int signal = 0;
            const int waited = sigwait(&service_signals, &signal);
            if (waited != 0)
                throw std::runtime_error("cannot wait for service signal: " +
                                         std::string(std::strerror(waited)));
#ifdef SIGHUP
            if (signal == SIGHUP) {
                try {
                    service.reload_config();
                } catch (const std::exception& e) {
                    macha::Log::error(std::string("configuration reload failed: ") + e.what());
                }
                continue;
            }
#endif
            break;
        }
        service.stop();
        return 0;
    } catch (const std::exception& e) {
        macha::Log::error(e.what());
        return 1;
    }
}
