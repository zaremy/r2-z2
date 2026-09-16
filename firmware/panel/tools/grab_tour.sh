#!/bin/bash
# Read every slot the screenshot tour wrote, in ONE esptool pass, and decode
# each to a PNG.
#
#   tools/grab_tour.sh [port] [outdir] [build dir]
#
# ONE pass because each esptool read resets the board: nine reads would be
# nine boots, and the tour would start over between them.
#
# Pass the build dir the tour was FLASHED from (normally build-tour). Without
# it this takes the first build* directory that has a partition table, which is
# a guess -- and a wrong table means reading the wrong flash offset.
set -e
PORT="${1:-/dev/cu.usbmodem2101}"
OUT="${2:-tour}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${3:-}"
NAMES=(status service r2-link diagnostics hw-test-ops provisioning voice camera about)
SLOTS=${#NAMES[@]}

if [ -z "$IDF_PATH" ]; then
    echo "IDF_PATH is not set -- run . \$HOME/esp/esp-idf/export.sh first" >&2
    exit 1
fi

# The slot size lives in panel_shot.h. Read it rather than repeat it: a second
# copy here would silently slice every frame in half the day it changes.
SLOT=$(sed -n 's/^#define PANEL_SHOT_SLOT_BYTES  *\(0x[0-9A-Fa-f]*\)u.*/\1/p' \
       "$HERE/../main/panel_shot.h")
if [ -z "$SLOT" ]; then
    echo "could not read PANEL_SHOT_SLOT_BYTES from main/panel_shot.h" >&2
    exit 1
fi
SLOT=$((SLOT))

# The firmware decides how many slots exist; this list only names them. If the
# two disagree, the names are wrong and so is every file this writes.
FW_SLOTS=$(sed -n 's/^#define PANEL_SHOT_SLOTS  *\([0-9]*\)u.*/\1/p' \
           "$HERE/../main/panel_shot.h")
if [ -n "$FW_SLOTS" ] && [ "$FW_SLOTS" != "$SLOTS" ]; then
    echo "the firmware writes $FW_SLOTS slots but this script names $SLOTS" >&2
    exit 1
fi

# Prefer the named build dir, else whichever build* has a partition table.
if [ -z "$BUILD" ]; then
    for d in "$HERE/../build-tour" "$HERE/../build" "$HERE"/../build*; do
        if [ -f "$d/partition_table/partition-table.bin" ]; then
            BUILD="$d"
            break
        fi
    done
fi
TABLE="$BUILD/partition_table/partition-table.bin"
if [ ! -f "$TABLE" ]; then
    echo "no partition table: looked for $TABLE" >&2
    echo "pass the build dir as the third argument (e.g. build-tour)" >&2
    exit 1
fi
echo "partition table: $TABLE"

OFF=$(python3 "$IDF_PATH/components/partition_table/gen_esp32part.py" "$TABLE" 2>/dev/null \
      | awk -F, '/^storage,/ {print $4}')
if [ -z "$OFF" ]; then
    echo "could not find the storage partition offset" >&2
    exit 1
fi

mkdir -p "$OUT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

python -m esptool --chip esp32s3 -p "$PORT" \
    read_flash "$OFF" $((SLOT * SLOTS)) "$TMP/tour.bin" >/dev/null

MISSING=0
for i in $(seq 0 $((SLOTS - 1))); do
    dd if="$TMP/tour.bin" of="$TMP/slot$i.bin" bs=$SLOT skip=$i count=1 2>/dev/null
    # A slot the tour never reached is erased flash, and decode_shot.py refuses
    # it by magic. Said out loud: a missing picture is a finding, not a gap.
    if python3 "$HERE/decode_shot.py" "$TMP/slot$i.bin" "$OUT/$i-${NAMES[$i]}.png"; then
        :
    else
        echo "slot $i (${NAMES[$i]}): NO FRAME -- the tour did not get this far"
        MISSING=$((MISSING + 1))
    fi
done
echo "---"
ls -1 "$OUT"
if [ "$MISSING" -gt 0 ]; then
    echo "$MISSING of $SLOTS slots were empty; check the board's log for TOUR errors" >&2
    exit 2
fi
