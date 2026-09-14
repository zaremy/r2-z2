#!/bin/bash
set -e
cd "$(dirname "$0")"
cc -std=c99 -Wall -Wextra -Werror -O1 -I../include \
   -o "${TMPDIR:-/tmp}/panel_gesture_test" test_panel_gesture.c ../panel_gesture.c
"${TMPDIR:-/tmp}/panel_gesture_test"
