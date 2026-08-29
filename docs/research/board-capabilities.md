# Waveshare ESP32-S3-Touch-AMOLED-1.8 — hardware and software surfaces

Source: `waveshareteam/ESP32-S3-Touch-AMOLED-1.8` @ `ed7c6a5` (official),
cross-referenced against `vthinkxie/claude-desktop-buddy-esp32` @ `61a0ce9`
(third-party, V1). Revision differences are in
[board-revision.md](board-revision.md).

**Measured on hardware. `board_check` has run (2026-08-23).** The board is at
`/dev/cu.usbmodem2101` (native USB-JTAG, VID `0x303a` / PID `0x1001` — no bridge
chip, no driver). Silicon, revision, I²C inventory and the PMU rails are
**OBSERVED**; the per-peripheral capability tables further down are still read
from source and are labelled where they are not yet confirmed.

Two things are measured but NOT yet proven, and both are called out where they
appear: the PMU rails were read after a chip reset rather than a power cycle, so
they may be the factory firmware's configuration rather than power-on defaults;
and the `0x51` / `0x6B` identifications are inferred from the peripheral table,
not from a constant this clone contains.

**The microSD path is measured too, 2026-08-24** — mounts, and content
survives an unmount. See the SD section below. Issue #81.

### OBSERVED 2026-08-22 — `esptool.py flash_id`, read-only, nothing flashed

| | |
|---|---|
| Chip | ESP32-S3 (QFN56), **silicon rev v0.2** |
| PSRAM | **8 MB** embedded (AP_3v3) |
| Flash | **16 MB**, manufacturer `0x20`, device `0x4018` |
| eFuse flash mode | quad (4 data lines), 3.3 V |
| Crystal | 40 MHz |
| Base MAC | `28:84:85:90:B1:B0` |

Note **silicon** revision v0.2 is not **board** revision V1/V2 — different
things, similar names, and conflating them is the obvious mistake here.

### OBSERVED 2026-08-23 — `board_check` running on the board

First firmware we ever ran on it. Transcript:
`R2Z2-vault/Experiments/data/board-check-20260823-000316.log`.

These re-confirm the `flash_id` table above through a completely different
instrument — the ESP-IDF bootloader and `esp_psram`, not `esptool`:

| | Reported by | Value |
|---|---|---|
| Chip revision | `boot: chip revision` | **v0.2** |
| eFuse block rev | `boot: efuse block revision` | v1.4 |
| Flash size | `boot.esp32s3: SPI Flash Size` | **16MB** |
| Flash mode / speed | `boot.esp32s3` | QIO @ 80 MHz |
| PSRAM | `octal_psram` + `esp_psram` | **8 MB**, vendor `0x0d` (AP), 64 Mbit die, 80 MHz |
| Base MAC | `esp_read_mac` | `28:84:85:90:B1:B0` |

So the 8 MB flash figure is now refuted by two independent instruments.

#### I²C inventory — OBSERVED

`0x15  0x18  0x20  0x34  0x51  0x6B`

Addresses are OBSERVED. The identifications are **INFERRED** from the peripheral
table below, except where a citation is given:

| Addr | Part | Basis |
|---|---|---|
| `0x15` | CST816 touch (V2) | OBSERVED — `board_variant.c:20`, and the detector's own verdict |
| `0x18` | ES8311 audio codec | `ES8311_CODEC_DEFAULT_ADDR (0x30)` in `espressif__esp_codec_dev/device/include/es8311_codec.h:18` — that is the **8-bit** address; `0x30 >> 1 = 0x18` |
| `0x20` | TCA9554-class IO expander | OBSERVED — `board_variant.c:19` |
| `0x34` | AXP2101 PMU | OBSERVED — chip-ID register `0x03` read back `0x4A`, matching `AXP2101Constants.h:5` |
| `0x51` | PCF85063A RTC | INFERRED. `PCF85063A_ADDRESS` is *used* at `91_pcf85063_rtc/main/pcf85063_rtc.c:118` but **defined nowhere in the clone** — it comes from a component fetched at that example's build time. Not yet verified. |
| `0x6B` | QMI8658 IMU | INFERRED. Same situation: `QMI8658_ADDRESS_HIGH/LOW` are used at `92_qmi8658_imu/main/qmi8658_imu.c:49-50`, defined nowhere in the clone. |

`0x38` (FT3168, V1 touch) did **not** answer. That is the decisive negative.

