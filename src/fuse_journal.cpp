// SPDX-License-Identifier: GPL-3.0-or-later
#include "fuse_journal.hpp"

#include "codec.hpp"
#include "crypto.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace macha {
namespace {

uint32_t be32(std::span<const uint8_t> bytes) {
    if (bytes.size() != 4)
        throw DecodeError("truncated FUSE journal frame length");
    return (static_cast<uint32_t>(bytes[0]) << 24U) |
           (static_cast<uint32_t>(bytes[1]) << 16U) |
           (static_cast<uint32_t>(bytes[2]) << 8U) |
           static_cast<uint32_t>(bytes[3]);
}

} // namespace

Bytes fuse_journal_frame(std::span<const uint8_t> payload) {
    if (payload.size() > fuse_journal_max_record)
        throw std::runtime_error("FUSE operation journal record too large");
    Writer frame;
    frame.u32(static_cast<uint32_t>(payload.size()));
    frame.raw(payload);
    frame.fixed(sha256(payload).bytes);
    return frame.take();
}

FuseJournalScanResult scan_fuse_journal_frames(std::span<const uint8_t> bytes,
                                               size_t first_frame_offset,
                                               const FuseJournalRecordConsumer& consume) {
    if (first_frame_offset > bytes.size())
        throw std::runtime_error("FUSE operation journal header is missing");

    size_t position = first_frame_offset;
    size_t last_good = position;
    while (position < bytes.size()) {
        if (bytes.size() - position < 4)
            break;
        const auto length = be32(bytes.subspan(position, 4));
        if (length > fuse_journal_max_record)
            throw std::runtime_error("FUSE operation journal record length is corrupt");
        const size_t frame_size = 4ULL + length + 32ULL;
        if (bytes.size() - position < frame_size)
            break;

        const auto payload = bytes.subspan(position + 4, length);
        Hash256 expected;
        std::copy_n(bytes.begin() + static_cast<ptrdiff_t>(position + 4 + length), 32,
                    expected.bytes.begin());
        if (sha256(payload) != expected) {
            // A crash can expose the final append at its full logical length
            // while tail sectors were not durably written. An EOF checksum
            // failure is therefore a torn append. Corruption before a later
            // frame is not a crash tail and remains fatal.
            if (position + frame_size == bytes.size())
                break;
            throw std::runtime_error("FUSE operation journal checksum mismatch");
        }

        consume(payload, position);
        position += frame_size;
        last_good = position;
    }

    return {last_good, bytes.size() - last_good};
}

} // namespace macha
