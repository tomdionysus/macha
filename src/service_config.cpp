// SPDX-License-Identifier: GPL-3.0-or-later
#include "service.hpp"

#include "ffmpeg_log.hpp"
#include "log.hpp"

namespace macha {

void Service::reload_config() {
    if (!node_.config().config_file) {
        Log::warn("configuration reload requested but this node was not started with --config");
        return;
    }
    auto updated = load_yaml_config(*node_.config().config_file);
    if (updated.state_path != node_.config().state_path || updated.key_file != node_.config().key_file)
        throw std::runtime_error("state_path/key_file cannot be changed by live reload");
    if (updated.fuse.spool_path != node_.config().fuse.spool_path ||
        updated.fuse.operation_journal_path != node_.config().fuse.operation_journal_path)
        throw std::runtime_error("FUSE spool/journal paths cannot be changed by live reload");
    if (updated.extent_size != node_.config().extent_size)
        throw std::runtime_error("extent_size cannot be changed for an existing namespace");
    if (updated.replication != node_.config().replication ||
        updated.metadata_min_write_replicas != node_.config().metadata_min_write_replicas)
        throw std::runtime_error("replica policy changes require a coordinated cluster restart");
    const auto& current_streaming = node_.config().streaming;
    const bool streaming_restart_required =
        updated.streaming.enabled != current_streaming.enabled ||
        updated.streaming.temp_path != current_streaming.temp_path ||
        updated.streaming.segment_memory_bytes != current_streaming.segment_memory_bytes ||
        updated.streaming.probe_bytes != current_streaming.probe_bytes ||
        updated.streaming.probe_analyze_duration != current_streaming.probe_analyze_duration ||
        updated.streaming.probe_timeout != current_streaming.probe_timeout;
    Log::set_logger(std::make_shared<ConsoleLogger>(updated.log_level));
    configure_ffmpeg_logging(updated.ffmpeg_log_level);
    node_.reconfigure_local(updated);
    scanner_.reconfigure(updated.catalogue.scanner);
    hydration_.reconfigure(updated.hydration, updated.read_ahead_extents);
    ingest_.reconfigure(updated.ingest);
    torrents_.reconfigure(updated.torrent);
    if (streaming_restart_required)
        Log::warn("streaming enable/buffer/probe/path changes require restart; live limits were reloaded");
    streaming_.reconfigure(updated.streaming);
    Log::info("reloaded storage backends, persistent cache, catalogue scanner, ingest, torrent, hydration and streaming limits");
}


} // namespace macha
