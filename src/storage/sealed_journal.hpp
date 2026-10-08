// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

namespace macha {

// An append-only journal of sealed frames, the one crash-safe journal the
// retention store and the object ledger share. A frame is
// be32 length | 12-byte nonce | 16-byte tag | AES-GCM ciphertext, sealed
// with the key and the journal's own 8-byte associated data. An append is
// written and fsynced before it returns. Replay reads frames in order and
// stops at the first that is torn, short or will not open, or that the
// caller refuses; the file is truncated there, so the next append follows
// the last durable frame. Frames can also be appended unsynced, made durable
// together by sync(), and read back by offset: the object ledger keeps its
// trie's nodes this way. Not thread-safe: its owner serialises it.
class SealedJournal {
  public:
    SealedJournal(std::filesystem::path path, std::array<uint8_t, 32> key,
                  std::array<uint8_t, 8> aad, uint32_t max_frame);
    ~SealedJournal();
    SealedJournal(const SealedJournal&) = delete;
    SealedJournal& operator=(const SealedJournal&) = delete;

    // Calls `apply` with each durable frame's plaintext, oldest first. A
    // frame `apply` throws on ends the replay as a torn one does, and is
    // logged with `what` and its offset. Frames up to `max_read_frame`
    // bytes are read (an older writer may have written larger ones).
    // Returns the frames applied.
    size_t replay(const std::function<void(std::span<const uint8_t>)>& apply,
                  uint32_t max_read_frame, std::string_view what);
    void append(std::span<const uint8_t> plaintext);
    // Appends without syncing; returns the frame's offset.
    uint64_t append_unsynced(std::span<const uint8_t> plaintext);
    // Makes every frame appended so far durable.
    void sync();
    // The plaintext of the frame at `offset`, as an append returned it.
    // Throws if it is not a whole frame that opens.
    Bytes read_at(uint64_t offset) const;
    // Empties the journal durably, once what it held is checkpointed.
    void reset();

    // Frames replayed or appended since open; bytes() is the file's length.
    uint64_t frames() const noexcept { return frames_; }
    uint64_t bytes() const noexcept { return bytes_; }
    const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
    std::array<uint8_t, 32> key_;
    std::array<uint8_t, 8> aad_;
    uint32_t max_frame_;
    uint64_t frames_{};
    uint64_t bytes_{};
    // Opened on first use and kept; closed before a replay or a reset.
    mutable int fd_{-1};
    int fd() const;
    void close_fd() const noexcept;
};

} // namespace macha
