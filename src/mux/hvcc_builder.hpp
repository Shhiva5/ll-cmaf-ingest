// hvcc_builder.hpp
//
// Builds the `hvcC` box body (HEVCDecoderConfigurationRecord, ISO/IEC
// 14496-15 section 8.3.3.1). This is the box that tells a player's decoder
// "here is HEVC video, here are its VPS/SPS/PPS, here is how NAL lengths
// are encoded in the mdat" -- without a correct hvcC, no player will even
// attempt to decode the track.
//
// We use lengthSizeMinusOne = 3 (i.e. 4-byte NAL length prefixes) uniformly,
// matching what our muxer writes into every mdat sample (see cmaf_muxer.hpp
// annexb_to_length_prefixed()).

#pragma once

#include <vector>
#include <span>
#include "../common/byte_io.hpp"
#include "../common/types.hpp"

namespace llcmaf {

struct HevcParamSets {
    std::vector<std::byte> vps; // NAL header + RBSP, no Annex-B start code
    std::vector<std::byte> sps;
    std::vector<std::byte> pps;
};

inline std::vector<std::byte> build_hvcc_box_body(const HevcSpsInfo& sps_info,
                                                   const HevcParamSets& params) {
    ByteWriter w;

    w.u8(1); // configurationVersion

    uint8_t byte1 = uint8_t((0 & 0x3) << 6) |               // general_profile_space
                    uint8_t((sps_info.general_tier_flag & 0x1) << 5) |
                    uint8_t(sps_info.general_profile_idc & 0x1F);
    w.u8(byte1);

    w.u32be(sps_info.general_profile_compatibility_flags);

    // 48-bit constraint indicator flags, written as 6 bytes big-endian.
    uint64_t constraint = sps_info.general_constraint_indicator_flags;
    for (int shift = 40; shift >= 0; shift -= 8) {
        w.u8(uint8_t((constraint >> shift) & 0xFF));
    }

    w.u8(sps_info.general_level_idc);

    w.u16be(0xF000 | 0);      // reserved(4)=1111 + min_spatial_segmentation_idc(12)=0
    w.u8(0xFC | 0);           // reserved(6)=111111 + parallelismType(2)=0 (unknown)
    w.u8(0xFC | (sps_info.chroma_format_idc & 0x3));
    w.u8(0xF8 | (sps_info.bit_depth_luma_minus8 & 0x7));
    w.u8(0xF8 | (sps_info.bit_depth_chroma_minus8 & 0x7));
    w.u16be(0);               // avgFrameRate = 0 (unspecified/stream not constant)

    // constantFrameRate(2)=0, numTemporalLayers(3)=1, temporalIdNested(1)=0,
    // lengthSizeMinusOne(2)=3 (4-byte lengths) -- packed as one byte:
    // 00 001 0 11
    w.u8(0b00001011);

    // Three arrays: VPS, SPS, PPS, each with exactly one NAL unit.
    w.u8(3); // numOfArrays

    auto write_array = [&](uint8_t nal_unit_type, const std::vector<std::byte>& nal) {
        w.u8(uint8_t(0x80 | (nal_unit_type & 0x3F))); // array_completeness=1, reserved=0, type
        w.u16be(1); // numNalus
        w.u16be(uint16_t(nal.size()));
        w.bytes(nal);
    };
    write_array(32, params.vps);
    write_array(33, params.sps);
    write_array(34, params.pps);

    return w.take();
}

} // namespace llcmaf
