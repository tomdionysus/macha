// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry.hpp"

#include "codec.hpp"
#include "crypto.hpp"
#include "durable_file.hpp"
#include "log.hpp"

#include <algorithm>
#include <limits>
#include <thread>
#include <ctime>
#include <fstream>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace macha {
namespace {
// TEL3: the tagged, length-delimited format introduced in 0.48.0. The magic
// changes with the format, so a node speaking the old positional one rejects
// the set outright -- "bad telemetry set" -- rather than misreading it. There
// is deliberately no compatibility with TEL1 or TEL2: every node moves at
// once, and a half-understood record is worse than a refused one.
constexpr std::array<uint8_t, 8> magic{'M', 'A', 'C', 'H', 'T', 'E', 'L', '3'};
constexpr size_t max_persisted_records = 1024;

uint64_t resident_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return static_cast<uint64_t>(info.resident_size);
#elif defined(__linux__)
    std::ifstream stream("/proc/self/statm");
    uint64_t virtual_pages = 0;
    uint64_t resident_pages = 0;
    if (!(stream >> virtual_pages >> resident_pages))
        return 0;
    const auto page_size = sysconf(_SC_PAGESIZE);
    return page_size > 0 ? resident_pages * static_cast<uint64_t>(page_size) : 0;
#else
    return 0;
#endif
}

uint64_t physical_memory_bytes() {
#if defined(__APPLE__)
    uint64_t bytes = 0;
    size_t size = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) != 0)
        return 0;
    return bytes;
#elif defined(__linux__)
    const auto pages = sysconf(_SC_PHYS_PAGES);
    const auto page_size = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page_size <= 0)
        return 0;
    return static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
#else
    return 0;
#endif
}

uint32_t load1_milli() {
    double load = 0.0;
    if (getloadavg(&load, 1) != 1 || load < 0.0)
        return 0;
    return static_cast<uint32_t>(std::min<double>(load * 1000.0, UINT32_MAX));
}

// A telemetry record is a sequence of tagged, length-delimited fields inside
// a length-delimited record. Nothing about it is positional.
//
// It was positional until 0.48.0: fields were appended in order, optional
// trailing ones were detected by asking the reader whether any bytes were
// left, and adding a field meant another level of nested "if there is more".
// That works between peers of one version and fails across two, which is the
// only time it matters. A newer sender's record carries bytes an older reader
// does not know to consume, so in a SET -- up to 64 records on the gossip
// path -- the older reader begins the next record part-way through the
// previous one and every record after it is garbage. One added field cost a
// mixed-version cluster every multi-node telemetry set it exchanged, for as
// long as the versions differed.
//
// With tags and lengths: an unknown field is skipped by its own length, a
// missing field keeps its default and means "this node did not say", and the
// record length says where the next record starts regardless of what either
// side understood. Fields may be added, and versions may differ, without a
// flag day.
enum TelemetryFieldId : uint16_t {
    field_node_id = 1,
    field_boot_id = 2,
    field_sequence = 3,
    field_observed_unix_ms = 4,
    field_version = 5,
    field_host = 6,
    field_failure_domain = 7,
    field_port = 8,
    field_storage_capacity = 9,
    field_storage_used = 10,
    field_cache_capacity = 11,
    field_cache_used = 12,
    field_metadata_generation = 13,
    field_uptime_ms = 14,
    field_rss_bytes = 15,
    field_process_cpu_milli_percent = 16,
    field_load1_milli = 17,
    field_storage_backends_online = 18,
    field_peers_known = 19,
    field_peers_active = 20,
    field_rpc_connections_created = 21,
    field_rpc_connections_reused = 22,
    field_rpc_connections_canonical = 23,
    field_phase = 24,
    field_api_endpoint = 25,
    field_cpu_cores = 26,
    field_memory_total_bytes = 27,
    field_playback_startup_timeout_ms = 28,
    field_playback_segment_timeout_ms = 29,
    field_playback_pipeline_idle_ms = 30,
    field_playback_session_idle_ms = 31,
    field_playback_max_sessions_per_account = 32,
};

