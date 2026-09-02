// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.hpp"
#include "crypto.hpp"
#include "ffmpeg_log.hpp"
#include "fuse_adapter.hpp"
#include "log.hpp"
#include "process_allocator.hpp"
#include "service.hpp"
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
        if (config.mount_path) {
            macha::prepare_fuse_mountpoint(*config.mount_path, config.fuse);
            std::filesystem::create_directories(*config.mount_path);
        }

        sigset_t service_signals;
        sigemptyset(&service_signals);
        sigaddset(&service_signals, SIGINT);
        sigaddset(&service_signals, SIGTERM);
#ifdef SIGHUP
        sigaddset(&service_signals, SIGHUP);
#endif
        if (!config.mount_path) {
            const int blocked = pthread_sigmask(SIG_BLOCK, &service_signals, nullptr);
            if (blocked != 0)
                throw std::runtime_error("cannot block service signals: " +
                                         std::string(std::strerror(blocked)));
        }

        macha::Service service(config, keys);
        service.start();

        if (config.mount_path) {
            int rc = macha::run_fuse(
                service.filesystem(), service.hydration().hydrator(), *config.mount_path,
                config.fuse, [&service] { service.request_stop(); },
                [&service](std::weak_ptr<macha::FuseFrontend> frontend) {
                    service.attach_fuse_frontend(std::move(frontend));
                });
            macha::Log::debug("shutdown: main received FUSE return; stopping service");
            service.stop();
            macha::Log::debug("shutdown: main service stopped");
            return rc;
        }

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
