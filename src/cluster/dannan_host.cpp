// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/dannan_host.hpp"

#include "cluster/dannan_bridge.hpp"
#include "cluster/telemetry_bridge.hpp"
#include "log.hpp"
#include "supervised.hpp"

#include <future>
#include <stdexcept>

namespace macha {

namespace {

// Requests reach Macha's routes as they always have: the handler bound for the type,
// or 501 when none is. Every answer is a reply, refusals included.
dannan::net::RequestHandler dispatch_to(MessageRoutes& routes) {
    return [&routes](const dannan::membership::NodeRecord& peer, dannan::sched::FrameType frame,
                     const dannan::net::Message& request) {
        dannan::net::Message copy;
        copy.type = request.type;
        copy.payload = request.payload;
        auto reply = routes.dispatch(node_info_from(peer), macha_frame(frame),
                                     macha_message(std::move(copy)));
        return dannan::net::Handled::processed(dannan_message(reply));
    };
}

} // namespace

MembershipSnapshot DannanMembership::peers() const {
    const auto roster = nodes_.snapshot();
    MembershipSnapshot out;
    for (const auto& record : roster.all)
        if (record.id != self_)
            out.all.push_back(node_info_from(record));
    for (const auto& record : roster.active)
        if (record.id != self_)
            out.active.push_back(node_info_from(record));
    return out;
}

bool DannanMembership::directly_reachable(const NodeId& id) const {
    return nodes_.directly_reachable(dannan_id(id));
}

bool DannanMembership::all_known_reachable() const {
    return nodes_.all_known_reachable();
}

namespace {
std::vector<NodeTelemetry> readable(const std::vector<dannan::status::Telemetry>& records) {
    std::vector<NodeTelemetry> out;
    out.reserve(records.size());
    for (const auto& record : records)
        if (auto telemetry = node_telemetry_from(record))
            out.push_back(std::move(*telemetry));
    return out;
}
} // namespace

std::optional<NodeTelemetry> DannanTelemetry::local() const {
    if (const auto record = telemetry_.local())
        return node_telemetry_from(*record);
    return std::nullopt;
}

std::vector<NodeTelemetry> DannanTelemetry::all() const {
    return readable(telemetry_.all());
}

std::vector<NodeTelemetry> DannanTelemetry::persisted() const {
    return readable(telemetry_.persisted());
}

void DannanTelemetry::persist() {
    telemetry_.persist();
}

std::vector<TelemetryView> DannanTelemetry::views(std::chrono::milliseconds fresh_for) const {
    std::vector<TelemetryView> out;
    for (const auto& view : telemetry_.views(fresh_for))
        if (auto telemetry = node_telemetry_from(view.telemetry))
            out.push_back(TelemetryView{std::move(*telemetry), view.age, view.fresh});
    return out;
}

DannanHost::DannanHost(dannan::NodeConfig config, MessageRoutes& routes, size_t io_threads,
                       MetadataNotice metadata_notice)
    : work_(boost::asio::make_work_guard(io_)) {
    if (!io_threads)
        throw std::invalid_argument("the cluster transport needs at least one io thread");
    // A handler that throws unwinds run(); the fault is recorded and run() resumes after
    // the backoff. Once the context is stopped run() returns and the thread ends.
    for (size_t i = 0; i < io_threads; ++i)
        io_threads_.emplace_back([this](std::stop_token stop) {
            run_supervised_loop("cluster-io", stop, [this] { io_.run(); });
        });
    try {
        node_ = std::make_unique<dannan::Node>(io_, std::move(config));
        auto& node_routes = node_->routes();
        for (const auto& entry : macha_registrations()) {
            if (entry.type == MessageType::metadata_notice) {
                node_routes.add(dannan_type(entry.type), entry.registration,
                                [notice = std::move(metadata_notice)](
                                    const dannan::membership::NodeRecord& peer,
                                    dannan::sched::FrameType, const dannan::net::Message& request) {
                                    if (notice) {
                                        dannan::net::Message copy;
                                        copy.type = request.type;
                                        copy.payload = request.payload;
                                        notice(node_info_from(peer), macha_message(std::move(copy)));
                                    }
                                    return dannan::net::Handled::processed();
                                });
                continue;
            }
            node_routes.add(dannan_type(entry.type), entry.registration, dispatch_to(routes));
        }
        for (const auto type : macha_retired_types())
            node_routes.retire(type);
    } catch (...) {
        node_.reset();
        work_.reset();
        io_.stop();
        io_threads_.clear();
        throw;
    }
}

DannanHost::~DannanHost() {
    stop();
}

void DannanHost::start() {
    if (started_.exchange(true))
        return;
    node_->start();
}

void DannanHost::request_stop() {
    if (node_)
        node_->request_stop();
}

void DannanHost::cancel_outbound_calls() {
    if (node_)
        node_->cancel_outbound_calls();
}

void DannanHost::stop() {
    if (stopped_.exchange(true))
        return;
    // The node goes first, whole, while the io threads still run: its client's stop
    // waits for work only they can finish.
    if (node_)
        node_->stop();
    node_.reset();
    work_.reset();
    io_.stop();
    io_threads_.clear();
}

dannan::membership::NodeRecord DannanHost::record_for(const NodeInfo& peer) const {
    const auto id = dannan_id(peer.id);
    for (const auto& record : node_->nodes().roster())
        if (record.id == id)
            return record;
    dannan::membership::NodeRecord record;
    record.id = id;
    record.host = peer.host;
    record.port = peer.port;
    record.failure_domain = peer.failure_domain;
    record.seen_unix_ms = peer.seen_unix_ms;
    record.capabilities =
        node_inbound_capable(peer) ? dannan::membership::capability_inbound : 0;
    return record;
}

AsyncRpc DannanHost::wrap(std::shared_ptr<std::promise<RpcReply>> promise,
                          std::shared_ptr<dannan::net::CallHandle> handle) {
    auto future = promise->get_future();
    return AsyncRpc(
        std::move(future), [handle] { handle->cancel(); }, [handle] { handle->abort(); },
        [handle](FrameType frame) { handle->promote(dannan_frame(frame)); },
        [handle] { return handle->idle_for(); });
}

namespace {

void settle(std::promise<RpcReply>& promise, dannan::net::CallOutcome outcome) {
    if (outcome.reply) {
        promise.set_value(
            RpcReply{node_info_from(outcome.peer), macha_message(std::move(*outcome.reply))});
        return;
    }
    promise.set_exception(std::make_exception_ptr(std::runtime_error(outcome.failure)));
}

} // namespace

AsyncRpc DannanHost::call_async(const NodeInfo& peer, MessageType type,
                                std::span<const uint8_t> payload, FrameType frame,
                                std::chrono::milliseconds deadline) {
    auto promise = std::make_shared<std::promise<RpcReply>>();
    auto handle = node_->routes().call(
        record_for(peer), dannan_type(type), dannan::net::Bytes(payload.begin(), payload.end()),
        dannan_frame(frame), deadline, io_.get_executor(),
        [promise](dannan::net::CallOutcome outcome) { settle(*promise, std::move(outcome)); });
    return wrap(std::move(promise), std::move(handle));
}

AsyncRpc DannanHost::call_async(const Endpoint& endpoint, MessageType type,
                                std::span<const uint8_t> payload, FrameType frame,
                                std::chrono::milliseconds deadline) {
    auto promise = std::make_shared<std::promise<RpcReply>>();
    auto handle = node_->routes().call(
        dannan::net::Endpoint{endpoint.host, endpoint.port}, dannan_type(type),
        dannan::net::Bytes(payload.begin(), payload.end()), dannan_frame(frame), deadline,
        io_.get_executor(),
        [promise](dannan::net::CallOutcome outcome) { settle(*promise, std::move(outcome)); });
    return wrap(std::move(promise), std::move(handle));
}

} // namespace macha
