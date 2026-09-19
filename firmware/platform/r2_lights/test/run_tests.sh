#!/bin/bash
# Host tests for r2_lights. No droid, no ESP-IDF.
set -e
cd "$(dirname "$0")"
cc -std=c99 -Wall -Wextra -Werror -O1 -I../include \
   -o "${TMPDIR:-/tmp}/r2_lights_test" test_r2_lights.c ../r2_lights.c -lm
"${TMPDIR:-/tmp}/r2_lights_test"

# THE GOLDEN MUST BE CURRENT. Without this a change to r2_lights.py leaves the
# old frames in golden.h and the port keeps passing against a table nobody
# ships any more.
fresh="${TMPDIR:-/tmp}/r2_lights_golden.h"
python3 ../tools/gen_golden.py "$fresh" >/dev/null
if ! cmp -s "$fresh" golden.h; then
    echo "FAIL: test/golden.h is stale against mac-prototype/r2_lights.py;" \
         "re-run tools/gen_golden.py" >&2
    exit 1
fi
echo "golden.h is current"
