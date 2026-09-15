#!/usr/bin/env bash
# Capture the panel's serial log for N seconds, WITHOUT resetting the board.
#
#   tools/panel_monitor.sh /dev/cu.usbmodem101 90 /tmp/panel.txt
#
# THREE THINGS THAT COST TIME TO LEARN, all of them non-obvious:
#
#   1. --no-reset is mandatory. This board's only port is the ESP32-S3 native
#      USB-Serial/JTAG, and a plain pyserial open RESETS THE CHIP INTO THE ROM
#      DOWNLOADER -- the app stops running and every read returns nothing, which
#      is indistinguishable from dead hardware. CLAUDE.md has the full story.
#
#   2. idf_monitor refuses to run without a TTY ("Monitor requires standard
#      input to be attached to TTY"), which a tool-driven shell does not have.
#      `script -q /dev/null` allocates a pty for it.
#
#   3. It has to be killed by name. The `script` wrapper does not forward the
#      signal, so killing the wrapper leaves the monitor holding the port and
#      the next capture fails with a misleading "could not open".
#
# The raw capture keeps its ANSI colour codes; tools/seg_touch.py strips them.
# Per-point touch logging is ESP_LOGD, so a default build emits the per-release
# `press:` verdict but no `point` lines -- raise the `touch` tag's level if you
# need the raw coordinates.
set -u

if [ $# -lt 1 ]; then
    echo "usage: $(basename "$0") <port> [seconds] [outfile]" >&2
    exit 2
fi

PORT="$1"
SECS="${2:-25}"
OUT="${3:-/tmp/panel.txt}"

script -q /dev/null python -m esp_idf_monitor --no-reset --port "$PORT" \
    --print_filter "*:V" --timestamps > "$OUT" 2>&1 &
WRAPPER=$!

sleep "$SECS"

# By name: see (3) above.
pkill -INT -f esp_idf_monitor 2>/dev/null
sleep 1
pkill -9 -f esp_idf_monitor 2>/dev/null
kill -9 "$WRAPPER" 2>/dev/null
wait "$WRAPPER" 2>/dev/null

echo "--- captured $(wc -l < "$OUT" | tr -d ' ') lines to $OUT ---"
