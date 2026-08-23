#!/bin/bash
# Launched via Finder so Terminal is the responsible process for CoreBluetooth
# (the agent cannot start this — see CLAUDE.md).
#
# STANCE CEILING. This is the top rung: it permits authored animations, which
# drive leg actions whose contents cannot be inspected before sending. 36 of
# 56 measured animation ids emit WADDLE and can fell him (#12). Only launch
# this for a supervised test, and keep him on a clear floor.
cd "$(dirname "$0")"
exec ./r2 daemon --allow stance
