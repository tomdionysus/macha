// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/telemetry.hpp"

#include <algorithm>
#include <stdexcept>

namespace macha {

std::string_view node_phase_name(NodePhase phase) {
    switch (phase) {
    case NodePhase::starting: return "starting";
    case NodePhase::recovering: return "recovering";
    case NodePhase::ready: return "ready";
    }
    return "unknown";
}

std::optional<ReleaseVersion> parse_release_version(std::string_view text) {
    unsigned parts[3]{};
    size_t at = 0;
    for (auto& part : parts) {
        const size_t start = at;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            if (part > 100000)
                return {};
            part = part * 10 + static_cast<unsigned>(text[at] - '0');
            ++at;
        }
        if (at == start)
            return {};
        if (&part != &parts[2]) {
            if (at >= text.size() || text[at] != '.')
                return {};
            ++at;
        }
    }
    return ReleaseVersion{parts[0], parts[1], parts[2]};
}

bool runs_at_least(const std::vector<NodeTelemetry>& telemetry, const NodeId& peer,
                   ReleaseVersion first) {
    const auto seen = std::find_if(telemetry.begin(), telemetry.end(),
                                   [&](const NodeTelemetry& item) { return item.node_id == peer; });
    if (seen == telemetry.end())
        return false;
    const auto version = parse_release_version(seen->version);
    return version && *version >= first;
}

TelemetryStore::TelemetryStore(std::shared_ptr<TelemetrySource> source)
    : source_(std::move(source)) {
    if (!source_)
        throw std::invalid_argument("telemetry needs the node's records to read");
}

std::optional<NodeTelemetry> TelemetryStore::local() const {
    return source_->local();
}

std::vector<NodeTelemetry> TelemetryStore::all() const {
    return source_->all();
}

std::vector<NodeTelemetry> TelemetryStore::persisted() const {
    return source_->persisted();
}

void TelemetryStore::persist() {
    source_->persist();
}

std::vector<TelemetryView> TelemetryStore::views(std::chrono::milliseconds fresh_for) const {
    return source_->views(fresh_for);
}

} // namespace macha
