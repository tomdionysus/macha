// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry.hpp"

#include "codec.hpp"
#include "crypto.hpp"
#include "durable_file.hpp"
#include "log.hpp"

#include <algorithm>
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
constexpr std::array<uint8_t, 8> magic{'M', 'A', 'C', 'H', 'T', 'E', 'L', '1'};
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

void encode(Writer& writer, const NodeTelemetry& value) {
    writer.fixed(value.node_id.bytes);
    writer.fixed(value.boot_id.bytes);
    writer.u64(value.sequence);
    writer.u64(value.observed_unix_ms);
    writer.string(value.version);
    writer.string(value.host);
    writer.string(value.failure_domain);
    writer.u16(value.port);
    writer.u64(value.storage_capacity);
    writer.u64(value.storage_used);
    writer.u64(value.cache_capacity);
    writer.u64(value.cache_used);
    writer.u64(value.metadata_generation);
    writer.u64(value.uptime_ms);
    writer.u64(value.rss_bytes);
    writer.u32(value.process_cpu_milli_percent);
    writer.u32(value.load1_milli);
    writer.u32(value.storage_backends_online);
    writer.u32(value.peers_known);
    writer.u32(value.peers_active);
    writer.u64(value.rpc_connections_created);
    writer.u64(value.rpc_connections_reused);
    writer.u64(value.rpc_connections_canonical);
    writer.u8(static_cast<uint8_t>(value.phase));
    writer.string(value.api_endpoint);
    writer.u32(value.cpu_cores);
    writer.u64(value.memory_total_bytes);
}

NodeTelemetry decode(Reader& reader) {
    NodeTelemetry value;
    value.node_id.bytes = reader.fixed<16>();
    value.boot_id.bytes = reader.fixed<16>();
    value.sequence = reader.u64();
    value.observed_unix_ms = reader.u64();
    value.version = reader.string(256);
    value.host = reader.string(4096);
    value.failure_domain = reader.string(4096);
    value.port = reader.u16();
    value.storage_capacity = reader.u64();
    value.storage_used = reader.u64();
    value.cache_capacity = reader.u64();
    value.cache_used = reader.u64();
    value.metadata_generation = reader.u64();
    value.uptime_ms = reader.u64();
    value.rss_bytes = reader.u64();
    value.process_cpu_milli_percent = reader.u32();
    value.load1_milli = reader.u32();
    value.storage_backends_online = reader.u32();
    value.peers_known = reader.u32();
    value.peers_active = reader.u32();
    value.rpc_connections_created = reader.u64();
    value.rpc_connections_reused = reader.u64();
    value.rpc_connections_canonical = reader.u64();
    // Optional trailing field: a record encoded before this field existed
    // simply ends here, and is treated as "ready" (NodeTelemetry's default)
    // rather than perpetually "recovering".
    if (reader.remaining()) {
        const auto phase = reader.u8();
        if (phase > static_cast<uint8_t>(NodePhase::ready))
            throw DecodeError("invalid telemetry node phase");
        value.phase = static_cast<NodePhase>(phase);
    }
    // Optional trailing field: a record encoded before the API endpoint
    // existed simply ends here, and the sender is treated as not reporting
    // one (NodeTelemetry's default).
    //
    // A record from a node that predates the endpoint replacing the old
    // host/port pair puts a bare hostname here. That is not an endpoint and
    // must not be treated as one -- a client concatenating a scheme onto it
    // is the guessing this field exists to remove -- so anything without a
    // scheme is read as "not reported".
    if (reader.remaining()) {
        value.api_endpoint = reader.string(512);
        if (value.api_endpoint.find("://") == std::string::npos)
            value.api_endpoint.clear();
    }
    // Optional trailing field: a record encoded before cpu_cores existed ends
    // here and reports no core count, which is the honest answer for a peer
    // that cannot tell us. During a rolling upgrade every node is briefly in
    // that position.
    if (reader.remaining())
        value.cpu_cores = reader.u32();
    // Optional trailing field: a record encoded before physical memory existed
    // ends here and reports none, which a consumer renders as unknown.
    if (reader.remaining())
        value.memory_total_bytes = reader.u64();
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
    for (uint32_t i = 0; i < count; ++i)
        values.push_back(decode(reader));
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
    uint64_t rpc_connections_canonical, NodePhase phase, std::string api_endpoint) {
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
