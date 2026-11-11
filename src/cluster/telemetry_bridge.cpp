// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/telemetry_bridge.hpp"

#include "cluster/dannan_bridge.hpp"
#include "codec.hpp"

#include <algorithm>

namespace macha {

namespace hf = hosted_field;
namespace df = dannan::status::field;

const std::vector<uint16_t>& hosted_telemetry_layout() {
    static const std::vector<uint16_t> layout = [] {
        std::vector<uint16_t> out;
        for (uint16_t id = hf::storage_capacity; id <= hf::playback_transcode_rates; ++id)
            out.push_back(id);
        return out;
    }();
    return layout;
}

void write_hosted_telemetry(dannan::status::Telemetry& record, const HostedTelemetry& hosted) {
    record.set_u64(hf::storage_capacity, hosted.storage_capacity);
    record.set_u64(hf::storage_used, hosted.storage_used);
    record.set_u64(hf::cache_capacity, hosted.cache_capacity);
    record.set_u64(hf::cache_used, hosted.cache_used);
    record.set_u64(hf::metadata_generation, hosted.metadata_generation);
    record.set_u32(hf::storage_backends_online, hosted.storage_backends_online);
    record.set_text(hf::api_endpoint, hosted.api_endpoint);
    const auto& p = hosted.playback;
    record.set_u32(hf::playback_startup_timeout_ms, p.startup_timeout_ms);
    record.set_u32(hf::playback_segment_timeout_ms, p.segment_timeout_ms);
    record.set_u32(hf::playback_pipeline_idle_ms, p.pipeline_idle_ms);
    record.set_u32(hf::playback_session_idle_ms, p.session_idle_ms);
    record.set_u32(hf::playback_max_sessions_per_account, p.max_sessions_per_account);
    record.set_u32(hf::playback_max_sessions, p.max_sessions);
    record.set_u32(hf::playback_transcode_entitlement_idle_ms, p.transcode_entitlement_idle_ms);
    record.set_u64(hf::cache_hits, hosted.cache.hits);
    record.set_u64(hf::cache_misses, hosted.cache.misses);
    record.set_u64(hf::cache_evictions, hosted.cache.evictions);
    record.set_u32(hf::playback_max_transcodes_per_account, p.max_transcodes_per_account);
    record.set_u32(hf::playback_startup_no_progress_ms, p.startup_no_progress_ms);
    record.set_u32(hf::playback_start_wait_max_ms, p.start_wait_max_ms);
    record.set_u32(hf::playback_start_failed_retention_ms, p.start_failed_retention_ms);
    if (p.transcode_rates.empty()) {
        // set_text removes a field for an empty value; Dannan 0.5.3's set_bytes keeps
        // an empty one.
        record.set_text(hf::playback_transcode_rates, {});
        return;
    }
    Writer rates;
    const auto count = std::min<size_t>(p.transcode_rates.size(), 256);
    rates.u16(static_cast<uint16_t>(count));
    for (size_t i = 0; i < count; ++i) {
        const auto& rate = p.transcode_rates[i];
        rates.string(rate.kind);
        rates.string(rate.codec);
        rates.u32(rate.bit_depth);
        rates.u32(rate.height_class);
        rates.u32(rate.rate_milli);
        rates.u32(rate.observations);
        rates.u32(rate.concurrent);
    }
    record.set_bytes(hf::playback_transcode_rates, rates.data());
}

namespace {

NodePhase macha_phase(dannan::status::Phase phase) {
    switch (phase) {
    case dannan::status::Phase::starting: return NodePhase::starting;
    case dannan::status::Phase::recovering: return NodePhase::recovering;
    case dannan::status::Phase::ready: return NodePhase::ready;
    }
    return NodePhase::starting;
}

std::vector<TranscodeRate> decode_rates(std::span<const uint8_t> bytes) {
    std::vector<TranscodeRate> out;
    Reader reader(bytes);
    const auto count = reader.u16();
    if (count > 256)
        throw DecodeError("too many transcode rates");
    for (uint16_t i = 0; i < count; ++i) {
        TranscodeRate rate;
        rate.kind = reader.string(16);
        rate.codec = reader.string(64);
        rate.bit_depth = reader.u32();
        rate.height_class = reader.u32();
        rate.rate_milli = reader.u32();
        rate.observations = reader.u32();
        rate.concurrent = reader.u32();
        out.push_back(std::move(rate));
    }
    reader.finish();
    return out;
}

} // namespace

std::optional<NodeTelemetry> node_telemetry_from(const dannan::status::Telemetry& record) {
    try {
        NodeTelemetry out;
        out.node_id = macha_id(record.node_id());
        out.boot_id = macha_id(record.boot_id());
        out.sequence = record.sequence();
        out.observed_unix_ms = record.observed_unix_ms();
        out.version = record.text(df::version);
        out.host = record.host();
        out.failure_domain = record.text(df::failure_domain);
        out.port = record.port();
        out.uptime_ms = record.u64(df::uptime_ms);
        out.rss_bytes = record.u64(df::rss_bytes);
        out.process_cpu_milli_percent = record.u32(df::process_cpu_milli_percent);
        out.load1_milli = record.u32(df::load1_milli);
        out.peers_known = record.u32(df::peers_known);
        out.peers_active = record.u32(df::peers_active);
        out.rpc_connections_created = record.u64(df::connections_created);
        out.rpc_connections_reused = record.u64(df::connections_reused);
        out.rpc_connections_canonical = record.u64(df::connections_canonical);
        out.phase = macha_phase(record.phase());
        out.cpu_cores = record.u32(df::cpu_cores);
        out.memory_total_bytes = record.u64(df::memory_total_bytes);
        out.node_name = record.text(df::node_name);
        out.traffic_window_ms = record.traffic_window_ms();
        for (const auto& entry : record.traffic())
            out.traffic.push_back(TrafficClass{entry.frame_class, entry.in_bytes, entry.out_bytes,
                                               entry.in_bytes_per_s, entry.out_bytes_per_s});

        out.storage_capacity = record.u64(hf::storage_capacity);
        out.storage_used = record.u64(hf::storage_used);
        out.cache_capacity = record.u64(hf::cache_capacity);
        out.cache_used = record.u64(hf::cache_used);
        out.metadata_generation = record.u64(hf::metadata_generation);
        out.storage_backends_online = record.u32(hf::storage_backends_online);
        out.api_endpoint = record.text(hf::api_endpoint);
        out.playback_startup_timeout_ms = record.u32(hf::playback_startup_timeout_ms);
        out.playback_segment_timeout_ms = record.u32(hf::playback_segment_timeout_ms);
        out.playback_pipeline_idle_ms = record.u32(hf::playback_pipeline_idle_ms);
        out.playback_session_idle_ms = record.u32(hf::playback_session_idle_ms);
        out.playback_max_sessions_per_account = record.u32(hf::playback_max_sessions_per_account);
        out.playback_max_sessions = record.u32(hf::playback_max_sessions);
        out.playback_transcode_entitlement_idle_ms =
            record.u32(hf::playback_transcode_entitlement_idle_ms);
        out.cache_hits = record.u64(hf::cache_hits);
        out.cache_misses = record.u64(hf::cache_misses);
        out.cache_evictions = record.u64(hf::cache_evictions);
        out.playback_max_transcodes_per_account =
            record.u32(hf::playback_max_transcodes_per_account);
        out.playback_startup_no_progress_ms = record.u32(hf::playback_startup_no_progress_ms);
        out.playback_start_wait_max_ms = record.u32(hf::playback_start_wait_max_ms);
        out.playback_start_failed_retention_ms = record.u32(hf::playback_start_failed_retention_ms);
        // An empty field says no rates, as an absent one does.
        if (const auto rates = record.get(hf::playback_transcode_rates); rates && !rates->empty())
            out.playback_transcode_rates = decode_rates(*rates);
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace macha
