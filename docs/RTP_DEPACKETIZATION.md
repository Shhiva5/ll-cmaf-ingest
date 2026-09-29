# RTP Receive and HEVC Depacketization

**Source:** `src/rtp/rtp_packet.hpp`, `src/rtp/hevc_depacketizer.hpp`,
`src/rtp/rtp_receiver.hpp`, `src/hevc/hevc_sps_parser.hpp`

## Purpose

This stage converts a UDP stream of RTP packets into a sequence of complete
HEVC NAL units, and extracts video parameters (resolution, profile, chroma
format, bit depth) from the stream's Sequence Parameter Set (SPS).

## Verification

```bash
scripts/gen_test_stream.sh          # streams HEVC over RTP to :5004
./build/ll_cmaf_ingest --rtp-port 5004 --http-port 8080
```

A successful run logs `[init] built init segment (735 bytes)`, confirming
that VPS/SPS/PPS were captured from real RTP-delivered NAL units and the
SPS was parsed correctly.

## RFC 7798 payload structures

ffmpeg's default RTP packetizer selects among three payload structures
depending on NAL size, per RFC 7798 ("RTP Payload Format for High
Efficiency Video Coding"):

- **Single NAL unit packet.** Most P/B-slice NALs fit within one RTP
  packet's payload budget. This is the common case.
- **Aggregation Packet (AP).** VPS, SPS, and PPS are each small (tens of
  bytes), so multiple parameter-set NALs are often packed into a single RTP
  packet rather than sent as three separate packets.
- **Fragmentation Unit (FU).** IDR frames are substantially larger than P
  frames and routinely exceed one packet's payload budget, requiring
  fragmentation across multiple RTP packets with start (`S`) and end (`E`)
  markers.

All three structures require correct handling. Implementing only
single-NAL support produces working playback until the first IDR frame
following the initial one, at which point fragmentation is exercised for
the first time on a long-running stream (see `scripts/gen_test_stream.sh`'s
`keyint` parameter for controlling this interval during testing).

## FU reassembly: two correctness requirements

1. **Reconstructing the original NAL header.** An FU's leading two bytes
   carry `nal_unit_type = 49` (the FU marker itself), not the original
   type; the true type is encoded in the low six bits of the FU header
   byte, present on every fragment. The reassembled NAL requires a
   reconstructed two-byte header carrying the original type before being
   passed downstream. Omitting this step causes every fragmented NAL
   (SPS/PPS/PPS aggregation and all IDR frames, in practice) to be
   misclassified as type 49 by every consumer -- SPS/PPS detection, IDR
   detection, and the muxer alike.

2. **Sequence gaps mid-fragment.** RFC 7798 does not prohibit packet loss
   or reordering within a fragmented NAL. If an intermediate fragment is
   lost, concatenating the remaining fragments produces a NAL with valid
   Annex-B framing that nonetheless decodes to corrupted video, or may
   crash a lenient decoder. `HevcDepacketizer::handle_fragmentation_unit`
   detects the sequence-number gap and discards the in-progress
   fragmentation unit rather than emitting corrupted output.

Both requirements are covered in `tests/test_hevc_depacketizer.cpp`:
`depacketizer_reassembles_two_fragment_fu` verifies header reconstruction;
`depacketizer_discards_fu_on_sequence_gap` verifies gap handling.

## SPS parsing scope

`hevc_sps_parser.hpp` parses only through `bit_depth_chroma_minus8` in the
SPS RBSP -- the fields required to construct the `hvcC` box and size the
video track. Subsequent fields (`log2_max_pic_order_cnt_lsb`,
short/long-term reference picture sets, VUI timing information) are not
parsed. Each additional field parsed introduces additional bit-level spec
compliance surface and additional failure modes on streams outside the
test fixtures' coverage; the parser is scoped to exactly what the muxer
requires.

## Regenerating test fixtures

The SPS bytes in `tests/test_hevc_sps_parser.cpp` and the FU/AP
construction logic in `tests/test_hevc_depacketizer.cpp` are extracted
from, or modeled on, a real libx265 encode. To regenerate:

```bash
ffmpeg -y -f lavfi -i testsrc=size=320x240:rate=25:duration=1 \
  -pix_fmt yuv420p -c:v libx265 -x265-params "keyint=25:log-level=error" \
  -preset ultrafast -f hevc test.hevc

python3 scripts/extract_nal_fixtures.py test.hevc
```

`scripts/extract_nal_fixtures.py` extracts VPS/SPS/PPS from any Annex-B
HEVC elementary stream and prints them as C++ arrays suitable for direct
use in test fixtures.

## Assumptions

See `docs/ARCHITECTURE.md` for the complete limitations table. The two
assumptions specific to this stage:

- **No B-frames.** RTP timestamps are treated as arriving in
  approximately presentation-time order at the access-unit assembly
  layer. This holds for streams encoded with `bframes=0`; see
  `docs/NETWORK_TESTING_AND_LATENCY.md` for the consequence of violating
  this assumption without a corresponding change to the jitter buffer.
- **Single slice per frame.** Multi-slice frames would require the
  access-unit assembler to recognize `first_slice_segment_in_pic_flag`
  rather than grouping strictly by RTP timestamp. ffmpeg's default HEVC
  encoder does not slice-split, so this path is untested.
