#!/bin/bash
# Host tests for r2_ops. No droid, no ESP-IDF.
set -e
cd "$(dirname "$0")"
cc -std=c99 -Wall -Wextra -Werror -O1 \
   -I../include -I../../r2_gate/include -I../../r2_packet/include \
   -I../../voice_react/include \
   -o "${TMPDIR:-/tmp}/r2_ops_test" test_r2_ops.c ../r2_ops.c \
   ../../r2_gate/r2_gate.c ../../r2_packet/r2_packet.c \
   ../../voice_react/voice_react.c -lm
"${TMPDIR:-/tmp}/r2_ops_test"
