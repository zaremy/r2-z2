# Port boundary — what survives the rewrite

The Mac prototype exists to learn behaviour abstractions, **not to be moved**
(`CLAUDE.md`). That rule is right, and it is also the reason this file has to
exist: "the code does not port" is routinely misread as "the work does not
port", and most of what this project has learned is not code at all.

Everything below is sorted by what happens to it at the boundary. The
categories behave completely differently and confusing them is expensive in
both directions — reimplementing a validated constant wastes a day, and
"porting" a Mac-shaped workaround wastes a week.

> **This is a map, not a source of truth.** Every claim here is a pointer to
> the document that owns it. When they disagree, the owning document wins and
> this one is stale. Read `research/r2-capabilities.md` and `decisions.md`
> alongside it — a summary omits, and this is a summary.

---

## A. Hardware truths — constrain any implementation, in any language

These are properties of the robot. They are true in C, they are true in
Python, and firmware that does not encode them will rediscover them the
expensive way. Every one is OBSERVED on `D2-6F6B`; the citation is where the
evidence lives, not where it is repeated.

### The dome

| Fact | Consequence for firmware |
|---|---|
| **No resting position.** Found at 103°, 3.3° and −0.06° across three sessions | A destination angle is meaningless. Bound every move by **travel from a freshly read position** — never by destination, never from a remembered angle |
| **Commanded travel below ~10.5° is silently ignored and still returns success.** 4/6/8/10° all moved ≤0.11°; 10.5° moved | The smallest legible gesture is **12°**. A "subtle tilt" is not a thing this hardware can do, and a sub-threshold command is indistinguishable from a successful one |
| **A move takes ~2.0–2.2 s regardless of distance.** 8.35° took 2.19 s; 22.61° took 2.07 s | Fixed-duration, not a slew rate. Anything that must feel quick **cannot move the dome** |
| **Undershoot of 2.4–5.7°** in the direction of travel, and it is a gradient across the range | A single correction constant fitted at one end is wrong at the other. Re-read the angle; do not model it |
| Drift accumulates because a sum-to-zero set of deltas still walks | Anchor to a **fixed persistent home**, not to where the current gesture began. See D-014's sibling failure in D-013 |

Owned by: **D-013**, `research/r2-capabilities.md` §S1c.

### Stance and locomotion

- **Authored animations drive leg actions and can fell him.** `EMOTE_YES` — a
  *nod* — emitted WADDLE three times and put R2 on the floor, with
  `perform_leg_action` never called by us. An animation is a stance command
  whose contents cannot be inspected before sending.
- **The tripod is needed to MOVE, not to STAND.** Bipod is a stable standing
  stance; WADDLE is what topples him.
- **He parks himself in bipod about a minute after the link drops** — on his
  own, not from `stop` and not from the disconnect. So "default to STOP" does
  **not** mean he is left stable, and any behaviour assuming a tripod at
  wake-up is assuming something false.

Owned by: **D-009**, **D-010**, `research/r2-capabilities.md` §Stance bring-up.

### Power

