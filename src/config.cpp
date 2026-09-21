// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.hpp"

#include "users.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace macha {
namespace {
unsigned parse_unsigned(const std::string& value, const char* what) {
    unsigned out = 0;
    auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), out);
    if (error != std::errc{} || end != value.data() + value.size())
        throw std::runtime_error(std::string("invalid ") + what + ": " + value);
    return out;
}

double parse_fraction(const YAML::Node& node, const char* what) {
    auto value = node.as<double>();
    if (value < 0.0 || value > 1.0)
        throw std::runtime_error(std::string(what) + " must be between 0 and 1");
    return value;
}

uint32_t parse_mode(const YAML::Node& node, const char* what) {
    if (!node.IsScalar())
        throw std::runtime_error(std::string(what) + " must be a scalar");
    auto text = node.as<std::string>();
    if (text.empty())
        throw std::runtime_error(std::string("invalid ") + what);
    unsigned long value = 0;
    size_t used = 0;
    try {
        value = std::stoul(text, &used, 8);
    } catch (...) {
        throw std::runtime_error(std::string("invalid ") + what + ": " + text);
    }
    if (used != text.size() || value > 07777)
        throw std::runtime_error(std::string("invalid ") + what + ": " + text);
    return static_cast<uint32_t>(value);
}

std::chrono::milliseconds milliseconds(const YAML::Node& node, const char* what) {
    auto value = node.as<long long>();
    if (value < 0)
        throw std::runtime_error(std::string(what) + " must be non-negative");
    return std::chrono::milliseconds(value);
}

uint64_t yaml_size(const YAML::Node& node) {
    if (node.IsScalar())
        return parse_size(node.as<std::string>());
    throw std::runtime_error("size must be a scalar");
}

Tristate yaml_tristate(const YAML::Node& node, const char* what) {
    if (!node.IsScalar())
        throw std::runtime_error(std::string(what) + " must be true, false or auto");
    return parse_tristate(node.as<std::string>(), what);
}

void parse_network(const YAML::Node& root, Config& c) {
    auto n = root["network"];
    if (!n)
        return;
    if (n["listen"])
        c.listen_host = n["listen"].as<std::string>();
    if (n["advertise"])
        c.advertise_host = n["advertise"].as<std::string>();
    if (n["port"])
        c.port = n["port"].as<uint16_t>();
    if (n["inbound_capable"])
        c.inbound_capable = yaml_tristate(n["inbound_capable"], "network.inbound_capable");
    if (n["failure_domain"])
        c.failure_domain = n["failure_domain"].as<std::string>();
    if (n["heartbeat_ms"])
        c.heartbeat = milliseconds(n["heartbeat_ms"], "heartbeat_ms");
    if (n["telemetry_interval_ms"])
        c.telemetry_interval = milliseconds(n["telemetry_interval_ms"], "telemetry_interval_ms");
    if (n["dead_after_ms"])
        c.dead_after = milliseconds(n["dead_after_ms"], "dead_after_ms");
    if (n["connect_timeout_ms"])
        c.connect_timeout = milliseconds(n["connect_timeout_ms"], "connect_timeout_ms");
    if (n["max_frame_size"])
        c.max_frame_size = yaml_size(n["max_frame_size"]);
    if (n["control_no_progress_deadline_ms"])
        c.control_no_progress_deadline =
            milliseconds(n["control_no_progress_deadline_ms"], "control_no_progress_deadline_ms");
    if (n["data_no_progress_deadline_ms"])
        c.data_no_progress_deadline =
            milliseconds(n["data_no_progress_deadline_ms"], "data_no_progress_deadline_ms");
    if (n["control_stall_notice_ms"])
        c.control_stall_notice =
            milliseconds(n["control_stall_notice_ms"], "control_stall_notice_ms");
    if (n["data_stall_notice_ms"])
        c.data_stall_notice =
            milliseconds(n["data_stall_notice_ms"], "data_stall_notice_ms");

    if (auto u = n["upnp"]) {
        if (u["enabled"])
            c.upnp.enabled = u["enabled"].as<bool>();
        if (u["external_port"])
            c.upnp.external_port = u["external_port"].as<uint16_t>();
        if (u["discovery_timeout_ms"])
            c.upnp.discovery_timeout =
                milliseconds(u["discovery_timeout_ms"], "network.upnp.discovery_timeout_ms");
        if (u["lease_seconds"])
            c.upnp.lease_seconds = u["lease_seconds"].as<uint32_t>();
    }
    if (auto e = n["external_ip"]) {
        if (e["enabled"])
            c.external_ip.enabled = e["enabled"].as<bool>();
        if (e["timeout_ms"])
            c.external_ip.timeout =
                milliseconds(e["timeout_ms"], "network.external_ip.timeout_ms");
    }
    if (auto check = n["connectivity_check"]) {
        if (check["enabled"])
            c.connectivity_check.enabled = check["enabled"].as<bool>();
        if (check["timeout_ms"])
            c.connectivity_check.timeout = milliseconds(
                check["timeout_ms"], "network.connectivity_check.timeout_ms");
    }
}

void parse_dht(const YAML::Node& root, Config& c) {
    auto d = root["dht"];
    if (!d)
        return;
    if (d["replicas"])
        c.replication = d["replicas"].as<size_t>();
    if (d["metadata_min_write_replicas"] && d["metadata_replicas"])
        throw std::runtime_error(
            "dht.metadata_min_write_replicas and legacy dht.metadata_replicas are mutually exclusive");
    if (d["metadata_min_write_replicas"])
        c.metadata_min_write_replicas = d["metadata_min_write_replicas"].as<size_t>();
    else if (d["metadata_replicas"]) {
        // 0.18 metadata_replicas was a fixed voter count whose write floor was
        // majority. Preserve that durability when reading an old config.
        const auto legacy = d["metadata_replicas"].as<size_t>();
        c.metadata_min_write_replicas = legacy ? legacy / 2 + 1 : 0;
    }
    if (d["min_write_replicas"])
        c.min_write_replicas = d["min_write_replicas"].as<size_t>();
    if (d["retention_check_batch_size"])
        c.retention_check_batch_size = d["retention_check_batch_size"].as<size_t>();
    if (d["retention_check_concurrency"])
        c.retention_check_concurrency = d["retention_check_concurrency"].as<size_t>();
    if (d["write_stall_ms"])
        c.write_stall = milliseconds(d["write_stall_ms"], "dht.write_stall_ms");
    if (d["extent_size"])
        c.extent_size = yaml_size(d["extent_size"]);
    if (d["data_inflight_bytes"])
        c.data_inflight_bytes = yaml_size(d["data_inflight_bytes"]);
    if (d["data_credit_no_progress_deadline_ms"])
        c.data_credit_no_progress_deadline = milliseconds(d["data_credit_no_progress_deadline_ms"],
                                                          "dht.data_credit_no_progress_deadline_ms");
    if (d["data_viewer_reserve_bytes"])
        c.data_viewer_reserve_bytes = yaml_size(d["data_viewer_reserve_bytes"]);
    if (d["read_ahead"])
        c.read_ahead_extents = d["read_ahead"].as<size_t>();
    if (d["metadata_cache_ms"])
        c.metadata_cache = milliseconds(d["metadata_cache_ms"], "metadata_cache_ms");
    if (d["metadata_materialization_cache_bytes"])
        c.metadata_materialization_cache_bytes =
            yaml_size(d["metadata_materialization_cache_bytes"]);
}

