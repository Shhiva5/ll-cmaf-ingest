// cmaf_muxer.hpp
//
// Turns a stream of AccessUnits into CMAF-compatible fragmented MP4:
//
//   1. build_init_segment() -- called once, after we've seen VPS/SPS/PPS.
//      Produces ftyp + moov (with mvex, marking the file as fragmented).
//
//   2. mux_chunk() -- called once per delivery chunk (every 1-2 frames, per
//      the low-latency design). Produces styp + moof + mdat for exactly the
//      access units passed in.
//
// This project treats a "CMAF segment" as an IDR-aligned group of chunks
// (for playlist/manifest bookkeeping -- see docs/LIVE_DELIVERY.md) and a "CMAF chunk"
// as the actual moof+mdat delivery unit. The muxer itself only deals in
// chunks; the caller (app/ingest_main.cpp) decides when to start a new
// segment (on every IDR) versus continue the current one.
//
// Key correctness details (see docs/ARCHITECTURE.md for the "why"):
//   - default-base-is-moof (tfhd flag 0x020000) so each fragment is
//     self-contained and doesn't need to know the mdat's absolute file
//     offset -- required for a fragment to be meaningful when streamed
//     over HTTP chunked-encoding before the "file" even has a fixed length.
//   - tfdt carries the absolute decode time in the track's timescale, so a
//     player joining mid-stream (or a chunk arriving after a gap) can still
//     place it correctly.
//   - NAL units are rewritten from Annex-B (00 00 01 / 00 00 00 01 start
//     codes) to length-prefixed (4-byte big-endian length + NAL bytes) in
//     the mdat, per ISOBMFF's `avc1`/`hvc1` sample format -- this must match
//     lengthSizeMinusOne in the hvcC box (we use 4 bytes / value 3 there).

#pragma once

#include <vector>
#include <span>
#include <cstdint>
#include "../common/types.hpp"
#include "mp4_boxes.hpp"
#include "hvcc_builder.hpp"

namespace llcmaf {

struct CmafMuxerConfig {
    uint32_t timescale = 90000;   // track timescale; matches RTP clock for simplicity
    uint32_t width = 1920;
    uint32_t height = 1080;
};

class CmafMuxer {
public:
    explicit CmafMuxer(CmafMuxerConfig cfg) : cfg_(cfg) {}

    // Builds ftyp + moov. Call once, as soon as VPS/SPS/PPS are available
    // (typically parsed from the first IDR access unit).
    std::vector<std::byte> build_init_segment(const HevcSpsInfo& sps_info,
                                               const HevcParamSets& params) {
        ByteWriter out;
        write_ftyp(out);
        write_moov(out, sps_info, params);
        return out.take();
    }

    // Builds styp + moof + mdat for one chunk (one or more access units that
    // should be delivered together). `base_decode_time` is the tfdt value:
    // the decode timestamp of the first sample in this chunk, in
    // cfg_.timescale units, monotonically increasing across the whole track
    // (i.e. NOT reset per segment -- only the CMAF "segment" boundary in the
    // manifest resets, the underlying timeline does not).
    std::vector<std::byte> mux_chunk(std::span<const AccessUnit> access_units,
                                      uint64_t base_decode_time_in_timescale,
                                      uint32_t sample_duration_in_timescale) {
        ByteWriter out;
        write_styp(out);

        // We need the trun's data_offset patched to point past moof -> mdat
        // header. Build moof first into a temp buffer so we know its size,
        // then the mdat, then combine -- simplest approach that stays
        // correct without a second box-size pre-pass.
        ByteWriter moof;
        size_t trun_dataoffset_field_pos = 0; // filled in by write_traf
        write_moof(moof, access_units, base_decode_time_in_timescale,
                   sample_duration_in_timescale, trun_dataoffset_field_pos);

        // data_offset in trun = size of moof box + 8 (mdat header) since we
        // use default-base-is-moof (offset is relative to the start of moof).
        uint32_t data_offset = uint32_t(moof.size() + 8);
        moof.patch_u32be(trun_dataoffset_field_pos, data_offset);

        out.bytes(moof.data());
        write_mdat(out, access_units);

        seq_number_++;
        return out.take();
    }

private:
    // --- ftyp -----------------------------------------------------------
    void write_ftyp(ByteWriter& out) {
        write_box(out, "ftyp", [&](ByteWriter& w) {
            w.fourcc("cmf2");    // major brand: CMAF
            w.u32be(0);          // minor version
            w.fourcc("cmf2");
            w.fourcc("iso6");    // supports fragmented, styp-based delivery
            w.fourcc("hvc1");    // codec brand hint
        });
    }

