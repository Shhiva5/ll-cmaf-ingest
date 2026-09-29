// mp4_boxes.hpp
//
// Generic ISOBMFF (ISO/IEC 14496-12) box serialization helpers. A "box" is:
//
//   uint32 size (including this header)
//   char[4] type
//   uint8[]  body           (size - 8 bytes)
//
// A "full box" additionally starts its body with:
//
//   uint8  version
//   uint24 flags
//
// We build boxes depth-first with a simple pattern: write a placeholder
// 4-byte size, write the fourcc, write the body (which may itself contain
// nested boxes built the same way), then patch the size once the body's
// length is known. This mirrors how every real muxer does it and avoids
// needing a two-pass "compute sizes first" design.

#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include "../common/byte_io.hpp"

namespace llcmaf {

// Writes a box whose body is produced by `write_body(ByteWriter&)`.
// Returns nothing -- writes directly into `out`.
inline void write_box(ByteWriter& out, const char (&fourcc)[5],
                       const std::function<void(ByteWriter&)>& write_body) {
    size_t size_offset = out.size();
    out.u32be(0); // placeholder, patched below
    out.fourcc(fourcc);
    write_body(out);
    uint32_t total_size = uint32_t(out.size() - size_offset);
    out.patch_u32be(size_offset, total_size);
}

// Same as write_box but prefixes the body with the 4-byte
// version(1)+flags(3) header required by "full boxes" (mvhd, tfhd, trun, ...).
inline void write_full_box(ByteWriter& out, const char (&fourcc)[5],
                            uint8_t version, uint32_t flags,
                            const std::function<void(ByteWriter&)>& write_body) {
    write_box(out, fourcc, [&](ByteWriter& w) {
        w.u8(version);
        w.u24be(flags);
        write_body(w);
    });
}

} // namespace llcmaf
