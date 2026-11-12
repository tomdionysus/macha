// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cluster/frame_type.hpp"
#include "types.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <vector>
#include "retained_memory.hpp"

#include <dannan/net/outbound.h>

namespace macha {
enum class MessageType : uint16_t {
    ping = 1,
    members = 2,
    have_object = 515,
    get_object = 516,
    put_object = 517,
    get_metadata = 518,
    cas_metadata = 519,
    seed_metadata = 520,
    metadata_notice = 521,
    get_committed_metadata = 522,
    checkpoint_metadata = 523,
    session_retire = 3,
    delete_object = 525,
    promote_read_ahead = 4,
    promote_foreground = 5,
    cancel_transfer = 6,
    commit_metadata = 529,
    cas_metadata_delta = 530,
    get_control_object = 531,
    put_control_object = 532,
    get_metadata_identity = 533,
    put_object_deferred = 534,
    object_durability_barrier = 535,
    telemetry = 7,
    identity_resets = 8,
    get_metadata_history_entry = 538,
    has_metadata_history_entry = 539,
    put_metadata_history_entry = 540,
    get_metadata_heads = 541,
    put_metadata_commit = 542,
    accept_metadata_commit = 543,
    retain_objects = 544,
    get_ingest_jobs = 545,
    ingest_job_action = 546,
    get_torrent_jobs = 547,
    torrent_job_action = 548,
    // 549 and 550 (once 37 and 38) are not in use.
    session_sync = 551,
    have_objects = 552,
    // CONTROL-plane counterpart of have_objects (which answers from
    // local_store() only), so a metadata publication skips control objects
    // the peer already holds. Reply: have_control_objects_reply; a peer that
    // errors is sent the whole graph.
    have_control_objects = 557,
    // An action on a claimed torrent while metadata cannot be written: the
    // owner applies it and journals the intent until it can publish.
    // Reply: torrent_intent_reply.
    torrent_intent = 558,
    // Repair's batched presence probe. Unlike have_objects (index lookup),
    // each id is read, decrypted and hashed like have_object, so a corrupt
    // copy reports absent. At most have_valid_objects_max ids. Reply:
    // have_valid_objects_reply; a peer that errors is probed per object.
    have_valid_objects = 559,
    // 560 and 636 (once 48 and 124) are retired; never reuse them.
    // A question to the peer's held trie (ledger/trie_diff): its root,
    // pinning that snapshot, or the subtrees or records below prefixes of a
    // pinned snapshot. Reply: trie_diff_reply. Sent only to peers that
    // advertise a version with it.
    trie_diff = 561,
    // The files the peer's scanner could not match (catalogue hints), unless
    // they are unchanged since the revision the asker names. Reply:
    // unmatched_hints_reply. Sent only to peers that advertise a version with
    // it.
    get_unmatched_hints = 562,
    // An ingest job the API pinned to the peer: it reads `path` on its own
    // filesystem and runs the job. Reply: ingest_submit_reply, with the job's
    // id or the peer's refusal. A peer before 0.91.1 answers not_implemented.
    submit_ingest_job = 563,
    // A history hash's record as a self-contained full-body entry,
    // materialised by the server (get_metadata_history_entry returns the
    // stored frame, possibly a delta the caller cannot replay).
    // Reply: metadata_history_entry_reply.
    get_metadata_history_record = 553,
    user_sync = 554,
    // For nodes that accept no inbound connections. dial_request is a
    // notification (request_id 0) over an existing CONTROL route asking a
    // non-dialable peer to open a lane to the sender. dial_back_probe asks a
    // peer to make one throwaway connection to the sender's advertised
    // endpoint and report whether the handshake completed; it is how
    // `network.inbound_capable: auto` resolves.
    dial_request = 9,
    dial_back_probe = 10,
    ok = 100,
    error = 127,
    members_reply = 101,
    bool_reply = 615,
    object_reply = 616,
    metadata_reply = 617,
    cas_reply = 618,
    control_object_reply = 619,
    metadata_identity_reply = 620,
    telemetry_reply = 102,
    identity_resets_reply = 103,
    metadata_history_entry_reply = 623,
    metadata_heads_reply = 624,
    ingest_jobs_reply = 625,
    ingest_job_action_reply = 626,
    torrent_jobs_reply = 627,
    torrent_job_action_reply = 628,
    session_sync_reply = 629,
    have_objects_reply = 630,
    user_sync_reply = 631,
    dial_back_probe_reply = 104,
    have_control_objects_reply = 633,
    torrent_intent_reply = 634,
    have_valid_objects_reply = 635,
    trie_diff_reply = 637,
    unmatched_hints_reply = 638,
    ingest_submit_reply = 639
};

// Each id in a have_valid_objects request is a full read on the peer, so a
// request is bounded to one data worker's handler-sized job.
inline constexpr size_t have_valid_objects_max = 16;

// The transport's per-session limits a pipelining caller sizes its window against:
// replies outstanding and messages queued for the writer.
inline constexpr size_t max_pending_rpc_requests = dannan::net::max_pending_replies;
inline constexpr size_t max_peer_outbound_messages = dannan::net::max_outbound_messages;

const char* frame_type_name(FrameType) noexcept;
const char* message_type_name(MessageType) noexcept;
unsigned frame_type_priority(FrameType) noexcept;
// The frame a call of this type takes when its caller names none.
FrameType default_frame_type(MessageType) noexcept;

struct RpcMessage {
    MessageType type{MessageType::error};
    Bytes payload;
    // Fragment memory leases follow the message through executor/future
    // handoffs, so no asynchronous gap goes uncharged.
    std::shared_ptr<std::vector<RetainedMemoryLedger::Lease>> retained_memory;