void parse_maintenance(const YAML::Node& root, Config& c) {
    auto m = root["maintenance"];
    if (!m)
        return;
    if (m["interval_ms"])
        c.maintenance.interval = milliseconds(m["interval_ms"], "maintenance.interval_ms");
    if (m["foreground_quiet_ms"])
        c.maintenance.foreground_quiet =
            milliseconds(m["foreground_quiet_ms"], "maintenance.foreground_quiet_ms");
    if (m["garbage_grace_ms"])
        c.maintenance.garbage_grace =
            milliseconds(m["garbage_grace_ms"], "maintenance.garbage_grace_ms");
    if (m["busy_bandwidth_fraction"])
        c.maintenance.busy_bandwidth_fraction =
            parse_fraction(m["busy_bandwidth_fraction"], "busy_bandwidth_fraction");
    if (m["idle_bandwidth_fraction"])
        c.maintenance.idle_bandwidth_fraction =
            parse_fraction(m["idle_bandwidth_fraction"], "idle_bandwidth_fraction");
    if (m["cpu_target"])
        c.maintenance.cpu_target = parse_fraction(m["cpu_target"], "cpu_target");
    if (m["background_concurrency"])
        c.maintenance.background_concurrency = m["background_concurrency"].as<size_t>();
    if (m["initial_bandwidth"])
        c.maintenance.initial_bandwidth = yaml_size(m["initial_bandwidth"]);
    if (m["max_bandwidth"])
        c.maintenance.max_bandwidth = yaml_size(m["max_bandwidth"]);
    if (m["scrub_fraction"])
        c.maintenance.scrub_fraction = parse_fraction(m["scrub_fraction"], "scrub_fraction");
    if (m["scrub_interval_ms"])
        c.maintenance.scrub_interval =
            milliseconds(m["scrub_interval_ms"], "maintenance.scrub_interval_ms");
    if (m["no_progress_backoff_ms"])
        c.maintenance.no_progress_backoff =
            milliseconds(m["no_progress_backoff_ms"], "maintenance.no_progress_backoff_ms");
}

void parse_filesystem(const YAML::Node& root, Config& c) {
    auto f = root["filesystem"];
    if (!f)
        return;
    if (f["root_uid"])
        c.filesystem.root_uid = f["root_uid"].as<uint32_t>();
    if (f["root_gid"])
        c.filesystem.root_gid = f["root_gid"].as<uint32_t>();
    if (f["root_mode"])
        c.filesystem.root_mode = parse_mode(f["root_mode"], "filesystem.root_mode");

    // 0.12.x compatibility: old FUSE keys under filesystem remain accepted.
    if (f["allow_other"]) c.fuse.allow_other = f["allow_other"].as<bool>();
    if (f["entry_timeout_ms"]) c.fuse.entry_timeout = milliseconds(f["entry_timeout_ms"], "filesystem.entry_timeout_ms");
    if (f["attr_timeout_ms"]) c.fuse.attr_timeout = milliseconds(f["attr_timeout_ms"], "filesystem.attr_timeout_ms");
    if (f["negative_timeout_ms"]) c.fuse.negative_timeout = milliseconds(f["negative_timeout_ms"], "filesystem.negative_timeout_ms");
}