    void write_styp(ByteWriter& out) {
        write_box(out, "styp", [&](ByteWriter& w) {
            w.fourcc("cmf2");
            w.u32be(0);
            w.fourcc("cmf2");
            w.fourcc("iso6");
        });
    }

    // --- moov -------------------------------------------------------------
    void write_moov(ByteWriter& out, const HevcSpsInfo& sps_info,
                     const HevcParamSets& params) {
        write_box(out, "moov", [&](ByteWriter& w) {
            write_mvhd(w);
            write_trak(w, sps_info, params);
            write_mvex(w);
        });
    }

    void write_mvhd(ByteWriter& out) {
        write_full_box(out, "mvhd", 0, 0, [&](ByteWriter& w) {
            w.u32be(0); w.u32be(0);          // creation/modification time
            w.u32be(cfg_.timescale);
            w.u32be(0);                      // duration unknown (fragmented/live)
            w.u32be(0x00010000);             // rate = 1.0
            w.u16be(0x0100);                 // volume = 1.0
            w.u16be(0);                      // reserved
            w.u32be(0); w.u32be(0);          // reserved[2]
            // unity matrix
            const uint32_t matrix[9] = {0x00010000,0,0, 0,0x00010000,0, 0,0,0x40000000};
            for (auto v : matrix) w.u32be(v);
            for (int i = 0; i < 6; ++i) w.u32be(0); // pre_defined
            w.u32be(2); // next_track_ID
        });
    }

    void write_trak(ByteWriter& out, const HevcSpsInfo& sps_info,
                     const HevcParamSets& params) {
        write_box(out, "trak", [&](ByteWriter& w) {
            write_tkhd(w);
            write_box(w, "mdia", [&](ByteWriter& w2) {
                write_mdhd(w2);
                write_hdlr(w2);
                write_minf(w2, sps_info, params);
            });
        });
    }

    void write_tkhd(ByteWriter& out) {
        write_full_box(out, "tkhd", 0, /*flags=*/0x000007, [&](ByteWriter& w) { // enabled|in_movie|in_preview
            w.u32be(0); w.u32be(0);      // creation/modification time
            w.u32be(1);                  // track_ID
            w.u32be(0);                  // reserved
            w.u32be(0);                  // duration
            w.u32be(0); w.u32be(0);      // reserved[2]
            w.u16be(0);                  // layer
            w.u16be(0);                  // alternate_group
            w.u16be(0);                  // volume (0 for video)
            w.u16be(0);                  // reserved
            const uint32_t matrix[9] = {0x00010000,0,0, 0,0x00010000,0, 0,0,0x40000000};
            for (auto v : matrix) w.u32be(v);
            w.u32be(cfg_.width << 16);   // width, 16.16 fixed point
            w.u32be(cfg_.height << 16);  // height, 16.16 fixed point
        });
    }

    void write_mdhd(ByteWriter& out) {
        write_full_box(out, "mdhd", 0, 0, [&](ByteWriter& w) {
            w.u32be(0); w.u32be(0);       // creation/modification time
            w.u32be(cfg_.timescale);
            w.u32be(0);                   // duration unknown
            w.u16be(0x55C4);              // language = "und"
            w.u16be(0);                   // pre_defined
        });
    }

