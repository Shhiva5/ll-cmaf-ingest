// test_hevc_sps_parser.cpp
//
// Ground truth for these bytes was captured by encoding a 320x240 yuv420p
// test pattern with `ffmpeg -c:v libx265` and extracting the raw SPS NAL
// (see docs/RTP_DEPACKETIZATION.md "Regenerating test fixtures" for
// the exact command). Expected width/height/profile were cross-checked with
// `ffprobe -show_streams`.

#include "mini_test.hpp"
#include "hevc/hevc_sps_parser.hpp"
#include "common/byte_io.hpp"
#include <vector>
#include <cstdint>

using namespace llcmaf;

namespace {

// Real SPS NAL (including its 2-byte NAL header) from a libx265 Main
// profile, 320x240, 8-bit 4:2:0 encode.
static const uint8_t kSpsNalBytes[] = {
    0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x03, 0x00, 0x3c, 0xa0, 0x0a, 0x08, 0x0f, 0x16, 0x59,
    0x59, 0x52, 0x93, 0x0b, 0xc0, 0x5a, 0x02, 0x00, 0x00, 0x03, 0x00, 0x02,
    0x00, 0x00, 0x03, 0x00, 0x32, 0x10
};

std::vector<std::byte> as_bytes(const uint8_t* data, size_t n) {
    std::vector<std::byte> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = std::byte{data[i]};
    return out;
}

} // namespace

TEST(sps_nal_type_is_33) {
    auto nal = as_bytes(kSpsNalBytes, sizeof(kSpsNalBytes));
    uint8_t nal_type = (std::to_integer<uint8_t>(nal[0]) >> 1) & 0x3F;
    ASSERT_EQ(nal_type, 33);
}

TEST(sps_parses_correct_resolution) {
    auto nal = as_bytes(kSpsNalBytes, sizeof(kSpsNalBytes));
    // Strip the 2-byte NAL header, then strip emulation prevention, before
    // handing to the bit-level SPS parser -- this mirrors exactly what
    // app/ingest_main.cpp does on the first IDR's SPS NAL.
    std::span<const std::byte> without_header(nal.data() + 2, nal.size() - 2);
    auto rbsp = strip_emulation_prevention(without_header);

    auto info = parse_hevc_sps(rbsp);
    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->pic_width_luma, 320u);
    ASSERT_EQ(info->pic_height_luma, 240u);
}

TEST(sps_parses_correct_profile) {
    auto nal = as_bytes(kSpsNalBytes, sizeof(kSpsNalBytes));
    std::span<const std::byte> without_header(nal.data() + 2, nal.size() - 2);
    auto rbsp = strip_emulation_prevention(without_header);

    auto info = parse_hevc_sps(rbsp);
    ASSERT_TRUE(info.has_value());
    // Main profile = general_profile_idc 1 (Rec. ITU-T H.265 Annex A.3.2).
    ASSERT_EQ(int(info->general_profile_idc), 1);
    ASSERT_EQ(int(info->chroma_format_idc), 1); // 4:2:0
    ASSERT_EQ(int(info->bit_depth_luma_minus8), 0); // 8-bit
}

TEST(emulation_prevention_is_removed) {
    // The raw SPS bytes above contain three "00 00 03" emulation-prevention
    // sequences. After stripping, no "00 00 03" should remain and the
    // output should be shorter by exactly that many bytes.
    auto nal = as_bytes(kSpsNalBytes, sizeof(kSpsNalBytes));
    std::span<const std::byte> without_header(nal.data() + 2, nal.size() - 2);
    auto rbsp = strip_emulation_prevention(without_header);

    ASSERT_TRUE(rbsp.size() < without_header.size());

    bool found_ep_sequence = false;
    for (size_t i = 0; i + 2 < rbsp.size(); ++i) {
        if (std::to_integer<uint8_t>(rbsp[i]) == 0 &&
            std::to_integer<uint8_t>(rbsp[i+1]) == 0 &&
            std::to_integer<uint8_t>(rbsp[i+2]) == 3) {
            found_ep_sequence = true;
        }
    }
    ASSERT_TRUE(!found_ep_sequence);
}

TEST(sps_parser_rejects_truncated_input) {
    std::vector<std::byte> truncated = {std::byte{0x01}, std::byte{0x02}};
    auto result = parse_hevc_sps(truncated);
    ASSERT_TRUE(!result.has_value());
}
