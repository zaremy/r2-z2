# Camera and vision — can a camera plug into the backpack, and where does it live?

Started from one question — *"how do we add a camera, can we just plug one into
USB-C?"* — and the answer moved three times: to **no**, then to a placement
ruling, then to a constraint nobody had costed.

**Nothing here is decided except placement.** When this hardens it needs an ADR
in `decisions.md` covering placement, the event-driven duty cycle and the tier
split. This file is the evidence; the ruling is not written yet.

Sources retrieved 2026-08-24; external links in [source-map.md](source-map.md)
are the standing record for repos, and the bibliography at the end covers the
vendor docs this file rests on.

---

## Summary

1. **USB-C plug-and-play is impossible on the backpack** — three independent
   reasons, all OBSERVED.
2. **The camera ultimately lives in the dome** — operator ruling, 2026-08-24.
3. **The binding constraint is power, not space** — so it must be *told to
   look*, never stream.
4. **The dome build is two pieces on an FPC ribbon** — sensor head at the
   aperture, board and cell wherever they fit. That leaves exactly one viable
   option.

---

## 1. Why USB-C is not the answer

Three reasons, and any one alone is fatal.

### The board has no camera interface — OBSERVED

Waveshare's schematic PDF for this board contains **no camera net at all**:
decompressing every content stream and searching the net names returns no
`CAM`, no `PCLK`, no `HREF`, no `SIO_C`. The vendor wiki lists the expansion as
*7 GPIO, 1 I²C, 1 UART, 1 USB solder pad* on 1.27 mm pads.

A DVP sensor such as the OV2640 needs D0-D7, PCLK, VSYNC, HREF, XCLK, RESET,
PWDN and SCCB SDA/SCL — **14 to 16 pins**. The board breaks out seven, and the
rest are spoken for: the display is a QSPI AMOLED and audio is an ES8311 I²S
codec, both inventoried in
[board-capabilities.md](board-capabilities.md). The ESP32-S3 also has no
MIPI-CSI peripheral; its only native camera path is the 8-bit parallel LCD_CAM
(DVP) block. **DVP on this board is arithmetically impossible**, not merely
inconvenient.

### The USB-C port is a device port with no outward 5 V — OBSERVED

The same schematic shows USB-C `DP1/DN1` and `DP2/DN2` joined through 22 Ω
series resistors to nets `USB_P` / `USB_N`, landing on **GPIO19 and GPIO20** —
the ESP32-S3's native USB-OTG pins. `VBUS` from the connector reaches the
AXP2101 PMU's charger input and is mirrored onto the expansion pad. There is
**no VBUS power-switch part** anywhere in the design, and the board's own supply
is a 3.7 V single cell through the AXP2101, so there is no 5 V rail to switch.

Espressif's own guidance is explicit that an OTG cable is not sufficient: the
port does not source VBUS, peripherals fail to enumerate below ~4.75 V, and the
S3 cannot supply 500 mA from a GPIO — you must route 5 V to the receptacle and
gate it with a high-side switch. On a battery-powered backpack that is a boost
converter *and* a load switch: a daughterboard, not an adapter.

There is a second consequence that is easy to miss. GPIO19/20 are the same pins
carrying the native USB-Serial-JTAG we flash and log over — `/dev/cu.usbmodem2101`,
VID `0x303a` / PID `0x1001`, recorded in
[board-capabilities.md](board-capabilities.md). Host mode takes the console with
it, and every bring-up session so far has depended on that console.

### Full-Speed USB caps the camera anyway — OBSERVED

The ESP32-S3's USB peripheral is **Full-Speed only (12 Mbps)** with a hardware
maximum packet size of 512 bytes. Espressif's `usb_stream` / UVC host component
documents the consequences: cameras must support **MJPEG** and be USB 1.1
Full-Speed compatible; isochronous streams must stay under ~4 Mbps (500 KB/s)
and bulk under ~8.8 Mbps (1100 KB/s); the practical ceiling is around
**640×480**, with 480×800 @ 15 fps quoted as the maximum. The host controller
has 8 channels, shared with any hub.

