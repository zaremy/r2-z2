#!/bin/bash
# Launched via Finder so Terminal is the responsible process for CoreBluetooth.
cd "$(dirname "$0")"
exec ./r2 daemon --allow dome
