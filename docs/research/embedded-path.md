# Embedded foundation — comparing the viable paths

Question: what should the ESP32-S3 backpack firmware be built on?
Decided on evidence, not preference. The user's stated bias — reuse proven code
over rewriting drivers — turns out to be *supported* here, but it points at a
different repo than expected.

---

## The three candidates

### A. ESP-IDF + the official Waveshare managed BSP

- **Board support:** first-party, current (`ed7c6a5`, 2026-08-10), and
  **revision-aware** — `13_display_colorbar` and the `board_variant` component
  detect V1/V2 at runtime.
- **BLE central:** ESP-IDF's NimBLE. This is *exactly* what
  `claude-r2d2-buddy/main/r2d2_central.c` is written against — it uses
  `ble_gap_disc`, `ble_gattc_disc_all_svcs`, `ble_gattc_write_flat`,
  `ble_gattc_write_no_rsp_flat`, `ble_gap_connect`. Portable to S3 essentially
  unchanged.
- **Wi-Fi + BLE coexistence:** native, supported, configurable.
- **Audio:** `12_i2s_codec` uses `esp_codec_dev` with ES8311 — the standard
  Espressif audio stack, and the on-ramp to ESP-SR later.
- **LVGL:** `14_lvgl_demo_v9` (LVGL 9) and `00_bsp_quickstart` via the BSP.
- **PSRAM:** OCT @ 80 MHz configured in the vendor defaults.
- **OTA:** ESP-IDF native. Partitioning is ours to choose.
- **Cost:** ESP-IDF is not installed on this host; needs setup. Verbose C.
  IDF **v5.5.x or v6.0.x** required (`docs/GETTING_STARTED.md:17`).

### B. Arduino / PlatformIO, following `vthinkxie`

- **Board support:** third-party and, for the 1.8", **V1-only**
  (`BOARD_DISPLAY_CO5300 0`, FT3168 @ 0x38). Porting to V2 means swapping the
  display driver, the touch driver, and adding the panel column offset — real
  work on the exact layer we least want to own.
- **BLE central:** the repo's BLE is `ble_bridge_nimble.cpp` in the
  **peripheral** role (it advertises Nordic UART to Claude Desktop). We would
  be writing the central role ourselves in Arduino-NimBLE, discarding the C
  implementation that already works.
- **Wi-Fi + BLE:** available, but the project pins a specific pioarduino
  platform zip — a third-party fork of the Espressif platform.
- **Strength:** genuinely good UI/HAL architecture, and the fastest path to
  *pixels*.
- **Cost:** we would inherit V1 board assumptions and rewrite the one piece
  (R2 BLE central) that is already solved.

### C. Hybrid — ESP-IDF with the official BSP, borrowing `vthinkxie`'s structure

Not a third toolchain: option A, with `vthinkxie` read as an architecture
reference rather than compiled. Its `src/hw/{display,touch,power,audio,imu,
rtc,expander}.cpp` split, its NVS persistence, and its battery UI are worth
imitating in the `firmware/platform/` layer.

---

## Comparison

| Criterion | A: ESP-IDF + official BSP | B: Arduino/PlatformIO (`vthinkxie`) |
|---|---|---|
| V2 board compatibility | ✅ runtime detection, first-party | ❌ V1 hardcoded for 1.8" |
| BLE **central** to R2 | ✅ `r2d2_central.c` drops in | ❌ rewrite required |
| Wi-Fi + BLE coexistence | ✅ native, tunable | ⚠️ via forked platform |
| Audio (ES8311 + mic) | ✅ `esp_codec_dev`, → ESP-SR | ✅ own ES8311 lib |
| LVGL | ✅ LVGL 9 via BSP | ✅ LVGL, proven UI |
| PSRAM framebuffer | ✅ vendor defaults | ✅ demonstrated |
| Long-running reliability | ✅ task watchdogs, heap tooling | ⚠️ Arduino loop model |
| Upstream code reuse | ✅ **both** BSP and BLE central | ⚠️ UI only |
| Build complexity | ⚠️ IDF install, first-build network | ✅ `pio run` |
| Debugging | ✅ full IDF tooling | ⚠️ serial-first |
| OTA later | ✅ native | ✅ possible |

---

## Recommendation: **A, informed by C**

ESP-IDF v5.5.x with the managed BSP `waveshare/esp32_s3_touch_amoled_1_8 ^2.0.3`,
NimBLE in central role, LVGL 9 for the service panel.

The decisive argument is that it is the **only path where both hard parts are
already solved by someone else**:

1. **The R2 BLE central exists and is IDF/NimBLE C.**
   `claude-r2d2-buddy/main/r2d2_central.c` (414 lines) is the single most
   valuable artifact found. It already handles the two things a naive port gets
   wrong — the mandatory *second* service discovery after the magic write
   (`:96-110`), and byte-at-a-time notification reassembly with SOP resync
   (`:68-93`).
2. **The board support is first-party and revision-aware** — which the Arduino
   third-party path, for our specific board revision, is not.

Choosing B would mean rewriting (1) in order to reuse a UI, while inheriting
wrong-revision display and touch drivers. That inverts the effort.

### C6 → S3 port assessment for `claude-r2d2-buddy`