So the best case after the hardware work is VGA MJPEG at a handful of frames per
second, occupying the console port, the USB stack's buffers, and a slice of the
same 8 MB PSRAM that already has to hold an LVGL framebuffer. D-015 already
flags that **T2 headroom is unmeasured** for BLE central + Wi-Fi + TLS + LVGL +
an audio uplink together. Vision would be a second continuous uplink stacked on
a budget nobody has measured once.

### The compute question answers itself the same way voice did

Nothing about "vision" here means on-device vision. Object recognition, face
recognition and scene understanding of the kind a persistent character needs do
not fit on an ESP32-S3 — the same argument D-015 makes for speech, for the same
reason. The realistic shape is **capture JPEG → vision model → text back**,
which is the cloud path D-015 already committed to and the deferred home server
already anticipates.

That is good news for scoping. A camera adds **no new architectural axis** — one
more sensor feeding the same provider-agnostic cloud client, under the same rule
that losing the cloud must degrade R2 to blind-and-mute, not dead.

---

## 2. Placement: the dome — operator ruling 2026-08-24

The short-term rig can be mounted anywhere convenient; **ultimately it lives in
the dome**. That settles the character question well: a dome camera means R2
*looks* at things by turning his head, so the sensor and the existing expressive
motion are the same gesture. It is also the placement `CLAUDE.md`'s character
boundary implies — the body is the character interface, and a lens on the
backpack would read as a gadget bolted on.

**The dome does not spin freely, which helps.** Usable travel is
**−145.2° … +170.2°** (OBSERVED, S1c — see
[r2-capabilities.md](r2-capabilities.md)) — about 315°, not continuous rotation.
A cable crossing the joint is therefore not a slip-ring problem in principle. It
is still ~315° of repeated twist on a hand-soldered lead inside a sealed toy,
cycled every time he looks around. **A self-contained node — board plus its own
cell, nothing crossing the joint — removes the failure mode entirely.**

### The assembly is two pieces on a ribbon

**Assume the dome build is two pieces joined by an FPC ribbon.** The XIAO's
camera is not soldered down; it hangs off a 24-pin FPC ribbon, and longer
extension ribbons exist. This splits the fit problem into two easy ones: only
the ~10 mm sensor head must sit at the aperture, while the board and cell go
wherever there is room around the light walls. The alternative — one rigid
21 × 17.5 mm assembly that must be *both* at the lens and in free space — solves
two constraints at one location inside a skull nobody has opened.

Two cautions, both of which bite:

- **It is DVP, not MIPI-CSI.** Several retail listings for these ribbons claim
  CSI-2 and they are wrong. Parallel pixel data over a long unshielded ribbon is
  the classic place this turns flaky, and the usable length is **empirical, not
  specified**. UNKNOWN until benched.
- **The OV5640-with-heat-sink variant is the wrong sensor.** The heat sink exists
  because it runs hot, which is the opposite of what §3 demands, and 5 MP
  autofocus buys nothing when the frame goes to a vision model at modest
  resolution.

### Mechanical unknowns — checks with the dome off, not research

Teardowns (Fictiv, MicrocontrollerTips) describe the head as an outer cover over
an inner *skull*, joined by cantilever snaps, with the skull screwed to the
gearbox output by three screws, and the skull holding **three LED PCBAs** behind
moulded light walls. Both sources are **textual** — no dimensions are published,
so everything below is UNKNOWN until the dome is opened:

- Is there a usable **unlit aperture**? The one obvious clear lens on the dome
  face is the holo projector (bit 7, OBSERVED as a white dimmable fixture in
  [r2-capabilities.md](r2-capabilities.md)). Looking out through a lit LED is not
  a plan. This is the first check, because it fixes where the sensor head goes
  and everything routes to it.
- Does a **~10 mm sensor head** seat there, square and at the right depth?
- Is there a pocket for **board + cell**, not necessarily near the lens?
- Can the **ribbon route** between them without crossing a light wall, with
  enough slack to be strain-relieved?
