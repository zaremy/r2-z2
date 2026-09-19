#!/bin/bash
set -e
cd "$(dirname "$0")"
cc -std=c99 -Wall -Wextra -Werror -O1 -I../include -I../../r2_lights/include \
   -o "${TMPDIR:-/tmp}/panel_state_test" test_panel_state.c ../panel_state.c \
   ../../r2_lights/r2_lights.c -lm
"${TMPDIR:-/tmp}/panel_state_test"
cc -std=c99 -Wall -Wextra -Werror -O1 -I../include -I../../r2_lights/include \
   -o "${TMPDIR:-/tmp}/panel_wake_test" test_panel_wake.c ../panel_wake.c ../panel_state.c
"${TMPDIR:-/tmp}/panel_wake_test"