#### AXP2101 rail inventory — AC11 table, derived by hand

**This table is derived from the datasheet constants, not from the firmware's
decoder.** That is the whole point of AC11: a decoder must not be allowed to
satisfy its own acceptance criterion. Formulas from
`reference/…/XPowersLib/src/REG/AXP2101Constants.h`; enable-bit positions from
`XPowersAXP2101.tpp` `isEnableDCn()` / `isEnableXLDOn()`.

Encodings used:

- DCDC1 — `(raw & 0x1F) × 100 mV + 1500 mV` (`:135-137`)
- DCDC2, DCDC4 — `raw & 0x7F`; `< 71` → `×10 + 500`; `≥ 71` → `×20 − 200` (`:139-148`, `:171-180`)
- DCDC3 — `raw & 0x7F`; `< 71` → `×10 + 500`; `71–87` → `×20 − 200`; `≥ 88` → `×100 − 7200` (`:158-167`)
- DCDC5 — `raw & 0x1F`; `0x19` → 1200 mV; else `×100 + 1400` (`:184-188`)
- ALDO1-4, BLDO1-2, DLDO1-2 — `(raw & 0x1F) × 100 mV + 500 mV` (`:196-222`, `:232-238`)
- CPUSLDO — `(raw & 0x1F) × 50 mV + 500 mV` (`:226-228`)
- Enables — `0x80` bits 0-4 = DCDC1-5; `0x90` bits 0-7 = ALDO1, ALDO2, ALDO3, ALDO4, BLDO1, BLDO2, CPUSLDO, DLDO1; `0x91` bit 0 = DLDO2

| Reg | Rail | Raw | Enable bit | Hand-derived |
|---|---|---|---|---|
| `0x03` | IC_TYPE | `0x4A` | — | matches AXP2101 chip id (`AXP2101Constants.h:5`) |
| `0x00` | STATUS1 | `0x20` | — | not decoded; recorded raw |
| `0x01` | STATUS2 | `0x15` | — | not decoded; recorded raw |
| `0x80` | DC_ONOFF | `0x0F` | — | DCDC1-4 **on**, DCDC5 **off** |
| `0x81` | DC_FORCE_PWM | `0x00` | — | no rail forced to PWM |
| `0x82` | DCDC1 | `0x12` | on | 18 × 100 + 1500 = **3300 mV** |
| `0x83` | DCDC2 | `0x28` | on | 40 < 71 → 40 × 10 + 500 = **900 mV** |
| `0x84` | DCDC3 | `0x46` | on | 70 < 71 → 70 × 10 + 500 = **1200 mV** |
| `0x85` | DCDC4 | `0x64` | on | 100 ≥ 71 → 100 × 20 − 200 = **1800 mV** |
| `0x86` | DCDC5 | `0x00` | **off** | would be 1400 mV; rail is off, so the field is not a rail state |
| `0x90` | LDO_ONOFF0 | `0xFF` | — | ALDO1-4, BLDO1-2, CPUSLDO, DLDO1 all **on** |
| `0x91` | LDO_ONOFF1 | `0x01` | — | DLDO2 **on** |
| `0x92` | ALDO1 | `0x1C` | on | 28 × 100 + 500 = **3300 mV** |
| `0x93` | ALDO2 | `0x1C` | on | 28 × 100 + 500 = **3300 mV** |
| `0x94` | ALDO3 | `0x19` | on | 25 × 100 + 500 = **3000 mV** |
| `0x95` | ALDO4 | `0x0D` | on | 13 × 100 + 500 = **1800 mV** |
| `0x96` | BLDO1 | `0x07` | on | 7 × 100 + 500 = **1200 mV** |
| `0x97` | BLDO2 | `0x17` | on | 23 × 100 + 500 = **2800 mV** |
| `0x98` | CPUSLDO | `0x0E` | on | 14 × 50 + 500 = **1200 mV** |
| `0x99` | DLDO1 | `0x00` | on | 0 × 100 + 500 = **500 mV** — the register floor, see below |
| `0x9A` | DLDO2 | `0x00` | on | 0 × 100 + 500 = **500 mV** — the register floor, see below |

**The hand derivation agrees with the firmware's decoder on every row.** That
corroborates the decoder; it is not what makes the table valid. The table stands
on the raw bytes and the constants.