- Does the gearbox tolerate **~10 g** on its output? It has driven a decorated
  plastic dome and nothing else, ever.

---

## 3. Power is the binding constraint, not space

Everyone assumes the hard part of a dome camera is fitting it in. It is not.

A XIAO-class board draws roughly **100–110 mA active**, ~2 mA in light sleep and
**14 µA in deep sleep**. A LiPo that fits alongside the dome's light PCBAs is on
the order of **150 mAh**. That is **about 80 minutes of continuous Wi-Fi
streaming**, and then R2 is blind until someone disassembles his head.

Continuous vision in the dome on its own cell is therefore not on the table, and
no board in §4 changes that — the Grove module's 80 mA peak is the same order.
The options are to wire it to R2's own battery, which re-opens the 315°-of-twist
joint problem §2 just closed, or to **stop streaming**.

**Stop streaming is the right answer, and R2 already knows when to look.** The
backpack is what commands the dome. It knows the instant it has turned his head
toward a noise, the instant he wakes, the instant something in the behaviour
engine wants to know what is there. A node that sleeps at 14 µA and is *told* to
take one frame collapses the duty cycle to near zero: at one capture per ten
seconds the average draw is single-digit milliamps and the same 150 mAh cell
lasts tens of hours; at a handful of captures per interaction it lasts weeks.

The camera stops being a video feed and becomes **a sense R2 uses on purpose**.
He looks *at* things. That is also the more characterful behaviour, and it is the
same shape as the dome itself — a fixed-duration deliberate move (D-013), not a
continuous servo. Vision that fires on intent rather than free-running matches
the machine it is bolted to.

---

## 4. Options

Scored against the ribbon assumption: **sensor must detach from the board**,
then small, light, wireless, and runnable without a wire crossing the joint.

| | Sensor detaches | Size / weight | Sensor | Power | ≈ Cost | Dome |
|---|---|---|---|---|---|---|
| **A. XIAO ESP32S3 Sense** | **Yes — 24-pin FPC** | **21 × 17.5 mm** + ~10 mm head | OV2640 2 MP (newer OV3660); OV5640 drops in | LiPo charge circuit onboard | ~$15–20 | **Yes — the only one** |
| B. M5Stack Unit CamS3 | No — sealed | 40 × 24 × 11 mm, 10.8 g | OV2640 | external | ~$20 | No |
| C. M5Stack Timer Camera X | No — sealed | 48 × 24 × 15 mm, 15 g | OV3660, 66.5° FOV | 140 mAh built in | ~$20 | No |
| D. Grove Vision AI V2 | Yes — CSI | thumb-sized | Himax WiseEye2 NPU | 80 mA peak | ~$25 | Pairs with A (§5) |
| E. ESP32-P4-EYE | Integrated kit | dev-kit with LCD | OV2710 MIPI-CSI, 1080p | battery connector | ~$25–30 | No |
| F. Generic ESP32-S3-CAM | Varies | varies | OV2640 / OV5640 DVP | external | ~$3–15 | Bench only |
| G. Mac webcam / phone | — | — | anything | — | $0 | n/a |

**A wins, and under the ribbon assumption it wins uncontested.** It is the
smallest capable ESP32-S3 camera board sold, with 8 MB PSRAM and an onboard LiPo
charge circuit — but the deciding property is not size, it is that **the sensor
comes off**. B, C and E are sealed units whose lens is wherever the enclosure
puts it, which inside a dome means nowhere useful. The detachable connector also
makes the lens a *choice*: OV2640 modules ship in **66° through 222°** variants
and the stock lens is narrow. A droid that turns his head wants ~**120°** — wide
enough to catch someone entering the room, not so wide that faces smear at the
edge.

**E is the tempting wrong answer.** The ESP32-P4 is a real generational step —
MIPI-CSI with a hardware ISP, 1080p H.264 at 30 fps, Wi-Fi 6 via a companion C6.
None of that is a constraint R2 has. The bottleneck is a cloud round-trip and a
battery, not encode throughput, and the EYE arrives as a handheld camera with its
own LCD and rotary encoder.

