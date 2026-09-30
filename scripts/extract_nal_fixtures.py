#!/usr/bin/env python3
"""extract_nal_fixtures.py -- pulls VPS/SPS/PPS NALs out of an Annex-B HEVC
elementary stream and prints them as ready-to-paste C++ uint8_t arrays, for
regenerating the fixtures used in tests/test_hevc_sps_parser.cpp and
tests/test_hevc_depacketizer.cpp.

Usage:
    ffmpeg -y -f lavfi -i testsrc=size=320x240:rate=25:duration=1 \\
      -pix_fmt yuv420p -c:v libx265 -x265-params "keyint=25:bframes=0" \\
      -preset ultrafast -f hevc test.hevc

    python3 extract_nal_fixtures.py test.hevc
"""
import sys


def find_start_codes(data: bytes):
    starts = []
    i = 0
    n = len(data)
    while i < n - 3:
        if data[i:i + 3] == b"\x00\x00\x01":
            starts.append((i, 3))
            i += 3
        elif data[i:i + 4] == b"\x00\x00\x00\x01":
            starts.append((i, 4))
            i += 4
        else:
            i += 1
    return starts


def split_nals(data: bytes):
    starts = find_start_codes(data)
    nals = []
    for idx, (pos, sclen) in enumerate(starts):
        nal_start = pos + sclen
        nal_end = starts[idx + 1][0] if idx + 1 < len(starts) else len(data)
        nal = data[nal_start:nal_end]
        if not nal:
            continue
        nal_type = (nal[0] >> 1) & 0x3F
        nals.append((nal_type, nal))
    return nals


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)

    data = open(sys.argv[1], "rb").read()
    nals = split_nals(data)

    names = {32: "vps", 33: "sps", 34: "pps"}
    seen = set()
    for nal_type, nal in nals:
        if nal_type in names and nal_type not in seen:
            seen.add(nal_type)
            name = names[nal_type]
            hex_bytes = ", ".join(f"0x{b:02x}" for b in nal)
            print(f"// {name} ({len(nal)} bytes, nal_unit_type={nal_type})")
            print(f"static const uint8_t k{name.upper()}_bytes[] = {{ {hex_bytes} }};")
            print()

    print(f"// First 15 NAL types in stream order: {[t for t, _ in nals[:15]]}")
    print("// (32=VPS, 33=SPS, 34=PPS, 39=prefix SEI, 19/20=IDR, 1=TRAIL_R, 0=TRAIL_N)")


if __name__ == "__main__":
    main()