> [!warning]
> **`DLDO1` and `DLDO2` read enabled with their voltage register at `0x00`.**
> `0x00` decodes to the minimum the field can express (500 mV), which is also
> the register's reset value — so "enabled at `0x00`" most plausibly means the
> enable bit is set and the voltage was never programmed. Whether either rail is
> physically connected to anything is **UNKNOWN**: the vendor repo ships no
> schematic. Do not treat 500 mV as a measured rail voltage. It is a register
> value whose meaning is unresolved.

> [!warning]
> **These are the rails as the FACTORY FIRMWARE left them, not proven power-on
> defaults.** The AXP2101 keeps its register state as long as it has power, and
> everything since the factory image was overwritten has been a *chip* reset
> (`rst:0x15 USB_UART_CHIP_RESET`), never a PMU power cycle. `board_check`
> writes nothing. So this read cannot distinguish "AXP2101 POR defaults" from
> "whatever the factory firmware configured before we replaced it" — and the
> spec's inference was specifically about *power-on defaults*.
>
> Cheap unrun experiment that settles it: **unplug, replug, re-run.** Our
> firmware writes no PMU registers, so a cold-boot read is the POR state. If it
> matches the table above, the inference holds as stated; if it differs, then
> what the SD step will actually see is the cold-boot values, not these.
>
> `LDO_ONOFF0 = 0xFF` — every LDO enabled — is the row that makes this worth
> checking rather than assuming.

**Nothing the SD path needs is gated off**, under either reading. AC12 clears.

### microSD — OBSERVED 2026-08-24

`firmware/sd_check/` (ours). Transcript:
`R2Z2-vault/Experiments/data/sd-check-20260824-224535.log`.

| | |
|---|---|
| Card under test | `SD32G`, **SDHC**, 30436 MB |
| Mount point | `/sdcard` (`BSP_SD_MOUNT_POINT`) |
| Clock | 20.00 MHz (at the reported limit) |
| Bus width | **1-bit** (`SSR: bus_width=1`) |
| CSD | ver=2, sector_size=512, capacity=62333952, read_bl_len=9 |
| Persistence across unmount | **PASS** |

**The persistence result is the one that took work.** The vendor example
(`09_sdmmc/main/sd_card_example_main.c`) reads at `:71` and unmounts at `:76` —
its last read is *before* the unmount, so it never demonstrates that anything
survives. `sd_check` does mount → write → read → **unmount → remount → re-read**,
and the re-read compares exact bytes against a per-boot nonce from
`esp_random()`. Without the nonce, a read-back could be satisfied by a previous
run's file, which is a false pass no amount of re-running would expose.

> [!warning]
> **Long filenames are OFF, and the failure looks like a broken card.**
> `CONFIG_FATFS_LFN_NONE` is the ESP-IDF default, so FAT 8.3 is all that works:
> `fopen()` fails outright on any name longer than **8 characters before the
> dot**. A first attempt used `r2z2_sd_check.txt` (13) and the write failed with
> nothing whatever wrong with the card. The BSP prints
> `Warning: Long filenames on SD card are disabled in menuconfig!` at every
> mount, which is the tell.
>
> **This is a live constraint, not a test artifact.** Anything storing memories,
> dated logs or per-session files on this card needs
> `CONFIG_FATFS_LFN_HEAP` — `session-2026-08-24.json` is not a legal 8.3 name.
> Left unset here on purpose; see the config warning below for why.

**Bus width is 1-bit**, not 4-bit, so throughput has a ceiling roughly a quarter
of what the interface could do. Not chased — nothing needs the bandwidth yet —
but worth knowing before anything streams audio to the card.

### Config that hangs this board — OBSERVED 2026-08-24, cost a manual recovery

The vendor's `09_sdmmc/sdkconfig.defaults` carries three settings that
`board_check`'s does not:

```
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
```

With those three, `sd_check` **hung during app startup** — the log stops between
`Disabling RNG early entropy source` and the `octal_psram` banner, i.e. inside
PSRAM init. No panic, no reboot loop, just silence.

**The expensive part is what the hang does to recovery.** It wedges the
USB-Serial-JTAG peripheral, so `esptool` can no longer reset the chip into
download mode: `Failed to connect to ESP32-S3: No serial data received`, on
`default_reset`, `usb_reset` and `no_reset` alike. This board is native USB-JTAG
(VID `0x303a` / PID `0x1001`, no bridge chip), so **there is no UART-side
DTR/RTS reset to fall back on.** Recovery required physically power-cycling the
board; after a clean power-up `esptool` connected on the first try.

