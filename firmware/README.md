# firmware/ — ESP32-S3 backpack

Bring-up started. Foundation decided (D-005): **ESP-IDF v5.5.x + the managed
BSP `waveshare/esp32_s3_touch_amoled_1_8 ^2.0.3`**, NimBLE in central role,
LVGL 9.

What exists today:

```
platform/board_revision/   vendored revision detector (see its PROVENANCE.md)
board_check/               read-only diagnostic: chip, PSRAM, flash, MAC,
                           board variant, I²C inventory, AXP2101 rails
```

`board_check` links no display driver and initialises no panel — that is what
lets it run before the revision is known (prerequisite 2 below).

Prerequisites, in order:

1. Install ESP-IDF v5.5.x — **done**, v5.5.5 at `~/esp/esp-idf` (per-shell
   `. ~/esp/esp-idf/export.sh`; the shell profile is deliberately untouched).
2. Confirm the board revision before flashing anything **except one
   revision-neutral diagnostic** — see `../docs/research/board-revision.md`.
   **Do not flash `vthinkxie` firmware.**

   The carve-out exists because the revision is only knowable by running code
   on the board, so the rule as originally written forbade its own
   precondition. A rule the first real task must quietly break is a rule that
   stops being believed, so it is amended rather than ignored. The diagnostic
   must satisfy **all** of:

   - it initialises no panel controller, and **links no display driver** —
     checked by command, not by reading:
     `xtensa-esp32s3-elf-nm <elf> | grep -ci 'co5300\|bsp_display'` prints `0`;
   - it is **ours**, built from this repo — never a third-party or vendor image;
   - a **verified full-flash backup exists first** (size matches what
     `esptool.py flash_id` reports, SHA-256 recorded).

   `board_check/` is that diagnostic. Nothing else qualifies today.
3. Follow the first embedded slice in `../docs/research/embedded-path.md`.

**Read `../docs/port-boundary.md` before writing any of it.** The Mac
prototype does not port, but most of what it learned does — hardware truths
that constrain any implementation, protocol constants already cross-validated
three ways, and the handful of designs worth rebuilding in C. It also names
what is pure Mac scaffolding, which looks like architecture and is not.

It ends with the one that will bite: **prove the link by reading the battery
voltage, not by moving the dome.** The dome has no home position, ignores
small commands while reporting success, and the obvious first test against it
is one whose negative result means nothing.

Planned layout for the rest (do not scaffold ahead of need):

```
platform/  display · touch · power · audio · storage · imu · connectivity
r2/        ble · protocol · commands · capabilities
behavior/  semantic behaviors · idle · mood · scheduler
brain/     state · memory · cloud client · decision boundary
ui/        status · settings · diagnostics · hardware test
```

The first real file will be a port of
`reference/claude-r2d2-buddy/main/r2d2_central.c` — retargeted C6→S3,
`CONFIG_BT_NIMBLE_MAX_CONNECTIONS` 2→1, with `nus_peripheral.c` dropped (D-006).