---

## 5. Two tiers, and only one needs the cloud

Option D is not competing with A — it is a different layer, and the pair answers
the privacy question in §7 rather than deferring it.

The **Grove Vision AI V2** runs a Himax WiseEye2 (Cortex-M55 + Ethos-U55 NPU) and
infers *on the module*: person detection at **48–76 ms (13–21 fps)**, image
classification at 15 ms, 17-joint pose at ~8 fps, 80 mA peak, models deployable
without writing inference code. It emits **a fact** — someone is present, someone
is facing you, someone walked past — not a frame.

That maps onto the split `intent.md` already mandates and D-015 already used for
voice:

- **Tier 1 — local, always available: presence and attention.** Is a person here?
  Facing me? Did something move? This is the reflex layer, it must survive a dead
  internet, and at tens of milliseconds it is fast enough to drive a head turn
  that reads as a reaction rather than a delay. No image leaves the house.
- **Tier 2 — cloud, rare: "what is that?"** One JPEG, on purpose, when the
  behaviour engine has a question Tier 1 cannot answer. Seconds of latency are
  fine because the character is *deliberating*, and the volume is low enough that
  cost never becomes a design input.

`intent.md` says the cloud is for the unusual. Tier 1 is the routine and it is
local; Tier 2 is the unusual and it is not. Worth recording: this is the **second
sensor to land in the same shape**. That is a pattern in the architecture, not a
coincidence, and it argues for writing the shape down once rather than
re-deriving it per sensor.

**The honest caveat:** two boards is more than one. Tier 1 only earns its board
once a behaviour *consumes* "a person is present" and does something visibly
different with it. Until then **A alone, event-driven, answers every question on
the table.** D is the upgrade path, not the starting kit.

---

## 6. Staging and parts

The software loop does not have to wait for the dome. In this order, so the dome
job is purely mechanical when it comes:

1. **Mac webcam, now, zero hardware.** The Mac is already the development host
   and holds the BLE link. Proves the only novel part — capture → vision model →
   a behaviour R2 performs — against the same provider-agnostic client D-015
   committed to. Needs no robot.
2. **Bench the ribbon before anything is opened.** How long can the FPC run
   before frames corrupt? Its answer is the mounting envelope every later
   decision is drawn inside. Needs no robot and no disassembly.
3. **Node velcroed to the outside of the dome, sensor separate from board, as it
   will be inside.** Real POV, real dome-slaved framing, reversible in seconds.
   Answers what a webcam cannot: what he can see from that height, whether
   turning the head to look *reads* as looking, how stale a frame is by the time
   it matters.
4. **Inside the dome**, once the above hold and the §2 mechanical checks have
   answers.

**Parts to order:** two XIAO ESP32S3 Sense (bench + droid), a ~120° OV2640 in
24-pin DVP, assorted 24-pin FPC extension ribbons to find the length limit, and a
3.7 V LiPo around 150 mAh with a JST-SH 1.0 mm connector — the cell chosen
*after* the teardown, since it has to fit a pocket nobody has measured. Roughly
$50–65 all in. Skip the OV5640/heat-sink variant, and skip the Grove module until
a behaviour consumes presence.

Two things to check before ordering: the camera connector must be the **24-pin
DVP FPC** the XIAO uses, not the 8-pin flavour sold for ESP32-CAM boards — the
listings reuse the same photos — and newer XIAO Sense units ship **OV3660**
rather than OV2640, which changes which spare lens modules mate with it.

---

## 7. Open — operator's call

**May household images leave the house?** D-015 scoped the cloud exposure for
**audio**. A camera in a home is a different privacy object, and the ruling should
be taken deliberately rather than inherited from the voice decision. The two-tier
design in §5 exists partly to make "no" a cheap answer rather than a sacrifice.

---

## Bibliography

External sources, retrieved 2026-08-24.