    void write_hdlr(ByteWriter& out) {
        write_full_box(out, "hdlr", 0, 0, [&](ByteWriter& w) {
            w.u32be(0);           // pre_defined
            w.fourcc("vide");     // handler_type
            w.u32be(0); w.u32be(0); w.u32be(0); // reserved[3]
            const char name[] = "LLCmafVideoHandler";
            for (char c : name) w.u8(uint8_t(c)); // NUL-terminated string
        });
    }

    void write_minf(ByteWriter& out, const HevcSpsInfo& sps_info,
                     const HevcParamSets& params) {
        write_box(out, "minf", [&](ByteWriter& w) {
            write_full_box(w, "vmhd", 0, 1, [&](ByteWriter& w2) {
                w2.u16be(0); w2.u16be(0); w2.u16be(0); w2.u16be(0); // graphicsmode + opcolor
            });
            write_box(w, "dinf", [&](ByteWriter& w2) {
                write_full_box(w2, "dref", 0, 0, [&](ByteWriter& w3) {
                    w3.u32be(1); // entry_count
                    // flags=1 on the "url " entry means "media data is in
                    // this same file" -- no actual URL string follows.
                    write_full_box(w3, "url ", 0, 1, [&](ByteWriter&) {});
                });
            });
            write_box(w, "stbl", [&](ByteWriter& w2) {
                write_stsd(w2, sps_info, params);
                // Empty sample tables -- all actual sample info lives in moof/traf
                // for a fragmented track, but these boxes are still required to
                // exist (with zero entries) for spec compliance / player leniency.
                write_full_box(w2, "stts", 0, 0, [&](ByteWriter& w3) { w3.u32be(0); });
                write_full_box(w2, "stsc", 0, 0, [&](ByteWriter& w3) { w3.u32be(0); });
                write_full_box(w2, "stsz", 0, 0, [&](ByteWriter& w3) { w3.u32be(0); w3.u32be(0); });
                write_full_box(w2, "stco", 0, 0, [&](ByteWriter& w3) { w3.u32be(0); });
            });
        });
    }

    void write_stsd(ByteWriter& out, const HevcSpsInfo& sps_info,
                     const HevcParamSets& params) {
        write_full_box(out, "stsd", 0, 0, [&](ByteWriter& w) {
            w.u32be(1); // entry_count
            write_box(w, "hvc1", [&](ByteWriter& w2) {
                // VisualSampleEntry fields (ISO/IEC 14496-12 12.1.3.2)
                w2.u32be(0); w2.u16be(0);      // reserved[6]
                w2.u16be(1);                    // data_reference_index
                w2.u16be(0); w2.u16be(0);      // pre_defined, reserved
                w2.u32be(0); w2.u32be(0); w2.u32be(0); // pre_defined[3]
                w2.u16be(uint16_t(cfg_.width));
                w2.u16be(uint16_t(cfg_.height));
                w2.u32be(0x00480000);           // horizresolution 72dpi
                w2.u32be(0x00480000);           // vertresolution 72dpi
                w2.u32be(0);                     // reserved
                w2.u16be(1);                     // frame_count
                for (int i = 0; i < 32; ++i) w2.u8(0); // compressorname (empty, padded)
                w2.u16be(0x0018);                // depth = 24
                w2.u16be(uint16_t(0xFFFF));      // pre_defined = -1

                write_box(w2, "hvcC", [&](ByteWriter& w3) {
                    auto body = build_hvcc_box_body(sps_info, params);
                    w3.bytes(body);
                });
            });
        });
    }

    void write_mvex(ByteWriter& out) {
        write_box(out, "mvex", [&](ByteWriter& w) {
            write_full_box(w, "trex", 0, 0, [&](ByteWriter& w2) {
                w2.u32be(1); // track_ID
                w2.u32be(1); // default_sample_description_index
                w2.u32be(0); // default_sample_duration (we set it explicitly per trun)
                w2.u32be(0); // default_sample_size
                w2.u32be(0); // default_sample_flags
            });
        });
    }