void parse_fuse(const YAML::Node& root, Config& c) {
    auto f = root["fuse"];
    if (!f) return;
    if (f["mount_path"]) c.fuse.mount_path = std::filesystem::path(f["mount_path"].as<std::string>());
    if (f["allow_other"]) c.fuse.allow_other = f["allow_other"].as<bool>();
    if (f["spool_path"])
        c.fuse.spool_path = std::filesystem::path(f["spool_path"].as<std::string>());
    if (f["operation_journal_path"])
        c.fuse.operation_journal_path =
            std::filesystem::path(f["operation_journal_path"].as<std::string>());
    if (f["entry_timeout_ms"]) c.fuse.entry_timeout = milliseconds(f["entry_timeout_ms"], "fuse.entry_timeout_ms");
    if (f["attr_timeout_ms"]) c.fuse.attr_timeout = milliseconds(f["attr_timeout_ms"], "fuse.attr_timeout_ms");
    if (f["negative_timeout_ms"]) c.fuse.negative_timeout = milliseconds(f["negative_timeout_ms"], "fuse.negative_timeout_ms");
    if (f["absolute_request_timeout_ms"]) c.fuse.absolute_request_timeout = milliseconds(f["absolute_request_timeout_ms"], "fuse.absolute_request_timeout_ms");
    if (f["request_workers"]) c.fuse.request_workers = f["request_workers"].as<size_t>();
    if (f["max_pending_requests"]) c.fuse.max_pending_requests = f["max_pending_requests"].as<size_t>();
    if (f["max_pending_write_bytes"])
        c.fuse.max_pending_write_bytes = yaml_size(f["max_pending_write_bytes"]);
    if (f["commit_workers"]) c.fuse.commit_workers = f["commit_workers"].as<size_t>();
    if (f["recovery_commit_workers"]) c.fuse.recovery_commit_workers = f["recovery_commit_workers"].as<size_t>();
    if (f["foreground_commit_workers"]) c.fuse.foreground_commit_workers = f["foreground_commit_workers"].as<size_t>();
    if (f["publication_quiet_ms"]) c.fuse.publication_quiet = milliseconds(f["publication_quiet_ms"], "fuse.publication_quiet_ms");
    if (f["publication_no_progress_deadline_ms"])
        c.fuse.publication_no_progress_deadline =
            milliseconds(f["publication_no_progress_deadline_ms"],
                         "fuse.publication_no_progress_deadline_ms");
    if (f["publication_retry_initial_backoff_ms"])
        c.fuse.publication_retry.initial_backoff =
            milliseconds(f["publication_retry_initial_backoff_ms"], "fuse.publication_retry_initial_backoff_ms");
    if (f["publication_retry_max_backoff_ms"])
        c.fuse.publication_retry.max_backoff =
            milliseconds(f["publication_retry_max_backoff_ms"], "fuse.publication_retry_max_backoff_ms");
    if (f["publication_retry_window_ms"])
        c.fuse.publication_retry.failure_window =
            milliseconds(f["publication_retry_window_ms"], "fuse.publication_retry_window_ms");
    if (f["publication_retry_max_failures"])
        c.fuse.publication_retry.max_failures_in_window = f["publication_retry_max_failures"].as<size_t>();
    if (f["publication_retry_max_failing_ms"])
        c.fuse.publication_retry.max_failing_duration =
            milliseconds(f["publication_retry_max_failing_ms"], "fuse.publication_retry_max_failing_ms");
    if (f["namespace_retry_initial_backoff_ms"])
        c.fuse.namespace_retry.initial_backoff =
            milliseconds(f["namespace_retry_initial_backoff_ms"], "fuse.namespace_retry_initial_backoff_ms");
    if (f["namespace_retry_max_backoff_ms"])
        c.fuse.namespace_retry.max_backoff =
            milliseconds(f["namespace_retry_max_backoff_ms"], "fuse.namespace_retry_max_backoff_ms");
    if (f["namespace_retry_window_ms"])
        c.fuse.namespace_retry.failure_window =
            milliseconds(f["namespace_retry_window_ms"], "fuse.namespace_retry_window_ms");
    if (f["namespace_retry_max_failures"])
        c.fuse.namespace_retry.max_failures_in_window = f["namespace_retry_max_failures"].as<size_t>();
    if (f["namespace_retry_max_failing_ms"])
        c.fuse.namespace_retry.max_failing_duration =
            milliseconds(f["namespace_retry_max_failing_ms"], "fuse.namespace_retry_max_failing_ms");
    if (f["viewer_weight"]) c.fuse.viewer_weight = f["viewer_weight"].as<size_t>();
    if (f["loader_weight"]) c.fuse.loader_weight = f["loader_weight"].as<size_t>();
    if (f["publication_quantum_bytes"])
        c.fuse.publication_quantum_bytes = yaml_size(f["publication_quantum_bytes"]);
    if (f["publication_inflight_bytes"])
        c.fuse.publication_inflight_bytes = yaml_size(f["publication_inflight_bytes"]);
    if (f["publication_pipeline_bytes"])
        c.fuse.publication_pipeline_bytes = yaml_size(f["publication_pipeline_bytes"]);
    if (f["publication_max_open_writers"])
        c.fuse.publication_max_open_writers = f["publication_max_open_writers"].as<size_t>();
    if (f["max_pending_operations"]) c.fuse.max_pending_operations = f["max_pending_operations"].as<size_t>();
    if (f["max_operation_metadata_bytes"])
        c.fuse.max_operation_metadata_bytes = yaml_size(f["max_operation_metadata_bytes"]);
    if (f["namespace_batch_operations"])
        c.fuse.namespace_batch_operations = f["namespace_batch_operations"].as<size_t>();
    if (f["namespace_batch_bytes"])
        c.fuse.namespace_batch_bytes = yaml_size(f["namespace_batch_bytes"]);
    if (f["max_spool_bytes"]) c.fuse.max_spool_bytes = yaml_size(f["max_spool_bytes"]);
    if (f["spool_reserve_free"]) c.fuse.spool_reserve_free = yaml_size(f["spool_reserve_free"]);
    if (f["max_orphan_bytes"]) c.fuse.max_orphan_bytes = yaml_size(f["max_orphan_bytes"]);
    if (f["max_operation_journal_bytes"]) c.fuse.max_operation_journal_bytes = yaml_size(f["max_operation_journal_bytes"]);
    if (f["hydration_priority"]) c.fuse.hydration_priority = f["hydration_priority"].as<uint32_t>();
    if (f["read_ahead_extents"]) c.fuse.read_ahead_extents = f["read_ahead_extents"].as<size_t>();
    if (f["hint_lifetime_ms"]) c.fuse.hint_lifetime = milliseconds(f["hint_lifetime_ms"], "fuse.hint_lifetime_ms");
    if (f["write_through_cache"]) c.fuse.write_through_cache = f["write_through_cache"].as<bool>();
    if (f["fail_closed_mountpoint"]) c.fuse.fail_closed_mountpoint = f["fail_closed_mountpoint"].as<bool>();
    if (f["unmount_if_mounted"]) c.fuse.unmount_if_mounted = f["unmount_if_mounted"].as<bool>();
    if (f["watchdog_interval_ms"]) c.fuse.watchdog_interval = milliseconds(f["watchdog_interval_ms"], "fuse.watchdog_interval_ms");
    if (f["initial_namespace_timeout_ms"]) c.fuse.initial_namespace_timeout = milliseconds(f["initial_namespace_timeout_ms"], "fuse.initial_namespace_timeout_ms");
    if (auto t = f["timeouts"]) {
        if (t["lookup_ms"]) c.fuse.timeouts.lookup = milliseconds(t["lookup_ms"], "fuse.timeouts.lookup_ms");
        if (t["namespace_ms"]) c.fuse.timeouts.namespace_mutation = milliseconds(t["namespace_ms"], "fuse.timeouts.namespace_ms");
        if (t["read_ms"]) c.fuse.timeouts.read = milliseconds(t["read_ms"], "fuse.timeouts.read_ms");
        if (t["write_ms"]) c.fuse.timeouts.write = milliseconds(t["write_ms"], "fuse.timeouts.write_ms");
        if (t["sync_ms"]) c.fuse.timeouts.sync = milliseconds(t["sync_ms"], "fuse.timeouts.sync_ms");
        if (t["lifecycle_ms"]) c.fuse.timeouts.lifecycle = milliseconds(t["lifecycle_ms"], "fuse.timeouts.lifecycle_ms");
    }
}