- Waveshare. "ESP32-S3-Touch-AMOLED-1.8 schematic (PDF)".
  <https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8/ESP32-S3-Touch-AMOLED-1.8.pdf>
  — content streams decompressed locally and net names searched directly, per the
  "read source, not README" rule.
- Waveshare Wiki. "ESP32-S3-Touch-AMOLED-1.8".
  <https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8>
- Espressif. "USB Stream Component — ESP-IoT-Solution".
  <https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_host/usb_stream.html>
- Espressif. "Introduction to USB Camera Solution — ESP-Techpedia".
  <https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/solution-introduction/camera/usb-camera-solution.html>
- Espressif. "USB — ESP-FAQ".
  <https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/usb.html>
- Espressif. "USB Host Solutions — ESP-IoT-Solution".
  <https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_overview/usb_host_solutions.html>
- Espressif. "ESP32-P4-EYE User Guide — esp-dev-kits".
  <https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4-eye/user_guide.html>
- Seeed Studio. "XIAO ESP32S3 Sense".
  <https://www.seeedstudio.com/XIAO-ESP32S3-Sense-p-5639.html>
- Seeed Studio Wiki. "Camera Usage in XIAO ESP32S3 Sense".
  <https://wiki.seeedstudio.com/xiao_esp32s3_camera_usage/>
- Seeed Studio Forum. "XIAO ESP32-S3 Sense — extension cable for camera sensor module".
  <https://forum.seeedstudio.com/t/xiao-esp32-s3-sense-extension-cable-for-camera-sensor-module/293706>
- etechnophiles. "XIAO ESP32S3 Board Review — Pinout, Specs & Projects".
  <https://www.etechnophiles.com/xiao-esp32s3-review-pinout/> — power figures.
- ESP32s.com. "OV2640 2MP Camera Module, 66°-222° Wide Angle".
  <https://esp32s.com/product/24pin-ov2640-camera-module-for-esp32-cam-camera-module-2mp-180-66-120-160-222-200-degree-650nm-850nm-night-vision-dvp/>
- Seeed Studio. "OV5640 Camera for XIAO ESP32S3 Sense (with heat sink)".
  <https://www.seeedstudio.com/OV5640-Camera-for-XIAO-ESP32S3-Sense-With-Heat-Sink-p-5739.html>
- Seeed Studio Wiki. "Grove Vision AI Module V2".
  <https://wiki.seeedstudio.com/grove_vision_ai_v2a/>
- Edge Impulse Docs. "Seeed Grove Vision AI Module V2 (WiseEye2)".
  <https://docs.edgeimpulse.com/hardware/boards/seeed-grove-vision-ai-module-v2-wise-eye-2>
- M5Stack Docs. "Unit CamS3". <https://docs.m5stack.com/en/unit/Unit-CamS3>
- Adafruit. "M5Stack ESP32 Timer Camera X". <https://www.adafruit.com/product/4959>
- Fictiv. "Sphero R2-D2 Teardown".
  <https://www.fictiv.com/teardowns/sphero-r2d2-teardown>
- MicrocontrollerTips. "Teardown: Inside Sphero's R2-D2 toy".
  <https://www.microcontrollertips.com/teardown-inside-spheros-r2-d2-toy/>

## Method and evidence labels

Repo docs were read first to fix what is already OBSERVED about this unit. The
vendor schematic PDF was fetched and its compressed content streams decompressed
locally so net names could be searched directly rather than read off a product
page — the rule [board-revision.md](board-revision.md) earns the hard way. USB
and camera limits come from Espressif's own documentation rather than blog
summaries.

- **OBSERVED** — the absence of a camera net and the GPIO19/20 USB routing
  (schematic); the Full-Speed UVC ceilings (vendor docs); the dome's usable
  travel and the holo projector (our own S1c measurements).
- **INFERRED** — the RAM and radio contention with LVGL + BLE + audio; the
  duty-cycle arithmetic in §3, which is calculated from datasheet figures and has
  not been measured on a board we own.
- **UNKNOWN** — every dimension inside the dome; the usable FPC ribbon length.
