#!/bin/bash
# The one daemon launcher. Double-click it, or pass a manifest.
#
# WHY A .command AND NOT A SHELL FUNCTION — the explanation now lives HERE and
# only here. macOS attributes Bluetooth to the RESPONSIBLE process. Launched
# from Finder, launchd spawns this and Terminal is responsible, so CoreBluetooth
# permits it. The same commands run from an agent-spawned shell are SIGABRTed
# (claude.app has no usage description). That is why the agent cannot start the
# link for you — see CLAUDE.md.
#
#   ./start-daemon.command                     read-only (safe default)
#   ./start-daemon.command manifests/x.json    the manifest's computed ceiling
#   ./start-daemon.command dome                a bare tier, the old way
#
# THIS REPLACES start-daemon-leds.command AND start-daemon-stance.command.
# They were near-identical copies of the paragraph above, differing only in a
# tier — because the ceiling had to be chosen before the link existed. That is
# the thing #88 removes: a manifest declares the WHOLE session's plan, you
# approve it once, and the ceiling is computed from the ops rather than picked
# by opening a different file.
set -uo pipefail
cd "$(dirname "$0")"

ARG="${1:-}"

if [ -z "$ARG" ]; then
    echo "No manifest and no tier — starting READ-ONLY."
    echo "Pass a manifest path to run a whole session on one approval."
    echo
    exec ./r2 daemon --allow read
fi

if [ -f "$ARG" ]; then
    # The daemon prints the plan and the computed ceiling before the radio is
    # touched, so what you approve is the plan, not a bare tier whose users
    # you cannot see.
    exec ./r2 daemon --manifest "$ARG"
fi

case "$ARG" in
    read|leds|audio|dome|stance|motion)
        # The old way, kept because a manifest is not always worth writing —
        # a two-op poke at the battery does not need a document. Anything that
        # would have needed a RELAUNCH should be a manifest instead.
        if [ "$ARG" = "stance" ]; then
            echo "STANCE is the top rung: it permits authored animations, whose"
            echo "leg actions cannot be inspected before sending. 36 of 56"
            echo "measured animation ids emit WADDLE and can fell him (#12)."
            echo "Keep him on a clear floor and supervise this."
            echo
        fi
        exec ./r2 daemon --allow "$ARG" ;;
    *)
        echo "Not a manifest file and not a tier: '$ARG'"
        echo "  tiers: read leds audio dome stance"
        echo "  or a path to a session manifest (see manifests/)"
        exit 2 ;;
esac