    // --- moof / traf / trun -------------------------------------------
    void write_moof(ByteWriter& out, std::span<const AccessUnit> aus,
                     uint64_t base_decode_time, uint32_t sample_duration,
                     size_t& trun_dataoffset_field_pos_out) {
        write_box(out, "moof", [&](ByteWriter& w) {
            write_full_box(w, "mfhd", 0, 0, [&](ByteWriter& w2) {
                w2.u32be(seq_number_);
            });
            write_traf(w, aus, base_decode_time, sample_duration,
                       trun_dataoffset_field_pos_out);
        });
    }

    void write_traf(ByteWriter& out, std::span<const AccessUnit> aus,
                     uint64_t base_decode_time, uint32_t sample_duration,
                     size_t& trun_dataoffset_field_pos_out) {
        write_box(out, "traf", [&](ByteWriter& w) {
            // tfhd: default-base-is-moof (0x020000) so data_offset in trun is
            // relative to the moof start, not an absolute file offset --
            // essential for HTTP chunked streaming where there is no single
            // seekable "file".
            write_full_box(w, "tfhd", 0, /*flags=*/0x020000, [&](ByteWriter& w2) {
                w2.u32be(1); // track_ID
            });

            write_full_box(w, "tfdt", 1 /*version 1 = 64-bit baseMediaDecodeTime*/, 0,
                            [&](ByteWriter& w2) {
                w2.u64be(base_decode_time);
            });

            write_trun(w, aus, sample_duration, trun_dataoffset_field_pos_out);
        });
    }

    // Per-sample flags, matching the de facto constants used by Shaka
    // Packager / Bento4 / dash.js for the ISOBMFF sample_flags bitfield
    // (ISO/IEC 14496-12 8.8.3.1): sample_depends_on / sample_is_non_sync.
    static constexpr uint32_t kSyncSampleFlags    = 0x02000000; // IDR / sync sample
    static constexpr uint32_t kNonSyncSampleFlags = 0x01010000; // depends on a sync sample

    void write_trun(ByteWriter& out, std::span<const AccessUnit> aus,
                     uint32_t sample_duration, size_t& dataoffset_field_pos_out) {
        // trun flags: data-offset-present (0x000001) | sample-duration-present
        // (0x000100) | sample-size-present (0x000200) | sample-flags-present
        // (0x000400). We give every sample explicit duration/size/flags
        // rather than relying on trex defaults, for clarity and because our
        // sample durations can vary slightly under real (non-CFR) input.
        constexpr uint32_t flags = 0x000001 | 0x000100 | 0x000200 | 0x000400;
        write_full_box(out, "trun", 0, flags, [&](ByteWriter& w) {
            w.u32be(uint32_t(aus.size())); // sample_count

            // data_offset is unknown until the whole moof (and therefore the
            // mdat's start) has been fully serialized. Record where this
            // field lands in the output buffer so mux_chunk() can patch it
            // afterwards, and write a zero placeholder for now.
            dataoffset_field_pos_out = w.size();
            w.u32be(0); // placeholder data_offset

            for (const auto& au : aus) {
                uint32_t sample_size = 0;
                for (const auto& nal : au.nals) {
                    sample_size += 4 /*length prefix*/ + uint32_t(nal.payload.size());
                }
                w.u32be(sample_duration);
                w.u32be(sample_size);
                w.u32be(au.is_idr ? kSyncSampleFlags : kNonSyncSampleFlags);
            }
        });
    }

    void write_mdat(ByteWriter& out, std::span<const AccessUnit> aus) {
        write_box(out, "mdat", [&](ByteWriter& w) {
            for (const auto& au : aus) {
                for (const auto& nal : au.nals) {
                    // ISOBMFF sample format: 4-byte big-endian length prefix
                    // + NAL bytes (no Annex-B start code, no emulation
                    // prevention removal -- those stay as encoded).
                    w.u32be(uint32_t(nal.payload.size()));
                    w.bytes(nal.payload);
                }
            }
        });
    }

    CmafMuxerConfig cfg_;
    uint32_t seq_number_ = 1;
};

} // namespace llcmaf
