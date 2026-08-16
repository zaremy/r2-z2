# Initial findings — executive synthesis

Init pass, 2026-08-15. Six repos cloned and read, packet layer independently
reimplemented and cross-validated, Mac environment built, board revision risk
resolved on paper. Detail lives in the sibling documents; this is the summary.

---

## Already solved (by someone else, verified in source)

1. **The complete Sphero V2 wire protocol.**
   `sphero-r2d2/spherov2/controls/v2.py:14-159` — framing, flags, escaping,
   checksum, sequencing, response parsing. Corroborated independently by
   `freer2/index.js:20-27` (2020) and `claude-r2d2-buddy/main/translator.c:12-34`.
2. **The R2-D2 connection handshake**, including the non-obvious part: the
   anti-DoS write `usetheforce...band` to `00020005-…` and the fact that
   **MAIN_SERVICE only appears afterwards**, requiring a second service
   discovery (`claude-r2d2-buddy/main/r2d2_central.c:96-110`).
3. **An ESP32 BLE central for R2-D2 that works** —
   `r2d2_central.c`, 414 lines of ESP-IDF/NimBLE, including two-phase
   discovery, byte-at-a-time notification reassembly with SOP resync
   (`:68-93`), a TX queue task, wake/keepalive (`:274-282`, `:389-405`), and
   disconnect→rescan recovery (`:333-349`).
4. **The full expressive inventory** — 388 sound ids and 51 animation ids,
   enumerated and clustered (`r2-capabilities.md`).
5. **Revision-aware board bring-up** — Waveshare ships runtime V1/V2 detection
   (`board_variant.c`, `13_display_colorbar`) plus 17 first-party ESP-IDF
   examples covering every subsystem the service panel needs.

## Reusable

| Asset | Where | How |
|---|---|---|
| R2 BLE central | `claude-r2d2-buddy/main/r2d2_central.c` | Near-verbatim; retarget C6→S3, connections 2→1 |
| UUIDs + magic string | `claude-r2d2-buddy/main/common.h:18-39` | Verbatim (NimBLE LE byte arrays) |
| Sphero packet encoder | `translator.c:12-37` | Reuse shape; **must add escaping** |
| Board support | managed component `waveshare/esp32_s3_touch_amoled_1_8 ^2.0.3` | As-is |
| Revision detection | `board_variant.c:74-120` | As-is |
| Service-panel architecture | `vthinkxie/src/hw/*` | **Read, don't compile** — V1 board |
| Sound/animation tables | `spherov2/toy/r2d2.py:27-468` | Data, transcribe to C |

## Needs porting or building

- **Sphero packet layer in C with escaping.** `translator.c`'s encoder skips
  escaping — safe for its fixed payloads, wrong in general.
- **Everything above the wire.** Semantic behavior library, choreography
  scheduler, persistent state, mood/boredom/energy, service-panel UI. No
  upstream does any of this; the pet is entirely ours. This is correct — it is
  the actual product.
- **Reconnect as a first-class feature.** `spherov2` has none;
  `r2d2_central.c` has the right skeleton.

## Largest technical risks

1. **The animation-ID conflict.** `spherov2` and `claude-r2d2-buddy` disagree on
   what animation ids mean, agreeing on only 1 of 7 overlapping entries
   (`r2-capabilities.md` §3). The entire semantic vocabulary sits on this
   table. **Only hardware resolves it.**
2. **"Feels alive" is unvalidated and unfalsifiable on paper.** The robot's
   expressive range is *combinatorial, not generative* — fixed sounds, fixed
   animations. Whether timing, restraint and variation are enough is the
   project's central bet, and it is cheapest to test on the Mac, early, before
   any embedded work.
3. **Wi-Fi + BLE-central coexistence on one radio.** Supported, unmeasured.
   Mitigation is a design rule: R2 control degrades last.
4. **Power and physical mounting of the backpack.** Entirely unaddressed.
   Weight and battery on a small droid are real constraints, not software.
5. **BSP source unread** (managed component, fetched at build) — how it handles
   V1/V2 is UNKNOWN until first build.

## Resolved risks

- **V1/V2 flashing hazard — resolved.** `vthinkxie`'s 1.8" env is V1-only
  (`BOARD_DISPLAY_CO5300 0`, FT3168 @ 0x38). Do not flash it. Waveshare's own
  ESP-IDF examples are revision-safe. See `board-revision.md`.
- **Protocol correctness — resolved as far as it can be off-hardware.** Our
  independent implementation is byte-identical to `spherov2` *and* to the
  shipped C firmware across wake, battery, LED, audio, head float (±),
  animation play/stop, and a deliberate escaping stress case.