void put_field(Writer& writer, uint16_t id, std::span<const uint8_t> value) {
    if (value.size() > std::numeric_limits<uint16_t>::max())
        throw std::runtime_error("telemetry field too large");
    writer.u16(id);
    writer.u16(static_cast<uint16_t>(value.size()));
    writer.raw(value);
}

// Absence already means "this node did not say", and a default-valued field
// says nothing a decoder would not have assumed -- every record decodes into a
// freshly defaulted struct, never merged into a previous one. So a zero or an
// empty string is simply left out. On a node with streaming disabled, an edge
// node holding no extents, or one with an empty cache, that is most of the
// record; at up to 64 records a set, it is the difference between a gossip
// message and a large one. Nothing is lost: a reader cannot distinguish an
// omitted zero from a transmitted one, because they mean the same thing.
template <typename T> void put_uint(Writer& writer, uint16_t id, T value) {
    if (value == T{})
        return;
    Writer body;
    if constexpr (sizeof(T) == 8)
        body.u64(static_cast<uint64_t>(value));
    else if constexpr (sizeof(T) == 4)
        body.u32(static_cast<uint32_t>(value));
    else if constexpr (sizeof(T) == 2)
        body.u16(static_cast<uint16_t>(value));
    else
        body.u8(static_cast<uint8_t>(value));
    put_field(writer, id, body.data());
}

void put_string(Writer& writer, uint16_t id, const std::string& value) {
    if (value.empty())
        return;
    put_field(writer, id,
              {reinterpret_cast<const uint8_t*>(value.data()), value.size()});
}

void encode(Writer& writer, const NodeTelemetry& value) {
    Writer body;
    put_field(body, field_node_id, value.node_id.bytes);
    put_field(body, field_boot_id, value.boot_id.bytes);
    put_uint(body, field_sequence, value.sequence);
    put_uint(body, field_observed_unix_ms, value.observed_unix_ms);
    put_string(body, field_version, value.version);
    put_string(body, field_host, value.host);
    put_string(body, field_failure_domain, value.failure_domain);
    put_uint(body, field_port, value.port);
    put_uint(body, field_storage_capacity, value.storage_capacity);
    put_uint(body, field_storage_used, value.storage_used);
    put_uint(body, field_cache_capacity, value.cache_capacity);
    put_uint(body, field_cache_used, value.cache_used);
    put_uint(body, field_metadata_generation, value.metadata_generation);
    put_uint(body, field_uptime_ms, value.uptime_ms);
    put_uint(body, field_rss_bytes, value.rss_bytes);
    put_uint(body, field_process_cpu_milli_percent, value.process_cpu_milli_percent);
    put_uint(body, field_load1_milli, value.load1_milli);
    put_uint(body, field_storage_backends_online, value.storage_backends_online);
    put_uint(body, field_peers_known, value.peers_known);
    put_uint(body, field_peers_active, value.peers_active);
    put_uint(body, field_rpc_connections_created, value.rpc_connections_created);
    put_uint(body, field_rpc_connections_reused, value.rpc_connections_reused);
    put_uint(body, field_rpc_connections_canonical, value.rpc_connections_canonical);
    // Always stated even at its default: an omitted phase reads as `ready`,
    // which is an assertion about the node rather than an absence of one.
    put_field(body, field_phase, std::array<uint8_t, 1>{static_cast<uint8_t>(value.phase)});
    put_string(body, field_api_endpoint, value.api_endpoint);
    put_uint(body, field_cpu_cores, value.cpu_cores);
    put_uint(body, field_memory_total_bytes, value.memory_total_bytes);
    put_uint(body, field_playback_startup_timeout_ms, value.playback_startup_timeout_ms);
    put_uint(body, field_playback_segment_timeout_ms, value.playback_segment_timeout_ms);
    put_uint(body, field_playback_pipeline_idle_ms, value.playback_pipeline_idle_ms);
    put_uint(body, field_playback_session_idle_ms, value.playback_session_idle_ms);
    put_uint(body, field_playback_max_sessions_per_account,
             value.playback_max_sessions_per_account);

    // The record's own length, so a reader that understood none of the above
    // still knows exactly where the next record begins.
    const auto& encoded = body.data();
    writer.u32(static_cast<uint32_t>(encoded.size()));
    writer.raw(encoded);
}