Removing all three restored a clean boot. **NOT bisected** — the cache-line
setting is the prime suspect because it is the one that interacts with the PSRAM
cache, but that is a hypothesis, not a measurement. If you want any of them, add
one at a time and expect to reach for the buttons.

The rule this supports: **on this board, prefer the sdkconfig already OBSERVED
to boot over the one the vendor example ships.** `firmware/board_check/` is the
known-good reference; `firmware/sd_check/` now matches it exactly.

### Board revision: **V2** — OBSERVED 2026-08-23 (was INFERRED 2026-08-22)

Not yet OBSERVED on the bus; the OBSERVED test is `board_check` seeing `0x15`
answer after the touch reset release (issue #79, AC1/AC2). But two independent
lines of evidence agree, both derived from the full-flash backup taken before
the first flash:

1. **The dumped factory image is byte-identical to the vendor's V2 recovery
   image** — `Firmware/ESP32-S3-Touch-AMOLED-1.8-V2-FactoryXiaozhi_260601.bin`,
   0.00% differing across bootloader, partition table, factory app, `ota_0` and
   `assets`. Against the V1 image (`…FactoryXiaozhi_250805.bin`), per region:
   bootloader 57.1%, partition table 2.7%, factory app 90.8%, `ota_0` 95.0%,
   `assets` 94.5%. Factory-app SHA-256 `fd24fdd8…` matches V2 exactly; V1's is
   `f3bdf13d…`.

   The partition table's 2.7% is the honest outlier and is stated rather than
   folded into a range: two ESP-IDF partition tables are mostly identical
   padding, so a small percentage there is expected and is not evidence of
   similarity. The five figures are given individually because a "57–95%" range
   — which an earlier draft of this section published — quietly excluded it.
2. **The image links only V2 drivers.** `strings -a factory-backup.bin | grep -c`,
   case-sensitive, per spelling:

   | Spelling | Count | | Spelling | Count |
   |---|---|---|---|---|
   | `CO5300` | 1 | | `FT3168` | **0** |
   | `co5300` | 30 | | `ft3168` | **0** |
   | `CST816S` | 2 | | `SH8601` | **0** |
   | `cst816` | 12 | | `sh8601` | **0** |

   Symbols present include `esp_lcd_new_panel_co5300` and
   `esp_lcd_touch_cst816s_del`. The V1 parts appear **zero** times in any
   casing.

   Counted per spelling on purpose. An earlier draft published "`CST816S` (14)"
   and "`CO5300` (31)" — those were `grep -ci` totals across every casing,
   attributed to one capitalisation each. The conclusion did not change, because
   the V1 counts are zero case-insensitively too, but the numbers as written
   were wrong and would not have reproduced.

The step that makes this strong rather than suggestive: the factory demo
**runs** on this board. A build that links the CO5300 panel and CST816S touch
drivers and contains no V1 driver at all could not drive V1 hardware, so the
hardware matches the drivers present.

**CONFIRMED on the bus 2026-08-23.** `board_variant_detect()` reported
`modified CO5300 + CST816`, `0x15` answered and `0x38` did not. The pre-flash
inference was correct, and the two methods are independent: one compared flash
contents, the other probed the physical bus.

Consequence: **D-005 Amendment A's reversal is not triggered** — the BSP being
V2-only is a match, not a hazard. Corroborated a third way by the dependency
tree that `01_project_template` resolves: it pulls `espressif/esp_lcd_co5300`
and **no SH8601 driver at all** (`grep -ci sh8601 dependencies.lock` → 0). Note
it *does* pull both touch drivers, `esp_lcd_touch_cst816s` and
`esp_lcd_touch_ft5x06` — which is consistent, because D-005 Amendment A scopes
the V2-only claim to the **display**, not to touch.

Backup: `~/esp/r2z2-board-backups/factory-backup.bin`, 16,777,216 bytes,
SHA-256 `6f188fb9d35ee793a3423934a4fa4e7c1fef9cc9dae76f9f177dabe854a6cdb3`.
Kept outside the repo — it is a vendor binary.

---

## 1. Silicon and memory

**OBSERVED** — from `examples/esp-idf/14_lvgl_demo_v9/sdkconfig.defaults`:

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHMODE_QIO
CONFIG_ESPTOOLPY_FLASHSIZE_16MB
CONFIG_SPIRAM=y  CONFIG_SPIRAM_MODE_OCT=y  CONFIG_SPIRAM_SPEED_80M=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
```

Octal (OPI) PSRAM at 80 MHz, 240 MHz dual-core, 1 kHz tick. `vthinkxie` calls
the part **ESP32-S3R8 — 8 MB OPI PSRAM, 8 MB flash**; the flash figure
conflicts with the 16 MB above.

**RESOLVED 2026-08-22 — flash is 16 MB.** `esptool.py flash_id` reports
"Detected flash size: 16MB" on the physical board, and the full-image dump is
exactly 16,777,216 bytes with the partition table's last entry ending at
`0x1000000`. The `vthinkxie` 8 MB figure was inferred from a part number and is
**REFUTED** for this board; PSRAM at 8 MB is correct. See the OBSERVED table at
the top of this file.

ESP32-S3 has **Wi-Fi 4 + Bluetooth LE 5** on a shared radio. Coexistence is
supported by ESP-IDF but is a real scheduling constraint — see
[embedded-path.md](embedded-path.md).

## 2. Peripherals

| Function | Part | Bus / address | Source |
|---|---|---|---|
| Display | SH8601 (V1) / **CO5300 (V2)** | QSPI, `SPI2_HOST` | `13_display_colorbar/main/display_colorbar_main.c:15-32` |
| Touch | FT3168 `0x38` (V1) / **CST820 `0x15` (V2)** | I²C | `board_variant.c:20-21` |
| IO expander | TCA9554-class | I²C `0x20` | `board_variant.c:19` |
| PMU / battery | AXP2101 | I²C | `examples/esp-idf/90_axp2101_pmu/` |
| RTC | PCF85063A | I²C | `examples/esp-idf/91_pcf85063_rtc/` |
| IMU | QMI8658 (6-axis) | I²C | `examples/esp-idf/92_qmi8658_imu/` |
| Audio codec | ES8311 | I²C ctrl + I²S data | `examples/esp-idf/12_i2s_codec/` |
| Microphone | onboard, analog into ES8311 | I²S RX | `12_i2s_codec` sets `digital_mic = false` |
| Speaker | amp gated by `BSP_POWER_AMP_IO` | — | `12_i2s_codec/main/*.c:134` |
| Storage | microSD over SDMMC | `BSP_SD_CMD/CLK/D0` | **OBSERVED 2026-08-24** — mounts, 1-bit, 20 MHz, survives unmount. `firmware/sd_check/` |
| Wi-Fi | ESP32-S3 native | — | `examples/esp-idf/10_wifi_station/` |

### Pin map (V1/V2 common — OBSERVED)

Display QSPI (`display_colorbar_main.c:21-26`):
`CS=12, PCLK=11, D0=4, D1=5, D2=6, D3=7`.

I²C (`board_variant.c:15-16`, `display_colorbar_main.c:28-29`):
**`SDA=15, SCL=14`** @ 400 kHz, internal pullups. Shared by expander, PMU,
touch, RTC, IMU, codec.

IO expander bits (`board_variant.c:26-30`):
`BIT0 = LCD_RST, BIT1 = DSI_PWR_EN, BIT2 = TOUCH_RST, BIT7 = SD_CS`.
Config register `0x03`, output register `0x01`.

I²S / audio — the official examples use BSP macros (`BSP_I2S_MCLK`,
`BSP_I2S_SCLK`, `BSP_I2S_LCLK`, `BSP_I2S_DOUT`, `BSP_I2S_DSIN`,
`BSP_POWER_AMP_IO`) rather than raw numbers. `vthinkxie`'s V1 header gives
concrete values that should be treated as **INFERRED for V2 until checked**:
`MCLK=16, BCLK=9, WS=45, DI=10, DO=8, PA_CTRL=46`, touch INT `21`, BOOT key
`GPIO0`.

**Prefer the BSP macros over hardcoded pins.** They are the vendor's contract
and they absorb revision differences.

## 3. Software surfaces

### The BSP is a managed component, not vendored code

**OBSERVED** — every ESP-IDF example declares
(`00_board_check/main/idf_component.yml`):

```yaml
dependencies:
  idf: ">=5.5,<6.1"
  waveshare/esp32_s3_touch_amoled_1_8:
    version: "^2.0.3"
    public: true
```

So the drivers are **not in this repo** — the IDF Component Manager fetches
them into `managed_components/` at build time. Consequences:

- We have not yet read the BSP source; it is not in the clone. **UNKNOWN**
  until first build: exactly how the BSP selects the V1 vs V2 panel driver, and
  whether `bsp_display_start()` handles both transparently. The presence of a
  separate `board_variant` component in three examples suggests the BSP may
  *not* expose variant detection itself.
- A build needs network access the first time.
- `^2.0.3` floats within 2.x. Pin an exact version once something works.

### Toolchain requirement

**OBSERVED** — `docs/GETTING_STARTED.md:17`: "Use ESP-IDF **v5.5.x or v6.0.x**".
CI builds with 5.5.5 and 6.0.2 for `esp32s3`. Note `claude-r2d2-buddy` was
built on **5.4.2** — a version gap to bridge, though its BLE code uses stable
NimBLE GATT APIs.

### BSP capability macros

`00_board_check/main/board_check_main.c:52-57` reads: `BSP_CAPS_DISPLAY`,
`BSP_LCD_H_RES`/`V_RES`, `BSP_CAPS_TOUCH`, `BSP_CAPS_AUDIO_SPEAKER`,
`BSP_CAPS_AUDIO_MIC`, `BSP_CAPS_SDCARD`, `BSP_I2C_SDA`/`SCL`,
`BSP_SD_CMD`/`CLK`/`D0`. That single example is the cheapest possible
confirmation of the whole capability surface — run it first.

### Examples mapped to our subsystems

| Our need | Example |
|---|---|
| First contact, capability dump | `00_board_check` |
| Display + touch + LVGL + brightness + SD | `00_bsp_quickstart` |
| Project skeleton on managed BSP | `01_project_template` |
| Settings persistence | `03_nvs_counter` |
| Task/queue structure for the behavior engine | `04_freertos_tasks` |
| Full I²C bus dump (revision ID) | `08_i2c_tools` |
| Logs / memory / character data on SD | `09_sdmmc` |
| Cloud connectivity | `10_wifi_station` |
| Speaker + mic path | `12_i2s_codec` |
| Panel bring-up + **runtime V1/V2 detect** | `13_display_colorbar` |
| Service-panel UI | `14_lvgl_demo_v9` (LVGL 9) |
| Battery telemetry for the status screen | `90_axp2101_pmu` |
| Quiet hours / wall clock across reboots | `91_pcf85063_rtc` |
| "Was I picked up / bumped" | `92_qmi8658_imu` |

**INFERRED** — that list covers every subsystem the service panel needs, with
first-party code, before we write a line of our own driver. The brief's
instinct to reuse rather than rewrite is well supported by the evidence.

## 3b. S4 display bring-up — OBSERVED 2026-08-29

**The panel renders. First pixels in this project.**

`reference/.../examples/esp-idf/13_display_colorbar`, unmodified, built against
ESP-IDF **v5.5.5** and flashed to the board. Serial reported the full path and
the **operator confirmed colour bars on the glass by eye** — the claim is
OBSERVED on real hardware, not inferred from a returning draw call.

```
display_colorbar: Detected V2 board revision
display_colorbar: Initialize CO5300 over QSPI
co5300_spi: LCD panel create success, version: 2.1.0
display_colorbar: Drawing RGB565 color bars
```

Two things that settle open questions:

- **V2 agreed again — by a second independent IMPLEMENTATION, not a third
  instrument.** The example printed "Detected V2 board revision" from its own
  `detect_v2_board()`. That is the *same* `0x15` heuristic `board_variant.c`
  and the bus map already use, re-implemented, which is what
  `board-revision.md:70` already calls it. Agreement between three readings of
  one heuristic is not three instruments, and calling it that would inflate
  the evidence.
- **The panel lit without THIS app touching the expander or PMU — and that is
  NOT the same as "no setup is needed". INFERRED, with a known confound.**
  What is OBSERVED: `13_display_colorbar` never writes `0x20` or `0x34`, passes
  `reset_gpio_num = GPIO_NUM_NC`, and the bars appeared. The Waveshare BSP file
  likewise only creates the TCA9554 handle and never calls
  `esp_io_expander_set_level`.

  **The confound:** `board_variant.c:56-63` — run by `board_check` in an
  earlier session — *does* drive the expander, writing `IO_EXPANDER_OUTPUT_MASK`
  and so asserting `LCD_RST` and `DSI_PWR_EN`. Every reset since has been a
  *chip* reset (`rst:0x15 USB_UART_CHIP_RESET`), never a PMU or board power
  cycle, which is the same caveat this file already records for the AXP2101
  rails above. The expander's output latches plausibly survived, so the
  colorbar may have inherited an already-enabled panel rather than proving it
  needs nothing.

  **The test nobody has run: power the board down fully, then flash and boot
  ONLY the colorbar.** Until then, "the minimal path lights a cold board" is
  unproven. Recorded this way because the opposite was first assumed, then
  over-corrected into an equally unearned claim.

### `00_bsp_quickstart` does NOT boot — UNRESOLVED

Same board, same toolchain, minutes apart. It builds clean and flashes clean
(`Hash of data verified`), then emits **zero serial bytes — not even bootloader
output**, where `13_display_colorbar` prints its full boot log on the same port.

Eliminated:

| Hypothesis | Test | Result |
|---|---|---|
| PSRAM XIP settings hang init | rebuilt with `CONFIG_SPIRAM_FETCH_INSTRUCTIONS=n`, `CONFIG_SPIRAM_RODATA=n` | still silent |
| Console routed elsewhere | diffed `CONFIG_ESP_CONSOLE_*` against the working example | **identical** |
| Chip bricked / USB recovery wedged | `esptool.py chip_id` | answers normally |

**Cause UNKNOWN.** Zero bytes *including no bootloader output* points earlier
than the app — which also argues against the partition table, since the
bootloader must run (and print) before it can parse one.

There are **sixteen** settings in `00_bsp_quickstart/sdkconfig.defaults`
absent from the colorbar's, and only two were tested:

```
CONFIG_SPIRAM=y                      CONFIG_COMPILER_OPTIMIZATION_PERF=y
CONFIG_SPIRAM_MODE_OCT=y             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_SPIRAM_SPEED_80M=y            CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y   CONFIG_FREERTOS_HZ=1000
CONFIG_SPIRAM_RODATA=y               CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_LV_* (fonts, perf monitor, FAST_MEM_USE_IRAM)
```

**Note especially that PSRAM itself was never ruled out.** Only the two XIP
settings were disabled; `CONFIG_SPIRAM=y` stayed on, and the colorbar ships no
PSRAM settings at all. And three of the others —
`CONFIG_FREERTOS_HZ=1000`, `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`,
`CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y` — are **exactly the trio this file
already records at "Config that hangs this board", OBSERVED 2026-08-24, which
cost a manual recovery.** They are the prime suspects, not the partition table.

Not chased further because the display question was already answered.

> [!note]
> `firmware/sdkconfig.no-psram-xip.defaults` exists to layer over a vendor
> example without patching the gitignored clone:
> `idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;<that file>" build`.

### Factory image restored

`~/esp/r2z2-board-backups/factory-backup.bin`, 16,777,216 bytes.

Precisely what was checked, because "verified" without a named surface is worth
nothing: the file's sha256 was computed **before** writing and matched its
`.sha256` sidecar; `esptool` then reported `Hash of data verified`, which is its
own check of the data it transferred. **The flash was never read back and
re-hashed afterwards.** An earlier draft of this section claimed the sha was
"verified before *and* after writing". It was not.

What IS proven, and by the strongest available instrument: the operator
confirmed the stock Xiaozhi UI came back on screen. The backup restores to a
working system.

## 4. Gaps and cautions

- **No schematic in the repo.** Vendor README says so, and adds that CI
  "does not replace hardware validation of pins, PSRAM, USB, display, touch,
  audio, or sensors." Treat pin claims as INFERRED until `00_board_check` and
  `08_i2c_tools` agree with them.
- **Microphone is analog into ES8311** (`digital_mic = false`), so mic capture
  is I²S RX through the codec, not a PDM peripheral. Matters when we get to
  wake-word — and voice is deliberately late in the plan.
- **Speaker amp is gated by a GPIO** (`BSP_POWER_AMP_IO`). Leaving it enabled
  wastes power; a quiet-hours feature should actually cut the amp.
- **UNKNOWN — power budget.** No measurement of what the AMOLED + Wi-Fi + BLE
  scanning draw together, and the backpack rides on R2. Screen-off/dim
  behaviour is a first-class requirement, not a nicety.
- **UNKNOWN — physical mounting.** Backpack dimensions, weight, and how power
  is supplied (own battery via AXP2101, or tapped from R2) are entirely
  unaddressed. This is a real open problem, not a software one.
