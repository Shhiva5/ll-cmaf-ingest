// hevc_depacketizer.hpp
//
// Implements RFC 7798 ("RTP Payload Format for High Efficiency Video Coding
// (HEVC)") payload structures 1-3:
//
//   1. Single NAL unit packet  - RTP payload IS one NAL unit, verbatim.
//   2. Aggregation Packet (AP) - multiple small NAL units packed into one
//      RTP packet, each prefixed with a 16-bit size.
//   3. Fragmentation Unit (FU) - one large NAL unit split across multiple
//      RTP packets (S=start, E=end bits).
//
// HEVC NAL header (2 bytes, after the RTP payload's first byte which doubles
// as part of this header):
//
//   +---------------+---------------+
//   |0|1|2|3|4|5|6|7|0|1|2|3|4|5|6|7|
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |F|   Type    |  LayerId  | TID |
//   +-------------+-----------------+
//
//   F (1 bit)        forbidden_zero_bit, must be 0.
//   Type (6 bits)    nal_unit_type. Values 48/49 are RTP-specific
//                    (AP / FU) and never appear in the actual bitstream.
//   LayerId (6 bits) always 0 for the base layer streams this project uses.
//   TID (3 bits)     temporal id + 1.
//
// FU header (1 byte), replaces the original NAL's type field with 49:
//
//   +---------------+
//   |0|1|2|3|4|5|6|7|
//   +-+-+-+-+-+-+-+-+
//   |S|E|  FuType   |
//   +---------------+
//
//   S = start of fragment, E = end of fragment, FuType = the ORIGINAL
//   nal_unit_type that was fragmented.
//
// Design: the depacketizer is a single-producer state machine keyed by RTP
// sequence number. It does NOT itself handle out-of-order arrival across
// whole NAL units -- that is the JitterBuffer's job, one layer up. What it
// does handle is out-of-order *packets within a fragmented NAL*, which RFC
// 7798 explicitly allows for but which basically never happens in practice
// on a single UDP flow; we detect and drop the in-progress FU if sequence
// numbers jump, rather than silently emitting a corrupt NAL.

#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <functional>
#include <optional>
#include "../common/types.hpp"
#include "../common/byte_io.hpp"
#include "rtp_packet.hpp"

namespace llcmaf {

class HevcDepacketizer {
public:
    // Called once per fully-reassembled NAL unit, in arrival order.
    using NalCallback = std::function<void(NalUnit)>;

    explicit HevcDepacketizer(NalCallback on_nal) : on_nal_(std::move(on_nal)) {}

    // Feed one RTP packet's payload (post RTP-header-parse) plus its
    // timestamp/sequence/arrival time. Emits zero or more NALs via the
    // callback synchronously.
    void on_rtp_packet(const RtpHeader& hdr, MonoNs arrival_ns) {
        if (hdr.payload.size() < 2) return; // need at least the 2-byte NAL header

        uint8_t byte0 = std::to_integer<uint8_t>(hdr.payload[0]);
        uint8_t nal_type = (byte0 >> 1) & 0x3F;

        if (nal_type < 48) {
            emit_single_nal(hdr, arrival_ns);
        } else if (nal_type == 48) {
            handle_aggregation_packet(hdr, arrival_ns);
        } else if (nal_type == 49) {
            handle_fragmentation_unit(hdr, arrival_ns);
        }
        // Type 50 (PACI) is not used by ffmpeg's RTP muxer; ignored here.

        last_seq_ = hdr.sequence_number;
        have_last_seq_ = true;
    }

    // Discard any in-flight fragmented NAL (call this if the jitter buffer /
    // receiver detects a gap so large that resuming reassembly would produce
    // a corrupt frame).
    void reset() { fu_in_progress_.clear(); }

private:
    void emit_single_nal(const RtpHeader& hdr, MonoNs arrival_ns) {
        NalUnit nal;
        nal.nal_unit_type = (std::to_integer<uint8_t>(hdr.payload[0]) >> 1) & 0x3F;
        nal.rtp_timestamp = hdr.timestamp;
        nal.arrival_time_ns = arrival_ns;
        nal.payload.assign(hdr.payload.begin(), hdr.payload.end());
        on_nal_(std::move(nal));
    }

