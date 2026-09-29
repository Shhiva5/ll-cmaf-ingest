# Architecture

```
 ffmpeg (encoder + RTP sender)
        │  UDP/RTP, HEVC, 90kHz clock
        ▼
 ┌──────────────────────┐
 │   RtpReceiver        │  src/rtp/rtp_receiver.hpp
 │   (own thread)       │  - recv() loop, parses RTP header
 └─────────┬────────────┘  - timestamps arrival with steady_clock
           │ RtpHeader + arrival time
           ▼
 ┌──────────────────────┐
 │ HevcDepacketizer     │  src/rtp/hevc_depacketizer.hpp
 │ (RFC 7798)           │  - single NAL / aggregation packet / fragmentation unit
 └─────────┬────────────┘
           │ NalUnit (one at a time, arrival order)
           ▼
 ┌──────────────────────┐
 │ AccessUnitAssembler  │  src/app/ingest_main.cpp
 │                      │  - groups NALs sharing an RTP timestamp into one AU
 └─────────┬────────────┘
           │ AccessUnit
           ▼
 ┌──────────────────────┐
 │   JitterBuffer       │  src/jitter/jitter_buffer.hpp
 │   (own thread)       │  - PTS-ordered reassembly
 │                      │  - EWMA jitter estimate -> adaptive playout delay
 │                      │  - skip-forward on loss (no indefinite stall)
 └─────────┬────────────┘
           │ AccessUnit, released at a controlled pace
           ▼
 ┌──────────────────────┐
 │   CmafMuxer          │  src/mux/cmaf_muxer.hpp
 │                      │  - ftyp+moov once (init segment)
 │                      │  - styp+moof+mdat per chunk
 └─────────┬────────────┘
           │ bytes
           ▼
 ┌──────────────────────┐
 │ HttpChunkedServer    │  src/server/http_chunked_server.hpp
 │ (accept thread +     │  - /init.mp4, /live.cmfv (chunked), /stats.json
 │  thread per viewer)  │
 └─────────┬────────────┘
           │ HTTP/1.1 chunked transfer encoding
           ▼
      browser (MSE) / ffplay / hls.js
```

## Design rationale

**Blocking sockets on dedicated threads, rather than io_uring or epoll.**
At single-stream, low-single-digit-Mbps HEVC bitrates, a blocking `recv()`
loop on a dedicated thread keeps pace with the source without difficulty.
An io_uring-based design becomes justified at hundreds of concurrent
streams; at this project's scale it would substantially increase
implementation and debugging surface for no measurable benefit. See
`docs/RTP_DEPACKETIZATION.md`.

**One CMAF chunk per access unit, rather than per N frames.** Low-latency
CMAF delivers a chunk as soon as a frame is ready, rather than
accumulating a batch before delivery; this is the defining property of the
format. The trade-off is increased HTTP chunk-framing overhead per frame,
which is immaterial at this project's scale.

**`default-base-is-moof` in `tfhd`, rather than absolute file offsets.**
Streaming over HTTP chunked encoding has no concept of a file with a fixed
length; each `moof`/`mdat` pair must be self-describing relative to its
own start. This is a functional requirement for the format to operate over
a live HTTP connection, not a stylistic choice.

**PTS-ordered jitter buffer with an implicit decode-order assumption.**
The jitter buffer reorders access units by presentation timestamp, which
is correct and necessary to compensate for out-of-order UDP delivery, but
assumes the input stream's decode order and presentation order are
identical -- equivalent to assuming no B-frames. See the decode-order
defect documented in `docs/NETWORK_TESTING_AND_LATENCY.md` for the
consequence of violating this assumption, its diagnosis, the applied fix,
and the scope of a complete solution.

**Custom test framework rather than GoogleTest or Catch2.** This
eliminates external build dependencies; `cmake && make` with only a
compiler and the standard library is sufficient. See `tests/mini_test.hpp`.

## Known limitations

The following are deliberate, documented scope decisions rather than
oversights. Each entry includes the corresponding extension path.

| Limitation | Rationale | Extension path |
|---|---|---|
| No B-frame / composition-time-offset (`ctts`) support | Test streams are encoded with `bframes=0`; see the decode-order defect in `docs/NETWORK_TESTING_AND_LATENCY.md` | Add a `ctts` box and modify `JitterBuffer` to track DTS/PTS separately |
| No retransmit/NACK on loss | Skip-forward to the next available frame is simpler to reason about and still demonstrates the relevant jitter-buffer trade-offs | Add RTCP NACK generation and a retransmit buffer on the sender |
| AV1 not implemented | HEVC provides sufficient parsing depth for the project's scope | Mirror `src/hevc/` with `src/av1/`, substituting an OBU parser for the NAL parser |
| Thread-per-viewer HTTP server | Adequate for a small number of concurrent viewers | Migrate to epoll or io_uring for higher concurrency |
| No TLS | Outside the scope of this project | Deploy behind a reverse proxy (nginx, Caddy) for TLS termination |
| Sequential broadcast to viewers | A slow viewer's `write()` call briefly blocks the muxer thread | Give each viewer an independent outbound queue and writer thread |
