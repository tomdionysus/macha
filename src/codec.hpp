// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.hpp"
#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace macha {
class DecodeError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class Writer {
    Bytes data_;

  public:
    void u8(uint8_t);
    void u16(uint16_t);
    void u32(uint32_t);
    void u64(uint64_t);
    void i64(int64_t value) {
        u64(static_cast<uint64_t>(value));
    }
    void raw(std::span<const uint8_t>);
    void bytes(std::span<const uint8_t>);
    void string(const std::string&);

    template <size_t N> void fixed(const std::array<uint8_t, N>& value) {
        raw(value);
    }

    const Bytes& data() const {
        return data_;
    }
    Bytes take() {
        return std::move(data_);
    }
};

class Reader {
    std::span<const uint8_t> data_;
    size_t position_{};

    void need(size_t) const;

  public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}
    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    int64_t i64() {
        return static_cast<int64_t>(u64());
    }
    Bytes raw(size_t);
    Bytes bytes(size_t maximum = 256 * 1024 * 1024);
    std::string string(size_t maximum = 65536);

    template <size_t N> std::array<uint8_t, N> fixed() {
        need(N);
        std::array<uint8_t, N> out{};
        std::copy_n(data_.begin() + position_, N, out.begin());
        position_ += N;
        return out;
    }

    size_t remaining() const {
        return data_.size() - position_;
    }
    void finish() const;
};

void encode_node_info(Writer&, const NodeInfo&);
NodeInfo decode_node_info(Reader&);
} // namespace macha