void parse_catalogue(const YAML::Node& root, Config& c) {
    auto catalogue = root["catalogue"];
    if (!catalogue)
        return;
    if (auto api = catalogue["api"]) {
        if (api["enabled"])
            c.catalogue.api.enabled = api["enabled"].as<bool>();
        if (api["listen"])
            c.catalogue.api.listen = api["listen"].as<std::string>();
        if (api["port"])
            c.catalogue.api.port = api["port"].as<uint16_t>();
        if (api["advertised_endpoint"]) {
            auto value = api["advertised_endpoint"].as<std::string>();
            // This string is handed to clients verbatim and they build every
            // request URL from it, so a malformed one produces requests that
            // fail somewhere far away with no hint of where it came from.
            // Reject it here, where the file that set it is still in hand.
            const bool http = value.starts_with("http://");
            const bool https = value.starts_with("https://");
            if (!http && !https)
                throw std::runtime_error(
                    "catalogue.api.advertised_endpoint must begin with http:// or https:// "
                    "(it is a complete origin, not a host)");
            const auto authority = value.substr(http ? 7 : 8);
            if (authority.empty())
                throw std::runtime_error("catalogue.api.advertised_endpoint has no host");
            // A proxy fronting a node at a subpath is not a deployment this
            // serves: a client treats this as an origin and would append its
            // own paths, silently 404ing against the prefix.
            if (authority.find('/') != std::string::npos)
                throw std::runtime_error(
                    "catalogue.api.advertised_endpoint must not contain a path; "
                    "it is scheme://host[:port] only");
            if (authority.find('@') != std::string::npos ||
                authority.find('?') != std::string::npos ||
                authority.find('#') != std::string::npos)
                throw std::runtime_error(
                    "catalogue.api.advertised_endpoint must be scheme://host[:port] only");
            // An unbracketed IPv6 literal cannot be parsed back out of a URL:
            // its colons are indistinguishable from a port separator.
            const auto colons = std::count(authority.begin(), authority.end(), ':');
            if (colons > 1 && authority.front() != '[')
                throw std::runtime_error(
                    "catalogue.api.advertised_endpoint: bracket an IPv6 literal, e.g. "
                    "http://[2001:db8::1]:7438");
            c.catalogue.api.advertised_endpoint = std::move(value);
        }
        if (api["token_file"])
            c.catalogue.api.token_file = std::filesystem::path(api["token_file"].as<std::string>());
        if (api["max_request_bytes"])
            c.catalogue.api.max_request_bytes = yaml_size(api["max_request_bytes"]);
        if (api["workers"])
            c.catalogue.api.workers = api["workers"].as<size_t>();
        if (api["control_workers"])
            c.catalogue.api.control_workers = api["control_workers"].as<size_t>();
        // 0.43.0 renamed this: the server no longer queues accepted
        // connections for a worker, it holds them open. The old key is
        // still read as the connection cap so an existing config keeps
        // its meaning.
        if (api["max_queued_connections"])
            c.catalogue.api.max_connections = api["max_queued_connections"].as<size_t>();
        if (api["max_connections"])
            c.catalogue.api.max_connections = api["max_connections"].as<size_t>();
        if (api["max_queued_requests"])
            c.catalogue.api.max_queued_requests = api["max_queued_requests"].as<size_t>();
        if (api["client_io_timeout_ms"])
            c.catalogue.api.client_io_timeout =
                milliseconds(api["client_io_timeout_ms"], "catalogue.api.client_io_timeout_ms");
        if (api["stream_chunk_bytes"])
            c.catalogue.api.stream_chunk_bytes = yaml_size(api["stream_chunk_bytes"]);
        if (api["staging_chunks"])
            c.catalogue.api.staging_chunks = api["staging_chunks"].as<size_t>();
        if (api["slow_request_threshold_ms"])
            c.catalogue.api.slow_request_threshold = milliseconds(
                api["slow_request_threshold_ms"], "catalogue.api.slow_request_threshold_ms");
        if (api["compression"])
            c.catalogue.api.compression.enabled = api["compression"].as<bool>();
        if (api["compression_min_bytes"])
            c.catalogue.api.compression.min_bytes = yaml_size(api["compression_min_bytes"]);
        if (api["compression_level"])
            c.catalogue.api.compression.level = api["compression_level"].as<int>();
        if (api["compression_max_asset_bytes"])
            c.catalogue.api.compression.max_asset_bytes =
                yaml_size(api["compression_max_asset_bytes"]);
        if (api["reactor_stall_threshold_ms"])
            c.catalogue.api.reactor_stall_threshold = milliseconds(
                api["reactor_stall_threshold_ms"], "catalogue.api.reactor_stall_threshold_ms");
        if (api["keep_alive_max_requests"])
            c.catalogue.api.keep_alive_max_requests = api["keep_alive_max_requests"].as<size_t>();
        if (api["keep_alive_idle_timeout_ms"])
            c.catalogue.api.keep_alive_idle_timeout =
                milliseconds(api["keep_alive_idle_timeout_ms"], "catalogue.api.keep_alive_idle_timeout_ms");
        if (api["artwork_capability_ttl_ms"])
            c.catalogue.api.artwork_capability_ttl =
                milliseconds(api["artwork_capability_ttl_ms"], "catalogue.api.artwork_capability_ttl_ms");
    }
    if (auto scanner = catalogue["scanner"]) {
        if (scanner["enabled"])
            c.catalogue.scanner.enabled = scanner["enabled"].as<bool>();
        if (scanner["interval_ms"])
            c.catalogue.scanner.interval = milliseconds(scanner["interval_ms"], "catalogue.scanner.interval_ms");
        if (scanner["rescan_debounce_ms"])
            c.catalogue.scanner.rescan_debounce = milliseconds(
                scanner["rescan_debounce_ms"], "catalogue.scanner.rescan_debounce_ms");
        if (scanner["rescan_max_delay_ms"])
            c.catalogue.scanner.rescan_max_delay = milliseconds(
                scanner["rescan_max_delay_ms"], "catalogue.scanner.rescan_max_delay_ms");
        if (scanner["max_provider_requests_per_scan"])
            c.catalogue.scanner.max_provider_requests_per_scan =
                scanner["max_provider_requests_per_scan"].as<size_t>();
        if (scanner["provider_batch_delay_ms"])
            c.catalogue.scanner.provider_batch_delay = milliseconds(
                scanner["provider_batch_delay_ms"], "catalogue.scanner.provider_batch_delay_ms");
        if (scanner["max_artwork_bytes"])
            c.catalogue.scanner.max_artwork_bytes = yaml_size(scanner["max_artwork_bytes"]);
        if (auto providers = scanner["providers"]) {
            const auto parse_roots = [](const YAML::Node& provider,
                                        std::vector<std::string>& roots,
                                        std::string_view name) {
                auto configured = provider["roots"];
                if (!configured) return;
                if (!configured.IsSequence())
                    throw std::runtime_error("catalogue.scanner.providers." +
                                             std::string(name) + ".roots must be a sequence");
                roots.clear();
                for (const auto& root : configured)
                    roots.push_back(root.as<std::string>());
            };
            const auto parse_tmdb = [](const YAML::Node& tmdb, CatalogueTmdbConfig& config) {
                if (!tmdb) return;
                if (tmdb["enabled"]) config.enabled = tmdb["enabled"].as<bool>();
                if (tmdb["token_file"])
                    config.token_file = std::filesystem::path(tmdb["token_file"].as<std::string>());
                if (tmdb["language"]) config.language = tmdb["language"].as<std::string>();
                if (tmdb["image_size"]) config.image_size = tmdb["image_size"].as<std::string>();
            };

            if (auto movies = providers["movies"]) {
                if (movies["enabled"]) c.catalogue.scanner.movies.enabled = movies["enabled"].as<bool>();
                parse_roots(movies, c.catalogue.scanner.movies.roots, "movies");
                parse_tmdb(movies["tmdb"], c.catalogue.scanner.movies.tmdb);
            }
            if (auto tv = providers["tv"]) {
                if (tv["enabled"]) c.catalogue.scanner.tv.enabled = tv["enabled"].as<bool>();
                parse_roots(tv, c.catalogue.scanner.tv.roots, "tv");
                parse_tmdb(tv["tmdb"], c.catalogue.scanner.tv.tmdb);
            }
            if (auto music = providers["music"]) {
                if (music["enabled"]) c.catalogue.scanner.music.enabled = music["enabled"].as<bool>();
                parse_roots(music, c.catalogue.scanner.music.roots, "music");
                if (auto mb = music["musicbrainz"]) {
                    if (mb["enabled"]) c.catalogue.scanner.music.musicbrainz.enabled = mb["enabled"].as<bool>();
                    if (mb["contact"]) c.catalogue.scanner.music.musicbrainz.contact = mb["contact"].as<std::string>();
                    if (mb["cover_size"]) c.catalogue.scanner.music.musicbrainz.cover_size = mb["cover_size"].as<std::string>();
                }
                if (auto discogs = music["discogs"]) {
                    if (discogs["enabled"]) c.catalogue.scanner.music.discogs.enabled = discogs["enabled"].as<bool>();
                    if (discogs["token_file"])
                        c.catalogue.scanner.music.discogs.token_file =
                            std::filesystem::path(discogs["token_file"].as<std::string>());
                }
            }
        }
    }
}

