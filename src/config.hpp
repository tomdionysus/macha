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
    std::chrono::milliseconds garbage_grace{std::chrono::hours(24)};
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

struct CatalogueApiConfig {
    bool enabled{};
    std::string listen{"127.0.0.1"};
    uint16_t port{7438};
    std::optional<std::filesystem::path> token_file;
    size_t max_request_bytes{8 * 1024 * 1024};
};

struct CatalogueTmdbConfig {
    bool enabled{true};
    std::optional<std::filesystem::path> token_file;
    std::string language{"en-GB"};
    std::string image_size{"w500"};
};

struct CatalogueMusicBrainzConfig {
    bool enabled{true};
    std::string contact{"https://github.com/tomdionysus/macha"};
    std::string cover_size{"500"};
};

struct CatalogueScannerConfig {
    bool enabled{};
    std::chrono::milliseconds interval{std::chrono::hours(6)};
    std::vector<std::string> roots{"/"};
    size_t max_artwork_bytes{16 * 1024 * 1024};
    CatalogueTmdbConfig tmdb;
    CatalogueMusicBrainzConfig musicbrainz;
};

struct CatalogueConfig {
    CatalogueApiConfig api;
    CatalogueScannerConfig scanner;
};

struct HydrationEngineConfig {
    bool enabled{true};
    uint32_t priority{};
};

struct HydrationConfig {
    bool enabled{true};
    std::chrono::milliseconds interval{100};
    std::chrono::milliseconds active_timeout{30000};
    size_t max_inflight{4};
    HydrationEngineConfig read_ahead{true, 1000};
    HydrationEngineConfig current_file{true, 700};
    HydrationEngineConfig catalogue{true, 300};
    size_t catalogue_lookahead{1};
};

struct Config {
    std::filesystem::path state_path;
    std::vector<StorageBackendConfig> storage_backends;
    CacheConfig cache;
    MaintenanceConfig maintenance;
    FilesystemConfig filesystem;
    CatalogueConfig catalogue;
    HydrationConfig hydration;

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
    size_t max_frame_size{256 * 1024};
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
