#!/usr/bin/env bash
# gen_test_stream.sh -- streams a synthetic HEVC RTP source into the ingest
# engine using ffmpeg, standing in for a real encoder/camera source.
#
# IMPORTANT: -x265-params bframes=0 is not incidental. The jitter buffer
# reorders access units by presentation timestamp (see
# src/jitter/jitter_buffer.hpp), which silently assumes decode order ==
# presentation order -- true only when there are no B-frames. Removing this
# flag reproduces the exact decode-order corruption bug documented in
# docs/TESTING_LATENCY.md. Don't remove it unless you've also
# implemented the ctts/DTS-tracking fix described there.
#
# Usage:
#   ./gen_test_stream.sh [rtp_port] [duration_seconds] [keyint]
#
# Defaults produce a 1280x720 test pattern, IDR every 2 seconds (keyint=50 at
# 25fps), streamed at real-time pace (-re) to localhost.

set -euo pipefail

RTP_PORT="${1:-5004}"
DURATION="${2:-60}"
KEYINT="${3:-50}"
WIDTH="${WIDTH:-1280}"
HEIGHT="${HEIGHT:-720}"
FPS="${FPS:-25}"
DEST_HOST="${DEST_HOST:-127.0.0.1}"

echo "Streaming ${WIDTH}x${HEIGHT}@${FPS}fps HEVC to rtp://${DEST_HOST}:${RTP_PORT}"
echo "  duration=${DURATION}s  keyint=${KEYINT} (IDR every $(echo "scale=1; $KEYINT/$FPS" | bc 2>/dev/null || echo "?")s)"
echo "  Make sure ll_cmaf_ingest is already running with --rtp-port ${RTP_PORT}"
echo ""

ffmpeg -y -re \
  -f lavfi -i "testsrc=size=${WIDTH}x${HEIGHT}:rate=${FPS}:duration=${DURATION}" \
  -pix_fmt yuv420p \
  -c:v libx265 \
  -x265-params "keyint=${KEYINT}:bframes=0:log-level=error" \
  -preset ultrafast \
  -f rtp "rtp://${DEST_HOST}:${RTP_PORT}"
