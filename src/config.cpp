// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
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
            throw std::runtime_error("each storage backend requires path and nonzero limit");
    }
    if (!config.replication || !config.metadata_replication)
        throw std::runtime_error("replication must be nonzero");
    if (config.replication > 31 || config.metadata_replication > 31)
        throw std::runtime_error("replica count must be <= 31");
    if (config.read_ahead_extents > 64)
        throw std::runtime_error("read-ahead must be <= 64");
    if (config.connect_timeout.count() < 100 || config.metadata_cache.count() < 0)
        throw std::runtime_error("invalid network/cache timeout");
    if (config.max_frame_size < 4 * 1024 || config.max_frame_size > 4ULL * 1024 * 1024)
        throw std::runtime_error("network.max_frame_size must be 4K..4M");
    if (config.heartbeat.count() <= 0 || config.dead_after.count() <= 0)
        throw std::runtime_error("network heartbeat and dead_after must be > 0");
    if (config.metadata_cache > std::chrono::seconds(5))
        throw std::runtime_error("metadata cache must be <= 5000ms");
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
    if (config.catalogue.api.stream_chunk_bytes < 16 * 1024 ||
        config.catalogue.api.stream_chunk_bytes > 4ULL * 1024 * 1024)
        throw std::runtime_error("catalogue.api.stream_chunk_bytes must be 16K..4M");
    if (config.maintenance.no_progress_backoff < std::chrono::milliseconds(500) ||
        config.maintenance.no_progress_backoff > std::chrono::hours(1))
        throw std::runtime_error("maintenance.no_progress_backoff_ms must be 500..3600000");
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
    if (config.catalogue.scanner.mutation_debounce < std::chrono::seconds(1) ||
        config.catalogue.scanner.mutation_debounce > std::chrono::minutes(10))
        throw std::runtime_error("catalogue.scanner.mutation_debounce_ms must be 1000..600000");
    if (config.catalogue.scanner.roots.empty())
        throw std::runtime_error("catalogue.scanner.roots must not be empty");
    for (const auto& root : config.catalogue.scanner.roots) {
        if (root.empty() || root.front() != '/')
            throw std::runtime_error("catalogue.scanner.roots must contain absolute paths");
    }
    if (config.catalogue.scanner.max_artwork_bytes < 64 * 1024 ||
        config.catalogue.scanner.max_artwork_bytes > 128ULL * 1024 * 1024)
        throw std::runtime_error("catalogue.scanner.max_artwork_bytes must be 64K..128M");
    if (config.catalogue.scanner.musicbrainz.enabled &&
        config.catalogue.scanner.musicbrainz.contact.empty())
        throw std::runtime_error("catalogue.scanner.providers.musicbrainz.contact is required");
    if (config.catalogue.scanner.enabled && config.catalogue.scanner.tmdb.enabled &&
        !config.catalogue.scanner.tmdb.token_file)
        throw std::runtime_error("catalogue.scanner.providers.tmdb.token_file is required when TMDB scanning is enabled");
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
    if (n["failure_domain"])
        c.failure_domain = n["failure_domain"].as<std::string>();
    if (n["heartbeat_ms"])
        c.heartbeat = milliseconds(n["heartbeat_ms"], "heartbeat_ms");
    if (n["dead_after_ms"])
        c.dead_after = milliseconds(n["dead_after_ms"], "dead_after_ms");
    if (n["connect_timeout_ms"])
        c.connect_timeout = milliseconds(n["connect_timeout_ms"], "connect_timeout_ms");
    if (n["max_frame_size"])
        c.max_frame_size = yaml_size(n["max_frame_size"]);
    if (n["control_stall_notice_ms"])
        c.control_stall_notice =
            milliseconds(n["control_stall_notice_ms"], "control_stall_notice_ms");
    if (n["data_stall_notice_ms"])
        c.data_stall_notice =
            milliseconds(n["data_stall_notice_ms"], "data_stall_notice_ms");
}

