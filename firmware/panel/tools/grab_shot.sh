#!/bin/bash
# Grab the panel's current frame as a PNG. No operator, no phone.
#
#   tools/grab_shot.sh [port] [out.png] [slot]
#
# The board writes the frame to the 'storage' partition; this reads it back
# over the same USB cable with esptool and decodes it. esptool verifies its own
# read, so a corrupt transfer fails here rather than producing a plausible
# picture.
set -e
PORT="${1:-/dev/cu.usbmodem2101}"
OUT="${2:-panel.png}"
# The tour build writes one frame per slot; a plain build writes slot 0.
# The slot size lives in main/panel_shot.h; read it rather than repeat it.
SLOT="${3:-0}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# storage partition offset comes from OUR partitions.csv (D-022), read from the
# built table rather than hardcoded -- the table has already changed once.
OFF=$(python3 "$IDF_PATH/components/partition_table/gen_esp32part.py" \
        "$HERE/../build/partition_table/partition-table.bin" 2>/dev/null \
      | awk -F, '/^storage,/ {print $4}')
[ -n "$OFF" ] || { echo "could not find the storage partition offset" >&2; exit 1; }

SLOT_BYTES=$(sed -n 's/^#define PANEL_SHOT_SLOT_BYTES  *\(0x[0-9A-Fa-f]*\)u.*/\1/p' "$HERE/../main/panel_shot.h")
if [ -z "$SLOT_BYTES" ]; then
    echo "could not read PANEL_SHOT_SLOT_BYTES from main/panel_shot.h" >&2
    exit 1
fi
SLOT_BYTES=$((SLOT_BYTES))
FW_SLOTS=$(sed -n 's/^#define PANEL_SHOT_SLOTS  *\([0-9]*\)u.*/\1/p' \
           "$HERE/../main/panel_shot.h")
if [ -n "$FW_SLOTS" ] && [ "$SLOT" -ge "$FW_SLOTS" ]; then
    echo "slot $SLOT does not exist: the firmware writes $FW_SLOTS" >&2
    exit 1
fi

# header (16 B) + 368*448*2
SIZE=$((16 + 368 * 448 * 2))
# The default reset behaviour is correct here even though it restarts the app:
# reading flash needs the ROM downloader, and the frame is ALREADY in flash, so
# it survives the reset that fetches it. (This is the same board whose only
# port is the native USB-Serial/JTAG, where a plain serial open drops it into
# the downloader -- here that is the behaviour we want rather than the trap.)
python -m esptool --chip esp32s3 -p "$PORT" \
    read_flash $((OFF + SLOT * SLOT_BYTES)) "$SIZE" "$TMP/shot.bin" >/dev/null
python3 "$HERE/decode_shot.py" "$TMP/shot.bin" "$OUT"
