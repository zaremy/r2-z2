#!/bin/bash
# LED-tier daemon. Launched via Finder/`open` so Terminal is the responsible
# process for CoreBluetooth (see CLAUDE.md). Ceiling is `leds`: all 8 LED bits,
# no dome, no stance, no locomotion. The dome is refusable by the daemon
# itself, not by anyone remembering not to move it.
cd "$(dirname "$0")"
exec ./r2 daemon --allow leds
