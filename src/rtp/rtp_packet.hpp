// rtp_packet.hpp
//
// Parses the 12-byte RTP fixed header (RFC 3550 section 5.1) plus optional
// CSRC list and header extension. We don't need RTCP, SRTP, or most of the
// optional fields for this project -- ffmpeg's RTP sender uses a vanilla
// header with no extensions in the default `-f rtp` muxer.
//
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |V=2|P|X|  CC   |M|     PT      |       sequence number        |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                           timestamp                          |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |           synchronization source (SSRC) identifier            |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

#pragma once

#include <cstdint>
#include <span>
#include <optional>
#include "../common/byte_io.hpp"

namespace llcmaf {

struct RtpHeader {
    uint8_t version = 2;
    bool padding = false;
    bool extension = false;
    uint8_t csrc_count = 0;
    bool marker = false;          // for HEVC: set on the last packet of an access unit
    uint8_t payload_type = 0;
    uint16_t sequence_number = 0;
    uint32_t timestamp = 0;       // 90kHz clock for video
    uint32_t ssrc = 0;
    std::span<const std::byte> payload; // remainder after header (+ extensions/CSRC)
};

// Parses an RTP packet in-place (returns a view into `packet`, no copies).
// Returns std::nullopt if the packet is too short or not RTP v2.
inline std::optional<RtpHeader> parse_rtp_header(std::span<const std::byte> packet) {
    if (packet.size() < 12) return std::nullopt;

    ByteReader r(packet);
    uint8_t b0 = r.u8();
    uint8_t b1 = r.u8();

    RtpHeader h;
    h.version = (b0 >> 6) & 0x3;
    if (h.version != 2) return std::nullopt;
    h.padding = (b0 >> 5) & 0x1;
    h.extension = (b0 >> 4) & 0x1;
    h.csrc_count = b0 & 0x0F;

    h.marker = (b1 >> 7) & 0x1;
    h.payload_type = b1 & 0x7F;

    h.sequence_number = r.u16be();
    h.timestamp = r.u32be();
    h.ssrc = r.u32be();

    // Skip CSRC list (each 4 bytes).
    size_t csrc_bytes = size_t(h.csrc_count) * 4;
    if (!r.has(csrc_bytes)) return std::nullopt;
    r.skip(csrc_bytes);

    // Skip generic header extension (RFC 3550 5.3.1) if present.
    if (h.extension) {
        if (!r.has(4)) return std::nullopt;
        r.skip(2); // profile-specific id, ignored
        uint16_t ext_len_words = r.u16be();
        size_t ext_bytes = size_t(ext_len_words) * 4;
        if (!r.has(ext_bytes)) return std::nullopt;
        r.skip(ext_bytes);
    }

    std::span<const std::byte> payload = r.remainder();

    // Strip RTP padding from the tail, if signalled.
    if (h.padding && !payload.empty()) {
        uint8_t pad_len = std::to_integer<uint8_t>(payload.back());
        if (pad_len <= payload.size()) {
            payload = payload.first(payload.size() - pad_len);
        }
    }

    h.payload = payload;
    return h;
}

} // namespace llcmaf