**Our keepalive IS the wake command** — `DID 0x13 / CID 0x0D` every ~3 s. Any
`sleep` we send is undone within three seconds, which is why he has no idle
timeout in practice and why there is no working "off". Do not read "he stayed
awake" as a firmware property; it is us. A soft power control is a
**session-lifecycle** change, not a new op (#38).

### Lights

- **A colour we set is STATE, not a command.** It survives the link dropping,
  survives animations played over it, and the firmware's own resting
  alternation does **not** resume and overwrite it. Whatever a session leaves
  lit is what the household sees until something changes it.
- **Every value change flickers.** There is no smooth fade to be had from
  these fixtures — a design language built on interpolation will not render.
- The holo projector (bit 7) and logic displays (bit 3) are **brightness only,
  no colour**. Writing an RGB triple to either is a category error the daemon
  accepts and R2 renders as whichever write landed last.

Owned by: **D-012** *and its Amendment A*, `behaviour-states.md`. Read both —
D-012 alone once said modulation was "untested" while the capability doc had
already measured that it flickers.

### Sensing

| Fact | Value |
|---|---|
| Sensor stream rate | **4.00 Hz** measured |
| Enable sequence | **three calls, ordered**: interval 0 → extended mask → real interval. Copied from spherov2 `controls/v2.py`, not invented; setting the extended mask against a live interval does not reliably take |
| Wire format | big-endian `float32` per enabled component, in **declaration order** of the sensor tables |
| Touch signal strength | **157.9×** the rest limit at peak, **9 of 10** channels corroborating |
| Corroboration floor | **2 channels**. Single-channel firing produced a false positive in 1 of 4 no-touch controls |
| Event ring | 200 entries — 50 s of stream. Anything that stops draining for longer loses data |
| Notifications | `animation_complete` fires unprompted; **`leg_action_complete` does not** — it needs an explicit enable, and a session that forgets sees zero leg events and calls a known waddler safe |

Owned by: `research/r2-capabilities.md` §S1e, §S1f, §S2b.

---

## B. Constants that port verbatim

Cross-validated against at least two independent implementations
(`spherov2`, `claude-r2d2-buddy`, and a clean-room trace). Copy them; do not
re-derive them.

| What | Where it lives now |
|---|---|
| GATT UUIDs and the anti-DoS handshake payload | `mac-prototype/r2_probe.py:58-61` |
| Packet framing — SOP/EOP/ESC and the three escape codes | `r2_probe.py:68-69` |
| Device and command IDs (power, animatronic, IO, sensor) | `r2_probe.py:76-121` |
| Sensor bit masks, both tables | `r2_probe.py:133-151` |
| `seq = 0xFF` marks an unsolicited notification | `research/r2-protocol.md` |

`claude-r2d2-buddy/main/r2d2_central.c` is already NimBLE C in the central
role and is a near-drop-in for the ESP32 side — see `research/embedded-path.md`
and the pinned SHA in `research/source-map.md`.

**Two safety constants are policy, not protocol**, and should be re-decided
rather than copied blindly: `MAX_HEAD_MOVE = 45.0°` (travel permitted in one
commanded move, `r2_probe.py:846`) and `MAX_SETTLE_S = 10.0` (ceiling on any
in-op sleep, `r2_probe.py:1177`).

---

## C. Designs to reimplement — shapes, not code

These are the things the Mac prototype was actually for. None of the code
moves; every one of the *shapes* should.

**The permission ladder.** `read < leds < audio < dome < stance`, cumulative,
fixed at process start and never raised at runtime. Two details are
load-bearing and were both learned by getting them wrong:

- **The guard belongs on the send path, not the construction path.** A check
  in the object constructor is bypassed by any caller that skips construction.
- **The tier must be computed from what a behaviour actually does**, not
  declared alongside it, so it cannot drift from the behaviour it describes.

**Semantic behaviours composed from primitives.** A behaviour is a sequence of
phrases; a phrase is a set of steps that must read as one gesture, plus the
pause after it. Authored animations are **not** the substrate — they are
unrepresentable in the vocabulary on purpose (D-013). Each behaviour carries
duration, interruptibility, energy and cooldown, because those four are what
let a scheduler run R2 without making him twitchy.

**Sensing discipline.** The rubric that works, and the four ways it failed
first:

- Thresholds come from an **empirical rest null** — chop the baseline into
  windows the same length as a trial and take the worst excursion rest itself
  produces. A sigma rule applied to the max over a window fires on everything.
- Compare **like-for-like window lengths**, or the comparison means nothing.
- **Peak-to-peak, not deviation from a mean.** Touch is a disturbance, not a
  displacement; a channel that merely sits somewhere new is not a touch.
- **Flush before you measure.** A ring that kept filling during a gap sweeps
  history into the window.
- **Run the case that must NOT fire before any real trial, and again at the
  end.** A detector stuck on produces the loud answer, and loud reads as
  success — this cost ten wasted trials once already.

**Reactive-loop structure.** Detect → *settle* → perform → *recover* →
cooldown. Both waits are measured, not fixed sleeps, on the same statistic
that armed the loop. Two findings that will recur on any platform: the
behaviour's own motion disturbs the sensors that triggered it, and a hand
resting on the robot never produces the quiet a settle waits for, so the cap
is load-bearing rather than a safety net.

**Default to STOP.** Disconnect or failure results in stop, not last-command —
and *stop* means the stop command, not merely ceasing to send.

---

## D. Does not port at all

Everything here is a **Mac-agent workaround** and has no meaning on the
backpack. Recognising them as such is the point of this section: they look
like architecture and they are scaffolding.

- **The file-queue bridge** (`.bridge/requests`, `.bridge/responses`). It
  exists because macOS attributes Bluetooth to the *responsible* process and
  an agent-spawned process is SIGABRTed. On the ESP32 the firmware owns the
  radio; there is no queue and no daemon.
- **The `.command` + `open -a Terminal` launcher**, and the daemon lock that
  keeps two of them apart. Same cause, same irrelevance.
- **Python, and the `spherov2` dependency.** `spherov2` was a *reference* for
  the protocol and the enable ordering; it is not a runtime dependency of
  anything that ships.
- **Runtime threshold derivation from a 30 s baseline**, most likely. It is
  right for a session an operator supervises. A robot that wakes up in a
  household cannot ask everyone to hold still for half a minute first, and
  what replaces it is an open design question, not a port.

---

## The order to rebuild in

From `intent.md`, unchanged by anything here:

```
board bring-up → ESP32→R2 BLE → service panel → local behavior engine
→ Wi-Fi/cloud → mic / wake word / STT → persistent memory → higher autonomy
```

**Before the first flash:** the board revision must be resolved. V1 is
SH8601/FT3168, V2 is CO5300/CST816 — same part number, same product page,
different silicon. The vendor BSP detects it at runtime, so the safe first
flash is the vendor quickstart. **Do not flash `vthinkxie` first**; its 1.8"
target is V1-only. See `research/board-revision.md`.

## The first thing that will bite

Bring-up will want to prove the BLE link by moving something, because motion
is the unambiguous signal. The bring-up order exists to stop exactly that:
**read-only → LEDs → audio → small dome → stance → locomotion**, each
individually opt-in, never bundled.

The dome is the tempting first mover and it is the worst choice — it has no
home position to return to, it ignores small commands while reporting success,
and the first honest-looking test you can write against it ("command 5°, see
if it moved") is a test whose negative result means nothing.

**Read the battery voltage instead.** It proves the link, the framing, the
handshake and the notification path, and it cannot move him.
