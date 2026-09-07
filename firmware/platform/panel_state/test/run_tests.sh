#!/bin/bash
set -e
cd "$(dirname "$0")"
cc -std=c99 -Wall -Wextra -Werror -O1 -I../include \
   -o "${TMPDIR:-/tmp}/panel_state_test" test_panel_state.c ../panel_state.c
"${TMPDIR:-/tmp}/panel_state_test"
