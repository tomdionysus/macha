// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
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

void validate(Config& config) {
    if (config.state_path.empty())
        throw std::runtime_error("state_path is required");
    if (config.key_file.empty())
        throw std::runtime_error("key_file is required");
    if (!config.port)
        throw std::runtime_error("network port must be nonzero");
    if (config.cache.max_blocks && config.cache.path.empty())
        throw std::runtime_error("cache.path is required when cache.max_blocks is nonzero");
    if (config.storage_backends.empty())
        throw std::runtime_error("at least one storage backend is required");
    for (const auto& backend : config.storage_backends) {
        if (backend.path.empty() || !backend.limit)
            throw std::runtime_error("each storage.data backend requires path and nonzero limit");
    }
    if (!config.metadata_store.limit)
        throw std::runtime_error("storage.metadata.limit must be nonzero");
    const auto validate_packing = [](const StoragePackingConfig& packing, const char* what) {
        if (!packing.threshold) {
            if (packing.target_size)
                throw std::runtime_error(std::string(what) + ".target_size requires a nonzero threshold");
            return;
        }
        if (!packing.target_size || packing.target_size < packing.threshold)
            throw std::runtime_error(std::string(what) + ".target_size must be >= threshold");
    };
    validate_packing(config.storage_packing, "storage.data.packing");
    validate_packing(config.metadata_store.packing, "storage.metadata.packing");
    if (!config.replication || !config.metadata_min_write_replicas || !config.min_write_replicas)
        throw std::runtime_error("replication must be nonzero");
    if (config.min_write_replicas > config.replication)
        throw std::runtime_error("dht.min_write_replicas must be <= dht.replicas");
    if (config.write_stall.count() <= 0)
        throw std::runtime_error("dht.write_stall_ms must be > 0");
    if (!config.data_inflight_bytes || !config.data_viewer_reserve_bytes ||
        config.data_viewer_reserve_bytes >= config.data_inflight_bytes)
        throw std::runtime_error(
            "dht.data_inflight_bytes must exceed nonzero data_viewer_reserve_bytes");
    if (config.extent_size >
        config.data_inflight_bytes - config.data_viewer_reserve_bytes)
        throw std::runtime_error(
            "dht DATA non-viewer capacity must fit one complete extent");
    // DATA placement uses a compact bounded owner set. Metadata publication is
    // an any-node durability floor and must not inherit that historical voter
    // count limit; large clusters may legitimately require more than 31 copies.
    if (config.replication > 31)
        throw std::runtime_error("dht.replicas must be <= 31");
    if (config.read_ahead_extents > 64)
        throw std::runtime_error("read-ahead must be <= 64");
    if (config.connect_timeout.count() < 100 || config.metadata_cache.count() < 0)
        throw std::runtime_error("invalid network/cache timeout");
    if (config.upnp.discovery_timeout < std::chrono::milliseconds(100) ||
        config.upnp.discovery_timeout > std::chrono::seconds(30))
        throw std::runtime_error("network.upnp.discovery_timeout_ms must be 100..30000");
    if (config.external_ip.timeout < std::chrono::milliseconds(100) ||
        config.external_ip.timeout > std::chrono::seconds(30))
        throw std::runtime_error("network.external_ip.timeout_ms must be 100..30000");
    if (config.connectivity_check.timeout < std::chrono::milliseconds(100) ||
        config.connectivity_check.timeout > std::chrono::seconds(30))
        throw std::runtime_error("network.connectivity_check.timeout_ms must be 100..30000");
    if (config.max_frame_size < 4 * 1024 || config.max_frame_size > 4ULL * 1024 * 1024)
        throw std::runtime_error("network.max_frame_size must be 4K..4M");
    if (config.heartbeat.count() <= 0 || config.dead_after.count() <= 0)
        throw std::runtime_error("network heartbeat and dead_after must be > 0");
    if (config.metadata_cache > std::chrono::seconds(5))
        throw std::runtime_error("metadata cache must be <= 5000ms");
    if (config.fuse.entry_timeout > std::chrono::seconds(5) ||
        config.fuse.attr_timeout > std::chrono::seconds(5) ||
        config.fuse.negative_timeout > std::chrono::seconds(5))
        throw std::runtime_error("fuse kernel cache timeouts must be <= 5000ms");
    if (config.fuse.spool_path && config.fuse.spool_path->empty())
        throw std::runtime_error("fuse.spool_path must not be empty");
    if (config.fuse.operation_journal_path && config.fuse.operation_journal_path->empty())
        throw std::runtime_error("fuse.operation_journal_path must not be empty");
    const auto fuse_max = std::chrono::seconds(30);
    const auto positive = [](std::chrono::milliseconds value) { return value.count() > 0; };
    if (!positive(config.fuse.absolute_request_timeout) ||
        config.fuse.absolute_request_timeout > fuse_max)
        throw std::runtime_error("fuse.absolute_request_timeout_ms must be 1..30000");
    const std::array fuse_timeouts{config.fuse.timeouts.lookup,
                                   config.fuse.timeouts.namespace_mutation,
                                   config.fuse.timeouts.read, config.fuse.timeouts.write,
                                   config.fuse.timeouts.sync, config.fuse.timeouts.lifecycle};
    for (auto timeout : fuse_timeouts) {
        if (!positive(timeout) || timeout > config.fuse.absolute_request_timeout)
            throw std::runtime_error("fuse operation timeouts must be >0 and <= absolute_request_timeout_ms");
    }
    if (config.fuse.request_workers < 6 || config.fuse.request_workers > 256)
        throw std::runtime_error("fuse.request_workers must be 6..256");
    if (!config.fuse.commit_workers || config.fuse.commit_workers > 64)
        throw std::runtime_error("fuse.commit_workers must be 1..64");
    if (!config.fuse.recovery_commit_workers || config.fuse.recovery_commit_workers > 64)
        throw std::runtime_error("fuse.recovery_commit_workers must be 1..64");
    if (!config.fuse.foreground_commit_workers ||
        config.fuse.foreground_commit_workers > config.fuse.commit_workers)
        throw std::runtime_error("fuse.foreground_commit_workers must be 1..commit_workers");
    if (config.fuse.publication_quiet < std::chrono::milliseconds(0) ||
        config.fuse.publication_quiet > std::chrono::seconds(30))
        throw std::runtime_error("fuse.publication_quiet_ms must be 0..30000");
    if (!config.fuse.viewer_weight || config.fuse.viewer_weight > 10000 ||
        !config.fuse.loader_weight || config.fuse.loader_weight > 10000)
        throw std::runtime_error("fuse viewer_weight and loader_weight must be 1..10000");
    constexpr uint64_t publication_chunk = 256ULL * 1024;
    if (config.fuse.publication_quantum_bytes < publication_chunk ||
        config.fuse.publication_quantum_bytes > 1024ULL * 1024 * 1024 ||
        config.fuse.publication_quantum_bytes % publication_chunk ||
        config.fuse.publication_quantum_bytes < config.extent_size ||
        config.fuse.publication_quantum_bytes % config.extent_size)
        throw std::runtime_error(
            "fuse.publication_quantum_bytes must be an extent-size multiple from extent_size..1G");
    if (config.fuse.publication_inflight_bytes < config.fuse.publication_quantum_bytes ||
        config.fuse.publication_inflight_bytes > 16ULL * 1024 * 1024 * 1024 ||
        config.fuse.publication_inflight_bytes % config.fuse.publication_quantum_bytes)
        throw std::runtime_error(
            "fuse.publication_inflight_bytes must be a quantum multiple from quantum..16G");
    if (!config.fuse.publication_pipeline_bytes)
        config.fuse.publication_pipeline_bytes =
            std::min<uint64_t>(config.fuse.publication_quantum_bytes,
                               static_cast<uint64_t>(config.extent_size) * 2);
    if (config.fuse.publication_pipeline_bytes < config.extent_size ||
        config.fuse.publication_pipeline_bytes > config.fuse.publication_quantum_bytes ||
        config.fuse.publication_pipeline_bytes % config.extent_size ||
        config.fuse.publication_pipeline_bytes / config.extent_size > 8)
        throw std::runtime_error(
            "fuse.publication_pipeline_bytes must be an extent-size multiple from "
            "extent_size..min(publication_quantum_bytes, 8 extents)");
    if (!config.fuse.max_pending_requests || config.fuse.max_pending_requests > 65536 ||
        !config.fuse.max_pending_operations || config.fuse.max_pending_operations > 65536)
        throw std::runtime_error("fuse pending queue limits must be 1..65536");
    if (!config.fuse.namespace_batch_operations ||
        config.fuse.namespace_batch_operations > config.fuse.max_pending_operations)
        throw std::runtime_error(
            "fuse.namespace_batch_operations must be 1..max_pending_operations");
    if (!config.fuse.namespace_batch_bytes || config.fuse.namespace_batch_bytes > 64ULL * 1024 * 1024)
        throw std::runtime_error("fuse.namespace_batch_bytes must be 1..64M");
    if (!config.fuse.max_spool_bytes)
        throw std::runtime_error("fuse.max_spool_bytes must be nonzero");
    if (config.fuse.max_operation_journal_bytes < 4096)
        throw std::runtime_error("fuse.max_operation_journal_bytes must be at least 4096");
    if (config.fuse.read_ahead_extents > 64)
        throw std::runtime_error("fuse.read_ahead_extents must be <= 64");
    if (config.fuse.watchdog_interval < std::chrono::milliseconds(50))
        throw std::runtime_error("fuse.watchdog_interval_ms must be >= 50ms");
    if (config.extent_size < 1024 * 1024 || config.extent_size > 64ULL * 1024 * 1024)
        throw std::runtime_error("extent size must be 1M..64M");
    if (config.catalogue.api.enabled && !config.catalogue.api.port)
        throw std::runtime_error("catalogue.api.port must be nonzero");
    if (config.catalogue.api.max_request_bytes < 1024)
        throw std::runtime_error("catalogue.api.max_request_bytes must be >= 1K");
    if (!config.catalogue.api.workers || config.catalogue.api.workers > 256)
        throw std::runtime_error("catalogue.api.workers must be 1..256");
    if (!config.catalogue.api.max_queued_connections || config.catalogue.api.max_queued_connections > 4096)
        throw std::runtime_error("catalogue.api.max_queued_connections must be 1..4096");
    if (config.catalogue.api.client_io_timeout < std::chrono::seconds(1) ||
        config.catalogue.api.client_io_timeout > std::chrono::minutes(5))
        throw std::runtime_error("catalogue.api.client_io_timeout_ms must be 1000..300000");
    if (config.catalogue.api.stream_chunk_bytes < 16 * 1024 ||
        config.catalogue.api.stream_chunk_bytes > 4ULL * 1024 * 1024)
        throw std::runtime_error("catalogue.api.stream_chunk_bytes must be 16K..4M");
    if (config.maintenance.no_progress_backoff < std::chrono::milliseconds(500) ||
        config.maintenance.no_progress_backoff > std::chrono::hours(1))
        throw std::runtime_error("maintenance.no_progress_backoff_ms must be 500..3600000");
    if (config.maintenance.scrub_interval < std::chrono::minutes(1) ||
        config.maintenance.scrub_interval > std::chrono::hours(24 * 365))
        throw std::runtime_error("maintenance.scrub_interval_ms must be 60000..31536000000");
    if (config.ingest.staging_path.empty())
        config.ingest.staging_path = config.state_path / "tmp" / "ingest";
    if (config.ingest.enabled && !config.catalogue.scanner.enabled)
        throw std::runtime_error("ingest requires catalogue.scanner.enabled");
    if (config.ingest.staging_limit < 1024ULL * 1024)
        throw std::runtime_error("ingest.staging_limit must be >= 1M");
    if (config.ingest.copy_chunk_bytes < 64ULL * 1024 ||
        config.ingest.copy_chunk_bytes > 8ULL * 1024 * 1024)
        throw std::runtime_error("ingest.copy_chunk_bytes must be 64K..8M");
    if (config.ingest.checkpoint_bytes < config.ingest.copy_chunk_bytes ||
        config.ingest.checkpoint_bytes > 4ULL * 1024 * 1024 * 1024)
        throw std::runtime_error("ingest.checkpoint_bytes must be >= copy_chunk_bytes and <= 4G");
    if (config.ingest.blocked_retry < std::chrono::milliseconds(500) ||
        config.ingest.blocked_retry > std::chrono::minutes(10))
        throw std::runtime_error("ingest.blocked_retry_ms must be 500..600000");
    if (config.torrent.enabled && !config.ingest.enabled)
        throw std::runtime_error("torrent requires ingest.enabled");
#ifndef MACHA_HAVE_LIBTORRENT
    if (config.torrent.enabled)
        throw std::runtime_error("torrent support was not built (libtorrent-rasterbar not found)");
#endif
    if (!config.torrent.max_active || config.torrent.max_active > 64)
        throw std::runtime_error("torrent.max_active must be 1..64");
    for (const auto& provider : config.torrent.search_providers) {
        if (!provider.enabled) continue;
        if (provider.name.empty())
            throw std::runtime_error("torrent.search.providers requires a non-empty name");
        if (provider.type != "torznab")
            throw std::runtime_error("unsupported torrent search provider type: " + provider.type);
        if (!provider.url.starts_with("http://") && !provider.url.starts_with("https://"))
            throw std::runtime_error("torrent search provider URL must use http or https");
        if (!provider.max_results || provider.max_results > 1000)
            throw std::runtime_error("torrent search provider max_results must be 1..1000");
    }
    if (config.streaming.enabled && !config.catalogue.api.enabled)
        throw std::runtime_error("streaming requires catalogue.api.enabled");
    if (!config.streaming.max_sessions || config.streaming.max_sessions > 1024)
        throw std::runtime_error("streaming.max_sessions must be 1..1024");
    if (config.streaming.max_video_transcodes > config.streaming.max_sessions ||
        config.streaming.max_audio_transcodes > config.streaming.max_sessions)
        throw std::runtime_error("streaming transcode limits cannot exceed max_sessions");
    if (config.streaming.session_idle < std::chrono::seconds(30))
        throw std::runtime_error("streaming.session_idle_ms must be >= 30000");
    if (config.streaming.startup_timeout < std::chrono::milliseconds(1000) ||
        config.streaming.startup_timeout > std::chrono::minutes(2))
        throw std::runtime_error("streaming.startup_timeout_ms must be 1000..120000");
    if (config.streaming.segment_duration < std::chrono::milliseconds(1000) ||
        config.streaming.segment_duration > std::chrono::seconds(20))
        throw std::runtime_error("streaming.segment_duration_ms must be 1000..20000");
    if (config.streaming.max_ahead_segments < 2 || config.streaming.max_ahead_segments > 120)
        throw std::runtime_error("streaming.max_ahead_segments must be 2..120");
    if (config.streaming.segment_memory_bytes < 4ULL * 1024 * 1024 ||
        config.streaming.segment_memory_bytes > 4ULL * 1024 * 1024 * 1024)
        throw std::runtime_error("streaming.segment_memory_bytes must be 4M..4G");
    if (config.streaming.probe_bytes < 256ULL * 1024 || config.streaming.probe_bytes > 64ULL * 1024 * 1024)
        throw std::runtime_error("streaming.probe_bytes must be 256K..64M");
    if (config.streaming.probe_analyze_duration < std::chrono::milliseconds(250) ||
        config.streaming.probe_analyze_duration > std::chrono::seconds(30))
        throw std::runtime_error("streaming.probe_analyze_duration_ms must be 250..30000");
    if (config.streaming.probe_timeout < std::chrono::seconds(2) ||
        config.streaming.probe_timeout > std::chrono::minutes(2))
        throw std::runtime_error("streaming.probe_timeout_ms must be 2000..120000");
    if (!config.streaming.temp_path)
        config.streaming.temp_path = config.state_path / "tmp" / "playback";
    if (config.catalogue.scanner.interval < std::chrono::seconds(10))
        throw std::runtime_error("catalogue.scanner.interval_ms must be >= 10000");
    if (config.catalogue.scanner.rescan_debounce < std::chrono::seconds(1) ||
        config.catalogue.scanner.rescan_debounce > std::chrono::minutes(10))
        throw std::runtime_error("catalogue.scanner.rescan_debounce_ms must be 1000..600000");
    if (config.catalogue.scanner.rescan_max_delay < std::chrono::seconds(1))
        throw std::runtime_error("catalogue.scanner.rescan_max_delay_ms must be >= 1000");
    if (config.catalogue.scanner.rescan_max_delay < config.catalogue.scanner.rescan_debounce)
        throw std::runtime_error("catalogue.scanner.rescan_max_delay_ms must be >= rescan_debounce_ms");
    if (!config.catalogue.scanner.max_provider_requests_per_scan ||
        config.catalogue.scanner.max_provider_requests_per_scan > 10000)
        throw std::runtime_error("catalogue.scanner.max_provider_requests_per_scan must be 1..10000");
    if (config.catalogue.scanner.provider_batch_delay < std::chrono::seconds(1) ||
        config.catalogue.scanner.provider_batch_delay > std::chrono::hours(1))
        throw std::runtime_error("catalogue.scanner.provider_batch_delay_ms must be 1000..3600000");
    const auto validate_provider_roots = [](std::string_view name, bool enabled,
                                            const std::vector<std::string>& roots) {
        if (!enabled) return;
        if (roots.empty())
            throw std::runtime_error("catalogue.scanner.providers." + std::string(name) +
                                     ".roots must not be empty");
        for (const auto& root : roots) {
            if (root.empty() || root.front() != '/')
                throw std::runtime_error("catalogue.scanner.providers." + std::string(name) +
                                         ".roots must contain absolute paths");
        }
    };
    validate_provider_roots("movies", config.catalogue.scanner.movies.enabled,
                            config.catalogue.scanner.movies.roots);
    validate_provider_roots("tv", config.catalogue.scanner.tv.enabled,
                            config.catalogue.scanner.tv.roots);
    validate_provider_roots("music", config.catalogue.scanner.music.enabled,
                            config.catalogue.scanner.music.roots);
    if (config.catalogue.scanner.max_artwork_bytes < 64 * 1024 ||
        config.catalogue.scanner.max_artwork_bytes > 128ULL * 1024 * 1024)
        throw std::runtime_error("catalogue.scanner.max_artwork_bytes must be 64K..128M");
    if (config.catalogue.scanner.music.enabled &&
        config.catalogue.scanner.music.musicbrainz.enabled &&
        config.catalogue.scanner.music.musicbrainz.contact.empty())
        throw std::runtime_error(
            "catalogue.scanner.providers.music.musicbrainz.contact is required");
    if (config.catalogue.scanner.enabled &&
        config.catalogue.scanner.music.enabled &&
        config.catalogue.scanner.music.discogs.enabled &&
        !config.catalogue.scanner.music.discogs.token_file)
        throw std::runtime_error(
            "catalogue.scanner.providers.music.discogs.token_file is required when Discogs is enabled");
    if (config.catalogue.scanner.enabled &&
        config.catalogue.scanner.movies.enabled &&
        config.catalogue.scanner.movies.tmdb.enabled &&
        !config.catalogue.scanner.movies.tmdb.token_file)
        throw std::runtime_error(
            "catalogue.scanner.providers.movies.tmdb.token_file is required when movie scanning is enabled");
    if (config.catalogue.scanner.enabled &&
        config.catalogue.scanner.tv.enabled &&
        config.catalogue.scanner.tv.tmdb.enabled &&
        !config.catalogue.scanner.tv.tmdb.token_file)
        throw std::runtime_error(
            "catalogue.scanner.providers.tv.tmdb.token_file is required when TV scanning is enabled");
    if (config.hydration.interval < std::chrono::milliseconds(10))
        throw std::runtime_error("hydration.interval_ms must be >= 10ms");
    if (config.hydration.active_timeout < std::chrono::milliseconds(1000))
        throw std::runtime_error("hydration.active_timeout_ms must be >= 1000ms");
    if (!config.hydration.max_inflight || config.hydration.max_inflight > 64)
        throw std::runtime_error("hydration.max_inflight must be 1..64");
    if (config.hydration.catalogue_lookahead > 16)
        throw std::runtime_error("hydration.catalogue_lookahead must be <= 16");
    for (const auto* engine : {&config.hydration.read_ahead, &config.hydration.current_file,
                               &config.hydration.catalogue}) {
        if (engine->enabled && (!engine->priority || engine->priority > 1000000))
            throw std::runtime_error("hydration engine priority must be 1..1000000");
    }
    if (config.maintenance.interval < std::chrono::milliseconds(50))
        throw std::runtime_error("maintenance interval must be >= 50ms");
    if (config.maintenance.cpu_target <= 0.0 || config.maintenance.cpu_target > 1.0)
        throw std::runtime_error("maintenance cpu_target must be > 0 and <= 1");
    if (config.maintenance.busy_bandwidth_fraction < 0.0 ||
        config.maintenance.busy_bandwidth_fraction > 1.0 ||
        config.maintenance.idle_bandwidth_fraction < 0.0 ||
        config.maintenance.idle_bandwidth_fraction > 1.0 ||
        config.maintenance.scrub_fraction < 0.0 || config.maintenance.scrub_fraction > 1.0)
        throw std::runtime_error("maintenance fractions must be between 0 and 1");
}

 } // namespace