uint64_t field_uint(const Bytes& value, size_t width, const char* what) {
    if (value.size() != width)
        throw DecodeError(std::string("telemetry field ") + what + " has the wrong width");
    uint64_t out = 0;
    for (auto byte : value)
        out = (out << 8) | byte;
    return out;
}

NodeTelemetry decode(Reader& reader) {
    const auto length = reader.u32();
    auto body = reader.raw(length);
    Reader fields(body);
    NodeTelemetry value;
    while (fields.remaining()) {
        const auto id = fields.u16();
        const auto size = fields.u16();
        auto payload = fields.raw(size);
        switch (id) {
        case field_node_id:
            if (payload.size() != value.node_id.bytes.size())
                throw DecodeError("telemetry node_id has the wrong width");
            std::copy(payload.begin(), payload.end(), value.node_id.bytes.begin());
            break;
        case field_boot_id:
            if (payload.size() != value.boot_id.bytes.size())
                throw DecodeError("telemetry boot_id has the wrong width");
            std::copy(payload.begin(), payload.end(), value.boot_id.bytes.begin());
            break;
        case field_sequence: value.sequence = field_uint(payload, 8, "sequence"); break;
        case field_observed_unix_ms:
            value.observed_unix_ms = field_uint(payload, 8, "observed_unix_ms");
            break;
        case field_version:
            value.version.assign(payload.begin(), payload.end());
            break;
        case field_host: value.host.assign(payload.begin(), payload.end()); break;
        case field_failure_domain:
            value.failure_domain.assign(payload.begin(), payload.end());
            break;
        case field_port:
            value.port = static_cast<uint16_t>(field_uint(payload, 2, "port"));
            break;
        case field_storage_capacity:
            value.storage_capacity = field_uint(payload, 8, "storage_capacity");
            break;
        case field_storage_used:
            value.storage_used = field_uint(payload, 8, "storage_used");
            break;
        case field_cache_capacity:
            value.cache_capacity = field_uint(payload, 8, "cache_capacity");
            break;
        case field_cache_used: value.cache_used = field_uint(payload, 8, "cache_used"); break;
        case field_metadata_generation:
            value.metadata_generation = field_uint(payload, 8, "metadata_generation");
            break;
        case field_uptime_ms: value.uptime_ms = field_uint(payload, 8, "uptime_ms"); break;
        case field_rss_bytes: value.rss_bytes = field_uint(payload, 8, "rss_bytes"); break;
        case field_process_cpu_milli_percent:
            value.process_cpu_milli_percent =
                static_cast<uint32_t>(field_uint(payload, 4, "process_cpu_milli_percent"));
            break;
        case field_load1_milli:
            value.load1_milli = static_cast<uint32_t>(field_uint(payload, 4, "load1_milli"));
            break;
        case field_storage_backends_online:
            value.storage_backends_online =
                static_cast<uint32_t>(field_uint(payload, 4, "storage_backends_online"));
            break;
        case field_peers_known:
            value.peers_known = static_cast<uint32_t>(field_uint(payload, 4, "peers_known"));
            break;
        case field_peers_active:
            value.peers_active = static_cast<uint32_t>(field_uint(payload, 4, "peers_active"));
            break;
        case field_rpc_connections_created:
            value.rpc_connections_created = field_uint(payload, 8, "rpc_connections_created");
            break;
        case field_rpc_connections_reused:
            value.rpc_connections_reused = field_uint(payload, 8, "rpc_connections_reused");
            break;
        case field_rpc_connections_canonical:
            value.rpc_connections_canonical =
                field_uint(payload, 8, "rpc_connections_canonical");
            break;
        case field_phase:
        {
            const auto phase = static_cast<uint8_t>(field_uint(payload, 1, "phase"));
            if (phase > static_cast<uint8_t>(NodePhase::ready))
                throw DecodeError("invalid telemetry node phase");
            value.phase = static_cast<NodePhase>(phase);
        }
            break;
        case field_api_endpoint:
            value.api_endpoint.assign(payload.begin(), payload.end());
            break;
        case field_cpu_cores:
            value.cpu_cores = static_cast<uint32_t>(field_uint(payload, 4, "cpu_cores"));
            break;
        case field_memory_total_bytes:
            value.memory_total_bytes = field_uint(payload, 8, "memory_total_bytes");
            break;
        case field_playback_startup_timeout_ms:
            value.playback_startup_timeout_ms =
                static_cast<uint32_t>(field_uint(payload, 4, "playback_startup_timeout_ms"));
            break;
        case field_playback_segment_timeout_ms:
            value.playback_segment_timeout_ms =
                static_cast<uint32_t>(field_uint(payload, 4, "playback_segment_timeout_ms"));
            break;
        case field_playback_pipeline_idle_ms:
            value.playback_pipeline_idle_ms =
                static_cast<uint32_t>(field_uint(payload, 4, "playback_pipeline_idle_ms"));
            break;
        case field_playback_session_idle_ms:
            value.playback_session_idle_ms =
                static_cast<uint32_t>(field_uint(payload, 4, "playback_session_idle_ms"));
            break;
        case field_playback_max_sessions_per_account:
            value.playback_max_sessions_per_account = static_cast<uint32_t>(
                field_uint(payload, 4, "playback_max_sessions_per_account"));
            break;
        default:
            // A field this build does not know. Skipped by its own length,
            // which is the entire point.
            break;
        }
    }
    if (!value.sequence)
        throw DecodeError("telemetry sequence must be nonzero");
    return value;
}

} // namespace