void parse_ingest(const YAML::Node& root, Config& c) {
    auto ingest = root["ingest"];
    if (!ingest) return;
    if (ingest["enabled"]) c.ingest.enabled = ingest["enabled"].as<bool>();
    if (ingest["staging_path"])
        c.ingest.staging_path = std::filesystem::path(ingest["staging_path"].as<std::string>());
    if (ingest["staging_limit"]) c.ingest.staging_limit = yaml_size(ingest["staging_limit"]);
    if (ingest["copy_chunk_bytes"]) c.ingest.copy_chunk_bytes = yaml_size(ingest["copy_chunk_bytes"]);
    if (ingest["checkpoint_bytes"]) c.ingest.checkpoint_bytes = yaml_size(ingest["checkpoint_bytes"]);
    if (ingest["blocked_retry_ms"])
        c.ingest.blocked_retry = milliseconds(ingest["blocked_retry_ms"], "ingest.blocked_retry_ms");
    if (ingest["max_concurrent_jobs"])
        c.ingest.max_concurrent_jobs = ingest["max_concurrent_jobs"].as<size_t>();
    if (auto cleanup = ingest["cleanup"]) {
        if (cleanup["delete_owned_source_on_clear"])
            c.ingest.delete_owned_source_on_clear = cleanup["delete_owned_source_on_clear"].as<bool>();
        if (cleanup["delete_external_source_on_clear"])
            c.ingest.delete_external_source_on_clear = cleanup["delete_external_source_on_clear"].as<bool>();
        if (cleanup["delete_owned_source_on_cancel"])
            c.ingest.delete_owned_source_on_cancel = cleanup["delete_owned_source_on_cancel"].as<bool>();
    }
    if (auto roots = ingest["source_roots"]) {
        if (!roots.IsSequence()) throw std::runtime_error("ingest.source_roots must be a sequence");
        c.ingest.source_roots.clear();
        for (const auto& value : roots)
            c.ingest.source_roots.emplace_back(value.as<std::string>());
    }
}

void parse_torrent(const YAML::Node& root, Config& c) {
    auto torrent = root["torrent"];
    if (!torrent) return;
    if (torrent["enabled"]) c.torrent.enabled = torrent["enabled"].as<bool>();
    if (torrent["max_active"]) c.torrent.max_active = torrent["max_active"].as<size_t>();
    if (torrent["listen_interfaces"])
        c.torrent.listen_interfaces = torrent["listen_interfaces"].as<std::string>();
    if (torrent["listen_port"]) c.torrent.listen_port = torrent["listen_port"].as<uint16_t>();
    if (torrent["max_download_rate"]) c.torrent.max_download_rate = yaml_size(torrent["max_download_rate"]);
    if (torrent["max_upload_rate"]) c.torrent.max_upload_rate = yaml_size(torrent["max_upload_rate"]);
    if (torrent["dht"]) c.torrent.dht = torrent["dht"].as<bool>();
    if (torrent["pex"]) c.torrent.pex = torrent["pex"].as<bool>();
    if (torrent["lsd"]) c.torrent.lsd = torrent["lsd"].as<bool>();
    if (torrent["upnp"]) c.torrent.upnp = torrent["upnp"].as<bool>();
    if (torrent["natpmp"]) c.torrent.natpmp = torrent["natpmp"].as<bool>();
    if (auto search = torrent["search"]) {
        if (auto providers = search["providers"]) {
            // YAML `providers:` with only commented examples is a null node.
            // Treat it exactly like an empty list rather than rejecting an
            // otherwise valid torrent configuration.
            if (!providers.IsNull()) {
                if (!providers.IsSequence())
                    throw std::runtime_error("torrent.search.providers must be a sequence");
                c.torrent.search_providers.clear();
                for (const auto& value : providers) {
                    TorrentSearchProviderConfig provider;
                    if (value["enabled"]) provider.enabled = value["enabled"].as<bool>();
                    if (value["name"]) provider.name = value["name"].as<std::string>();
                    if (value["type"]) provider.type = value["type"].as<std::string>();
                    if (value["url"]) provider.url = value["url"].as<std::string>();
                    if (value["api_key_file"])
                        provider.api_key_file = std::filesystem::path(value["api_key_file"].as<std::string>());
                    if (value["max_results"]) provider.max_results = value["max_results"].as<size_t>();
                    c.torrent.search_providers.push_back(std::move(provider));
                }
            }
        }
    }
}