    RpcMessage() = default;
    RpcMessage(MessageType message_type, Bytes message_payload,
               std::shared_ptr<std::vector<RetainedMemoryLedger::Lease>> memory = {})
        : type(message_type), payload(std::move(message_payload)),
          retained_memory(std::move(memory)) {}
};

struct RpcReply {
    NodeInfo peer;
    RpcMessage message;
};

struct RpcStats {
    uint64_t connections_created{};
    uint64_t connections_reused{};
    uint64_t canonical_connections{};
};

struct RpcServerWorkStats {
    struct Timing {
        uint64_t requests{};
        uint64_t queue_wait_us_total{};
        uint64_t queue_wait_us_max{};
        uint64_t handler_us_total{};
        uint64_t handler_us_max{};
    };

    size_t metadata_pending_jobs{};
    size_t metadata_pending_bytes{};
    size_t metadata_active_jobs{};
    uint64_t metadata_rejected_jobs{};
    size_t fast_control_pending_bytes{};
    size_t control_pending_bytes{};
    size_t data_pending_bytes{};
    uint64_t rejected_jobs{};
    std::map<FrameType, Timing> frame_timings;
    std::map<MessageType, Timing> message_timings;
};

class AsyncRpc {
    std::future<RpcReply> future_;
    std::function<void()> cancel_;
    std::function<void()> abort_;
    std::function<void(FrameType)> promote_;
    std::function<std::chrono::milliseconds()> idle_;

  public:
    AsyncRpc() = default;
    AsyncRpc(std::future<RpcReply>, std::function<void()>, std::function<void()>,
             std::function<void(FrameType)> = {}, std::function<std::chrono::milliseconds()> = {});
    ~AsyncRpc();
    AsyncRpc(AsyncRpc&&) noexcept;
    AsyncRpc& operator=(AsyncRpc&&) noexcept;
    AsyncRpc(const AsyncRpc&) = delete;
    AsyncRpc& operator=(const AsyncRpc&) = delete;

    bool valid() const;
    std::future_status wait_for(std::chrono::milliseconds);
    RpcReply get();
    void cancel();
    void abort();
    void promote(FrameType);
    std::function<void(FrameType)> promotion_callback() const {
        return promote_;
    }
    std::chrono::milliseconds idle_for() const;
};

} // namespace macha
