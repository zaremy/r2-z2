# board_revision — vendored, unmodified

`board_variant.c` and `include/board_variant.h` are copied **verbatim** from the
vendor repository. Do not edit them in place. If they need to change, change the
caller, or re-vendor from a newer upstream and update this file.

| | |
|---|---|
| Upstream | `waveshareteam/ESP32-S3-Touch-AMOLED-1.8` |
| Commit | `ed7c6a5` |
| Path | `examples/esp-idf/90_axp2101_pmu/components/board_variant/` |
| Vendored | 2026-08-22 |
| Modified | no — byte-identical to upstream |

Verify byte-identity against a fresh clone at that SHA:

```
shasum -a 256 board_variant.c include/board_variant.h
822d9e664bfaa7ca99e0f16e02cf3997477d2aaab3294743b9cc307faf1d1c9e  board_variant.c
15e2f7d90d99e29f9f40ebe0e877fdda9afeb3074c814ec6d73e13e8130dad75  include/board_variant.h
```

`docs/research/source-map.md` requires a recorded SHA for every external source;
this is that record. `reference/` is gitignored, so without this file the
provenance would not survive a fresh clone.

## Why this lives in `platform/`, not in `board_check/`

`board_check` is a throwaway diagnostic. Revision detection is not: display and
LVGL bring-up both need it, and it must be re-run on any board swap and after
any BSP bump. A diagnostic owning a component two later slices depend on is how
a stale second copy gets made. The diagnostic is merely the first caller.

## What it does, and the one thing that surprises people

`board_variant_detect()` probes CST816 at `0x15` (→ V2) and falls back to FT3168
at `0x38` (→ V1). Before probing it **releases the touch controller's reset**
by writing the IO expander at `0x20` (`board_variant.c:41-72`). Without that
release neither touch address answers and a bare I²C scan reports an empty bus,
which reads as "unknown board" rather than as "you skipped a step". See
`docs/research/board-revision.md`.

It also **caches** its result in a file-static (`s_detected`,
`board_variant.c:32-33`), so a stale or failed probe cannot be retried in the
same boot. Retrying means a power cycle.

## Component name

The ESP-IDF component name comes from the directory, so this is component
`board_revision` while the header and symbols remain `board_variant` — that
mismatch is deliberate, and is the cost of vendoring the source unmodified.
Depend on it with `REQUIRES board_revision`.
