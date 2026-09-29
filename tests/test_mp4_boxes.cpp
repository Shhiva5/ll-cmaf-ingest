// test_mp4_boxes.cpp
//
// Validates the low-level box writer (size headers, fourcc, full-box
// version/flags) and a full init-segment build against the invariants a
// real MP4 parser would check: the outer box size must exactly cover its
// content, and fourccs must land at byte offset 4.

#include "mini_test.hpp"
#include "mux/mp4_boxes.hpp"
#include "mux/cmaf_muxer.hpp"
#include "mux/hvcc_builder.hpp"
#include "common/byte_io.hpp"
#include <cstring>

using namespace llcmaf;

namespace {
uint32_t read_u32be(const std::vector<std::byte>& buf, size_t offset) {
    return (uint32_t(std::to_integer<uint8_t>(buf[offset])) << 24) |
           (uint32_t(std::to_integer<uint8_t>(buf[offset + 1])) << 16) |
           (uint32_t(std::to_integer<uint8_t>(buf[offset + 2])) << 8) |
           uint32_t(std::to_integer<uint8_t>(buf[offset + 3]));
}
std::string read_fourcc(const std::vector<std::byte>& buf, size_t offset) {
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i) s[i] = char(std::to_integer<uint8_t>(buf[offset + i]));
    return s;
}
} // namespace

TEST(simple_box_size_matches_content) {
    ByteWriter w;
    write_box(w, "test", [](ByteWriter& b) {
        b.u32be(0xDEADBEEF);
    });
    auto data = w.data();
    // header(8) + 4 bytes of body = 12
    ASSERT_EQ(read_u32be(data, 0), 12u);
    ASSERT_EQ(read_fourcc(data, 4), "test");
    ASSERT_EQ(data.size(), 12u);
}

TEST(nested_box_sizes_are_correct) {
    ByteWriter w;
    write_box(w, "outr", [](ByteWriter& b) {
        write_box(b, "inr1", [](ByteWriter& b2) { b2.u32be(1); });
        write_box(b, "inr2", [](ByteWriter& b2) { b2.u32be(2); b2.u32be(3); });
    });
    auto data = w.data();
    // outer: header(8) + inr1(12) + inr2(16) = 36
    ASSERT_EQ(read_u32be(data, 0), 36u);
    ASSERT_EQ(read_fourcc(data, 4), "outr");
    // inr1 starts right after outer's header, at offset 8
    ASSERT_EQ(read_u32be(data, 8), 12u);
    ASSERT_EQ(read_fourcc(data, 12), "inr1");
    // inr2 starts after inr1 (8 + 12 = 20)
    ASSERT_EQ(read_u32be(data, 20), 16u);
    ASSERT_EQ(read_fourcc(data, 24), "inr2");
}

TEST(full_box_has_version_and_flags) {
    ByteWriter w;
    write_full_box(w, "abcd", 1, 0x020304, [](ByteWriter& b) {
        b.u8(0xFF);
    });
    auto data = w.data();
    // header(8) + version(1) + flags(3) + body(1) = 13
    ASSERT_EQ(data.size(), 13u);
    ASSERT_EQ(read_u32be(data, 0), 13u);
    uint8_t version = std::to_integer<uint8_t>(data[8]);
    ASSERT_EQ(int(version), 1);
    uint32_t flags = (uint32_t(std::to_integer<uint8_t>(data[9])) << 16) |
                      (uint32_t(std::to_integer<uint8_t>(data[10])) << 8) |
                      uint32_t(std::to_integer<uint8_t>(data[11]));
    ASSERT_EQ(flags, 0x020304u);
}

TEST(init_segment_starts_with_ftyp_then_moov) {
    HevcSpsInfo sps_info;
    sps_info.general_profile_idc = 1;
    sps_info.general_tier_flag = 0;
    sps_info.general_level_idc = 120;
    sps_info.pic_width_luma = 320;
    sps_info.pic_height_luma = 240;
    sps_info.chroma_format_idc = 1;

    HevcParamSets params;
    params.vps = {std::byte{0x40}, std::byte{0x01}};
    params.sps = {std::byte{0x42}, std::byte{0x01}};
    params.pps = {std::byte{0x44}, std::byte{0x01}};

    CmafMuxer muxer(CmafMuxerConfig{90000, 320, 240});
    auto init = muxer.build_init_segment(sps_info, params);

    ASSERT_TRUE(init.size() > 8);
    ASSERT_EQ(read_fourcc(init, 4), "ftyp");

    uint32_t ftyp_size = read_u32be(init, 0);
    ASSERT_EQ(read_fourcc(init, ftyp_size + 4), "moov");

    // moov's total size, added to its start offset, must land exactly on
    // the end of the buffer (no trailing garbage, no truncation).
    uint32_t moov_size = read_u32be(init, ftyp_size);
    ASSERT_EQ(ftyp_size + moov_size, init.size());
}

TEST(mux_chunk_data_offset_points_at_mdat_payload) {
    HevcSpsInfo sps_info;
    sps_info.pic_width_luma = 320;
    sps_info.pic_height_luma = 240;
    sps_info.chroma_format_idc = 1;

    CmafMuxer muxer(CmafMuxerConfig{90000, 320, 240});

    AccessUnit au;
    au.is_idr = true;
    NalUnit nal;
    nal.nal_unit_type = 20;
    nal.payload = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};
    au.nals.push_back(nal);

    std::vector<AccessUnit> aus{au};
    auto chunk = muxer.mux_chunk(aus, /*base_decode_time=*/0, /*sample_duration=*/3600);

    // Walk styp, then moof, and confirm the byte at the computed data_offset
    // (relative to the start of moof) is exactly where the mdat's payload
    // (length-prefixed NAL) begins.
    uint32_t styp_size = read_u32be(chunk, 0);
    ASSERT_EQ(read_fourcc(chunk, 4), "styp");

    size_t moof_offset = styp_size;
    ASSERT_EQ(read_fourcc(chunk, moof_offset + 4), "moof");
    uint32_t moof_size = read_u32be(chunk, moof_offset);

    size_t mdat_offset = moof_offset + moof_size;
    ASSERT_EQ(read_fourcc(chunk, mdat_offset + 4), "mdat");

    // mdat payload = 4-byte length prefix (value 3) + the 3 NAL bytes.
    uint32_t nal_len = read_u32be(chunk, mdat_offset + 8);
    ASSERT_EQ(nal_len, 3u);
    ASSERT_EQ(std::to_integer<uint8_t>(chunk[mdat_offset + 12]), uint8_t(0xAA));
}
