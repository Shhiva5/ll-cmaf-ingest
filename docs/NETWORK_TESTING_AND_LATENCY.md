# Network Impairment Testing and Latency Measurement

**Source:** `scripts/netem_profiles.sh`; methodology reference:
`scripts/measure_latency.md`

## Defect: decode-order corruption under B-frame encoding

The muxer, depacketizer, and jitter buffer each had passing unit tests
prior to end-to-end validation. The first full-pipeline test -- an ffmpeg
HEVC RTP source into the running server, with the captured init segment
and live fragments concatenated and decoded via `ffmpeg -f null -` --
produced systematic decode errors:

```
Could not find ref with POC 32
Could not find ref with POC 30
Could not find ref with POC 26
... (repeated across approximately 90% of decoded frames)
```

### Diagnosis

The test stream was encoded with libx265's default B-frame settings
(`has_b_frames=2` per `ffprobe`). An HEVC RTP timestamp encodes a
presentation timestamp (PTS), not a decode timestamp (DTS); with B-frames
present, decode order and presentation order differ. `JitterBuffer` sorts
access units by PTS (derived from the RTP timestamp) prior to release.
This ordering is correct for compensating out-of-order network delivery,
but it also re-orders frames that arrived from the encoder in correct
decode order into presentation order before they reach the muxer -- so
`mdat` samples were written in PTS order rather than DTS order. A decoder
consuming that stream encounters B-frames whose reference frames have not
yet been decoded, because the corrupted ordering places them after rather
than before the frames that depend on them.

### Fix applied

Test stream generation was constrained to `-x265-params bframes=0`, which
makes PTS and DTS identical, eliminating the ordering discrepancy. Verified
by re-running the same capture-and-decode procedure: reference errors
dropped from approximately 100 occurrences to zero, with one remaining
artifact traced to a capture process terminated before the final in-flight
HTTP chunk was drained -- a test-harness timing detail, not a pipeline
defect.

### Rationale for constraining the input rather than extending the muxer

`docs/RTP_DEPACKETIZATION.md` documents "no B-frames" as an explicit
assumption prior to this defect being identified; the test stream
generation script had not yet been updated to enforce that assumption.
Given the project's scope, constraining and documenting the input is the
appropriate response; the complete fix is a larger, separately scoped
change:

### Extension path

Track DTS and PTS as separate fields on `AccessUnit`. `JitterBuffer` would
reorder by decode-order readiness (frames released to the muxer in decode
order), while a composition-time offset (the `ctts` box, ISO/IEC 14496-12
section 8.6.1.3) would carry the PTS-minus-DTS delta so presentation order
is preserved for the player. This is a self-contained extension, not
implemented in the current codebase.

## Network impairment harness

```bash
# See scripts/netem_profiles.sh for the complete, runnable version.
sudo ip netns add ns_server
sudo ip netns add ns_client
sudo ip link add veth0 type veth peer name veth1
sudo ip link set veth0 netns ns_server
sudo ip link set veth1 netns ns_client
sudo ip netns exec ns_server ip addr add 10.0.0.1/24 dev veth0
sudo ip netns exec ns_client ip addr add 10.0.0.2/24 dev veth1
sudo ip netns exec ns_server ip link set veth0 up
sudo ip netns exec ns_client ip link set veth1 up
sudo ip netns exec ns_server ip link set lo up
sudo ip netns exec ns_client ip link set lo up

sudo ip netns exec ns_server tc qdisc add dev veth0 root netem \
  delay 40ms 10ms distribution normal loss 1% 25% reorder 0.5% 50%
```

`scripts/netem_profiles.sh` provides five named impairment profiles
(clean, good-wifi, bad-wifi, congested-cellular, satellite), applied and
cleared with single commands.

## Glass-to-glass latency: methodology and implementation status

### Approach

For a single-machine test configuration, a monotonic capture timestamp is
embedded in an SEI NAL at the point each frame is generated, and compared
against the client's render-time clock reading at decode. A single machine
requires no clock synchronization, since both timestamps derive from the
same monotonic clock.

### Implemented

`/stats.json` exposes `jitter_estimate_ms`, `current_delay_ms`,
`frames_released`, and `sequence_gaps` in real time, sufficient to measure
the jitter buffer's behavior end-to-end through the muxer and HTTP layers,
and to observe its response to a `tc netem` profile change. This was
validated against a live ffmpeg-sourced stream during development (over
100 RTP packets received, approximately 90 frames released, delay tracking
the configured value).

### Not implemented

SEI-embedded capture timestamps and a matching client-side extraction
tool. The remaining implementation work:

1. A frame-source component that writes a monotonic timestamp into an SEI
   NAL (`nal_unit_type` 39, `user_data_unregistered`) at the moment each
   frame is handed to the encoder.
2. A client that consumes `/live.cmfv`, decodes it, extracts the SEI
   payload from each decoded frame, and records
   `render_time_ns - capture_time_ns` per frame.
3. Aggregation into p50/p95/p99 per network profile, tabulated against
   each `netem_profiles.sh` profile.

See `scripts/measure_latency.md` for the complete SEI payload
specification and aggregation methodology.

## Recommended measurement procedure

Run each of the five `netem_profiles.sh` profiles for approximately 30
seconds under both `--jitter-mode fixed` and `--jitter-mode adaptive`,
logging `/stats.json` at 1 Hz. Once SEI-based measurement is implemented,
log per-frame glass-to-glass latency as well. The primary comparison of
interest: whether adaptive mode's p99 latency degrades more gracefully
than fixed mode's frame-drop rate under the `bad-wifi` and
`congested-cellular` profiles.
