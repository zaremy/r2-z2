#!/bin/bash
# Grab the panel's last TALK capture as a WAV. No operator, no phone.
#
#   tools/grab_audio.sh [port] [out.wav]
#
# Same mechanism as grab_shot.sh: the board writes the capture to the
# 'storage' partition (PANEL_MIC_DUMP builds only -- see main/panel_mic.c),
# this reads it back over the same USB cable with esptool and decodes it.
set -e
PORT="${1:-/dev/cu.usbmodem2101}"
OUT="${2:-panel_talk.wav}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# storage partition offset comes from OUR partitions.csv (D-022), read from
# the built table rather than hardcoded -- the table has already changed once.
OFF=$(python3 "$IDF_PATH/components/partition_table/gen_esp32part.py" \
        "$HERE/../build/partition_table/partition-table.bin" 2>/dev/null \
      | awk -F, '/^storage,/ {print $4}')
[ -n "$OFF" ] || { echo "could not find the storage partition offset" >&2; exit 1; }

# PANEL_MIC_CAP_BYTES is an expression over two other macros, not a literal
# like panel_shot's SLOT_BYTES -- asking the real preprocessor for its value
# is the only way that does not repeat the arithmetic here and drift from it.
# cc -E only expands the macro; it still leaves an arithmetic C expression
# (with `u` suffixes), so bash has to evaluate that expression itself.
CAP_EXPR=$(printf '#include "panel_mic.h"\nPANEL_MIC_CAP_BYTES\n' \
           | cc -E -I"$HERE/../main" - 2>/dev/null | tail -1 | tr -d 'u')
CAP_BYTES=$((CAP_EXPR)) 2>/dev/null
case "$CAP_BYTES" in
    ''|*[!0-9]*)
        echo "could not evaluate PANEL_MIC_CAP_BYTES from main/panel_mic.h" >&2
        exit 1
        ;;
esac

# header (12 B) + the full 12 s cap: the dump is always shorter than a
# release before the cap, and decode_audio.py trims to the header's own
# `bytes` field, so reading the max is simpler than sizing the read exactly.
SIZE=$((12 + CAP_BYTES))
python -m esptool --chip esp32s3 -p "$PORT" \
    read_flash "$OFF" "$SIZE" "$TMP/mic.bin" >/dev/null
python3 "$HERE/decode_audio.py" "$TMP/mic.bin" "$OUT"
