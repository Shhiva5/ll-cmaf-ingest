// byte_io.hpp
//
// Small, dependency-free helpers for two things every stage of this pipeline
// needs:
//
//   1. ByteReader   - read big-endian integers out of a std::span<const std::byte>
//                     (used by the RTP header parser and the HEVC NAL parser).
//   2. BitReader    - read individual bits / exp-golomb codes out of a NAL's
//                     RBSP payload (used to parse SPS: width, height, profile).
//   3. ByteWriter   - append big-endian integers / raw bytes into a
//                     std::vector<std::byte> (used by the MP4 box writer).
//
// Design notes:
//   - Everything here operates on std::span/std::byte (C++20) so the rest of
//     the codebase never has to reinterpret_cast between uint8_t* and
//     std::byte* -- we do it once, at the edges (socket recv, file read).
//   - These are intentionally "dumb" -- no exceptions on out-of-range reads
//     in the hot path; callers check size() themselves. BitReader does throw
//     std::out_of_range because SPS parsing is not a hot path and we want
//     malformed streams to fail loudly during development.

#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
#include <stdexcept>
#include <bit>

namespace llcmaf {

// ---------------------------------------------------------------------------
// ByteReader: sequential big-endian reads over a read-only byte span.
// ---------------------------------------------------------------------------
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] size_t remaining() const { return data_.size() - pos_; }
    [[nodiscard]] size_t position() const { return pos_; }
    [[nodiscard]] bool has(size_t n) const { return remaining() >= n; }

    uint8_t u8() {
        require(1);
        return std::to_integer<uint8_t>(data_[pos_++]);
    }

    uint16_t u16be() {
        require(2);
        uint16_t v = (uint16_t(u8()) << 8);
        v |= u8();
        return v;
    }

    uint32_t u24be() {
        require(3);
        uint32_t v = uint32_t(u8()) << 16;
        v |= uint32_t(u8()) << 8;
        v |= u8();
        return v;
    }

    uint32_t u32be() {
        require(4);
        uint32_t v = uint32_t(u8()) << 24;
        v |= uint32_t(u8()) << 16;
        v |= uint32_t(u8()) << 8;
        v |= u8();
        return v;
    }

    uint64_t u64be() {
        uint64_t hi = u32be();
        uint64_t lo = u32be();
        return (hi << 32) | lo;
    }

    // Returns a view into the underlying span, does not copy.
    std::span<const std::byte> take(size_t n) {
        require(n);
        auto out = data_.subspan(pos_, n);
        pos_ += n;
        return out;
    }

    void skip(size_t n) {
        require(n);
        pos_ += n;
    }

    std::span<const std::byte> remainder() const {
        return data_.subspan(pos_);
    }

private:
    void require(size_t n) const {
        if (remaining() < n) throw std::out_of_range("ByteReader: read past end");
    }

    std::span<const std::byte> data_;
    size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// ByteWriter: append-only big-endian byte builder.
// ---------------------------------------------------------------------------
class ByteWriter {
public:
    void u8(uint8_t v) { buf_.push_back(std::byte{v}); }

    void u16be(uint16_t v) {
        u8(uint8_t(v >> 8));
        u8(uint8_t(v));
    }

    void u24be(uint32_t v) {
        u8(uint8_t(v >> 16));
        u8(uint8_t(v >> 8));
        u8(uint8_t(v));
    }

    void u32be(uint32_t v) {
        u8(uint8_t(v >> 24));
        u8(uint8_t(v >> 16));
        u8(uint8_t(v >> 8));
        u8(uint8_t(v));
    }

    void u64be(uint64_t v) {
        u32be(uint32_t(v >> 32));
        u32be(uint32_t(v));
    }

    void bytes(std::span<const std::byte> data) {
        buf_.insert(buf_.end(), data.begin(), data.end());
    }

    void fourcc(const char (&tag)[5]) {
        // tag is a 4-char string literal + trailing NUL, e.g. "ftyp"
        for (int i = 0; i < 4; ++i) u8(uint8_t(tag[i]));
    }

    [[nodiscard]] size_t size() const { return buf_.size(); }

    // Patch a previously written 32-bit big-endian value (used for box sizes,
    // which aren't known until after the box body is written).
    void patch_u32be(size_t offset, uint32_t v) {
        buf_[offset + 0] = std::byte(uint8_t(v >> 24));
        buf_[offset + 1] = std::byte(uint8_t(v >> 16));
        buf_[offset + 2] = std::byte(uint8_t(v >> 8));
        buf_[offset + 3] = std::byte(uint8_t(v));
    }

    [[nodiscard]] const std::vector<std::byte>& data() const { return buf_; }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(buf_); }

private:
    std::vector<std::byte> buf_;
};

// ---------------------------------------------------------------------------
// BitReader: MSB-first bit reader + Exp-Golomb decoding, for SPS parsing.
// Operates on an already emulation-prevention-stripped RBSP buffer.
// ---------------------------------------------------------------------------
class BitReader {
public:
    explicit BitReader(std::span<const std::byte> rbsp) : data_(rbsp) {}

    uint32_t bit() {
        if (byte_pos_ >= data_.size()) throw std::out_of_range("BitReader: past end");
        uint8_t byte = std::to_integer<uint8_t>(data_[byte_pos_]);
        uint32_t b = (byte >> (7 - bit_pos_)) & 0x1;
        if (++bit_pos_ == 8) { bit_pos_ = 0; ++byte_pos_; }
        return b;
    }

    uint32_t bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | bit();
        return v;
    }

    // Unsigned Exp-Golomb (ue(v)) per H.264/H.265 spec section 9.1.
    uint32_t ue() {
        int leading_zero_bits = 0;
        while (bit() == 0 && leading_zero_bits < 32) ++leading_zero_bits;
        if (leading_zero_bits == 0) return 0;
        uint32_t suffix = bits(leading_zero_bits);
        return (1u << leading_zero_bits) - 1 + suffix;
    }

    // Signed Exp-Golomb (se(v)) per spec section 9.1.1.
    int32_t se() {
        uint32_t code = ue();
        int32_t sign = (code & 1) ? 1 : -1;
        return sign * int32_t((code + 1) / 2);
    }

    [[nodiscard]] bool byte_aligned() const { return bit_pos_ == 0; }
    [[nodiscard]] size_t bits_consumed() const { return byte_pos_ * 8 + bit_pos_; }

private:
    std::span<const std::byte> data_;
    size_t byte_pos_ = 0;
    int bit_pos_ = 0;
};

// Removes H.264/H.265 emulation-prevention bytes (00 00 03 -> 00 00) from a
// NAL unit payload, producing the RBSP (Raw Byte Sequence Payload).
// This must be done before SPS/PPS bit-level parsing.
inline std::vector<std::byte> strip_emulation_prevention(std::span<const std::byte> nal) {
    std::vector<std::byte> out;
    out.reserve(nal.size());
    int zero_run = 0;
    for (size_t i = 0; i < nal.size(); ++i) {
        uint8_t b = std::to_integer<uint8_t>(nal[i]);
        if (zero_run >= 2 && b == 0x03) {
            // Drop the emulation prevention byte itself.
            zero_run = 0;
            continue;
        }
        out.push_back(nal[i]);
        zero_run = (b == 0x00) ? zero_run + 1 : 0;
    }
    return out;
}

} // namespace llcmaf
