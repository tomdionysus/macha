// SPDX-License-Identifier: GPL-3.0-or-later
#include "codec.hpp"

namespace macha {
void Writer::u8(uint8_t value) {
    data_.push_back(value);
}

void Writer::u16(uint16_t value) {
    data_.push_back(static_cast<uint8_t>(value >> 8));
    data_.push_back(static_cast<uint8_t>(value));
}

void Writer::u32(uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        data_.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void Writer::u64(uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        data_.push_back(static_cast<uint8_t>(value >> shift));
    }
}

void Writer::raw(std::span<const uint8_t> bytes) {
    data_.insert(data_.end(), bytes.begin(), bytes.end());
}

void Writer::bytes(std::span<const uint8_t> bytes) {
    if (bytes.size() > UINT32_MAX)
        throw std::runtime_error("encoded blob too large");
    u32(static_cast<uint32_t>(bytes.size()));
    raw(bytes);
}

void Writer::string(const std::string& value) {
    bytes({reinterpret_cast<const uint8_t*>(value.data()), value.size()});
}

void Reader::need(size_t count) const {
    if (count > remaining())
        throw DecodeError("truncated input");
}

uint8_t Reader::u8() {
    need(1);
    return data_[position_++];
}

uint16_t Reader::u16() {
    need(2);
    uint16_t value =
        static_cast<uint16_t>(data_[position_]) << 8 | static_cast<uint16_t>(data_[position_ + 1]);
    position_ += 2;
    return value;
}

uint32_t Reader::u32() {
    need(4);
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i)
        value = (value << 8) | data_[position_++];
    return value;
}

uint64_t Reader::u64() {
    need(8);
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | data_[position_++];
    return value;
}

Bytes Reader::raw(size_t count) {
    need(count);
    Bytes out(data_.begin() + position_, data_.begin() + position_ + count);
    position_ += count;
    return out;
}

Bytes Reader::bytes(size_t maximum) {
    auto count = u32();
    if (count > maximum)
        throw DecodeError("blob too large");
    return raw(count);
}

std::string Reader::string(size_t maximum) {
    auto value = bytes(maximum);
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void Reader::finish() const {
    if (position_ != data_.size())
        throw DecodeError("trailing input");
}

void encode_node_info(Writer& writer, const NodeInfo& node) {
    writer.fixed(node.id.bytes);
    writer.string(node.host);
    writer.string(node.failure_domain);
    writer.u16(node.port);
    writer.u64(node.capacity);
    writer.u64(node.used);
    writer.u64(node.seen_unix_ms);
    writer.u64(node.metadata_generation);
    writer.u32(node.metadata_write_replicas_required);
    writer.u8(node.flags);
}

NodeInfo decode_node_info(Reader& reader) {
    NodeInfo node;
    node.id.bytes = reader.fixed<16>();
    node.host = reader.string(4096);
    node.failure_domain = reader.string(4096);
    node.port = reader.u16();
    node.capacity = reader.u64();
    node.used = reader.u64();
    node.seen_unix_ms = reader.u64();
    node.metadata_generation = reader.u64();
    node.metadata_write_replicas_required = reader.u32();
    node.flags = reader.u8();
    return node;
}
} // namespace macha