void parse_streaming(const YAML::Node& root, Config& c) {
    auto streaming = root["streaming"];
    if (!streaming)
        return;
    if (streaming["enabled"])
        c.streaming.enabled = streaming["enabled"].as<bool>();
    // Legacy executable-path keys are accepted but ignored; media execution is
    // in-process through libav.
    if (streaming["temp_path"])
        c.streaming.temp_path = std::filesystem::path(streaming["temp_path"].as<std::string>());
    if (streaming["max_sessions"])
        c.streaming.max_sessions = streaming["max_sessions"].as<size_t>();
    if (streaming["max_sessions_per_account"])
        c.streaming.max_sessions_per_account =
            streaming["max_sessions_per_account"].as<size_t>();
    if (streaming["max_video_transcodes"])
        c.streaming.max_video_transcodes = streaming["max_video_transcodes"].as<size_t>();
    if (streaming["max_audio_transcodes"])
        c.streaming.max_audio_transcodes = streaming["max_audio_transcodes"].as<size_t>();
    if (streaming["video_decoder_threads"])
        c.streaming.video_decoder_threads = streaming["video_decoder_threads"].as<size_t>();
    if (streaming["video_encoder_threads"])
        c.streaming.video_encoder_threads = streaming["video_encoder_threads"].as<size_t>();
    if (streaming["session_idle_ms"])
        c.streaming.session_idle = milliseconds(streaming["session_idle_ms"], "streaming.session_idle_ms");
    if (streaming["session_unused_idle_ms"])
        c.streaming.session_unused_idle =
            milliseconds(streaming["session_unused_idle_ms"], "streaming.session_unused_idle_ms");
    if (streaming["pipeline_idle_ms"])
        c.streaming.pipeline_idle = milliseconds(streaming["pipeline_idle_ms"], "streaming.pipeline_idle_ms");
    if (streaming["startup_timeout_ms"])
        c.streaming.startup_timeout = milliseconds(streaming["startup_timeout_ms"], "streaming.startup_timeout_ms");
    if (streaming["segment_duration_ms"])
        c.streaming.segment_duration = milliseconds(streaming["segment_duration_ms"], "streaming.segment_duration_ms");
    if (streaming["max_ahead_segments"])
        c.streaming.max_ahead_segments = streaming["max_ahead_segments"].as<size_t>();
    if (streaming["segment_memory_bytes"])
        c.streaming.segment_memory_bytes = yaml_size(streaming["segment_memory_bytes"]);
    if (streaming["segment_hold_window"])
        c.streaming.segment_hold_window = streaming["segment_hold_window"].as<size_t>();
    if (streaming["max_session_holds"])
        c.streaming.max_session_holds = streaming["max_session_holds"].as<size_t>();
    if (streaming["max_concurrent_holds"])
        c.streaming.max_concurrent_holds = streaming["max_concurrent_holds"].as<size_t>();
    if (streaming["segment_timeout_ms"])
        c.streaming.segment_timeout =
            milliseconds(streaming["segment_timeout_ms"], "streaming.segment_timeout_ms");
    if (streaming["probe_bytes"])
        c.streaming.probe_bytes = yaml_size(streaming["probe_bytes"]);
    if (streaming["probe_analyze_duration_ms"])
        c.streaming.probe_analyze_duration =
            milliseconds(streaming["probe_analyze_duration_ms"], "streaming.probe_analyze_duration_ms");
    if (streaming["probe_timeout_ms"])
        c.streaming.probe_timeout =
            milliseconds(streaming["probe_timeout_ms"], "streaming.probe_timeout_ms");
}

void parse_runtime(const YAML::Node& root, Config& c) {
    auto runtime = root["runtime"];
    if (!runtime)
        return;
    if (runtime["glibc_arena_max"])
        c.runtime.glibc_arena_max = runtime["glibc_arena_max"].as<size_t>();
    if (runtime["retained_memory_bytes"])
        c.runtime.retained_memory_bytes = yaml_size(runtime["retained_memory_bytes"]);
    if (runtime["reassembly_memory_reserve_bytes"])
        c.runtime.reassembly_memory_reserve_bytes =
            yaml_size(runtime["reassembly_memory_reserve_bytes"]);
    if (runtime["control_memory_reserve_bytes"])
        c.runtime.control_memory_reserve_bytes =
            yaml_size(runtime["control_memory_reserve_bytes"]);
    if (runtime["viewer_memory_reserve_bytes"])
        c.runtime.viewer_memory_reserve_bytes =
            yaml_size(runtime["viewer_memory_reserve_bytes"]);
    if (runtime["loader_memory_reserve_bytes"])
        c.runtime.loader_memory_reserve_bytes =
            yaml_size(runtime["loader_memory_reserve_bytes"]);
}

void parse_web(const YAML::Node& root, Config& c) {
    auto web = root["web"];
    if (!web)
        return;
    if (web["enabled"])
        c.web.enabled = web["enabled"].as<bool>();
    if (web["root"])
        c.web.root = web["root"].as<std::string>();
    if (web["index"])
        c.web.index = web["index"].as<std::string>();
    if (c.web.index.empty())
        throw std::runtime_error("web.index must not be empty");
    if (c.web.index.find('/') != std::string::npos)
        throw std::runtime_error("web.index must be a file name inside web.root");
}