| Component | Verdict |
|---|---|
| `main/r2d2_central.c` | **Reuse ~unchanged.** Pure NimBLE GATT client; nothing C6-specific. |
| `main/common.h` UUIDs/magic | **Reuse verbatim** (NimBLE little-endian 128-bit arrays). |
| `main/translator.c` packet builder | **Reuse the encoder**, discard the Claude-event mapping. Note it omits escaping — fine for its fixed payloads, unsafe in general; our encoder must escape (see `r2-protocol.md` §4). |
| `main/nus_peripheral.c` | **Delete.** Only exists to be a BLE peripheral for Claude Desktop. |
| `sdkconfig.defaults` | **Rewrite.** `CONFIG_IDF_TARGET="esp32c6"` → `esp32s3`; `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2` → **1**; add PSRAM/flash/LVGL settings from `14_lvgl_demo_v9`. |
| IDF version | 5.4.2 → **5.5.x**. NimBLE GATT client API is stable across that gap; verify at build. |

**The simplification the brief predicted is real.** Dropping the peripheral role
takes us from dual-role BLE (central + peripheral, 2 connections, coexisting
with Wi-Fi) to **single-role central, 1 connection**. That removes the hardest
scheduling constraint on a shared radio before we add Wi-Fi.

### Risks in this recommendation

- **RESOLVED 2026-08-18 — the BSP is V2-native and cannot drive a V1 panel.**
  Settled by building `01_project_template` unmodified on ESP-IDF v5.5.5, which
  fetches the source. **The feared failure was backwards.** This risk was
  written as "the BSP may not handle V2"; it handles V2 and *only* V2.

  | | BSP v2.0.3 behaviour |
  |---|---|
  | Display | `esp_lcd_new_panel_co5300()` called **unconditionally** (`esp32_s3_touch_amoled_1_8.c:458`). No branch, no Kconfig, and **no SH8601 dependency exists** — `idf_component.yml` declares `espressif/esp_lcd_co5300: ^2.0.3` and nothing for V1 |
  | Touch | **Runtime probe.** CST816S `0x15` first, else FT5x06 `0x38`, else `ESP_ERR_NOT_FOUND` (`:502-517`) |
  | Panel X offset | `0x10`, applied **only when CST816S is found** (`:27`, `:507`) |

  The BSP therefore infers a **display** parameter from the **touch** probe.
  Its own README says so: *"BSP v2.0.3 keeps the FT5x06 panel offset unchanged
  and applies the CO5300 0x10 X offset when CST816S touch is detected."*

  Consequences, and they invert the risk this section recorded:

  - **On a V2 board (CO5300 + CST816), D-005 holds** — the BSP is exactly right
    and needs nothing from us.
  - **On a V1 board (SH8601 + FT3168), the BSP drives the panel with the CO5300
    init sequence.** Touch still works via the FT5x06 path and the offset is
    correctly not applied, but the display controller is wrong and there is no
    V1 driver in the tree to switch to.

  So the board revision question is now **more** load-bearing than when this
  document was written, and in the opposite direction: the risk is not that the
  BSP mishandles a new board, it is that it cannot handle an old one.

- **RESOLVED — the BSP does not expose variant detection.** The standalone
  `board_variant` component's existence was read as a hint; it is confirmed.
  The BSP probes touch internally and keeps the result private (there is no
  variant getter in `include/bsp/`), which is why three examples ship their own
  detector. Anything of ours that needs the variant must probe for itself.
- **UNKNOWN — Wi-Fi + BLE-central coexistence under load.** ESP-IDF supports
  it; sustained BLE central traffic (120 ms command cadence + keepalive)
  alongside TLS is not something we have measured. Mitigation: R2 control must
  degrade gracefully when Wi-Fi is busy, never the reverse — the pet's local
  life is the thing that must not stutter.
- **UNKNOWN — TLS memory on top of LVGL + PSRAM framebuffer.** The brief flags
  this correctly. Deliberately deferred; the first embedded slice has no cloud.
- ESP-IDF is not installed on this host. Setup is a prerequisite, not a risk.

---

## First embedded slice (no LLM, no voice, no memory, no autonomy)

In order, each independently verifiable:

0. **Back up the factory image and verify it** — `esptool.py read_flash 0 ALL`,
   size must equal what `flash_id` reports, record the SHA-256. Recovery images
   are revision-specific and the first flash is what reveals the revision, so
   this is the one step that cannot be redone later. DONE 2026-08-22.
1. **`firmware/board_check/`** (ours) → records chip/flash/PSRAM/MAC, board
   variant, I²C inventory and the AXP2101 rails in one flash. It links no
   display driver, which is what lets it run before the revision is known.
   Supersedes the vendor's `00_board_check` for this step; see
   [board-revision.md](board-revision.md).
2. **Confirm the board revision** by running the vendored detector, not a
   manual scan. `board_variant_detect()` ships in
   `examples/esp-idf/90_axp2101_pmu/components/board_variant/board_variant.c`
   (also in `91_pcf85063_rtc` and `92_qmi8658_imu`). It probes CST816 at
   I²C `0x15` → V2, else FT3168 at `0x38` → V1.

   > This step previously read "`08_i2c_tools` + `13_display_colorbar`".
   > **`08_i2c_tools` has no `components/` directory and does not ship the
   > detector** — following that instruction produces exactly the bare I²C
   > scan that the reset-release gotcha defeats. See
   > [board-revision.md](board-revision.md).
3. `01_project_template` → our own project skeleton on the managed BSP.
4. Add LVGL status screen: two labels, `R2 BLE: —` / `state: —`.
5. Port `r2d2_central.c` (drop `nus_peripheral`, target `esp32s3`,
   `MAX_CONNECTIONS=1`), wiring its connect/disconnect events to those labels.
6. Scan → connect → magic → wake → keepalive; watch the panel say `connected`.
7. One safe expression on a touch button: an LED set + one `R2_HEY_*` sound.
   **No locomotion.**
8. Pull R2's power; confirm the panel returns to `disconnected` and the board
   re-scans and recovers without a reboot.

Step 8 is the real acceptance test. Everything about "an entity that is
continuously there" depends on reconnect being boring.
