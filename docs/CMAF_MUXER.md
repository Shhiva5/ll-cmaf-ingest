# CMAF Muxer

**Source:** `src/mux/mp4_boxes.hpp`, `src/mux/hvcc_builder.hpp`, `src/mux/cmaf_muxer.hpp`

## Purpose

This stage converts a sequence of access units into spec-conformant
fragmented MP4, verifiable with standard tooling (`ffprobe`, `ffmpeg`) and
playable by real clients.

## Verification

```bash
curl -s --max-time 3 http://localhost:8080/live.cmfv -o live.mp4
ffprobe -show_streams live.mp4     # codec_name=hevc, profile=Main, probe_score=100
ffmpeg -i live.mp4 -f null -       # decodes cleanly, no reference errors
```

This validation loop caught a real decode-order defect during development;
see `docs/NETWORK_TESTING_AND_LATENCY.md` for the diagnosis.

## Box tree

```
ftyp                                   (once)
moov                                   (once)
  mvhd
  trak
    tkhd
    mdia
      mdhd, hdlr
      minf
        vmhd, dinf > dref > url
        stbl
          stsd > hvc1 > hvcC           decoder configuration
          stts, stsc, stsz, stco       required boxes, zero entries
  mvex > trex                          marks the file as fragmented

--- per chunk ---
styp                                   brand re-announcement
moof
  mfhd                                 fragment sequence number
  traf
    tfhd  (flags=default-base-is-moof)
    tfdt  (64-bit baseMediaDecodeTime)
    trun  (per-sample duration/size/flags + data_offset)
mdat                                   NAL bytes, length-prefixed
```

The `stts`/`stsc`/`stsz`/`stco` boxes inside `stbl` are required to be
present in a fragmented file, with zero entries -- all sample bookkeeping
for a fragmented track resides in each fragment's `traf`. Omitting these
boxes is accepted by lenient parsers but rejected by stricter ones; they
must be written, empty, rather than omitted.

## `trun`'s `data_offset` field

`data_offset` specifies, in bytes, where the first sample's data begins
relative to a base offset. With `default-base-is-moof` set in `tfhd`, that
base is the first byte of the `moof` box itself. The correct value is
therefore not known until the entire `moof` box (`mfhd` + `traf`, including
the `tfhd`, `tfdt`, and `trun` boxes themselves) has been fully serialized
and its total size is known -- `data_offset` must point past the complete
`moof` and past the 8-byte `mdat` header to the first sample byte.

The implementation (`CmafMuxer::mux_chunk`, `CmafMuxer::write_trun`)
resolves this with a reserve-then-patch pattern: the `trun` box is written
with a placeholder value of zero in the `data_offset` field, and the byte
offset at which that placeholder was written is recorded. Once the
complete `moof` has been serialized, `data_offset` is computed as
`moof.size() + 8` and patched directly into the buffer at the recorded
offset. This is the same pattern used for every box's `size` field;
`data_offset` is a second field requiring identical treatment.

An incorrect `data_offset` does not produce a parser error. Every box's own
size remains internally consistent, so the file structure parses as valid
ISOBMFF; only the pointer into `mdat` is wrong. The observable failure mode
is typically a decoder reporting a corrupt NAL unit or displaying garbled
video, not a rejected file. `tests/test_mp4_boxes.cpp`'s
`mux_chunk_data_offset_points_at_mdat_payload` test verifies correctness
by resolving `data_offset` and confirming the expected NAL length-prefix
and payload appear at that exact byte position, since no parser-level
validation would surface an incorrect value.

## Sample flags and sync samples

Each `trun` entry declares whether its sample is independently decodable
(a sync sample, i.e. an IDR) via a 32-bit `sample_flags` field. This
implementation uses the same constants used by Shaka Packager, Bento4, and
dash.js:

```cpp
kSyncSampleFlags    = 0x02000000
kNonSyncSampleFlags = 0x01010000
```

A player uses this field to determine valid entry points for a mid-stream
join.

## `hvc1` versus `hev1`

ISOBMFF defines two sample entry types for HEVC. `hvc1` stores VPS/SPS/PPS
exclusively in the `hvcC` box (out-of-band); `hev1` permits them to also
appear inline in the bitstream. CMAF and LL-HLS expect `hvc1`: players
parse parameter sets once from the init segment and do not expect them to
repeat in every fragment's `mdat`. This muxer places only VCL NALs in each
chunk's `mdat`, consistent with `hvc1` semantics.
