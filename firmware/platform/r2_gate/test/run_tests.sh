#!/bin/bash
# Host tests for r2_gate. No droid, no ESP-IDF.
set -e
cd "$(dirname "$0")"
cc -std=c99 -Wall -Wextra -Werror -O1 -I../include -I../../r2_packet/include \
   -o "${TMPDIR:-/tmp}/r2_gate_test" test_r2_gate.c ../r2_gate.c ../../r2_packet/r2_packet.c -lm
"${TMPDIR:-/tmp}/r2_gate_test"
