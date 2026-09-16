#!/usr/bin/env bash
# BUILD BOTH CONFIGURATIONS. There are two, and only one of them ships.
#
# `PANEL_SHOT_TOUR` is sticky in the CMake cache, so a plain `idf.py build`
# after a tour build is STILL A TOUR BUILD -- and the tour build is the one you
# run while iterating, because it is the one that produces pictures. So the
# shipping configuration can stop compiling and stay broken through any number
# of green builds and hardware runs.
#
# That is not hypothetical. A constant moved into panel_ui.h landed inside the
# tour's own `#ifdef`, panel_ui.c used it unconditionally, and the plain build
# was broken across nine consecutive green tour builds, a flash, and a live
# hardware verification. It was caught by the merge check, not by any of them.
#
# Run this before pushing anything that touches firmware/panel.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/.." || exit 2

fail=0
for tour in OFF ON; do
    printf '%-28s' "PANEL_SHOT_TOUR=$tour"
    if idf.py -DPANEL_SHOT_TOUR=$tour build >/tmp/bb-$tour.log 2>&1; then
        size=$(grep -o 'panel.bin binary size 0x[0-9a-f]*' /tmp/bb-$tour.log | tail -1)
        echo "OK   ${size:-built}"
    else
        echo "FAIL"
        grep -E "error:" /tmp/bb-$tour.log | head -5
        fail=1
    fi
done

# LEAVE THE CACHE ON THE SHIPPING CONFIGURATION, so the next bare `idf.py
# build` or `flash` is the one that goes on the droid. Ending on ON is how a
# tour build gets flashed by accident and sits there driving itself.
printf '%-28s' "restoring plain build"
if idf.py -DPANEL_SHOT_TOUR=OFF build >/tmp/bb-restore.log 2>&1; then echo "OK"
else echo "FAIL"; fail=1; fi

[ "$fail" = 0 ] && echo "both configurations build" || echo "SOMETHING DID NOT BUILD"
exit $fail
