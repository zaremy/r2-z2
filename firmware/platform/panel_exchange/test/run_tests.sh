#!/bin/bash
set -e
cd "$(dirname "$0")"
cc -std=c99 -Wall -Wextra -Werror -O1 -I../include \
   -o "${TMPDIR:-/tmp}/panel_exchange_test" test_panel_exchange.c ../panel_exchange.c
"${TMPDIR:-/tmp}/panel_exchange_test"
