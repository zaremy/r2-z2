#!/bin/bash
set -e
cd "$(dirname "$0")"
cc -std=c99 -Wall -Wextra -Werror -O1 -I../include \
   -o "${TMPDIR:-/tmp}/voice_react_test" test_voice_react.c ../voice_react.c -lm
"${TMPDIR:-/tmp}/voice_react_test"