uint64_t parse_size(const std::string& value) {
    size_t split = 0;
    while (split < value.size() && std::isdigit(static_cast<unsigned char>(value[split])))
        ++split;
    if (!split)
        throw std::runtime_error("invalid size: " + value);

    uint64_t number = 0;
    auto [end, error] = std::from_chars(value.data(), value.data() + split, number);
    if (error != std::errc{} || end != value.data() + split)
        throw std::runtime_error("invalid size: " + value);

    auto suffix = value.substr(split);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    uint64_t multiplier = 1;
    if (suffix.empty() || suffix == "b")
        multiplier = 1;
    else if (suffix == "k" || suffix == "kb" || suffix == "kib")
        multiplier = 1ULL << 10;
    else if (suffix == "m" || suffix == "mb" || suffix == "mib")
        multiplier = 1ULL << 20;
    else if (suffix == "g" || suffix == "gb" || suffix == "gib")
        multiplier = 1ULL << 30;
    else if (suffix == "t" || suffix == "tb" || suffix == "tib")
        multiplier = 1ULL << 40;
    else
        throw std::runtime_error("invalid size suffix: " + suffix);

    if (number > std::numeric_limits<uint64_t>::max() / multiplier)
        throw std::runtime_error("size overflows uint64: " + value);
    return number * multiplier;
}

