# Live Delivery: Chunked HTTP Transport and Jitter Buffer

**Source:** `src/server/http_chunked_server.hpp`, `src/jitter/jitter_buffer.hpp`

## Purpose

This stage delivers muxed CMAF chunks to connected clients over a
long-lived HTTP connection, with a jitter buffer absorbing network delay
variation between the RTP source and the muxer.

## Verification

```bash
./build/ll_cmaf_ingest --jitter-mode fixed --fixed-delay-ms 100 &
scripts/gen_test_stream.sh
ffplay http://localhost:8080/live.cmfv
curl http://localhost:8080/stats.json
```

## Chunked transfer encoding

A standard HTTP response requires `Content-Length` to be known before the
first byte is sent, which is incompatible with a stream of indeterminate
length. `Transfer-Encoding: chunked` allows repeated
`hex-length\r\n<bytes>\r\n` writes on one connection; a client's `fetch()` +
`ReadableStream`, or an MSE `SourceBuffer.appendBuffer` call, consumes each
chunk as it arrives rather than after the connection closes. This
mechanism, combined with CMAF-level chunking at the muxer, is what makes
low-latency delivery possible.

`TCP_NODELAY` is set on every accepted socket
(`HttpChunkedServer::accept_loop`) to disable Nagle's algorithm, which
would otherwise batch small, frequent chunk writes and introduce tens of
milliseconds of algorithmic delay.

## Defect: mutex deadlock in the stats endpoint

During end-to-end testing with a live ffmpeg source, requests to
`/stats.json` hung indefinitely. Root cause: `serve_stats()` held the
server's `mutex_` while invoking the stats-provider callback, and that
callback (configured in `ingest_main.cpp`) called `viewer_count()`, which
itself acquires the same `mutex_`. `std::mutex` is not reentrant; the
connection-handling thread deadlocked against itself on every request.

Fix (`HttpChunkedServer::serve_stats`): the callback is copied out of the
protected state under the lock, the lock is released, and the callback is
then invoked. This pattern applies generally to callback-based designs:
a lock should not be held across invocation of a callback whose
implementation is not fully controlled by the caller, since there is no
guarantee it will not re-enter the same lock.

This defect was not detected by unit tests, which exercised each method in
isolation; it was surfaced only when the components were integrated and
driven by a concurrent client.

## Jitter buffer: fixed and adaptive modes

`JitterBuffer::Mode::Fixed` (a constant playout delay) was implemented and
validated before the adaptive EWMA-based logic. A fixed-delay buffer has a
single parameter, which simplifies isolating defects in the muxer and HTTP
layer during their own validation, since any unexpected behavior under a
fixed delay cannot originate in the jitter estimator.

## Loss handling

The playout thread (`JitterBuffer::playout_loop`) does not block waiting
for a specific missing frame; it releases whatever access unit is at the
front of the queue once its deadline passes. If an access unit's
presentation-time slot was never populated (total loss), the subsequent
access unit's deadline is unaffected and still fires on schedule.
`tests/test_jitter_buffer.cpp`'s `loss_causes_skip_not_indefinite_stall`
test verifies this directly.

The cost of this approach: if the skipped access unit was not an IDR, the
decoder produces visible corruption until the next IDR arrives, since no
frame-repeat concealment or retransmission request is implemented. See
`docs/ARCHITECTURE.md`'s limitations table for the extension path (RTCP
NACK plus a retransmit buffer on the sender).

## Defect: `/live.cmfv` not self-contained for non-MSE clients

Initial end-to-end validation used `curl` to fetch `/init.mp4` and
`/live.cmfv` as two separate resources, concatenated by hand. This
validated the muxer's output but did not validate the delivery mechanism
against a client that opens a single URL. Pointing `ffplay`/`ffmpeg`
directly at `http://host:port/live.cmfv` failed:

```
could not find corresponding trex (id 1)
could not find corresponding track id 0
trun track id unknown, no tfhd was found
error reading header
Invalid data found when processing input
```

Diagnosis: libavformat's mov demuxer, used by both `ffplay` and `ffmpeg`,
requires `moov` to precede the first `moof` within the same byte stream it
is reading; it has no mechanism for retrieving a `moov` from a second URL.
Serving the init segment and live fragments as two independent HTTP
resources is compatible only with MSE-based players, where explicit
client-side logic fetches both and calls `appendBuffer()` on each in
sequence. A generic demuxer given a single URL has no equivalent
mechanism.

Fix (`HttpChunkedServer::broadcast_chunk`, `HttpChunkedServer::serve_live_stream`):
every connection to `/live.cmfv` now receives `ftyp`+`moov` as its own
first HTTP chunk -- immediately at connect time if the init segment
already exists, or prepended to the first fragment if the client connected
before the first IDR was parsed. `/init.mp4` remains available as a
separate resource for MSE-based clients that manage the two pieces
independently, but is no longer required for playback.

Verification: re-running the same demuxer used by `ffplay`
(`ffmpeg -i http://host:port/live.cmfv -f null -`) against the fix produced
a clean decode with zero header-parsing errors for a client connecting
before the stream began. A client connecting mid-GOP exhibits the
separately documented "corruption until next IDR" behavior described
above, which is unrelated to this defect.