void parse_session(const YAML::Node& root, Config& c) {
    auto session = root["session"];
    if (!session)
        return;
    if (session["anonymous_ttl_ms"])
        c.session.anonymous_ttl = milliseconds(session["anonymous_ttl_ms"], "session.anonymous_ttl_ms");
    if (session["max_sessions"])
        c.session.max_sessions = session["max_sessions"].as<size_t>();
    if (session["allow_anonymous"])
        c.session.allow_anonymous = session["allow_anonymous"].as<bool>();
    if (session["max_users"])
        c.session.max_users = session["max_users"].as<size_t>();
    if (session["max_concurrent_password_checks"])
        c.session.max_concurrent_password_checks =
            session["max_concurrent_password_checks"].as<size_t>();
    if (session["failed_login_attempts"])
        c.session.failed_login_attempts = session["failed_login_attempts"].as<size_t>();
    if (session["failed_login_lockout_ms"])
        c.session.failed_login_lockout =
            milliseconds(session["failed_login_lockout_ms"], "session.failed_login_lockout_ms");
}

void parse_hydration_engine(const YAML::Node& engines, const char* name,
                            HydrationEngineConfig& engine) {
    auto node = engines[name];
    if (!node)
        return;
    if (node["enabled"])
        engine.enabled = node["enabled"].as<bool>();
    if (node["priority"])
        engine.priority = node["priority"].as<uint32_t>();
}

void parse_hydration(const YAML::Node& root, Config& c) {
    auto hydration = root["hydration"];
    if (!hydration)
        return;
    if (hydration["enabled"])
        c.hydration.enabled = hydration["enabled"].as<bool>();
    if (hydration["interval_ms"])
        c.hydration.interval = milliseconds(hydration["interval_ms"], "hydration.interval_ms");
    if (hydration["active_timeout_ms"])
        c.hydration.active_timeout =
            milliseconds(hydration["active_timeout_ms"], "hydration.active_timeout_ms");
    if (hydration["max_inflight"])
        c.hydration.max_inflight = hydration["max_inflight"].as<size_t>();
    if (hydration["catalogue_lookahead"])
        c.hydration.catalogue_lookahead = hydration["catalogue_lookahead"].as<size_t>();
    if (auto engines = hydration["engines"]) {
        parse_hydration_engine(engines, "read_ahead", c.hydration.read_ahead);
        parse_hydration_engine(engines, "current_file", c.hydration.current_file);
        parse_hydration_engine(engines, "catalogue", c.hydration.catalogue);
    }
}
} // namespace

Config load_yaml_config(const std::filesystem::path& path) {
    auto root = YAML::LoadFile(path.string());
    if (!root.IsMap())
        throw std::runtime_error("configuration root must be a mapping");

    if (root["verbose"])
        throw std::runtime_error("obsolete configuration key: verbose");
    if (root["mount_path"])
        throw std::runtime_error("obsolete configuration key: mount_path (moved to fuse.mount_path)");
    if (auto network = root["network"]) {
        if (network["control_timeout_ms"])
            throw std::runtime_error("obsolete configuration key: network.control_timeout_ms");
        if (network["data_timeout_ms"])
            throw std::runtime_error("obsolete configuration key: network.data_timeout_ms");
    }

    Config c;
    c.config_file = path;
    if (root["state_path"])
        c.state_path = root["state_path"].as<std::string>();
    if (root["key_file"])
        c.key_file = root["key_file"].as<std::string>();
    if (root["plugin_path"])
        c.plugin_path = root["plugin_path"].as<std::string>();
    if (root["log_level"])
        c.log_level = parse_log_level(root["log_level"].as<std::string>());
    if (root["ffmpeg_log_level"])
        c.ffmpeg_log_level = parse_ffmpeg_log_level(root["ffmpeg_log_level"].as<std::string>());
    if (root["service_startup_timeout_ms"])
        c.service_startup_timeout =
            milliseconds(root["service_startup_timeout_ms"], "service_startup_timeout_ms");
    if (root["service_startup_no_progress_ms"])
        c.service_startup_no_progress =
            milliseconds(root["service_startup_no_progress_ms"], "service_startup_no_progress_ms");

    auto storage = root["storage"];
    if (!storage || !storage.IsMap())
        throw std::runtime_error("storage must be a mapping with data and metadata sections");
    if (storage["hosts_extents"])
        c.hosts_extents = yaml_tristate(storage["hosts_extents"], "storage.hosts_extents");
    // storage.data is optional since 0.42.0: an edge node (hosts_extents
    // false, or auto with nothing configured) stores no extents. A node that
    // does host them still needs at least one backend; validate() says so.
    auto data_storage = storage["data"];
    if (data_storage && !data_storage.IsMap())
        throw std::runtime_error("storage.data must be a mapping");
    if (data_storage) {
        auto backends = data_storage["backends"];
        if (backends && !backends.IsSequence())
            throw std::runtime_error("storage.data.backends must be a sequence");
        if (backends) {
            for (const auto& item : backends) {
                StorageBackendConfig backend;
                if (!item["path"] || !item["limit"])
                    throw std::runtime_error("storage.data backend requires path and limit");
                backend.path = item["path"].as<std::string>();
                backend.limit = yaml_size(item["limit"]);
                if (item["reserve_free"])
                    backend.reserve_free = yaml_size(item["reserve_free"]);
                c.storage_backends.push_back(std::move(backend));
            }
        }
        if (auto packing = data_storage["packing"]) {
            if (packing["threshold"])
                c.storage_packing.threshold = static_cast<size_t>(yaml_size(packing["threshold"]));
            if (packing["target_size"])
                c.storage_packing.target_size = static_cast<size_t>(yaml_size(packing["target_size"]));
        }
    }
    if (auto metadata = storage["metadata"]) {
        if (!metadata.IsMap())
            throw std::runtime_error("storage.metadata must be a mapping");
        if (metadata["path"])
            c.metadata_store.path = metadata["path"].as<std::string>();
        if (metadata["limit"])
            c.metadata_store.limit = yaml_size(metadata["limit"]);
        if (auto packing = metadata["packing"]) {
            if (packing["threshold"])
                c.metadata_store.packing.threshold = static_cast<size_t>(yaml_size(packing["threshold"]));
            if (packing["target_size"])
                c.metadata_store.packing.target_size = static_cast<size_t>(yaml_size(packing["target_size"]));
        }
    }

    auto cache = root["cache"];
    if (cache) {
        if (cache["path"])
            c.cache.path = cache["path"].as<std::string>();
        if (cache["max_blocks"])
            c.cache.max_blocks = cache["max_blocks"].as<size_t>();
        if (cache["prefer_metadata"])
            c.cache.prefer_metadata = cache["prefer_metadata"].as<bool>();
    }

    parse_network(root, c);
    parse_dht(root, c);
    parse_maintenance(root, c);
    parse_filesystem(root, c);
    parse_fuse(root, c);
    parse_catalogue(root, c);
    parse_ingest(root, c);
    parse_torrent(root, c);
    parse_streaming(root, c);
    parse_hydration(root, c);
    parse_runtime(root, c);
    parse_session(root, c);
    parse_web(root, c);

    if (auto bootstrap = root["bootstrap"]) {
        if (!bootstrap.IsSequence())
            throw std::runtime_error("bootstrap must be a sequence");
        for (const auto& item : bootstrap)
            c.bootstrap.push_back(parse_endpoint(item.as<std::string>(), c.port));
    }
    return normalize_config(std::move(c));
}

