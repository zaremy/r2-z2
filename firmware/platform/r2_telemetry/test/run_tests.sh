#!/bin/bash
set -e
cd "$(dirname "$0")"
cc -std=c99 -Wall -Wextra -Werror -O1 -I../include \
   -o "${TMPDIR:-/tmp}/r2_tm_test" test_r2_telemetry.c ../r2_telemetry.c
"${TMPDIR:-/tmp}/r2_tm_test"
