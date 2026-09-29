# LL-CMAF Ingest Engine

A C++20 live-streaming ingest engine that receives demuxed HEVC NAL units
over RTP, reassembles them through an adaptive jitter buffer, and packages
them into Low-Latency CMAF (fragmented MP4) chunks delivered over HTTP/1.1
chunked transfer encoding. The pipeline itself has no external media-library
dependencies; ffmpeg is used solely as a test source and as an independent
reference validator for the muxer's output.

This is an independent systems-programming project, built end to end by a
single engineer, covering RTP packet handling, adaptive jitter buffering,
and ISOBMFF/CMAF box-format construction.

## Documentation

| Document | Contents |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Pipeline diagram, design rationale, and documented scope limitations |
| [`docs/RTP_DEPACKETIZATION.md`](docs/RTP_DEPACKETIZATION.md) | RTP receive and RFC 7798 HEVC depacketization |
| [`docs/CMAF_MUXER.md`](docs/CMAF_MUXER.md) | ISOBMFF box construction and the fragmented-MP4 muxer |
| [`docs/LIVE_DELIVERY.md`](docs/LIVE_DELIVERY.md) | HTTP chunked delivery, the jitter buffer, and two defects found during integration testing |
| [`docs/NETWORK_TESTING_AND_LATENCY.md`](docs/NETWORK_TESTING_AND_LATENCY.md) | A decode-order defect case study, the `tc netem` test harness, and latency measurement methodology |

## Implementation status

| Component | Status |
|---|---|
| RTP receive (UDP) | Implemented |
| RFC 7798 HEVC depacketization (single NAL unit, aggregation packet, fragmentation unit) | Implemented, unit tested |
| HEVC SPS parsing (profile, tier, level, resolution, chroma format, bit depth) | Implemented, unit tested against a libx265 reference encode |
| Fragmented MP4 / CMAF muxing (`ftyp`, `moov`, `moof`, `mdat`, `hvcC`) | Implemented, unit tested and validated against `ffprobe`/`ffmpeg` decode |
| HTTP/1.1 chunked-transfer live delivery | Implemented, validated end to end against a live ffmpeg source |
| Jitter buffer: fixed-delay mode | Implemented, unit tested |
| Jitter buffer: adaptive mode (EWMA jitter estimation) | Implemented, unit tested |
| Loss handling (skip-forward on missing frames) | Implemented, unit tested |
| `tc netem` network impairment test harness | Implemented (`scripts/netem_profiles.sh`) |
| Glass-to-glass latency measurement (SEI timestamp injection and extraction) | Methodology documented; not implemented -- see `scripts/measure_latency.md` |
| AV1 support | Out of scope for this version -- see the limitations table in `docs/ARCHITECTURE.md` |
| B-frame / composition-time-offset support | Out of scope for this version; root cause documented in `docs/NETWORK_TESTING_AND_LATENCY.md` |

## Build

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc)
```

## Running the unit tests

```bash
./build/ll_cmaf_tests
```

## Running the engine

```bash
# Terminal 1: start the ingest engine
./build/ll_cmaf_ingest --rtp-port 5004 --http-port 8080 --jitter-mode adaptive

# Terminal 2: stream a synthetic HEVC test source into it
./scripts/gen_test_stream.sh 5004 30

# Terminal 3: play the live stream
ffplay http://localhost:8080/live.cmfv

# Query live jitter buffer and receiver statistics at any time
curl http://localhost:8080/stats.json
```

`/live.cmfv` is a self-contained stream: the init segment (`ftyp`+`moov`) is
sent as the first HTTP chunk on every connection, so a single URL is
sufficient for `ffplay`, VLC, `ffmpeg`, or a browser. `/init.mp4` remains
available as a separate endpoint for MSE-based players that manage the
init segment and media fragments independently.

## Validating the muxer output independently

Confirming correctness against the same demuxer `ffplay` uses:

```bash
ffmpeg -i http://localhost:8080/live.cmfv -f null -
```

Expected result: clean decode, no errors.

Inspecting the raw output on disk:

```bash
timeout 3 curl -s http://localhost:8080/live.cmfv -o live.mp4
ffprobe -show_streams live.mp4
ffmpeg -i live.mp4 -f null -
```

Expected result: `codec_name=hevc`, `probe_score=100`, and a clean decode.

## Project layout

```
src/
  common/    Shared types, byte/bit readers, box writer primitives
  rtp/       RTP header parsing, RFC 7798 HEVC depacketization, UDP receiver
  hevc/      HEVC SPS bit-level parser
  jitter/    Adaptive jitter buffer
  mux/       ISOBMFF box writer, hvcC builder, CMAF muxer
  server/    HTTP/1.1 chunked-transfer server
  app/       Pipeline wiring (ll_cmaf_ingest executable)
tests/       Lightweight custom test framework and unit tests
docs/        Architecture and per-component design documentation
scripts/     Test stream generation, tc netem profiles, latency methodology
```

## Requirements

- C++20 compiler (built and tested with g++ 13)
- CMake 3.16 or later
- POSIX sockets (Linux; `epoll` is not required)
- `ffmpeg`/`ffprobe`, for generating test streams and independently
  validating output (not a build or runtime dependency of the engine itself)
- `tc`/`iproute2`, for the network impairment test harness (typically
  preinstalled on Linux; otherwise `apt install iproute2`)

## Command-line reference

```
ll_cmaf_ingest [options]
  --rtp-port PORT         UDP port to receive HEVC RTP on (default 5004)
  --http-port PORT        HTTP port to serve LL-CMAF on (default 8080)
  --jitter-mode MODE      'fixed' or 'adaptive' (default adaptive)
  --fixed-delay-ms MS     Delay when --jitter-mode=fixed (default 100)
  --base-delay-ms MS      Adaptive mode floor delay (default 40)
  --jitter-multiplier K   Adaptive mode: delay = base + K * jitter (default 4.0)
```

Endpoints: `GET /init.mp4`, `GET /live.cmfv` (chunked transfer encoding),
`GET /stats.json`.
