# Waveshare ESP32-S3-Touch-AMOLED-1.8 — hardware and software surfaces

Source: `waveshareteam/ESP32-S3-Touch-AMOLED-1.8` @ `ed7c6a5` (official),
cross-referenced against `vthinkxie/claude-desktop-buddy-esp32` @ `61a0ce9`
(third-party, V1). Revision differences are in
[board-revision.md](board-revision.md).

**Nothing here is measured — the board has not been connected.** No USB serial
device was present on this host at init time (`ls /dev/cu.*` showed only
Bluetooth/debug ports). Everything below is read from source and must be
confirmed by `00_board_check` on first boot.

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
conflicts with the 16 MB above. **UNKNOWN — resolve with `00_board_check`.**

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
| Storage | microSD over SDMMC | `BSP_SD_CMD/CLK/D0` | `examples/esp-idf/09_sdmmc/` |
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
