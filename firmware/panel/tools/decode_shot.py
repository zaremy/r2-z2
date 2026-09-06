#!/usr/bin/env python3
"""Turn the panel's raw frame dump into a PNG.

  python3 tools/decode_shot.py shot.bin out.png

Layout is written by main/panel_shot.c: a 16-byte little-endian header
(magic "R2Z2", w, h, seq, bytes) followed by RGB565 rows.

The magic is checked, because an erased flash region reads back as 0xFF --
a perfectly plausible white image. A tool that exists to be believed instead
of the operator's eyes must fail loudly rather than render garbage.
"""
import struct
import sys
import zlib

MAGIC = 0x52325A32


def rgb565_row(raw, w):
    out = bytearray()
    for i in range(w):
        v = raw[2 * i] | (raw[2 * i + 1] << 8)
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        out += bytes(((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31))
    return out


def main(src, dst):
    data = open(src, "rb").read()
    if len(data) < 16:
        raise SystemExit("file too short to hold a header")
    magic, w, h, seq, nbytes = struct.unpack_from("<IHHII", data, 0)
    if magic != MAGIC:
        raise SystemExit(
            f"bad magic 0x{magic:08X} — the board has not written a frame here "
            "(erased flash reads as 0xFFFFFFFF)")
    if w == 0 or h == 0 or nbytes != w * h * 2:
        raise SystemExit(f"header disagrees with itself: {w}x{h}, {nbytes} bytes")
    if len(data) < 16 + nbytes:
        raise SystemExit(f"short read: {len(data) - 16} of {nbytes} pixel bytes")

    rows = bytearray()
    for y in range(h):
        rows.append(0)                       # PNG filter: none
        rows += rgb565_row(data[16 + y * w * 2: 16 + (y + 1) * w * 2], w)

    def chunk(tag, payload):
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    png += chunk(b"IEND", b"")
    open(dst, "wb").write(png)
    print(f"{dst}: {w}x{h}, capture #{seq}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
