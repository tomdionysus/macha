// SPDX-License-Identifier: GPL-3.0-or-later
#include "torrent/torrent_request.hpp"

#include <algorithm>
#include <tuple>
#include <vector>

namespace macha {
namespace {

constexpr uint8_t request_format = 1;

void write_node(Writer& w, const NodeId& node) { w.fixed(node.bytes); }

NodeId read_node(Reader& r) {
    NodeId node;
    node.bytes = r.fixed<sizeof(node.bytes)>();
    return node;
}

void write_optional_u64(Writer& w, const std::optional<uint64_t>& value) {
    w.u8(value ? 1 : 0);
    if (value) w.u64(*value);
}

std::optional<uint64_t> read_optional_u64(Reader& r) {
    const auto present = r.u8();
    if (present > 1) throw DecodeError("bad torrent request optional");
    if (!present) return std::nullopt;
    return r.u64();
}

// The request whose key is greater; on equal keys, the greater request, so
// the choice never depends on argument order.
// Returns the address, not a reference: GCC's dangling-reference analysis
// cannot see that the keys are not what is returned.
template <typename Key>
const TorrentRequest* later(const TorrentRequest& a, Key ka, const TorrentRequest& b, Key kb) {
    if (ka != kb) return ka > kb ? &a : &b;
    return a >= b ? &a : &b;
}

// Higher epoch; then the earlier claim; then the lower node id.
bool claim_beats(const TorrentClaim& x, const TorrentClaim& y) {
    if (x.epoch != y.epoch) return x.epoch > y.epoch;
    if (x.claimed_unix_ms != y.claimed_unix_ms) return x.claimed_unix_ms < y.claimed_unix_ms;
    return x.node_id < y.node_id;
}

bool live(const TorrentRequest& request) {
    return !request.removed_unix_ms && request.phase != TorrentPhase::cancelled &&
           request.phase != TorrentPhase::failed && request.desired != TorrentDesired::cancelled;
}

} // namespace

std::string_view torrent_phase_name(TorrentPhase phase) noexcept {
    switch (phase) {
    case TorrentPhase::awaiting_node: return "awaiting_node";
    case TorrentPhase::downloading: return "downloading";
    case TorrentPhase::importing: return "importing";
    case TorrentPhase::completed: return "completed";
    case TorrentPhase::failed: return "failed";
    case TorrentPhase::cancelled: return "cancelled";
    }
    return "failed";
}

std::optional<TorrentPhase> parse_torrent_phase(std::string_view name) {
    for (auto phase : {TorrentPhase::awaiting_node, TorrentPhase::downloading, TorrentPhase::importing,
                       TorrentPhase::completed, TorrentPhase::failed, TorrentPhase::cancelled})
        if (torrent_phase_name(phase) == name) return phase;
    return std::nullopt;
}

bool torrent_phase_terminal(TorrentPhase phase) noexcept {
    return phase == TorrentPhase::completed || phase == TorrentPhase::failed ||
           phase == TorrentPhase::cancelled;
}

std::string_view torrent_desired_name(TorrentDesired desired) noexcept {
    switch (desired) {
    case TorrentDesired::active: return "active";
    case TorrentDesired::paused: return "paused";
    case TorrentDesired::cancelled: return "cancelled";
    }
    return "cancelled";
}

void encode_torrent_request(Writer& w, const TorrentRequest& r) {
    w.u8(request_format);
    w.string(r.id);
    w.string(r.info_hash);
    w.string(r.source);
    w.u64(r.created_unix_ms);
    write_node(w, r.created_by);
    w.u8(r.pinned_node_id ? 1 : 0);
    if (r.pinned_node_id) write_node(w, *r.pinned_node_id);
    write_optional_u64(w, r.remove_after_ms);
    w.u64(r.settings_changed_unix_ms);
    write_node(w, r.settings_changed_by);
    w.u8(static_cast<uint8_t>(r.desired));
    w.u64(r.desired_changed_unix_ms);
    write_node(w, r.desired_changed_by);
    w.u8(r.claim ? 1 : 0);
    if (r.claim) {
        write_node(w, r.claim->node_id);
        w.u64(r.claim->epoch);
        w.u64(r.claim->claimed_unix_ms);
    }
    w.u8(static_cast<uint8_t>(r.phase));
    w.u64(r.phase_epoch);
    w.u64(r.progress_unix_ms);
    w.string(r.name);
    w.u64(r.bytes_total);
    w.string(r.ingest_job_id);
    w.string(r.error_code);
    w.string(r.error);
    w.u64(r.completed_unix_ms);
    w.u64(r.removed_unix_ms);
}

TorrentRequest decode_torrent_request(Reader& rd) {
    if (rd.u8() != request_format) throw DecodeError("unknown torrent request format");
    TorrentRequest r;
    r.id = rd.string(max_torrent_request_text);
    r.info_hash = rd.string(max_torrent_request_text);
    r.source = rd.string(max_torrent_request_text);
    r.created_unix_ms = rd.u64();
    r.created_by = read_node(rd);
    if (const auto pinned = rd.u8(); pinned > 1) throw DecodeError("bad torrent request pin");
    else if (pinned) r.pinned_node_id = read_node(rd);
    r.remove_after_ms = read_optional_u64(rd);
    r.settings_changed_unix_ms = rd.u64();
    r.settings_changed_by = read_node(rd);
    const auto desired = rd.u8();
    if (desired > static_cast<uint8_t>(TorrentDesired::cancelled)) throw DecodeError("bad torrent request desired");
    r.desired = static_cast<TorrentDesired>(desired);
    r.desired_changed_unix_ms = rd.u64();
    r.desired_changed_by = read_node(rd);
    if (const auto claimed = rd.u8(); claimed > 1) throw DecodeError("bad torrent request claim");
    else if (claimed) {
        TorrentClaim claim;
        claim.node_id = read_node(rd);
        claim.epoch = rd.u64();
        claim.claimed_unix_ms = rd.u64();
        r.claim = claim;
    }
    const auto phase = rd.u8();
    if (phase > static_cast<uint8_t>(TorrentPhase::cancelled)) throw DecodeError("bad torrent request phase");
    r.phase = static_cast<TorrentPhase>(phase);
    r.phase_epoch = rd.u64();
    r.progress_unix_ms = rd.u64();
    r.name = rd.string(max_torrent_request_text);
    r.bytes_total = rd.u64();
    r.ingest_job_id = rd.string(max_torrent_request_text);
    r.error_code = rd.string(max_torrent_request_text);
    r.error = rd.string(max_torrent_request_text);
    r.completed_unix_ms = rd.u64();
    r.removed_unix_ms = rd.u64();
    return r;
}

TorrentRequest merge_torrent_request(const TorrentRequest& a, const TorrentRequest& b) {
    // Immutable fields are the same on both; start from the greater request
    // so even a disagreement resolves the same way from either side.
    TorrentRequest out = a >= b ? a : b;

    const auto& settings = *later(a, std::tie(a.settings_changed_unix_ms, a.settings_changed_by), b,
                                 std::tie(b.settings_changed_unix_ms, b.settings_changed_by));
    out.pinned_node_id = settings.pinned_node_id;
    out.remove_after_ms = settings.remove_after_ms;
    out.settings_changed_unix_ms = settings.settings_changed_unix_ms;
    out.settings_changed_by = settings.settings_changed_by;

    // Cancel is final: it beats any later pause or resume.
    const auto cancelled_a = a.desired == TorrentDesired::cancelled;
    const auto cancelled_b = b.desired == TorrentDesired::cancelled;
    const auto& intent = *later(a, std::make_tuple(cancelled_a, a.desired_changed_unix_ms, a.desired_changed_by), b,
                               std::make_tuple(cancelled_b, b.desired_changed_unix_ms, b.desired_changed_by));
    out.desired = intent.desired;
    out.desired_changed_unix_ms = intent.desired_changed_unix_ms;
    out.desired_changed_by = intent.desired_changed_by;

    if (a.claim && b.claim)
        out.claim = claim_beats(*a.claim, *b.claim) ? a.claim : b.claim;
    else
        out.claim = a.claim ? a.claim : b.claim;

    const auto& owner = *later(a, std::make_tuple(a.phase_epoch, static_cast<uint8_t>(a.phase), a.progress_unix_ms), b,
                              std::make_tuple(b.phase_epoch, static_cast<uint8_t>(b.phase), b.progress_unix_ms));
    out.phase = owner.phase;
    out.phase_epoch = owner.phase_epoch;
    out.progress_unix_ms = owner.progress_unix_ms;
    out.name = owner.name;
    out.bytes_total = owner.bytes_total;
    out.ingest_job_id = owner.ingest_job_id;
    out.error_code = owner.error_code;
    out.error = owner.error;
    out.completed_unix_ms = owner.completed_unix_ms;

    out.removed_unix_ms = std::max(a.removed_unix_ms, b.removed_unix_ms);
    return out;
}

std::map<std::string, TorrentRequest, std::less<>>
merge_torrent_requests(const std::map<std::string, TorrentRequest, std::less<>>& base,
                       const std::map<std::string, TorrentRequest, std::less<>>& left,
                       const std::map<std::string, TorrentRequest, std::less<>>& right) {
    std::map<std::string, TorrentRequest, std::less<>> out;
    const auto keep_one_side = [&](const std::string& key, const TorrentRequest& present) {
        // Absent on the other side: new there, or erased there after its
        // grace. Erased stands unless this side changed it since the base.
        const auto in_base = base.find(key);
        if (in_base == base.end() || in_base->second != present) out.emplace(key, present);
    };
    for (const auto& [key, l] : left) {
        if (const auto r = right.find(key); r != right.end())
            out.emplace(key, merge_torrent_request(l, r->second));
        else
            keep_one_side(key, l);
    }
    for (const auto& [key, r] : right)
        if (!left.contains(key)) keep_one_side(key, r);

    // Two nodes took the same add at once: the earlier request keeps it.
    std::map<std::string, std::vector<TorrentRequest*>> by_hash;
    for (auto& [_, request] : out)
        if (live(request) && !request.info_hash.empty()) by_hash[request.info_hash].push_back(&request);
    for (auto& [_, group] : by_hash) {
        if (group.size() < 2) continue;
        std::sort(group.begin(), group.end(), [](const TorrentRequest* x, const TorrentRequest* y) {
            return std::tie(x->created_unix_ms, x->id) < std::tie(y->created_unix_ms, y->id);
        });
        for (size_t i = 1; i < group.size(); ++i) {
            auto& loser = *group[i];
            loser.desired = TorrentDesired::cancelled;
            loser.desired_changed_unix_ms = std::max(loser.desired_changed_unix_ms, group[0]->created_unix_ms);
            loser.desired_changed_by = NodeId{};
            loser.error_code = "duplicate_torrent";
            loser.error = "request " + group[0]->id + " holds the same torrent";
        }
    }
    return out;
}

} // namespace macha