Endpoint parse_endpoint(const std::string& value, uint16_t default_port) {
    if (value.empty())
        throw std::runtime_error("empty endpoint");

    Endpoint endpoint;
    if (value.front() == '[') {
        auto close = value.find(']');
        if (close == std::string::npos)
            throw std::runtime_error("bad endpoint: " + value);
        endpoint.host = value.substr(1, close - 1);
        if (close + 1 == value.size()) {
            endpoint.port = default_port;
        } else {
            if (value[close + 1] != ':')
                throw std::runtime_error("bad endpoint: " + value);
            auto port = parse_unsigned(value.substr(close + 2), "port");
            if (!port || port > 65535)
                throw std::runtime_error("bad port");
            endpoint.port = static_cast<uint16_t>(port);
        }
        return endpoint;
    }

    auto colon = value.rfind(':');
    if (colon == std::string::npos || value.find(':') != colon) {
        endpoint.host = value;
        endpoint.port = default_port;
        return endpoint;
    }

    endpoint.host = value.substr(0, colon);
    auto port = parse_unsigned(value.substr(colon + 1), "port");
    if (!port || port > 65535)
        throw std::runtime_error("bad port");
    endpoint.port = static_cast<uint16_t>(port);
    return endpoint;
}

Config normalize_config(Config c) {
    if (c.metadata_store.path.empty() && !c.state_path.empty())
        c.metadata_store.path = c.state_path / "metadata-objects";
    validate(c);
    return c;
}


} // namespace macha