std::string_view node_phase_name(NodePhase phase) {
    switch (phase) {
    case NodePhase::starting: return "starting";
    case NodePhase::recovering: return "recovering";
    case NodePhase::ready: return "ready";
    }
    return "unknown";
}

Bytes encode_node_telemetry(const NodeTelemetry& value) {
    Writer writer;
    writer.raw(magic);
    encode(writer, value);
    return writer.take();
}

NodeTelemetry decode_node_telemetry(std::span<const uint8_t> bytes) {
    Reader reader(bytes);
    auto got = reader.raw(magic.size());
    if (!std::equal(got.begin(), got.end(), magic.begin()))
        throw DecodeError("bad telemetry payload");
    auto value = decode(reader);
    reader.finish();
    return value;
}

Bytes encode_telemetry_set(const std::vector<NodeTelemetry>& values) {
    Writer writer;
    writer.raw(magic);
    writer.u32(static_cast<uint32_t>(values.size()));
    // Every record states its own length, so the set needs no second one.
    for (const auto& value : values)
        encode(writer, value);
    return writer.take();
}

std::vector<NodeTelemetry> decode_telemetry_set(std::span<const uint8_t> bytes) {
    Reader reader(bytes);
    auto got = reader.raw(magic.size());
    if (!std::equal(got.begin(), got.end(), magic.begin()))
        throw DecodeError("bad telemetry set");
    const auto count = reader.u32();
    if (count > 65536)
        throw DecodeError("too many telemetry records");
    std::vector<NodeTelemetry> values;
    values.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        values.push_back(decode(reader));
    }
    reader.finish();
    return values;
}

