# Glass-to-glass latency measurement: methodology

This file documents the measurement approach referenced from
`docs/NETWORK_TESTING_AND_LATENCY.md`. It specifies a component that is
documented but not yet implemented in the current codebase; read this
before implementing it so the SEI encoding on the sender side and the
extraction on the client side agree on a format.

## What "glass-to-glass" means here

Time from "a frame exists" (the moment the encoder was handed a raw frame)
to "a frame is on screen" (the moment the client-side decoder produces that
frame for rendering). This deliberately excludes true camera-to-eyeball
optics/display latency, which needs different (usually hardware) measurement
tooling entirely.

## SEI payload format

Use an unregistered user data SEI message (`nal_unit_type` 39, prefix SEI,
`payload_type` 5 per Rec. ITU-T H.265 D.2.6):

```
uuid_iso_iec_11578   (16 bytes) -- pick a fixed UUID for this project, e.g.
                                    all-zero except the last 8 bytes hold
                                    the payload described below (simplest
                                    approach for a single-purpose tool; a
                                    real UUID is not required for this
                                    purpose)
capture_time_ns       (8 bytes, big-endian uint64) -- nanoseconds since an
                                    arbitrary but *fixed-for-the-run* epoch,
                                    from CLOCK_MONOTONIC on the encoder host
```

Since sender and receiver are the same machine for this test setup, using
`CLOCK_MONOTONIC` (not wall clock) avoids NTP jump issues and does not
require any clock synchronization protocol.

## Sender side (not yet implemented)

Inject the SEI NAL immediately before each frame's VCL NALs, at the moment
the frame is hand off to the encoder. The cleanest way to do this without
patching libx265 itself: use ffmpeg's `-vf` filter chain with a small custom
filter, or a simpler alternative -- a wrapper around `avcodec_send_frame` in a
short libavcodec-based sender (replacing `ffmpeg -f rtp` from
`scripts/gen_test_stream.sh`) that calls `av_frame_new_side_data` with
`AV_FRAME_DATA_SEI_UNREGISTERED` set to the current monotonic time before
each `avcodec_send_frame` call.

## Client side (not yet implemented)

A small libavcodec-based reader that:
1. Fetches `/init.mp4` once, then streams `/live.cmfv` via chunked GET.
2. Feeds bytes into `avformat`/`avcodec` (or a minimal custom fMP4 parser,
   since this project already has one in `src/mux/` -- a reader is the
   dual of the writer already built).
3. On each decoded frame, extracts the SEI, computes
   `now_mono_ns() - capture_time_ns`, and appends to a per-run CSV:
   `frame_index,capture_time_ns,render_time_ns,latency_ms`.

## Aggregation

```python
import pandas as pd
df = pd.read_csv("run_bad-wifi_adaptive.csv")
print(df["latency_ms"].quantile([0.5, 0.95, 0.99]))
```

Run this once per (`netem_profiles.sh` profile) x (`--jitter-mode`) pair --
10 runs for 5 profiles x 2 modes, per `docs/NETWORK_TESTING_AND_LATENCY.md`
-- and build one summary table:

| Profile | Mode | p50 (ms) | p95 (ms) | p99 (ms) | Frames dropped |
|---|---|---|---|---|---|
| clean | fixed | | | | |
| clean | adaptive | | | | |
| good-wifi | fixed | | | | |
| good-wifi | adaptive | | | | |
| bad-wifi | fixed | | | | |
| bad-wifi | adaptive | | | | |
| congested-cellular | fixed | | | | |
| congested-cellular | adaptive | | | | |
| satellite | fixed | | | | |
| satellite | adaptive | | | | |

This table, plus one chart of p50/p95/p99 vs. profile faceted by mode, is
the single most valuable artifact for a project summary -- it is the
concrete, numeric proof that the adaptive jitter buffer does something
different (and hopefully better-behaved) than the fixed-delay baseline
under real impairment, rather than an unverified design claim.
