#!/bin/bash
# Grab the panel's current frame as a PNG. No operator, no phone.
#
#   tools/grab_shot.sh [port] [out.png]
#
# The board writes the frame to the 'storage' partition; this reads it back
# over the same USB cable with esptool and decodes it. esptool verifies its own
# read, so a corrupt transfer fails here rather than producing a plausible
# picture.
set -e
PORT="${1:-/dev/cu.usbmodem2101}"
OUT="${2:-panel.png}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# storage partition offset comes from OUR partitions.csv (D-022), read from the
# built table rather than hardcoded -- the table has already changed once.
OFF=$(python3 "$IDF_PATH/components/partition_table/gen_esp32part.py" \
        "$HERE/../build/partition_table/partition-table.bin" 2>/dev/null \
      | awk -F, '/^storage,/ {print $4}')
[ -n "$OFF" ] || { echo "could not find the storage partition offset" >&2; exit 1; }

# header (16 B) + 368*448*2
SIZE=$((16 + 368 * 448 * 2))
# The default reset behaviour is correct here even though it restarts the app:
# reading flash needs the ROM downloader, and the frame is ALREADY in flash, so
# it survives the reset that fetches it. (This is the same board whose only
# port is the native USB-Serial/JTAG, where a plain serial open drops it into
# the downloader -- here that is the behaviour we want rather than the trap.)
python -m esptool --chip esp32s3 -p "$PORT" \
    read_flash "$OFF" "$SIZE" "$TMP/shot.bin" >/dev/null
python3 "$HERE/decode_shot.py" "$TMP/shot.bin" "$OUT"
