#!/usr/bin/env python3
"""Turn the panel's raw TALK capture dump into a WAV.

  python3 tools/decode_audio.py mic.bin out.wav

Layout is written by main/panel_mic.c (PANEL_MIC_DUMP builds): a 12-byte
little-endian header (magic "MZ22", sample_rate, bytes) followed by raw
16-bit mono PCM.

The magic is checked, because an erased flash region reads back as 0xFF --
a perfectly plausible-looking (if very loud) noise capture. Same reasoning
as decode_shot.py's PNG magic check.
"""
import struct
import sys

MAGIC = 0x32325A4D


def main(src, dst):
    data = open(src, "rb").read()
    if len(data) < 12:
        raise SystemExit("file too short to hold a header")
    magic, sample_rate, nbytes = struct.unpack_from("<III", data, 0)
    if magic != MAGIC:
        raise SystemExit(
            f"bad magic 0x{magic:08X} — the board has not dumped a capture "
            "here (erased flash reads as 0xFFFFFFFF), or this is not a "
            "PANEL_MIC_DUMP build")
    if nbytes == 0:
        raise SystemExit("header says 0 bytes captured")
    if len(data) < 12 + nbytes:
        raise SystemExit(f"short read: {len(data) - 12} of {nbytes} PCM bytes")

    pcm = data[12:12 + nbytes]
    channels, bits = 1, 16
    byte_rate = sample_rate * channels * bits // 8
    block_align = channels * bits // 8

    wav = b"RIFF" + struct.pack("<I", 36 + nbytes) + b"WAVE"
    wav += b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, sample_rate,
                                 byte_rate, block_align, bits)
    wav += b"data" + struct.pack("<I", nbytes) + pcm
    open(dst, "wb").write(wav)
    print(f"{dst}: {nbytes} B, {sample_rate} Hz mono, "
          f"{nbytes / byte_rate:.2f} s")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
