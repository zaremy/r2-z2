# Source Map — external repositories

All clones live in `reference/` (gitignored). SHAs captured
2026-08-15. Every claim elsewhere in `docs/research/` cites these by
`repo/path/file.ext:line`.

## Provenance ledger

| Repo | Branch | SHA | Last commit | License | Reuse |
|---|---|---|---|---|---|
| [ccb/sphero-r2d2](https://github.com/ccb/sphero-r2d2) | `main` | `5401f31d7487f47d8e35f05bc0808e46e980143c` | 2026-01-16 | MIT (Chris Callison-Burch) | **Learn from** — protocol reference; we reimplemented the packet layer |
| [baoshi/claude-r2d2-buddy](https://github.com/baoshi/claude-r2d2-buddy) | `main` | `cca13eccb69769445d25625f562df32a3804e833` | 2026-04-30 | MIT (Baoshi) | **Reuse code** — `r2d2_central.c` is a near-drop-in BLE central |
| [waveshareteam/ESP32-S3-Touch-AMOLED-1.8](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8) | `main` | `ed7c6a5c1d59339fa18c271a7f00db8f151aaf29` | 2026-08-10 | Apache-2.0 | **Reuse** — official BSP component + examples; hardware source of truth |
| [vthinkxie/claude-desktop-buddy-esp32](https://github.com/vthinkxie/claude-desktop-buddy-esp32) | `main` | `61a0ce9f7410ed87de2032f226e511c9e59abbfe` | 2026-05-08 | MIT (Anthropic, PBC) | **Learn from only** — its 1.8" target is the *V1* board; do not flash |
| [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) | `main` | `a280c6421931431ba6905aee9d2b50b2bfd8c103` | 2026-04-16 | MIT (Anthropic, PBC) | **Optional** — Hardware Buddy wire protocol, if we ever add a Claude-activity sensor |
| [astagi/freer2](https://github.com/astagi/freer2) | `master` | `4920b866ee13685d83aad70febe4e1210567ed11` | 2020-04-04 | MIT (Andrea Stagi) | **Historical corroboration** — original reverse-engineering of the UUIDs/handshake |

## Per-repo notes

### ccb/sphero-r2d2 — Mac-side protocol reference

A vendored + extended fork of `spherov2.py` with added R2-D2 docs and
validation harnesses. 75 tracked files.

Files that mattered:

- `spherov2/controls/v2.py:14-159` — the entire V2 packet layer: `Packet`
  NamedTuple, `Flags`, `Encoding` (SOP/EOP/escape), `Error` enum,
  `build()`, `parse_response()`, `Manager` (sequence allocation), `Collector`
  (stream reassembly). **This is the single most valuable file in the repo.**
- `spherov2/helper.py` — `packet_chk()` = `0xff - (sum & 0xff)`.
- `spherov2/toy/__init__.py:20-121` — `Toy` lifecycle: handshake list, notify
  subscribe, 20-byte MTU chunking (`:79`), `cmd_safe_interval` sleep (`:81`).
- `spherov2/toy/__init__.py:127-131` — `ToyV2` send/response UUIDs. Note the
  large commented-out block of candidate UUIDs — evidence the author was
  probing, so treat the *uncommented* line as the tested one.
- `spherov2/toy/bb9e.py:22` — `_handshake`: the anti-DoS write. R2D2 inherits it.
- `spherov2/toy/r2d2.py` — R2-D2 identity (`:15`), `LEDs` (`:17-25`),
  `Audio` (388 ids, `:27-415`), `Animations` (51 ids, `:417-468`),
  `extended_sensors` incl. `r2_head_angle` (`:470-477`).
- `spherov2/commands/animatronic.py:29-87` — DID 23; play/stop animation, head
  and leg position, leg actions, idle-animation toggle, trophy mode.
- `spherov2/commands/io.py:51-217` — DID 26; audio playback, volume, LED masks.
- `spherov2/commands/power.py:63+` — DID 19; wake, sleep, battery.
- `spherov2/adapter/bleak_adapter.py` — thin Bleak wrapper. Writes with
  `response=True`.
- `docs/R2D2_PROTOCOL.md`, `docs/BLE_CHARACTERISTICS.md` — the author's own
  write-ups. Accurate where we checked them, but they are secondary; we
  verified against the code.

Warnings:
- `requirements.txt` pins `bleak>=1.1.1`; we install bleak 3.0.2. Not exercised
  end-to-end yet — this is why our probe does not import spherov2 at runtime.
- `spherov2/commands/io.py:55,87,91` and several others carry
  `# Untested / Unknown Param Names`. Treat those CIDs as UNKNOWN.

### baoshi/claude-r2d2-buddy — embedded BLE central reference

Only 12 tracked files; ESP-IDF, NimBLE, targets **ESP32-C6**
(`sdkconfig.defaults`: `CONFIG_IDF_TARGET="esp32c6"`,
`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`).

- `main/common.h:18-39` — R2-D2 UUIDs as NimBLE little-endian byte arrays,
  name prefix `"D2-"`, magic string.
- `main/r2d2_central.c` — **the reusable asset.** Two-phase GATT discovery,
  magic write, CCCD subscribe, 1-byte-notification reassembly (`:68-93`),
  TX queue task, wake (`:274-282`), keepalive (`:389-405`), disconnect/rescan
  (`:333-349`).
- `main/translator.c` — Claude-event → R2 behavior mapping. Useful as a
  *choreography* example (`:126-135` maps each event to animation + front LED +
  back LED + head angle simultaneously). Its animation-ID comments
  (`:93-108`) conflict with spherov2 — see `r2-capabilities.md`.
- `main/nus_peripheral.c` — Nordic UART peripheral for Claude Desktop.
  **We do not need this.** Dropping it removes the dual-role BLE requirement.

### waveshareteam/ESP32-S3-Touch-AMOLED-1.8 — hardware source of truth

3598 tracked files. Modernized repo (README states it supports *both* display
and touch revisions).

- `examples/esp-idf/` — 17 examples, all on the managed BSP component
  `waveshare/esp32_s3_touch_amoled_1_8` version `^2.0.3`
  (`00_board_check/main/idf_component.yml`).
- `examples/esp-idf/92_qmi8658_imu/components/board_variant/board_variant.c` —
  **runtime V1/V2 detection by I2C probe.** The most important file for us.
  Also present under `90_axp2101_pmu` and `91_pcf85063_rtc`.
- `examples/esp-idf/13_display_colorbar/main/display_colorbar_main.c:16-50` —
  QSPI pin map and the CO5300 init command sequence; `:53-76` a second,
  independent V2 probe implementation.
- `examples/esp-idf/12_i2s_codec/` — ES8311 via `esp_codec_dev`, `BSP_I2S_*`
  pins, `BSP_POWER_AMP_IO`, `digital_mic = false`.
- `examples/esp-idf/14_lvgl_demo_v9/sdkconfig.defaults` — the PSRAM/flash/LVGL
  config baseline (OCT PSRAM @ 80 MHz, 16 MB flash, 240 MHz, FreeRTOS 1000 Hz).
- `examples/arduino/` (16 sketches, SH8601/FT3168) vs
  `examples/arduino-v2/` (10 sketches, CO5300/CST820) — the explicit
  revision split on the Arduino side.
- `Firmware/ESP32-S3-Touch-AMOLED-1.8-V2-FactoryXiaozhi_260601.bin` — a V2
  factory image exists as a checked-in product asset.

Warnings:
- README explicitly states **no schematic is included** and that CI "does not
  replace hardware validation of pins, PSRAM, USB, display, touch, audio, or
  sensors."
- Naming is inconsistent: the marketing/README name is **CST820**, the ESP-IDF
  code says **CST816**, and the Arduino V2 driver identifiers say
  `Arduino_CST816x`. README notes those identifiers "do not describe the fitted
  V2 touch chip." All refer to the same 0x15 I2C address.

### vthinkxie/claude-desktop-buddy-esp32 — service-panel UI reference

PlatformIO + Arduino 3.x via pioarduino. Clean board-HAL separation
(`src/hw/*` + one header per board under `src/boards/`).

- `src/boards/board_waveshare_esp32s3_touch_amoled_1_8.h` — **targets V1**:
  `BOARD_DISPLAY_CO5300 0` (`:52`), FT3168 @ 0x38 (`:55`), plus the TCA9554
  expander pin map (`:44-49`) and full GPIO map (`:19-42`).
- `src/hw/display.cpp:6-79` — compile-time `Arduino_SH8601` vs `Arduino_CO5300`
  switch; the CO5300 path exists for the 1.75C/2.16 boards.
- `src/hw/input.cpp:58` — `Arduino_FT3x68` at `FT3168_DEVICE_ADDRESS`.
- `src/hw/{power,audio,imu,rtc,expander}.cpp` — the HAL split worth imitating.

Warnings:
- **Do not flash `-e waveshare-esp32s3-touch-amoled-1-8` to a V2 board.** It
  would drive a CO5300 panel with an SH8601 init sequence and probe touch at
  0x38 where nothing answers.
- Its board table claims 8 MB flash for the 1.8"; Waveshare's own
  `sdkconfig.defaults` says 16 MB. UNKNOWN — resolve on hardware.

### anthropics/claude-desktop-buddy — optional integration

Reference firmware (M5StickC Plus) plus `REFERENCE.md`, which documents the
wire protocol independently of the code: Nordic UART transport, heartbeat
snapshot, turn events, permission decisions, commands/acks, folder push,
pairing. Relevant only if we later let R2 *react to* Claude Code activity as
one sensor among many. Must stay a plugin, never the spine.

### astagi/freer2 — historical corroboration

Node.js + noble, 2020. `index.js:3-27` independently lists the same
CONNECT/MAIN service and characteristic UUIDs, the same magic string as a byte
array, the same SOP/EOP/escape constants, and DID/CID triples for init, off,
rotate, animation, carriage (leg action), move, accelerometer. It is the
provenance root for the constants that both newer repos inherited — which is
exactly why we re-derived them from spherov2 rather than copying.
