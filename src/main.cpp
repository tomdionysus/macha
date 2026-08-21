// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.hpp"
#include "crypto.hpp"
#include "fuse_adapter.hpp"
#include "ffmpeg_log.hpp"
#include "log.hpp"
#include "service.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <unistd.h>

namespace {
std::atomic_bool stopping{false};
std::atomic_bool reload_requested{false};
void signal_handler(int signal) {
#ifdef SIGHUP
    if (signal == SIGHUP) {
        reload_requested = true;
        return;
    }
#endif
    stopping = true;
}
} // namespace

int main(int argc, char** argv) {
    try {
        auto config = macha::parse_config(argc, argv);
        macha::Log::set_logger(
            std::make_shared<macha::ConsoleLogger>(config.log_level));
        macha::configure_ffmpeg_logging(config.ffmpeg_log_level);
        auto keys = macha::load_cluster_keys(config.key_file);
        macha::Service service(config, keys);
        service.start();

        if (config.mount_path) {
            std::filesystem::create_directories(*config.mount_path);
            int rc = macha::run_fuse(service.filesystem(), service.hydration().hydrator(),
                                        *config.mount_path, config.fuse,
                                        [&service] { service.request_stop(); });
            macha::Log::debug("shutdown: main received FUSE return; stopping service");
            service.stop();
            macha::Log::debug("shutdown: main service stopped");
            return rc;
        }

        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
#ifdef SIGHUP
        std::signal(SIGHUP, signal_handler);
#endif
        while (!stopping) {
            if (reload_requested.exchange(false)) {
                try {
                    service.reload_config();
                } catch (const std::exception& e) {
                    macha::Log::error(std::string("configuration reload failed: ") + e.what());
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        service.stop();
        return 0;
    } catch (const std::exception& e) {
        macha::Log::error(e.what());
        return 1;
    }
}
