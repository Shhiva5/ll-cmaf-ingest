// types.hpp
//
// Shared plain-data types that flow between pipeline stages:
//
//   RtpTimestamp -> HevcDepacketizer -> NalUnit -> JitterBuffer -> AccessUnit
//   -> CmafMuxer -> Chunk -> HttpChunkedServer
//
// Keeping these as simple, copyable/movable structs (no inheritance, no
// virtual dispatch) means the hot path is just vector<byte> moves.

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <chrono>

namespace llcmaf {

// Monotonic nanosecond timestamp, used for *everything* time-related in this
// project except RTP timestamps (which are a separate, encoder-clock domain).
// Always sourced from std::chrono::steady_clock so it's immune to NTP jumps.
using MonoNs = int64_t;

inline MonoNs now_mono_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// One HEVC NAL unit, already depacketized/reassembled from RTP, with Annex-B
// start code and emulation-prevention bytes still intact (the muxer strips
// the start code itself; the SPS parser strips emulation-prevention only
// when it needs to read bits).
struct NalUnit {
    uint8_t nal_unit_type = 0;        // bits 1-6 of the 2-byte HEVC NAL header
    uint32_t rtp_timestamp = 0;       // 90kHz clock, from the RTP header
    MonoNs arrival_time_ns = 0;       // when the last RTP packet of this NAL arrived
    std::vector<std::byte> payload;   // NAL header + RBSP, no Annex-B start code

    [[nodiscard]] bool is_idr() const {
        return nal_unit_type == 19 || nal_unit_type == 20; // IDR_W_RADL, IDR_N_LP
    }
    [[nodiscard]] bool is_vps() const { return nal_unit_type == 32; }
    [[nodiscard]] bool is_sps() const { return nal_unit_type == 33; }
    [[nodiscard]] bool is_pps() const { return nal_unit_type == 34; }
    [[nodiscard]] bool is_sei() const { return nal_unit_type == 39 || nal_unit_type == 40; }
    [[nodiscard]] bool is_vcl() const { return nal_unit_type <= 31; } // slice NALs
};

// One decodable access unit = one or more NALs sharing a presentation time
// (for our simplified single-slice-per-frame test streams, this is usually
// exactly one VCL NAL, possibly preceded by VPS/SPS/PPS on the first frame
// of each IDR period).
struct AccessUnit {
    uint32_t rtp_timestamp = 0;
    MonoNs pts_ns = 0;                // presentation time, derived from rtp_timestamp
    MonoNs capture_time_ns = 0;       // glass-to-glass origin, from injected SEI
                                       // (see docs/NETWORK_TESTING_AND_LATENCY.md)
    bool is_idr = false;
    std::vector<NalUnit> nals;
};

// Parsed subset of an HEVC SPS -- just enough to build the CMAF `hvcC` box
// and size the video track. Full SPS parsing has ~20 more fields we don't
// need for muxing.
struct HevcSpsInfo {
    uint8_t general_profile_idc = 0;
    uint8_t general_tier_flag = 0;
    uint8_t general_level_idc = 0;
    uint32_t general_profile_compatibility_flags = 0;
    uint64_t general_constraint_indicator_flags = 0;
    uint32_t pic_width_luma = 0;
    uint32_t pic_height_luma = 0;
    uint8_t chroma_format_idc = 1;
    uint8_t bit_depth_luma_minus8 = 0;
    uint8_t bit_depth_chroma_minus8 = 0;
};

} // namespace llcmaf
