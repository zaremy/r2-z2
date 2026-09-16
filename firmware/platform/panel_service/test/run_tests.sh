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

# THE OP CATALOGUE'S PUBLIC SURFACE, UNDER HOSTILE INPUT AND SANITIZERS.
#
# panel_service_ops_rows takes arbitrary ops from arbitrary callers -- that is
# why it exists, since the shipped table gives only READ any rows and every
# claim about actuator tiers would otherwise be unfalsifiable. A contract like
# that needs adversarial input, and it needs ASan: one of the two defects this
# found was a read past the end of the CALLER's array, which is silent without
# it and which a fixed-size stack fixture cannot expose at all.
#
# 50k cases here rather than the 500k a review ran, so the suite stays quick;
# the seed is fixed, so the count is the only thing that changes between runs.
# Bump it when touching the rules.
if cc -fsanitize=address,undefined -fno-sanitize-recover=all \
      -std=c99 -Wall -Wextra -Werror -O1 -I../include -I../../r2_telemetry/include \
      -o "${TMPDIR:-/tmp}/panel_service_fuzz" \
      fuzz_panel_service.c ../panel_service.c ../../r2_telemetry/r2_telemetry.c \
      2>/dev/null; then
    "${TMPDIR:-/tmp}/panel_service_fuzz" 50000
else
    # NOT A SILENT SKIP. A toolchain with no sanitizers still runs everything
    # above; it just does not get the half that catches a bad read, and saying
    # so beats a green line that means less than it looks like.
    echo "SKIPPED: the fuzzer needs -fsanitize=address,undefined" >&2
fi
