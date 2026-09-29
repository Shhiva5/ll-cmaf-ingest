// hevc_sps_parser.hpp
//
// Parses just enough of an HEVC Sequence Parameter Set (ITU-T H.265 section
// 7.3.2.2) to build the CMAF `hvcC` box and know the video's dimensions.
// We deliberately do NOT parse the full SPS (VUI timing, scaling lists,
// short/long-term ref pic sets, etc.) -- none of that is needed for muxing,
// and every additional field we parse is another way to crash on a stream
// we didn't test against.
//
// Input to parse_sps() must already have:
//   - the 2-byte NAL header stripped (caller passes payload.subspan(2))
//   - emulation-prevention bytes removed (strip_emulation_prevention())
//
// Reference: Rec. ITU-T H.265 (08/2021), section 7.3.2.2 "Sequence
// parameter set RBSP syntax".

#pragma once

#include <span>
#include <optional>
#include "../common/byte_io.hpp"
#include "../common/types.hpp"

namespace llcmaf {

inline std::optional<HevcSpsInfo> parse_hevc_sps(std::span<const std::byte> sps_rbsp_no_header) {
    try {
        BitReader br(sps_rbsp_no_header);

        br.bits(4);  // sps_video_parameter_set_id
        uint32_t max_sub_layers_minus1 = br.bits(3);
        br.bits(1);  // sps_temporal_id_nesting_flag

        // --- profile_tier_level(1, max_sub_layers_minus1) ---
        HevcSpsInfo info;
        br.bits(2);  // general_profile_space
        info.general_tier_flag = uint8_t(br.bits(1));
        info.general_profile_idc = uint8_t(br.bits(5));

        uint32_t compat_flags = 0;
        for (int i = 0; i < 32; ++i) compat_flags = (compat_flags << 1) | br.bit();
        info.general_profile_compatibility_flags = compat_flags;

        // 48 bits of constraint flags + reserved bits.
        uint64_t constraint = 0;
        for (int i = 0; i < 48; ++i) constraint = (constraint << 1) | br.bit();
        info.general_constraint_indicator_flags = constraint;

        info.general_level_idc = uint8_t(br.bits(8));

        // Sub-layer profile/level presence flags (rarely used in practice;
        // ffmpeg's default encode uses max_sub_layers_minus1 == 0 so this
        // loop typically doesn't execute, but we parse it correctly in case
        // a test stream sets it).
        std::vector<bool> sub_layer_profile_present(max_sub_layers_minus1);
        std::vector<bool> sub_layer_level_present(max_sub_layers_minus1);
        for (uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
            sub_layer_profile_present[i] = br.bit();
            sub_layer_level_present[i] = br.bit();
        }
        if (max_sub_layers_minus1 > 0) {
            for (uint32_t i = max_sub_layers_minus1; i < 8; ++i) br.bits(2); // reserved
        }
        for (uint32_t i = 0; i < max_sub_layers_minus1; ++i) {
            if (sub_layer_profile_present[i]) {
                br.bits(2); br.bits(1); br.bits(5);   // space/tier/idc
                br.bits(32);                          // compatibility flags
                br.bits(48);                          // constraint flags
            }
            if (sub_layer_level_present[i]) br.bits(8);
        }

        // --- back in sps_seq_parameter_set_rbsp() ---
        br.ue(); // sps_seq_parameter_set_id
        info.chroma_format_idc = uint8_t(br.ue());
        if (info.chroma_format_idc == 3) br.bits(1); // separate_colour_plane_flag

        info.pic_width_luma = br.ue();
        info.pic_height_luma = br.ue();

        if (br.bit()) { // conformance_window_flag
            br.ue(); br.ue(); br.ue(); br.ue(); // window offsets, unused for muxing
        }

        info.bit_depth_luma_minus8 = uint8_t(br.ue());
        info.bit_depth_chroma_minus8 = uint8_t(br.ue());

        // We stop here -- log2_max_pic_order_cnt_lsb_minus4 and everything
        // after it isn't needed for the hvcC box or track dimensions.
        return info;
    } catch (const std::out_of_range&) {
        return std::nullopt; // malformed/truncated SPS; caller should treat as unusable
    }
}

} // namespace llcmaf
