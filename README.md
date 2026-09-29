# ll-cmaf-ingest
At a high level, it takes an HEVC video stream arriving as RTP/UDP packets, reconstructs video frames, smooths network jitter, converts the frames into fragmented MP4/CMAF chunks, and immediately pushes those chunks to connected players over HTTP.