- **A real bug, caught by cross-checking.** The first draft of our probe used
  the 8-bit LED mask (CID `0x1C`), which `io.py:87` marks untested. R2D2
  inherits BB9E's **16-bit** mask (CID `0x0E`, `bb9e.py:153`) — the variant
  shipped firmware actually uses (`translator.c:66`). Fixed and re-verified
  three ways. This is the argument for reading source over READMEs, in one bug.

## Recommended first end-to-end slice

**Mac-side, and it is not the ESP32.** The board cannot answer the project's
central question; the robot can.

> Connect to R2 from the Mac, run a **capability survey** that plays every
> animation id 0-55 and a sample of each `R2_*` sound family, one at a time,
> logging what each actually does — then hand-build **three** semantic
> behaviors (`express_curious`, `express_happy`, `idle`) as timed choreographies
> and judge whether they read as intentional.

Why this first:
- It resolves risk #1 (the animation table), which everything else depends on.
- It attacks risk #2 while the cost of being wrong is a Python file.
- It needs no ESP-IDF, no board, no cloud, no voice.
- It produces the artifact the embedded port needs anyway: a verified
  behavior table.

The embedded slice in `embedded-path.md` §"First embedded slice" is the correct
*second* move, and it is deliberately small: board check → revision confirm →
BLE central port → status label → one safe expression → survive a power-cycle
of R2.

## Status against the brief's acceptance criteria

| Criterion | Status |
|---|---|
| Project repo + structure initialised | ✅ |
| External sources cloned, gitignored, SHAs recorded | ✅ 6 repos, `source-map.md` |
| Mac/toolchain status documented | ✅ below and in `mac-prototype/README.md` |
| Real source files inspected | ✅ |
| R2 BLE handshake traced | ✅ `r2-protocol.md` §3 |
| Major R2 commands traced | ✅ §7, 14 commands, 3-way verified |
| Waveshare V2 examples inspected | ✅ `board-capabilities.md` |
| V1/V2 mismatch risk understood | ✅ `board-revision.md` |
| C6→S3 reuse assessed | ✅ `embedded-path.md` |
| `vthinkxie` evaluated against V2 hardware | ✅ V1-only; reference not base |
| Mac discovers R2, **or** environment ready + safe probe prepared | ⚠️ **the latter** — see below |
| Can state the next slice with evidence | ✅ above |

### The one gap: BLE could not be exercised

`mac-prototype/r2_probe.py` and the `./r2` launcher are built and their
non-BLE paths run. The scan itself **aborts with SIGABRT** before touching the
radio, for an environment reason, not a code one:

> "This app has crashed because it attempted to access privacy-sensitive data
> without a usage description. The app's Info.plist must contain an
> `NSBluetoothAlwaysUsageDescription` key."
> — `~/Library/Logs/DiagnosticReports/Python-2026-08-15-141854.ips`

Homebrew's `Python.framework` re-execs into a `Python.app` whose `Info.plist`
lacks that key. We fixed that half: `mac-prototype/tools/R2Probe.app` is a
project-local, ad-hoc-signed copy of the bundle **with** the key. The remaining
half needs a human — macOS attributes the request to the *responsible* process
(here, `claude`), and granting Bluetooth requires an interactive System
Settings approval that a non-interactive session cannot produce.

**Unblocking is one action by you**, documented in `mac-prototype/README.md`:
run `./r2 scan` once from Terminal and approve the Bluetooth prompt.

**Resolved, 2026-08-15.** The grant was made and discovery works —
`D2-6F6B`, 1 match out of 13 BLE devices seen. Scope of what that proves is
narrow but real: the launcher, the bundle workaround, bleak on Python 3.14, and
the `D2-` name filter all work end to end. **No handshake and no command has
reached the robot yet**, so everything in `r2-protocol.md` marked OBSERVED is
still verified only against other implementations. `./r2 info` is the first
test that changes that.

## Environment recorded

| | |
|---|---|
| Host | macOS 15.7.3 (24G419), Apple Silicon `arm64`, Mac15,6 |
| Bluetooth | On, BCM_4388, GATT supported |
| git / cmake / node | 2.49.0 / 4.4.0 / v24.5.0 |
| Python | 3.14.6 (Homebrew); Apple 3.9.6 at `/usr/bin/python3` |
| Mac venv | `mac-prototype/.venv`, **bleak 3.0.2** |
| ESP-IDF | ❌ not installed — required for the embedded path (**v5.5.x**) |
| PlatformIO | ❌ not installed — not needed under the recommendation |
| ninja | ❌ not installed (IDF installer provides it) |
| USB serial | none present — board not connected at init |
