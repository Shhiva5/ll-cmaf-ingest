// test_hevc_depacketizer.cpp
//
// Constructs synthetic RTP payloads by hand (per RFC 7798 section 4) rather
// than capturing real packets, because this lets us precisely test each
// payload structure (single NAL / AP / FU) and the FU reassembly edge case
// (a sequence-number gap mid-fragment) in isolation.

#include "mini_test.hpp"
#include "rtp/hevc_depacketizer.hpp"
#include "rtp/rtp_packet.hpp"
#include <vector>

using namespace llcmaf;

namespace {

RtpHeader make_header(uint16_t seq, uint32_t ts, std::span<const std::byte> payload) {
    RtpHeader h;
    h.version = 2;
    h.sequence_number = seq;
    h.timestamp = ts;
    h.payload = payload;
    return h;
}

std::vector<std::byte> bytes(std::initializer_list<uint8_t> vals) {
    std::vector<std::byte> out;
    for (auto v : vals) out.push_back(std::byte{v});
    return out;
}

} // namespace

TEST(depacketizer_passes_through_single_nal) {
    std::vector<NalUnit> received;
    HevcDepacketizer depkt([&](NalUnit n) { received.push_back(std::move(n)); });

    // A "single NAL unit" packet: payload IS the NAL verbatim.
    // nal_unit_type = 1 (TRAIL_R): byte0 = (1 << 1) = 0x02
    auto payload = bytes({0x02, 0x01, 0xAA, 0xBB, 0xCC});
    auto hdr = make_header(100, 9000, payload);
    depkt.on_rtp_packet(hdr, 1000);

    ASSERT_EQ(received.size(), size_t(1));
    ASSERT_EQ(int(received[0].nal_unit_type), 1);
    ASSERT_EQ(received[0].payload.size(), size_t(5));
}

TEST(depacketizer_reassembles_two_fragment_fu) {
    std::vector<NalUnit> received;
    HevcDepacketizer depkt([&](NalUnit n) { received.push_back(std::move(n)); });

    // Original NAL type we're fragmenting: 19 (IDR_W_RADL).
    // FU indicator byte0/byte1: type field in byte0 must be 49 (FU).
    //   byte0 = (49 << 1) | 0 = 0x62 ; byte1 = 0x01 (layerid/tid, arbitrary)
    // FU header byte: S=1,E=0,type=19 -> 0x80 | 19 = 0x93 for start;
    //                 S=0,E=1,type=19 -> 0x40 | 19 = 0x53 for end.
    auto frag1 = bytes({0x62, 0x01, 0x93, 0xDE, 0xAD});       // start fragment
    auto frag2 = bytes({0x62, 0x01, 0x53, 0xBE, 0xEF, 0x00}); // end fragment

    depkt.on_rtp_packet(make_header(200, 5000, frag1), 1000);
    depkt.on_rtp_packet(make_header(201, 5000, frag2), 1010);

    ASSERT_EQ(received.size(), size_t(1));
    ASSERT_EQ(int(received[0].nal_unit_type), 19);
    // Reconstructed payload = 2-byte reconstructed NAL header + frag1's data
    // (minus its 3-byte FU prefix) + frag2's data (minus its 3-byte FU prefix)
    // = 2 + 2 + 3 = 7 bytes.
    ASSERT_EQ(received[0].payload.size(), size_t(7));

    // Reconstructed NAL header must carry nal_unit_type=19, not 49.
    uint8_t reconstructed_type = (std::to_integer<uint8_t>(received[0].payload[0]) >> 1) & 0x3F;
    ASSERT_EQ(int(reconstructed_type), 19);
}

TEST(depacketizer_discards_fu_on_sequence_gap) {
    std::vector<NalUnit> received;
    HevcDepacketizer depkt([&](NalUnit n) { received.push_back(std::move(n)); });

    auto frag1 = bytes({0x62, 0x01, 0x93, 0xDE, 0xAD}); // start, seq 300
    auto frag2 = bytes({0x62, 0x01, 0x53, 0xBE, 0xEF}); // end, but arrives as seq 305 (gap!)

    depkt.on_rtp_packet(make_header(300, 5000, frag1), 1000);
    depkt.on_rtp_packet(make_header(305, 5000, frag2), 1010); // 4-packet gap in between

    // The fragmented NAL must be discarded, not emitted corrupted.
    ASSERT_EQ(received.size(), size_t(0));
}

TEST(depacketizer_unpacks_aggregation_packet) {
    std::vector<NalUnit> received;
    HevcDepacketizer depkt([&](NalUnit n) { received.push_back(std::move(n)); });

    // AP payload: [2-byte AP header, type=48][2-byte size][nal][2-byte size][nal]
    // AP header byte0 = (48 << 1) = 0x60
    auto nal_a = bytes({0x44, 0x01, 0xAA});          // PPS-ish, type 34: (34<<1)=0x44
    auto nal_b = bytes({0x02, 0x01, 0xBB, 0xCC});    // TRAIL_R, type 1: (1<<1)=0x02

    std::vector<std::byte> ap;
    auto push_u16 = [&](uint16_t v) {
        ap.push_back(std::byte(uint8_t(v >> 8)));
        ap.push_back(std::byte(uint8_t(v)));
    };
    ap.push_back(std::byte{0x60}); ap.push_back(std::byte{0x01}); // AP header
    push_u16(uint16_t(nal_a.size()));
    ap.insert(ap.end(), nal_a.begin(), nal_a.end());
    push_u16(uint16_t(nal_b.size()));
    ap.insert(ap.end(), nal_b.begin(), nal_b.end());

    depkt.on_rtp_packet(make_header(400, 7000, ap), 2000);

    ASSERT_EQ(received.size(), size_t(2));
    ASSERT_EQ(int(received[0].nal_unit_type), 34);
    ASSERT_EQ(int(received[1].nal_unit_type), 1);
}
