// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/net.hpp"

namespace macha {

const char* frame_type_name(FrameType type) noexcept {
    switch (type) {
    case FrameType::control:
        return "control";
    case FrameType::foreground:
        return "foreground";
    case FrameType::read_ahead:
        return "read-ahead";
    case FrameType::speculative:
        return "speculative";
    case FrameType::loader:
        return "loader";
    }
    return "unknown";
}

const char* message_type_name(MessageType type) noexcept {
    switch (type) {
    case MessageType::ping:
        return "ping";
    case MessageType::members:
        return "members";
    case MessageType::have_object:
        return "have_object";
    case MessageType::get_object:
        return "get_object";
    case MessageType::put_object:
        return "put_object";
    case MessageType::put_object_deferred:
        return "put_object_deferred";
    case MessageType::object_durability_barrier:
        return "object_durability_barrier";
    case MessageType::get_metadata:
        return "get_metadata";
    case MessageType::cas_metadata:
        return "cas_metadata";
    case MessageType::cas_metadata_delta:
        return "cas_metadata_delta";
    case MessageType::seed_metadata:
        return "seed_metadata";
    case MessageType::metadata_notice:
        return "metadata_notice";
    case MessageType::get_committed_metadata:
        return "get_committed_metadata";
    case MessageType::checkpoint_metadata:
        return "checkpoint_metadata";
    case MessageType::session_retire:
        return "session_retire";
    case MessageType::delete_object:
        return "delete_object";
    case MessageType::promote_read_ahead:
        return "promote_read_ahead";
    case MessageType::promote_foreground:
        return "promote_foreground";
    case MessageType::cancel_transfer:
        return "cancel_transfer";
    case MessageType::commit_metadata:
        return "commit_metadata";
    case MessageType::get_control_object:
        return "get_control_object";
    case MessageType::put_control_object:
        return "put_control_object";
    case MessageType::get_metadata_identity:
        return "get_metadata_identity";
    case MessageType::telemetry:
        return "telemetry";
    case MessageType::identity_resets:
        return "identity_resets";
    case MessageType::get_metadata_history_entry:
        return "get_metadata_history_entry";
    case MessageType::get_metadata_history_record:
        return "get_metadata_history_record";
    case MessageType::has_metadata_history_entry:
        return "has_metadata_history_entry";
    case MessageType::put_metadata_history_entry:
        return "put_metadata_history_entry";
    case MessageType::get_metadata_heads:
        return "get_metadata_heads";
    case MessageType::put_metadata_commit:
        return "put_metadata_commit";
    case MessageType::accept_metadata_commit:
        return "accept_metadata_commit";
    case MessageType::retain_objects:
        return "retain_objects";
    case MessageType::get_ingest_jobs:
        return "get_ingest_jobs";
    case MessageType::ingest_job_action:
        return "ingest_job_action";
    case MessageType::get_torrent_jobs:
        return "get_torrent_jobs";
    case MessageType::torrent_job_action:
        return "torrent_job_action";
    case MessageType::session_sync:
        return "session_sync";
    case MessageType::have_objects:
        return "have_objects";
    case MessageType::have_control_objects:
        return "have_control_objects";
    case MessageType::have_valid_objects:
        return "have_valid_objects";
    case MessageType::trie_diff:
        return "trie_diff";
    case MessageType::get_unmatched_hints:
        return "get_unmatched_hints";
    case MessageType::submit_ingest_job:
        return "submit_ingest_job";
    case MessageType::torrent_intent:
        return "torrent_intent";
    case MessageType::dial_request:
        return "dial_request";
    case MessageType::dial_back_probe:
        return "dial_back_probe";
    case MessageType::dial_back_probe_reply:
        return "dial_back_probe_reply";
    case MessageType::ok:
        return "ok";
    case MessageType::error:
        return "error";
    case MessageType::members_reply:
        return "members_reply";
    case MessageType::bool_reply:
        return "bool_reply";
    case MessageType::object_reply:
        return "object_reply";
    case MessageType::metadata_reply:
        return "metadata_reply";
    case MessageType::cas_reply:
        return "cas_reply";
    case MessageType::control_object_reply:
        return "control_object_reply";
    case MessageType::metadata_identity_reply:
        return "metadata_identity_reply";
    case MessageType::telemetry_reply:
        return "telemetry_reply";
    case MessageType::identity_resets_reply:
        return "identity_resets_reply";
    case MessageType::metadata_history_entry_reply:
        return "metadata_history_entry_reply";
    case MessageType::metadata_heads_reply:
        return "metadata_heads_reply";
    case MessageType::ingest_jobs_reply:
        return "ingest_jobs_reply";
    case MessageType::ingest_job_action_reply:
        return "ingest_job_action_reply";
    case MessageType::torrent_jobs_reply:
        return "torrent_jobs_reply";
    case MessageType::torrent_job_action_reply:
        return "torrent_job_action_reply";
    case MessageType::session_sync_reply:
        return "session_sync_reply";
    case MessageType::have_objects_reply:
        return "have_objects_reply";
    case MessageType::have_control_objects_reply:
        return "have_control_objects_reply";
    case MessageType::have_valid_objects_reply:
        return "have_valid_objects_reply";
    case MessageType::trie_diff_reply:
        return "trie_diff_reply";
    case MessageType::unmatched_hints_reply:
        return "unmatched_hints_reply";
    case MessageType::ingest_submit_reply:
        return "ingest_submit_reply";
    case MessageType::torrent_intent_reply:
        return "torrent_intent_reply";
    case MessageType::user_sync:
        return "user_sync";
    case MessageType::user_sync_reply:
        return "user_sync_reply";
    }
    return "unknown";
}

unsigned frame_type_priority(FrameType type) noexcept {
    switch (type) {
    case FrameType::control:
        return 1;
    case FrameType::foreground:
        return 2;
    case FrameType::read_ahead:
        return 3;
    case FrameType::loader:
        return 4;
    case FrameType::speculative:
        return 5;
    }
    return 5;
}

FrameType default_frame_type(MessageType type) noexcept {
    if (type == MessageType::get_object || type == MessageType::put_object ||
        type == MessageType::put_object_deferred || type == MessageType::object_durability_barrier)
        return FrameType::foreground;
    if (type == MessageType::get_control_object || type == MessageType::put_control_object ||
        type == MessageType::telemetry || type == MessageType::have_object ||
        type == MessageType::have_objects || type == MessageType::have_valid_objects ||
        type == MessageType::trie_diff ||
        type == MessageType::retain_objects ||
        type == MessageType::delete_object)
        return FrameType::speculative;
    return FrameType::control;
}

AsyncRpc::AsyncRpc(std::future<RpcReply> future, std::function<void()> cancel,
                   std::function<void()> abort, std::function<void(FrameType)> promote,
                   std::function<std::chrono::milliseconds()> idle)
    : future_(std::move(future)), cancel_(std::move(cancel)), abort_(std::move(abort)),
      promote_(std::move(promote)), idle_(std::move(idle)) {}

AsyncRpc::AsyncRpc(AsyncRpc&& other) noexcept
    : future_(std::move(other.future_)), cancel_(std::move(other.cancel_)),
      abort_(std::move(other.abort_)), promote_(std::move(other.promote_)),
      idle_(std::move(other.idle_)) {
    other.cancel_ = {};
    other.abort_ = {};
    other.promote_ = {};
    other.idle_ = {};
}

AsyncRpc& AsyncRpc::operator=(AsyncRpc&& other) noexcept {
    if (this == &other)
        return *this;
    cancel();
    future_ = std::move(other.future_);
    cancel_ = std::move(other.cancel_);
    abort_ = std::move(other.abort_);
    promote_ = std::move(other.promote_);
    idle_ = std::move(other.idle_);
    other.cancel_ = {};
    other.abort_ = {};
    other.promote_ = {};
    other.idle_ = {};
    return *this;
}

AsyncRpc::~AsyncRpc() {
    cancel();
}

bool AsyncRpc::valid() const {
    return future_.valid();
}

std::future_status AsyncRpc::wait_for(std::chrono::milliseconds timeout) {
    return future_.wait_for(timeout);
}

RpcReply AsyncRpc::get() {
    cancel_ = {};
    abort_ = {};
    promote_ = {};
    idle_ = {};
    return future_.get();
}

void AsyncRpc::cancel() {
    if (cancel_) {
        auto cancel = std::move(cancel_);
        abort_ = {};
        promote_ = {};
        idle_ = {};
        cancel();
    }
}

void AsyncRpc::abort() {
    if (abort_) {
        auto abort = std::move(abort_);
        cancel_ = {};
        promote_ = {};
        idle_ = {};
        abort();
    } else {
        cancel();
    }
}

void AsyncRpc::promote(FrameType type) {
    if (promote_)
        promote_(type);
}

std::chrono::milliseconds AsyncRpc::idle_for() const {
    if (!idle_)
        return std::chrono::milliseconds::max();
    return idle_();
}

} // namespace macha