void parse_dht(const YAML::Node& root, Config& c) {
    auto d = root["dht"];
    if (!d)
        return;
    if (d["replicas"])
        c.replication = d["replicas"].as<size_t>();
    if (d["metadata_replicas"])
        c.metadata_replication = d["metadata_replicas"].as<size_t>();
    if (d["extent_size"])
        c.extent_size = yaml_size(d["extent_size"]);
    if (d["read_ahead"])
        c.read_ahead_extents = d["read_ahead"].as<size_t>();
    if (d["metadata_cache_ms"])
        c.metadata_cache = milliseconds(d["metadata_cache_ms"], "metadata_cache_ms");
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
    if (m["initial_bandwidth"])
        c.maintenance.initial_bandwidth = yaml_size(m["initial_bandwidth"]);
    if (m["max_bandwidth"])
        c.maintenance.max_bandwidth = yaml_size(m["max_bandwidth"]);
    if (m["scrub_fraction"])
        c.maintenance.scrub_fraction = parse_fraction(m["scrub_fraction"], "scrub_fraction");
    if (m["no_progress_backoff_ms"])
        c.maintenance.no_progress_backoff =
            milliseconds(m["no_progress_backoff_ms"], "maintenance.no_progress_backoff_ms");
}

void parse_filesystem(const YAML::Node& root, Config& c) {
    auto f = root["filesystem"];
    if (!f)
        return;
    if (f["allow_other"])
        c.filesystem.allow_other = f["allow_other"].as<bool>();
    if (f["root_uid"])
        c.filesystem.root_uid = f["root_uid"].as<uint32_t>();
    if (f["root_gid"])
        c.filesystem.root_gid = f["root_gid"].as<uint32_t>();
    if (f["root_mode"])
        c.filesystem.root_mode = parse_mode(f["root_mode"], "filesystem.root_mode");
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
        if (api["token_file"])
            c.catalogue.api.token_file = std::filesystem::path(api["token_file"].as<std::string>());
        if (api["max_request_bytes"])
            c.catalogue.api.max_request_bytes = yaml_size(api["max_request_bytes"]);
        if (api["workers"])
            c.catalogue.api.workers = api["workers"].as<size_t>();
        if (api["max_queued_connections"])
            c.catalogue.api.max_queued_connections = api["max_queued_connections"].as<size_t>();
        if (api["stream_chunk_bytes"])
            c.catalogue.api.stream_chunk_bytes = yaml_size(api["stream_chunk_bytes"]);
    }
    if (auto scanner = catalogue["scanner"]) {
        if (scanner["enabled"])
            c.catalogue.scanner.enabled = scanner["enabled"].as<bool>();
        if (scanner["interval_ms"])
            c.catalogue.scanner.interval = milliseconds(scanner["interval_ms"], "catalogue.scanner.interval_ms");
        if (scanner["mutation_debounce_ms"])
            c.catalogue.scanner.mutation_debounce = milliseconds(
                scanner["mutation_debounce_ms"], "catalogue.scanner.mutation_debounce_ms");
        if (scanner["max_artwork_bytes"])
            c.catalogue.scanner.max_artwork_bytes = yaml_size(scanner["max_artwork_bytes"]);
        if (auto roots = scanner["roots"]) {
            if (!roots.IsSequence())
                throw std::runtime_error("catalogue.scanner.roots must be a sequence");
            c.catalogue.scanner.roots.clear();
            for (const auto& root : roots)
                c.catalogue.scanner.roots.push_back(root.as<std::string>());
        }
        if (auto providers = scanner["providers"]) {
            if (auto tmdb = providers["tmdb"]) {
                if (tmdb["enabled"]) c.catalogue.scanner.tmdb.enabled = tmdb["enabled"].as<bool>();
                if (tmdb["token_file"]) c.catalogue.scanner.tmdb.token_file = std::filesystem::path(tmdb["token_file"].as<std::string>());
                if (tmdb["language"]) c.catalogue.scanner.tmdb.language = tmdb["language"].as<std::string>();
                if (tmdb["image_size"]) c.catalogue.scanner.tmdb.image_size = tmdb["image_size"].as<std::string>();
            }
            if (auto mb = providers["musicbrainz"]) {
                if (mb["enabled"]) c.catalogue.scanner.musicbrainz.enabled = mb["enabled"].as<bool>();
                if (mb["contact"]) c.catalogue.scanner.musicbrainz.contact = mb["contact"].as<std::string>();
                if (mb["cover_size"]) c.catalogue.scanner.musicbrainz.cover_size = mb["cover_size"].as<std::string>();
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
    // 0.7.0 originally exposed ffmpeg/ffprobe executable paths. The libav
    // backend no longer uses them, but silently tolerate those keys so an
    // existing 0.7.0 configuration keeps starting after this replacement.
    if (streaming["temp_path"])
        c.streaming.temp_path = std::filesystem::path(streaming["temp_path"].as<std::string>());
    if (streaming["max_sessions"])
        c.streaming.max_sessions = streaming["max_sessions"].as<size_t>();
    if (streaming["max_video_transcodes"])
        c.streaming.max_video_transcodes = streaming["max_video_transcodes"].as<size_t>();
    if (streaming["max_audio_transcodes"])
        c.streaming.max_audio_transcodes = streaming["max_audio_transcodes"].as<size_t>();
    if (streaming["session_idle_ms"])
        c.streaming.session_idle = milliseconds(streaming["session_idle_ms"], "streaming.session_idle_ms");
    if (streaming["startup_timeout_ms"])
        c.streaming.startup_timeout = milliseconds(streaming["startup_timeout_ms"], "streaming.startup_timeout_ms");
    if (streaming["segment_duration_ms"])
        c.streaming.segment_duration = milliseconds(streaming["segment_duration_ms"], "streaming.segment_duration_ms");
    if (streaming["max_ahead_segments"])
        c.streaming.max_ahead_segments = streaming["max_ahead_segments"].as<size_t>();
    if (streaming["segment_memory_bytes"])
        c.streaming.segment_memory_bytes = yaml_size(streaming["segment_memory_bytes"]);
    if (streaming["probe_bytes"])
        c.streaming.probe_bytes = yaml_size(streaming["probe_bytes"]);
    if (streaming["probe_analyze_duration_ms"])
        c.streaming.probe_analyze_duration =
            milliseconds(streaming["probe_analyze_duration_ms"], "streaming.probe_analyze_duration_ms");
    if (streaming["probe_timeout_ms"])
        c.streaming.probe_timeout =
            milliseconds(streaming["probe_timeout_ms"], "streaming.probe_timeout_ms");
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
    validate(c);
    return c;
}

Config load_yaml_config(const std::filesystem::path& path) {
    auto root = YAML::LoadFile(path.string());
    if (!root.IsMap())
        throw std::runtime_error("configuration root must be a mapping");

    if (root["verbose"])
        throw std::runtime_error("obsolete configuration key: verbose");
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
    if (root["mount_path"])
        c.mount_path = root["mount_path"].as<std::string>();
    if (root["log_level"])
        c.log_level = parse_log_level(root["log_level"].as<std::string>());

    auto storage = root["storage"];
    if (!storage || !storage.IsSequence())
        throw std::runtime_error("storage must be a sequence of backends");
    for (const auto& item : storage) {
        StorageBackendConfig backend;
        backend.path = item["path"].as<std::string>();
        backend.limit = yaml_size(item["limit"]);
        c.storage_backends.push_back(std::move(backend));
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
    parse_catalogue(root, c);
    parse_streaming(root, c);
    parse_hydration(root, c);

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
        << "--metadata-cache MS  --replicas N  --metadata-replicas N  --extent-size SIZE\n"
        << "--read-ahead N  --mount PATH  --state-path PATH  --cache-path PATH --cache-blocks N\n"
        << "--log-level LEVEL  (ALL|DEBUG|INFO|WARN|ERROR; default INFO)  --help\n";
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
        } else if (option == "--cache-path") {
            config.cache.path = need(i, "--cache-path");
        } else if (option == "--cache-blocks") {
            config.cache.max_blocks = parse_unsigned(need(i, "--cache-blocks"), "cache blocks");
        } else if (option == "--mount") {
            config.mount_path = need(i, "--mount");
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
        } else if (option == "--metadata-replicas") {
            config.metadata_replication =
                parse_unsigned(need(i, "--metadata-replicas"), "metadata replica count");
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