TelemetryStore::TelemetryStore(NodeId self, std::filesystem::path persisted_path)
    : self_(self), boot_id_(random_node_id()), persisted_path_(std::move(persisted_path)) {
    if (persisted_path_.empty() || !std::filesystem::exists(persisted_path_))
        return;
    try {
        const auto size = std::filesystem::file_size(persisted_path_);
        constexpr uint64_t max_persisted_bytes = 16ULL * 1024 * 1024;
        if (size > max_persisted_bytes)
            throw std::runtime_error("persisted telemetry is too large");
        std::ifstream input(persisted_path_, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open persisted telemetry");
        Bytes bytes(static_cast<size_t>(size));
        if (size && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
            throw std::runtime_error("cannot read persisted telemetry");
        auto values = decode_telemetry_set(bytes);
        if (values.size() > max_persisted_records)
            throw std::runtime_error("persisted telemetry has too many records");
        for (auto& telemetry : values) {
            auto found = persisted_.find(telemetry.node_id);
            if (found == persisted_.end() || found->second.observed_unix_ms < telemetry.observed_unix_ms)
                persisted_[telemetry.node_id] = std::move(telemetry);
        }
    } catch (const std::exception& error) {
        // Telemetry is observational state only. Corruption or an incompatible
        // cache must never prevent the node from starting.
        Log::warn("persisted telemetry ignored: " + std::string(error.what()));
        persisted_.clear();
    }
}

NodeTelemetry TelemetryStore::refresh_local(
    const NodeInfo& info, std::string version, uint64_t cache_capacity, uint64_t cache_used,
    uint32_t storage_backends_online, uint32_t peers_known, uint32_t peers_active,
    uint64_t rpc_connections_created, uint64_t rpc_connections_reused,
    uint64_t rpc_connections_canonical, NodePhase phase, std::string api_endpoint,
    PlaybackBudgets playback) {
    const auto now = Clock::now();
    const auto cpu_now = std::clock();
    const auto wall_seconds = std::chrono::duration<double>(now - previous_cpu_wall_).count();
    const auto cpu_seconds = static_cast<double>(cpu_now - previous_cpu_) / CLOCKS_PER_SEC;
    previous_cpu_wall_ = now;
    previous_cpu_ = cpu_now;

    NodeTelemetry telemetry;
    telemetry.node_id = self_;
    telemetry.boot_id = boot_id_;
    telemetry.sequence = ++sequence_;
    telemetry.observed_unix_ms = unix_ms();
    telemetry.version = std::move(version);
    telemetry.host = info.host;
    telemetry.failure_domain = info.failure_domain;
    telemetry.port = info.port;
    telemetry.storage_capacity = info.capacity;
    telemetry.storage_used = info.used;
    telemetry.cache_capacity = cache_capacity;
    telemetry.cache_used = cache_used;
    telemetry.metadata_generation = info.metadata_generation;
    telemetry.uptime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - started_).count());
    telemetry.rss_bytes = resident_bytes();
    const auto cpu_percent = wall_seconds > 0.0 ? std::max(0.0, cpu_seconds / wall_seconds * 100.0) : 0.0;
    telemetry.process_cpu_milli_percent =
        static_cast<uint32_t>(std::min<double>(cpu_percent * 1000.0, UINT32_MAX));
    telemetry.load1_milli = load1_milli();
    telemetry.storage_backends_online = storage_backends_online;
    telemetry.peers_known = peers_known;
    telemetry.peers_active = peers_active;
    telemetry.rpc_connections_created = rpc_connections_created;
    telemetry.rpc_connections_reused = rpc_connections_reused;
    telemetry.rpc_connections_canonical = rpc_connections_canonical;
    telemetry.phase = phase;
    telemetry.api_endpoint = std::move(api_endpoint);
    telemetry.cpu_cores = std::thread::hardware_concurrency();
    telemetry.memory_total_bytes = physical_memory_bytes();
    telemetry.playback_startup_timeout_ms = playback.startup_timeout_ms;
    telemetry.playback_segment_timeout_ms = playback.segment_timeout_ms;
    telemetry.playback_pipeline_idle_ms = playback.pipeline_idle_ms;
    telemetry.playback_session_idle_ms = playback.session_idle_ms;
    telemetry.playback_max_sessions_per_account = playback.max_sessions_per_account;
    observe(telemetry, true);
    return telemetry;
}

void TelemetryStore::observe(NodeTelemetry telemetry, bool direct) {
    if (telemetry.node_id == NodeId{} || !telemetry.sequence)
        return;
    std::lock_guard lock(mutex_);
    for (const auto& [_, reset] : identity_resets_) {
        if (!identity_reset_matches_endpoint(reset, telemetry.host, telemetry.port) ||
            !identity_reset_matches_node(reset, telemetry.node_id))
            continue;
        if (!direct && telemetry.observed_unix_ms <= reset.reset_unix_ms)
            return;
    }
    auto found = records_.find(telemetry.node_id);
    if (found != records_.end()) {
        const auto& current = found->second.telemetry;
        if (current.boot_id == telemetry.boot_id && current.sequence >= telemetry.sequence)
            return;
        if (current.boot_id != telemetry.boot_id &&
            current.observed_unix_ms > telemetry.observed_unix_ms + 60000)
            return;
    }
    records_[telemetry.node_id] = Record{std::move(telemetry), Clock::now()};
}


