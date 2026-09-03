#!/bin/bash
# Host tests for r2_packet. No droid, no ESP-IDF -- just a C compiler.
#
# Regenerates fixtures.h so the expectations cannot drift from the Python
# reference. If this checkout is read-only (a reviewer's sandbox, CI), the
# fixtures go to a temp dir instead of failing -- an unrunnable test suite is
# indistinguishable from a passing one to anyone who cannot run it.
set -e
cd "$(dirname "$0")"

if [ -w . ]; then GEN_DIR="."; else
  GEN_DIR="$(mktemp -d)"
  echo "note: read-only checkout, generating fixtures in $GEN_DIR"
fi
python3 gen_fixtures.py > "$GEN_DIR/fixtures.h"

cc -std=c99 -Wall -Wextra -Werror -O1 -I../include -I"$GEN_DIR" -I. \
   -o "${TMPDIR:-/tmp}/r2_packet_test" test_r2_packet.c ../r2_packet.c
"${TMPDIR:-/tmp}/r2_packet_test"
