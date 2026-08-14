// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "log.hpp"
#include "types.hpp"
#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

namespace macha {

struct StorageBackendConfig {
    std::filesystem::path path;
    uint64_t limit{};
};

struct CacheConfig {
    std::filesystem::path path;
    size_t max_blocks{};
    bool prefer_metadata{true};
};

struct MaintenanceConfig {
    std::chrono::milliseconds interval{500};
    std::chrono::milliseconds foreground_quiet{2000};
    double busy_bandwidth_fraction{0.0};
    double idle_bandwidth_fraction{0.50};
    double cpu_target{0.35};
    uint64_t initial_bandwidth{32ULL * 1024 * 1024};
    uint64_t max_bandwidth{}; // 0 = no configured cap; observed bandwidth is still used.
    double scrub_fraction{0.10};
};

struct FilesystemConfig {
    // Expose a FUSE mount to users other than the process that mounted it.
    // Required on macFUSE when the daemon is started with sudo but the volume
    // is intended to be used from the invoking user's session.
    bool allow_other{};

    // These values become the ownership/mode of the distributed filesystem
    // root when a brand-new metadata group is formed. They are stored in DHT
    // metadata thereafter; changing the config does not rewrite an existing
    // filesystem root.
    uint32_t root_uid{};
    uint32_t root_gid{};
    uint32_t root_mode{0755};
};

struct Config {
    std::filesystem::path state_path;
    std::vector<StorageBackendConfig> storage_backends;
    CacheConfig cache;
    MaintenanceConfig maintenance;
    FilesystemConfig filesystem;

    std::filesystem::path key_file;
    std::optional<std::filesystem::path> mount_path;
    std::optional<std::filesystem::path> config_file;
    std::string listen_host{"0.0.0.0"};
    std::string advertise_host;
    std::string failure_domain;
    uint16_t port{7437};
    size_t replication{3};
    size_t metadata_replication{3};
    size_t extent_size{16 * 1024 * 1024};
    size_t read_ahead_extents{3};
    std::vector<Endpoint> bootstrap;
    std::chrono::milliseconds heartbeat{5000};
    std::chrono::milliseconds dead_after{30000};
    std::chrono::milliseconds connect_timeout{2500};
    // These are observability thresholds only. They never terminate a healthy
    // RPC; 0 disables the corresponding stalled-request DEBUG message.
    std::chrono::milliseconds control_stall_notice{5000};
    std::chrono::milliseconds data_stall_notice{120000};
    std::chrono::milliseconds metadata_cache{250};
    LogLevel log_level{LogLevel::info};
};

Config parse_config(int, char**);
Config load_yaml_config(const std::filesystem::path&);
Config normalize_config(Config);
uint64_t parse_size(const std::string&);
Endpoint parse_endpoint(const std::string&, uint16_t default_port);
void print_usage(const char* executable);
} // namespace macha