    void handle_aggregation_packet(const RtpHeader& hdr, MonoNs arrival_ns) {
        // AP payload: [2-byte AP NAL header][ {2-byte size, NAL unit}... ]
        ByteReader r(hdr.payload);
        r.skip(2); // AP header itself carries no useful info beyond type=48
        while (r.remaining() > 2) {
            uint16_t size = r.u16be();
            if (!r.has(size)) break; // malformed/truncated aggregation unit
            auto nal_bytes = r.take(size);
            if (nal_bytes.size() < 2) continue;

            NalUnit nal;
            nal.nal_unit_type = (std::to_integer<uint8_t>(nal_bytes[0]) >> 1) & 0x3F;
            nal.rtp_timestamp = hdr.timestamp;
            nal.arrival_time_ns = arrival_ns;
            nal.payload.assign(nal_bytes.begin(), nal_bytes.end());
            on_nal_(std::move(nal));
        }
    }

    void handle_fragmentation_unit(const RtpHeader& hdr, MonoNs arrival_ns) {
        if (hdr.payload.size() < 3) return; // 2-byte NAL header + 1-byte FU header

        uint8_t byte0 = std::to_integer<uint8_t>(hdr.payload[0]);
        uint8_t byte1 = std::to_integer<uint8_t>(hdr.payload[1]);
        uint8_t fu_header = std::to_integer<uint8_t>(hdr.payload[2]);

        bool start = (fu_header >> 7) & 0x1;
        bool end   = (fu_header >> 6) & 0x1;
        uint8_t original_type = fu_header & 0x3F;

        if (start) {
            fu_in_progress_.clear();
            // Reconstruct the original 2-byte NAL header: same LayerId/TID,
            // but nal_unit_type replaced with the original (fragmented) type.
            uint8_t reconstructed_b0 = (byte0 & 0x81) | (original_type << 1);
            fu_in_progress_.push_back(std::byte{reconstructed_b0});
            fu_in_progress_.push_back(std::byte{byte1});
            fu_start_seq_ = hdr.sequence_number;
            fu_start_ts_ = hdr.timestamp;
        } else {
            if (fu_in_progress_.empty()) return; // FU continuation with no start seen; drop
            if (have_last_seq_ &&
                uint16_t(last_seq_ + 1) != hdr.sequence_number) {
                // Gap inside a fragmented NAL -> the NAL is unrecoverable.
                fu_in_progress_.clear();
                return;
            }
        }

        if (!fu_in_progress_.empty()) {
            // GCC 13 at -O2/-O3 emits a false-positive -Wstringop-overflow
            // here: its range analysis doesn't correlate the `size() < 3`
            // early-return at the top of this function with this
            // subspan(3)/insert() several branches later, and misjudges the
            // insert as potentially writing into a zero-capacity buffer.
            // hdr.payload.size() >= 3 is already guaranteed above, so
            // rest.size() == hdr.payload.size() - 3 is always well-defined
            // and non-negative. Verified: ASan/UBSan-clean under the same
            // FU-reassembly unit tests that exercise this path (see
            // tests/test_hevc_depacketizer.cpp).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
            auto rest = hdr.payload.subspan(3);
            fu_in_progress_.insert(fu_in_progress_.end(), rest.begin(), rest.end());
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        }

        if (end && !fu_in_progress_.empty()) {
            NalUnit nal;
            nal.nal_unit_type = original_type;
            nal.rtp_timestamp = fu_start_ts_;
            nal.arrival_time_ns = arrival_ns;
            nal.payload = std::move(fu_in_progress_);
            fu_in_progress_.clear();
            on_nal_(std::move(nal));
        }
    }

    NalCallback on_nal_;
    std::vector<std::byte> fu_in_progress_;
    uint16_t fu_start_seq_ = 0;
    uint32_t fu_start_ts_ = 0;
    uint16_t last_seq_ = 0;
    bool have_last_seq_ = false;
};

} // namespace llcmaf