void print_usage(const char* executable) {
    std::cout
        << "Usage: " << executable << " --config FILE [overrides]\n"
        << "--bootstrap HOST[:PORT] (repeatable)  --listen ADDR  --advertise HOST  --port PORT\n"
        << "--failure-domain NAME  --connect-timeout MS  --max-frame-size SIZE\n"
        << "--control-stall-notice MS  --data-stall-notice MS\n"
        << "--metadata-cache MS  --replicas N  --metadata-min-write-replicas N  --min-write-replicas N\n"
        << "--write-stall MS  --extent-size SIZE\n"
        << "--read-ahead N  --mount PATH  --state-path PATH  --cache-path PATH --cache-blocks N\n"
        << "--plugin-path PATH\n"
        << "--log-level LEVEL  (ALL|DEBUG|INFO|WARN|ERROR; default INFO)\n"
        << "--ffmpeg-log-level LEVEL  (QUIET|PANIC|FATAL|ERROR|WARN|INFO|VERBOSE|DEBUG|TRACE; default ERROR)  --help\n";
}

Config parse_config(int argc, char** argv) {
    std::optional<std::filesystem::path> yaml_file;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--help" || option == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        }
        if (option == "--config") {
            if (++i >= argc)
                throw std::runtime_error("--config requires value");
            yaml_file = argv[i];
        }
    }
    if (!yaml_file)
        throw std::runtime_error("--config is required");

    Config config = load_yaml_config(*yaml_file);
    std::vector<std::string> bootstrap_values;

    auto need = [&](int& index, const char* option) {
        if (++index >= argc)
            throw std::runtime_error(std::string(option) + " requires value");
        return std::string(argv[index]);
    };

    for (int i = 1; i < argc; ++i) {
        std::string option = argv[i];
        if (option == "--config") {
            (void)need(i, "--config");
        } else if (option == "--state-path") {
            config.state_path = need(i, "--state-path");
        } else if (option == "--key-file") {
            config.key_file = need(i, "--key-file");
        } else if (option == "--plugin-path") {
            config.plugin_path = need(i, "--plugin-path");
        } else if (option == "--cache-path") {
            config.cache.path = need(i, "--cache-path");
        } else if (option == "--cache-blocks") {
            config.cache.max_blocks = parse_unsigned(need(i, "--cache-blocks"), "cache blocks");
        } else if (option == "--mount") {
            config.fuse.mount_path = need(i, "--mount");
        } else if (option == "--listen") {
            config.listen_host = need(i, "--listen");
        } else if (option == "--advertise") {
            config.advertise_host = need(i, "--advertise");
        } else if (option == "--failure-domain") {
            config.failure_domain = need(i, "--failure-domain");
        } else if (option == "--bootstrap") {
            bootstrap_values.push_back(need(i, "--bootstrap"));
        } else if (option == "--port") {
            auto port = parse_unsigned(need(i, "--port"), "port");
            if (!port || port > 65535)
                throw std::runtime_error("bad port");
            config.port = static_cast<uint16_t>(port);
        } else if (option == "--replicas") {
            config.replication = parse_unsigned(need(i, "--replicas"), "replica count");
        } else if (option == "--metadata-min-write-replicas") {
            config.metadata_min_write_replicas = parse_unsigned(
                need(i, option.c_str()), "minimum metadata write replica count");
        } else if (option == "--metadata-replicas") {
            const auto legacy = parse_unsigned(need(i, option.c_str()), "legacy metadata replica count");
            config.metadata_min_write_replicas = legacy ? legacy / 2 + 1 : 0;
        } else if (option == "--min-write-replicas") {
            config.min_write_replicas =
                parse_unsigned(need(i, "--min-write-replicas"), "minimum write replica count");
        } else if (option == "--write-stall") {
            config.write_stall = std::chrono::milliseconds(
                parse_unsigned(need(i, "--write-stall"), "write stall"));
        } else if (option == "--extent-size") {
            config.extent_size = parse_size(need(i, "--extent-size"));
        } else if (option == "--read-ahead") {
            config.read_ahead_extents = parse_unsigned(need(i, "--read-ahead"), "read-ahead");
        } else if (option == "--connect-timeout") {
            config.connect_timeout = std::chrono::milliseconds(
                parse_unsigned(need(i, "--connect-timeout"), "connect timeout"));
        } else if (option == "--max-frame-size") {
            config.max_frame_size = parse_size(need(i, "--max-frame-size"));
        } else if (option == "--control-stall-notice") {
            config.control_stall_notice = std::chrono::milliseconds(parse_unsigned(
                need(i, option.c_str()), "control stall notice"));
        } else if (option == "--data-stall-notice") {
            config.data_stall_notice = std::chrono::milliseconds(
                parse_unsigned(need(i, option.c_str()), "data stall notice"));
        } else if (option == "--metadata-cache") {
            config.metadata_cache = std::chrono::milliseconds(
                parse_unsigned(need(i, "--metadata-cache"), "metadata cache"));
        } else if (option == "--log-level") {
            config.log_level = parse_log_level(need(i, "--log-level"));
        } else if (option == "--ffmpeg-log-level") {
            config.ffmpeg_log_level = parse_ffmpeg_log_level(need(i, "--ffmpeg-log-level"));
        } else if (option == "--help" || option == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown option: " + option);
        }
    }

    if (!bootstrap_values.empty()) {
        config.bootstrap.clear();
        for (const auto& bootstrap : bootstrap_values)
            config.bootstrap.push_back(parse_endpoint(bootstrap, config.port));
    }

    return normalize_config(std::move(config));
}
} // namespace macha
