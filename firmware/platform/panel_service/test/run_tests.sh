#!/bin/bash
set -e
cd "$(dirname "$0")"
cc -std=c99 -Wall -Wextra -Werror -O1 -I../include -I../../r2_telemetry/include \
   -o "${TMPDIR:-/tmp}/panel_service_test" test_panel_service.c ../panel_service.c \
   ../../r2_telemetry/r2_telemetry.c
"${TMPDIR:-/tmp}/panel_service_test"
cc -std=c99 -Wall -Wextra -Werror -O1 -I../include \
   -o "${TMPDIR:-/tmp}/panel_probe_test" test_panel_probe.c ../panel_probe.c
"${TMPDIR:-/tmp}/panel_probe_test"