void TelemetryStore::apply_identity_reset(const IdentityAssociationReset& reset) {
    if (reset.host.empty() || !reset.epoch)
        return;
    std::lock_guard lock(mutex_);
    const auto key = identity_reset_key(reset.host, reset.port);
    auto existing = identity_resets_.find(key);
    if (existing != identity_resets_.end() && existing->second.epoch >= reset.epoch)
        return;
    identity_resets_[key] = reset;
    std::erase_if(records_, [&](const auto& item) {
        const auto& telemetry = item.second.telemetry;
        return identity_reset_matches_endpoint(reset, telemetry.host, telemetry.port) &&
               identity_reset_matches_node(reset, telemetry.node_id);
    });
}

std::optional<NodeTelemetry> TelemetryStore::local() const {
    std::lock_guard lock(mutex_);
    auto found = records_.find(self_);
    if (found == records_.end())
        return {};
    return found->second.telemetry;
}

std::vector<NodeTelemetry> TelemetryStore::all() const {
    std::lock_guard lock(mutex_);
    std::vector<NodeTelemetry> out;
    out.reserve(records_.size());
    for (const auto& [_, record] : records_)
        out.push_back(record.telemetry);
    return out;
}

std::vector<NodeTelemetry> TelemetryStore::recent(std::chrono::milliseconds max_age,
                                                     size_t max_records) const {
    std::lock_guard lock(mutex_);
    const auto now = Clock::now();
    std::vector<NodeTelemetry> out;
    out.reserve(std::min(records_.size(), max_records));
    for (const auto& [_, record] : records_) {
        if (now - record.received <= max_age)
            out.push_back(record.telemetry);
    }
    std::sort(out.begin(), out.end(), [](const NodeTelemetry& a, const NodeTelemetry& b) {
        return a.observed_unix_ms > b.observed_unix_ms;
    });
    if (out.size() > max_records)
        out.resize(max_records);
    return out;
}

std::vector<NodeTelemetry> TelemetryStore::persisted() const {
    std::lock_guard lock(mutex_);
    std::vector<NodeTelemetry> out;
    out.reserve(persisted_.size());
    for (const auto& [_, telemetry] : persisted_)
        out.push_back(telemetry);
    return out;
}

void TelemetryStore::persist() {
    if (persisted_path_.empty())
        return;
    std::map<NodeId, NodeTelemetry> merged;
    {
        std::lock_guard lock(mutex_);
        merged = persisted_;
        for (const auto& [node, record] : records_) {
            auto found = merged.find(node);
            if (found == merged.end() || found->second.observed_unix_ms < record.telemetry.observed_unix_ms)
                merged[node] = record.telemetry;
        }
    }

    std::vector<NodeTelemetry> values;
    values.reserve(merged.size());
    for (const auto& [_, telemetry] : merged)
        values.push_back(telemetry);
    std::sort(values.begin(), values.end(), [](const NodeTelemetry& a, const NodeTelemetry& b) {
        return a.observed_unix_ms > b.observed_unix_ms;
    });
    if (values.size() > max_persisted_records)
        values.resize(max_persisted_records);
    const auto encoded = encode_telemetry_set(values);
    const auto contents = std::string_view(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    durable_replace_file(persisted_path_, contents);

    std::lock_guard lock(mutex_);
    // Mirror exactly the bounded set just made durable. This keeps the cache
    // bounded even across years of node replacements.
    persisted_.clear();
    for (auto& telemetry : values) {
        const auto node = telemetry.node_id;
        auto found = persisted_.find(node);
        if (found == persisted_.end() || found->second.observed_unix_ms < telemetry.observed_unix_ms)
            persisted_[node] = std::move(telemetry);
    }
}

std::vector<TelemetryView> TelemetryStore::views(std::chrono::milliseconds fresh_for) const {
    std::lock_guard lock(mutex_);
    const auto now = Clock::now();
    std::vector<TelemetryView> out;
    out.reserve(records_.size());
    for (const auto& [_, record] : records_) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - record.received);
        out.push_back({record.telemetry, age, age <= fresh_for});
    }
    return out;
}

} // namespace macha
