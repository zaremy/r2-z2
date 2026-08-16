# Waveshare ESP32-S3-Touch-AMOLED-1.8 — V1 vs V2

**Resolve this before flashing anything third-party.** The two revisions
share a part number, a product page, and a form factor, but ship *different
display and touch controllers*.

Source: `waveshareteam/ESP32-S3-Touch-AMOLED-1.8` @ `ed7c6a5`,
`vthinkxie/claude-desktop-buddy-esp32` @ `61a0ce9`.

---

## The two stacks

| | **Original (V1)** | **V2** |
|---|---|---|
| Display controller | **SH8601** (QSPI) | **CO5300** (QSPI) |
| Touch controller | **FT3168** @ I²C `0x38` | **CST820 / CST816** @ I²C `0x15` |
| Panel | 1.8" 368×448 AMOLED | same |
| Everything else | AXP2101 PMU, PCF85063A RTC, QMI8658 IMU, ES8311 audio, microSD, TCA9554-class expander @ `0x20` | same |

**OBSERVED** — the split is stated in the vendor README ("the original
SH8601/FT3168 board and the newer CO5300/CST820 board") and structurally
enforced by two parallel Arduino example sets:
`examples/arduino/` (16 sketches, V1) and `examples/arduino-v2/`
(10 sketches, V2), each with its own bundled `GFX_Library_for_Arduino` and
`SensorLib`.

### Naming warning

The same V2 touch part appears as **CST820** (README/marketing), **CST816**
(ESP-IDF code, `board_variant.h:12` `BOARD_VARIANT_CO5300_CST816`), and
`Arduino_CST816x` (bundled Arduino driver). The vendor README explicitly notes
those identifiers "do not describe the fitted V2 touch chip" — they are
compatible-driver names. **All three mean: the thing answering at I²C `0x15`.**
Match on the address, not the name.

---

## How to identify your board — in software, no disassembly

**OBSERVED** — Waveshare ships a runtime detector. Probe the I²C bus and see
which touch controller answers:

`examples/esp-idf/92_qmi8658_imu/components/board_variant/board_variant.c`
(also under `90_axp2101_pmu`, `91_pcf85063_rtc`):

1. Bring up I²C on **SDA = GPIO15, SCL = GPIO14** @ 400 kHz, internal pullups.
2. Release the touch reset **through the IO expander at `0x20`**
   (`release_touch_reset()`, `:41-72`): set the config register `0x03` to make
   `LCD_RST|DSI_PWR_EN|TOUCH_RST|SD_CS` outputs, drive output register `0x01`
   low-then-high, wait 20 ms then 150 ms. **This step is not optional** — the
   touch chip is held in reset at boot and will not answer without it.
3. Probe `0x15` → **V2 (CO5300 + CST816/CST820)**.
   Else probe `0x38` → **V1 (SH8601 + FT3168)**.
   Else unknown.

A second, independent implementation is at
`examples/esp-idf/13_display_colorbar/main/display_colorbar_main.c:53-76`
(`detect_v2_board()`), which skips the expander dance and probes `0x15`
directly — simpler, and it also selects a **`V2_PANEL_X_GAP` of `0x10`**
(16 px column offset) when V2 is detected. That offset is a real, concrete V1/V2
difference beyond the driver swap.

### Recommended first action on the board

Flash `examples/esp-idf/00_board_check` (serial only, does not touch the
panel), then `13_display_colorbar`, which prints
`Detected <V2|original> board revision` and then renders. Both are first-party
and revision-safe. `08_i2c_tools` is the belt-and-braces option: it dumps the
whole bus, so you will see `0x15` vs `0x38`, plus `0x20` (expander), AXP2101,
PCF85063A, QMI8658 and ES8311 addresses in one shot.

**UNKNOWN** — whether a physical silkscreen/revision marking exists. The
vendor repo contains **no schematic** (README: "A board-level schematic is not
included in this repository yet"). Software probe is the reliable method.

---

## What is safe to flash

| Firmware | V1 | V2 | Notes |
|---|---|---|---|
| `examples/esp-idf/*` (this repo) | ✅ | ✅ | Uses managed BSP `waveshare/esp32_s3_touch_amoled_1_8 ^2.0.3`; `13_display_colorbar` and the `board_variant` component branch at runtime |
| `examples/arduino/*` | ✅ | ❌ | SH8601 + FT3168 hardcoded |
| `examples/arduino-v2/*` | ❌ | ✅ | CO5300 + CST820 |
| `Firmware/…-V2-FactoryXiaozhi_260601.bin` | ❌ | ✅ | Checked-in V2 factory image — the recovery path |
| `Firmware/…-FactoryXiaozhi_250805.bin` | ✅ | ❌ | Original factory image |
| **`vthinkxie` `-e waveshare-esp32s3-touch-amoled-1-8`** | ✅ | **❌ DO NOT FLASH** | See below |

### Why vthinkxie's 1.8" build is V1-only

**OBSERVED** — `src/boards/board_waveshare_esp32s3_touch_amoled_1_8.h`:

- `:52` `#define BOARD_DISPLAY_CO5300 0` → `display.cpp:62` instantiates
  `Arduino_SH8601`.
- `:55` comment "Touch: FT3168 @ 0x38 via Arduino_DriveBus (Arduino_FT3x68)";
  `input.cpp:58` constructs `Arduino_FT3x68` at `FT3168_DEVICE_ADDRESS`.

**INFERRED** — flashed to a V2 board this yields an SH8601 init sequence sent
to a CO5300 controller (blank or corrupted panel), touch probing an address
where nothing answers (dead touch), and no `V2_PANEL_X_GAP` offset even if the
panel did come up. Probably not damaging — it is QSPI command traffic and an
I²C read that NAKs — but definitively non-functional, and debugging it from the
symptom would waste a day.

The repo's *other* environments (1.75C, 2.16) do use `Arduino_CO5300`, so the
CO5300 code path exists in-tree; it is simply not wired to the 1.8" board
header. A V2 port would mean flipping `BOARD_DISPLAY_CO5300` to 1, adding the
column offset, and swapping the touch driver to the CST81x/CST820 family — a
real change, not a rebuild.

**Verdict:** treat `vthinkxie` as a **service-panel architecture reference**
(its `src/hw/` HAL split, NVS persistence, battery UI, PSRAM framebuffer use),
not as our firmware base.

### Other unresolved V1/V2-adjacent discrepancy

**Flash size.** `vthinkxie`'s board table says the 1.8" is 8 MB flash (and it
ships `no_ota_8mb.csv`); Waveshare's own
`examples/esp-idf/14_lvgl_demo_v9/sdkconfig.defaults` sets
`CONFIG_ESPTOOLPY_FLASHSIZE_16MB` with OPI/OCT PSRAM at 80 MHz. **UNKNOWN**
which applies to our unit — `00_board_check` prints the real flash size and
PSRAM state, so this resolves itself on first boot. It matters for partitioning
and for whether OTA is affordable later.

---

## Checklist for first power-on

1. Do **not** flash anything third-party yet.
2. `idf.py -p <PORT> flash monitor` on `00_board_check` → record chip revision,
   flash size, PSRAM init state, BSP capability flags, free heap.
3. `08_i2c_tools` → record the full I²C address map.
4. `13_display_colorbar` → confirm the printed revision matches the probe, and
   that the panel renders.
5. Record all of it in `docs/decisions.md` with the date. Every later "which
   board do I have" question should be answered from that record, not re-derived.
