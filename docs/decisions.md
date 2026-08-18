# Decision log

Newest last. Each entry: what was decided, why, what would reverse it.

---

## D-001 — Initialize in place in `R2Z2/`, project name `r2-z2`
**2026-08-15**

The directory contained only `.DS_Store` and `R2Z2-vault/` (an Obsidian vault
with a single `Welcome.md`). Nothing to protect, no existing repo. Initialized
git here rather than nesting a project subdirectory.

Published as `github.com/zaremy/r2-z2` (private). The brief's working name was
`r2-pet`; renamed to `r2-z2` to match the directory and the vault.

The vault is left untouched and is available as durable KB/governance space;
it is gitignored rather than tracked, so notes there stay local.

*Reversed by:* nothing likely.

---

## D-002 — `reference/` is gitignored; clones are never vendored
**2026-08-15**

Six upstream repos (3800+ files, mixed MIT/Apache-2.0) are research inputs, not
dependencies. Vendoring them would bloat history and blur provenance. Exact SHAs
are pinned in [research/source-map.md](research/source-map.md); the clone
commands are in the root README.

They are kept **inside** the project at `reference/` rather than somewhere
external, so they are one `grep -r` away while working. Moved there from
`research/external/` on 2026-08-15 — same policy, shorter path, and the
now-empty `research/` tree removed so `docs/research/` is the only "research".

*Reversed by:* needing to fork one for real, at which point it becomes a proper
submodule or dependency with its own decision entry.

---

## D-003 — Reimplement the Sphero V2 packet layer rather than depend on `spherov2`
**2026-08-15**

`mac-prototype/r2_probe.py` has no `spherov2` import. Reasons:

1. The Mac prototype exists to teach us what to write in C. A traced,
   commented implementation is the porting artifact; a library dependency is not.
2. `sphero-r2d2` pins `bleak>=1.1.1` and we run 3.0.2 — an untested combination
   we would rather not have on the critical path during bring-up.
3. Writing it forced us to read the protocol properly, which is how the LED bug
   in D-004 was found.

Cost is real: we own the code. Mitigated by cross-validating every packet
against `spherov2` *and* the shipped C firmware — byte-identical across wake,
battery, LED, audio, head float (±), animation play/stop, and an escaping
stress case.

*Reversed by:* needing sensor streaming or drive control quickly, where
`spherov2`'s existing implementation would outweigh the porting benefit.

---

## D-004 — Use the 16-bit LED mask (DID `0x1A`, CID `0x0E`), not the 8-bit variant
**2026-08-15**

The probe initially used CID `0x1C` (8-bit mask). `spherov2/commands/io.py:87`
marks that variant `# Untested / Unknown Param Names`, while `bb9e.py:153`
exposes the **16-bit** version — and R2D2 subclasses BB9E.
`claude-r2d2-buddy/main/translator.c:66,74` uses CID `0x0E` with masks `0x0007`
and `0x0070` on real hardware.

Fixed, and re-verified three ways (ours = `spherov2` = C firmware, byte-exact).

*Reversed by:* hardware showing CID `0x0E` failing on our firmware version.

---

## D-005 — ESP-IDF v5.5.x + official managed BSP as the embedded foundation
**2026-08-15**

Chosen over Arduino/PlatformIO. Full analysis in
[research/embedded-path.md](research/embedded-path.md). It is the only path
where **both** hard parts are already solved by someone else: the working
ESP-IDF/NimBLE R2 BLE central in `claude-r2d2-buddy/main/r2d2_central.c`, and
first-party, revision-aware board support from Waveshare.

The Arduino path would mean rewriting the BLE central in order to reuse a UI —
while inheriting V1 display and touch drivers for a V2 board.

Explicitly *provisional*: the BSP is a managed component whose source we have
not read (it is fetched at build time). If it turns out not to handle the V2
panel cleanly, revisit.

*Reversed by:* the BSP failing on V2 hardware, or Wi-Fi/BLE coexistence proving
unworkable under IDF.

---

## D-006 — Drop the BLE peripheral role; ESP32 is central-only
**2026-08-15**

`claude-r2d2-buddy` is dual-role (Nordic UART peripheral for Claude Desktop +
central for R2), hence `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`. Our backpack is
autonomous and needs only central → R2. Dropping `nus_peripheral.c` and setting
`MAX_CONNECTIONS=1` removes dual-role radio contention before Wi-Fi is added.

Claude-activity awareness, if ever wanted, arrives over Wi-Fi as one sensor
among many — not as a second BLE role.

*Reversed by:* a concrete requirement to pair R2 with a desktop over BLE.

---

## D-007 — Do not flash `vthinkxie` firmware to the board
**2026-08-15**

`src/boards/board_waveshare_esp32s3_touch_amoled_1_8.h:52,55` sets
`BOARD_DISPLAY_CO5300 0` and uses FT3168 @ `0x38` — the **V1** stack. The
expected board is V2 (CO5300 + CST820 @ `0x15`). Flashing it would send an
SH8601 init sequence to a CO5300 panel and probe touch where nothing answers.

Repo remains valuable as a **service-panel architecture reference**
(`src/hw/` HAL split, NVS persistence, battery UI, PSRAM framebuffer).

**Substantially weakened 2026-08-15.** The retailer listing for the unit
actually purchased (Amazon B0DSVK5576) names **SH8601 + FT3168** in its title —
the V1 stack. If that holds, `vthinkxie`'s 1.8" env is the *correct* firmware
for this board and this decision's premise is void.

The decision stands only as "do not flash before the I²C probe confirms the
revision." The direction of the hazard is now unknown, not known.

*Reversed by:* `08_i2c_tools` reporting `0x38` (FT3168) — which the listing
predicts. Re-file this decision with the probe output either way.

---

## D-008 — Project-local `R2Probe.app` instead of patching Homebrew's Python
**2026-08-15**

macOS SIGABRTs any process touching CoreBluetooth whose bundle lacks
`NSBluetoothAlwaysUsageDescription`; Homebrew's `Python.app` lacks it. The
common fix is editing that plist in place, which is global, invisible, and
reverted by `brew upgrade`.

Instead `mac-prototype/tools/R2Probe.app` is a project-local, ad-hoc-signed
copy with the key added, launched by `mac-prototype/r2`, rebuildable via
`./r2 --rebuild`.

Does **not** remove the need for a one-time interactive TCC grant — see
`mac-prototype/README.md`.

*Reversed by:* Homebrew shipping the key upstream.

---

## D-009 — Split the `motion` tier into `dome` and `stance`
**2026-08-16**

The permission ladder topped out at `motion`, documented as "dome and
animations". **That description was false in the direction that matters.**

#11 played one authored animation — id 21 `EMOTE_YES`, a *nod* — at
`--allow motion`. It emitted `WADDLE`, `WADDLE`, `WADDLE`, `TRANSITIONING`,
`TWO_LEGS`, and **put R2 on the floor**, with `perform_leg_action` never
called by us. An authored animation is a stance command whose contents we do
not get to inspect before sending it.

So a session opened to survey the *dome* could change his stance and topple
him, while the ceiling's own name promised it could not. The ladder is this
project's core safety mechanism; a rung that grants more than it says is worse
than no rung, because it is trusted.

**Decision.** `TIERS = ["read", "leds", "audio", "dome", "stance"]`.

- `dome` — `set_head` plus the `DID 0x17` writes that cannot change stance
  (`notify`, `idle`). Bounded by `bounded_head_move`; worst case is a 45°
  turn of the head.
- `stance` — anything that can put him on the floor. `animation` sits here
  **because of the observation**, not by category.

`motion` is accepted as a deprecated alias and resolves **down** to `dome`,
never up. Someone who typed the old name now gets a refusal on `animation` —
a message on a terminal. Resolving it up would silently re-grant the ability
to knock the robot over. **When a rename is ambiguous, resolve toward less
capability.**

The alias is resolved once at daemon start, so the banner, the refusal
messages and the lock file all name the tier actually in force. Printing
`MOTION` while enforcing `dome` is the class of mismatch this removes.

`stance` (the read op, `get_leg_action`) stays at tier `read`: finding out
whether he is stable must never require opening a session that can
destabilise him.

*Reversed by:* evidence that `play_animation` cannot reach the legs — which
would contradict six `leg_action_complete` events already recorded.

*Does not fix:* `set_leg_position` and `perform_leg_action` have no op yet
(#22), and locomotion via `DID 0x16` remains unimplemented (#23). This makes
the ladder honest about what exists today; it does not add the missing rungs.

---

## D-010 — Excitement is choreographed by us; authored animations are not a behavior library
**2026-08-17** · *survey complete, 56/56*

`architecture.md` assumed semantic behaviors could delegate to authored
animations — `celebrate()` plays `EMOTE_LAUGH`, `express_curious()` plays
`WWM_CURIOUS`. The S1d survey (#12) measured what those animations do to the
body, across all 56 ids, with R2 free-standing and unassisted.

**36 of 56 emit `WADDLE`.** The survey asserted and **read back** `THREE_LEGS`
before every id; all 36 retracted the stabiliser themselves. Only **20 of 56**
are usable on a standing droid. The 36 include most of the emotional core —
most of `EMOTE_*` and most of `WWM_*`, `WWM_CURIOUS` among them. Not all:
`EMOTE_NO` (16) and `EMOTE_RETREAT` (17) touch no legs at all, and `EMOTE_DRIVE`
(11) and `EMOTE_FIERY` (18) only leave him in bipod.

**Decision.** Semantic behaviors compose dome + sound + light + explicit stance
themselves. They do **not** call `play_animation` for anything emotional. The
authored library remains available for a *seated or supported* R2 and as
timing reference, but it is not the vocabulary the character is built from.

Two rules follow, both from the operator's framing:

1. **Tripod down whenever he is "active".** Any behavior with energy in it
   deploys the stabiliser first and holds it. Stability is a precondition of
   expression, not a reaction to losing it.
2. **The mechanical leg sound belongs ON the deployment.** That sound is
   `R2_MOTOR = 2970` (`r2_assets.py:152`, `r2d2.py:315`), surveyed in S1b as
   *"long mechanical lift with clocklike ticking"* and the longest clip in the
   set. Fire it synchronised with the leg moving, not after speech with the leg
   already down. Deployment settles in 2.28-2.56 s (#22), so the clip has room
   to run underneath it.

### Why the boundary is `WADDLE` emission and not observed falls

Seven ids were observed to fell him. **That list is not the safety boundary,
and building one from observation is not possible.**

Repeat trials showed the leg-event sequences are near-deterministic — id 53
replayed 6 waddles in 3.29 s then 3.16 s; id 54 replayed 2 waddles in 1.86 s
then 1.69 s — while the **fall outcome did not reproduce**. Falling depends on
starting pose, residual momentum from the previous item, and the surface. An id
observed standing three times can fall on the fourth. **A waddler that did not
fall is lucky, not safe.**

Nor does the event stream predict it: waddle count, longest run and duration
all overlap between fallers and survivors. Id 42 waddled 10 times with a run of
8 and stayed up; id 22 waddled 4 times with a run of 2 and went down.
`leg_action_complete` reports state transitions with **no direction, distance
or force**, so the quantity that causes a fall is simply not in the signal.

### Neither preemptive nor reactive stance management can save an animation

Pre-deploying fails because the animation retracts the leg itself. Re-deploying
mid-animation was tested on hardware (id 37) and **refuted**: the command was
*accepted*, not refused, and still arrived far too late. Detection costs a
bridge round-trip (~0.3-0.5 s) and deployment settles in 2.28-2.56 s (#22),
against a fall that completes in well under a second. **Even at zero detection
latency the leg lands more than a second after he is down.** No polling rate
fixes it.

The only control available is not playing the animation.

*Reversed by:* a way to inspect an animation's leg track before playing it, a
per-animation stance lock in firmware, or a stabiliser that deploys in
materially under a second. None is known to exist.

*Does not fix:* AC4's seven conflict verdicts, AC6 interruption testing, and
the per-id energy/wear/cooldown fields remain open on #12.

---

## D-011 — The backpack speaker may carry character audio
**2026-08-17** · *operator ruling*

`CLAUDE.md` calls the backpack a service/debug/settings panel and forbids
adding character rendering to the *screen* without an entry here. Audio was
never named, and the omission mattered once voice was scoped: R2's body can
emit **only** the 388 factory sound ids in Sphero's firmware — the audio
command is play-by-id, stop-all and set-volume, with no data-bearing variant
(`docs/research/r2-protocol.md:207`). There is no upload path and no PCM
streaming. So any sound this project *authors* — droidspeak, words, anything —
can only come out of the backpack, a few centimetres behind and above his dome.

**This was never a hardware limit.** The backpack carries an ES8311 codec and
an NS4150B 3W class-D amplifier gated by GPIO46; it can play arbitrary audio
today. The only question was whether sound displaced from the body still reads
as *him*, and that is a taste judgement, not a measurement.

The evidence ran both ways and did not settle it. Sphero shipped BB-8 with its
audio on the phone, judged that worse, and paid to move the speaker into the
R2-D2 body for the follow-up. Amazon tested giving Astro a speaking voice, found
it "strange and creepy", and shipped a non-verbal body with speech demoted to a
visibly separate character. Against that, no HRI study anywhere establishes how
far a robot's voice can be displaced before the illusion breaks — the general
audio-visual literature says the effect survives without semantic congruence and
is strengthened by gaze, which the dome supplies. Full survey in the vault:
`Reference/Voice Stack Deep Research.md`.

**Decision.** The backpack speaker **may** carry character audio. Operator
ruling, 2026-08-17: *"yes, it would read as him."* Displacement is centimetres
on a 170 mm body, not a phone across the room, and the dome gives a gaze cue the
literature says reinforces attribution.

Three constraints ride with it, and they are what keep this from reopening the
whole boundary:

- **R2's own 212 native ids stay primary.** The backpack supplements the voice;
  it does not replace it. A behaviour that an `R2_*` id can carry uses the id.
- **Authored audio is co-timed with dome motion.** Firing backpack sound with a
  still dome is the configuration most likely to break the illusion, and the
  situational-context research says an isolated sound is the weakest form of
  expression regardless.
- **The screen boundary is untouched.** This decision is about audio only. The
  touchscreen remains a service panel.

**What this does not decide:** whether the authored audio is *droidspeak* or
*language*. Those are different characters and the second is the larger step —
Read and Belpaeme, whose work supports the non-verbal position, nonetheless
titled a 2014 HRI paper "Non-linguistic utterances should be used alongside
language, rather than on their own or as a replacement." Droidspeak is the
cheaper and more reversible move: every serious implementation, including the
professional Human Cyborg Relations vocalizer, is sample concatenation driven by
a sequencing layer — the same shape as our behaviour layer. Recommend starting
there and treating speech as a separate decision.

*Reversed by:* a playtest where backpack audio audibly detaches from the
body — the five-minute version is a sound played with and without a co-timed
dome turn, asking a listener where it came from.

*Does not fix:* nothing is buildable on this yet. Voice input is gated on a
capture measurement that has not been taken — one undocumented analog electret,
no beamforming, no echo cancellation reference signal for R2's own chirps, and
motors underneath it. Hosting, wake word and transcription remain open.

---

## D-012 — The LED base layer is system truth; animations play on top

**Status:** accepted 2026-08-17 · operator ruling, hardware-verified the same
session

**Decision.** The front and back RGB channels are a **two-layer surface**:

- **Base layer — system truth.** A persistent colour we set, meaning:

  | Colour | Meaning |
  |---|---|
  | **green** | success |
  | **blue** | neutral / on / waiting |
  | **red** | issue pending resolution |

- **Animation layer — transient expression.** Authored animations bring their
  own lights and mask the base while they play. They are theatre; they do not
  carry state.

Red is **reserved**, never decorative.

**Two axes, and keeping them separate is what makes the surface readable:**

| | **Steady** | **Blinking** |
|---|---|---|
| **when** | between interactions | during an interaction |
| **what it is** | status | expression |
| **red means** | issue pending resolution | annoyed |

Colour carries the *meaning*; steady-vs-blink carries the *mode*. So red is not
ambiguous between "annoyed" and "something is unresolved" — steady red is the
status reading, blinking red is the emotional one, and a glance tells you which
without knowing what just happened.

This is why the base layer is trustworthy: **status is checkable and emotion is
not.** A steady colour is a claim about the system that can be verified. Its
persistence is the whole value, and blinking is what borrows the channel
temporarily without overwriting that claim.

> [!info] The firmware's own default is semantically wrong for this
> At rest the front **alternates** red/blue — a blink, which under this scheme
> would read as "expressing something" while nothing is happening. Our steady
> set colour overrides it, which is exactly the behaviour measured (§3, *LED
> colour*). Owning the channel is not optional here; the default actively
> misreads.

**Why the layering is the point, not a compromise.** The first draft of this
decision filed "the semantics are invisible during an animation" as a cost. It
is not a cost — it is the design. Persistent state lives underneath, transient
expression plays above, and the hardware already behaves exactly this way
without being asked to: **the base colour reasserts itself when the animation
ends, unprompted.**

**Why this is implementable, which was not obvious.** The droid is not dark at
rest — his front alternates red/blue and his back alternates green/yellow on
their own. Three things were measured before accepting this (§3,
*LED colour*):

1. **Front and back are true RGB** (bits 0/1/2 and 4/5/6). Logic displays and
   the holo projector are **brightness only**, so this scheme applies to two
   fixtures, not four.
2. **A colour we set HOLDS** — the firmware's baseline alternation does not
   resume and overwrite it. Without this the whole scheme would be unbuildable.
3. **An animation transiently overrides it and the set colour returns
   afterwards**, unprompted. Our colour is a **base layer**, not a one-shot
   write.

**This pairs with D-010 rather than working around it.** D-010 ruled authored
animations out as the behaviour library — only 20 of 56 are usable standing,
and we cannot inspect what one will do before playing it. The base layer is the
channel we **can** control, so state lives there and the uncontrollable layer is
demoted to decoration. Losing animations as a state carrier costs nothing once
state has a home.

**Corroboration from the droid's own authoring:** id 4, adjudicated as
translator's `ANIM_SAD` ("denied"), uses red for a refusal — the firmware
already reaches for red on a negative.

> [!warning] But id 4 holds that red **steady**, and it is an expression
> Under the two-axis scheme a steady red is a *status* claim, so the droid's own
> authoring contradicts the axis it corroborates on colour. Not fatal — id 4 is
> an animation, and animations live on the masking layer where our conventions
> do not apply. It is a live counterexample to watch, though: if authored
> animations routinely hold steady colours, the blink/steady distinction will be
> muddied every time one plays.

**Untested and load-bearing: we have never driven a modulation ourselves.**
Every colour we set held *steady*, which is only the status half of the scheme.
Two things are unproven, and the expression half rests on both:

- **Blink** — toggling one colour on and off from our side. Neither the
  achievable rate nor whether it reads as deliberate rather than glitchy has
  been measured.
- **Alternation** — cycling between two colours. The firmware does this at rest
  (front red/blue, back green/yellow), so the *hardware* plainly can; what is
  unknown is whether **we** can drive it at a comparable rate over BLE, or
  whether the round-trip makes ours look sluggish next to the firmware's own.

The second is the sharper risk. Our modulation competes visually with a native
pattern the user has already seen, so "we can do it" is not the bar — it has to
not look worse.

**The model is a utility panel.** A green or red indicator on the outside tells
you the state from across the room; you open the panel and there is a small
screen for diagnostics. Nobody confuses the two, and nobody reads the diagnostic
screen to find out whether anything is wrong.

That is exactly the split here, and it is why a status light on R2's dome is
**not** service leaking onto the character. `CLAUDE.md` puts character on the
body and service on the backpack screen; the LED is an affordance, the screen is
the detail view, and they carry different *kinds* of thing:

| | **LED** | **Backpack screen** |
|---|---|---|
| carries | affordance — *that* something is up | details, troubleshooting |
| read at | a glance, across the room | up close, deliberately |
| in character? | yes — R2-D2's lights read as status in canon | no, and it does not need to be |

The LED never spells anything out; it signals, and the screen is where you go to
find out what. That keeps the boundary intact rather than bending it: **nothing
with a face or text goes on the body, and nothing needing a glance goes on the
screen.**

*Reversed by:* a playtest where the colour reads as arbitrary rather than
meaningful; by finding an animation that does **not** restore the base layer
(only one id was tested, n=1); or by the status reading feeling like a machine
indicator bolted onto a character.

*Does not decide:* brightness, the logic-display and holo-projector channels
(brightness-only, so they cannot carry this), what yellow means, transition
timing, or whether the two fixtures show the same colour or different ones.

---

## Open — to be decided on hardware

- ~~**Animation ID table.**~~ **RESOLVED 2026-08-17** — all 56 ids surveyed
  (S1d) and the 7 disputed ids adjudicated (#12 AC4). Gaps 20, 23, 28, 29, 30
  are valid and play. Neither source won outright: translator 2, spherov2 1,
  4 inconclusive. See *Animation ID conflict* in `r2-capabilities.md` §3.
  It did **not** unblock the semantic behavior library — D-010 did the opposite,
  ruling authored animations out as the library.
- **`enable_idle_animations` on or off.** Off during bring-up so every motion is
  attributable in logs; re-evaluate as a feature afterwards.
- **Board revision — confirm, do not assume.** Record `00_board_check` and
  `08_i2c_tools` output here with a date.
- **Flash size** — 8 MB (`vthinkxie`'s claim) vs 16 MB (Waveshare's
  `sdkconfig.defaults`). Affects partitioning and OTA.
- **Persistence media** — NVS / LittleFS / microSD split.
- **Keepalive period** — 3 s is inherited, not measured.
- **Backpack power and mounting.** Entirely unaddressed and not a software
  problem.

---

## D-013 — Behaviours are composed from primitives, and the dome is not a fine instrument

**Date:** 2026-08-17 · **Status:** accepted · **Supersedes nothing** ·
**Implements:** `mac-prototype/r2_behavior.py`

### Context

S1 surveyed what R2 *can do*, one channel at a time. Nothing had ever been
composed. The first attempt to drive dome, sound and light as a single gesture
turned up three constraints that change how the vocabulary must be written, and
two of them were only visible once channels ran together.

### Decision

**1. A semantic behaviour is composed from primitives, never from an authored
animation.**

An animation is a stance-tier command whose contents cannot be inspected before
sending — `EMOTE_YES`, a *nod*, emitted WADDLE three times and put R2 on the
floor (#11). The choreography layer therefore has **no name** for `animation`
or `set_stance`; they are not merely discouraged, they are unrepresentable, and
a beat that reaches the stance tier fails to validate. Every behaviour tops out
at `dome`.

This does not retire authored animations. It says they are not the substrate a
behaviour library is built on. D-010 already ruled that excitement is
choreographed by us; this extends the same reasoning to the whole vocabulary.

**2. The dome's minimum legible gesture is ~12°, and commands below the
threshold are silently ignored.**

Measured: 4°/6°/8°/10° produce no motion; 10.5° and above do. Every one of
them returns `ok: true`. A subtle tilt is not a thing this hardware can do, so
behaviours must be written in large moves — and the sub-threshold move must be
rejected at build time, because the hardware reports it as a success.

**3. Drift is corrected against a persistent home, not against where the beat
started.**

Every dome move undershoots by ~3°, so a gesture built from deltas never
returns to its origin. Anchoring the correction to each beat's own starting
angle fails silently: the residual is always ~3°, always below the threshold,
so the correction never fires and the dome walks anyway. Measured across five
successive settled readings: **−24.58° → −32.40° → −34.93° → −37.58° →
−41.17°** — monotonic, and the correction did not fire once. A **fixed** home
lets error accumulate until it is large enough to command, producing a bounded
sawtooth instead of a slide.

**4. Every beat ends on a D-012 status colour.**

An LED colour we set is state and survives the link dropping, so whatever a
beat leaves lit is what the household sees. A beat that ends mid-expression
leaves R2 permanently mid-expression. Validation rejects a beat whose last step
is not a colour reset to one of the three status colours.

### Consequences

- The behaviour table in `r2-capabilities.md` §4 needs its dome column rewritten:
  "±15° alternating", "quick ±30°", "small repeated twitch" — the last is not
  achievable at all, and any figure under 12° is fiction.
- **True simultaneity is unavailable** while composition happens outside the
  daemon: the queue loop is serial (`r2_probe.py:1443`). Batching a phrase's
  requests gets 0.12 s pacing and `settle=0` lets motion outlive its command,
  but overlapping channels properly needs the daemon-side timeline executor
  (#43). This is the strongest argument for that issue.
- Sound must be queued **before** the dome move it accompanies. The reverse was
  tried first and refuted by the operator: audio onset is slower than the batch
  spacing.
- Beats are slower than designed. A dome move costs ~2.2 s of dead time
  regardless of how far it travels, so a three-move gesture cannot run under
  ~7 s. Behaviours that need to feel quick must not move the dome.

### Rejected

- **Tuning the gaps to make the dropped moves land.** Pursued for one round on
  the hypothesis that moves issued into live travel were being discarded. It
  fitted n=2 and was wrong; the cause was the travel threshold. Recorded because
  the wrong explanation was the more natural one.
- **A correction constant for undershoot.** S1c already showed the deadband is a
  gradient across the range (2.4° at +170°, 5.7° at −145°), so a constant fitted
  at one end is wrong at the other. Re-reading the angle beats modelling it.

## D-014 — A service-state signal must be a transition, not a colour

**Date:** 2026-08-18 · **Status:** accepted · **Extends:** D-012 ·
**Implements:** `mac-prototype/r2_reactive.py`

### Context

D-012 settled *what* the LEDs mean: colour carries meaning, steady-vs-blink
carries mode, and the LED is the affordance that says *something is up* from
across the room. It did not settle how a signal gets **noticed**, and the first
feature that needed the operator to act on one exposed the gap.

`r2_reactive.py` runs a hands-off calibration and a negative control before it
starts listening. The operator has to know the moment listening begins, because
their hand is the input device. The arming signal was `BASE_NEUTRAL` blue —
correct by D-012's table, since blue is "on / waiting / neutral".

It was invisible. The previous session's own teardown leaves R2 on
`BASE_NEUTRAL`, so arming him blue changed nothing at all. The operator was
told to wait for a cue that had already happened, waited through the whole
120 s armed window, and the run returned zero reactions with no touch in it.
The console said `ARMED` and the console is not something they can see.

### Decision

**A state the operator must ACT on is signalled by a change they can see, not
by a colour they have to have been told to expect.**

Concretely, and bindingly:

1. **Establish a contrasting state first.** `r2_reactive` drives the LEDs
   **dark** for the entire hands-off stretch, then to blue at the instant it
   arms. The edge is the signal; the colour only says *which* state was
   entered.
2. **Never assume the prior state.** An LED colour we set is storage we own and
   survives the link dropping, so the state before a signal is whatever some
   earlier session left. A signal defined only by its destination is a no-op
   whenever the destination is already current — which is exactly the case
   after a clean teardown, i.e. the *normal* case.
3. **This is a service signal, and service signals on the body are fine.** The
   character boundary forbids *rendering* on the body, not signalling. Dark →
   blue spells nothing and shows no face.

### Consequences

- Any future affordance meaning "act now" must define its **before** state, not
  just its after. That is a new obligation on every such signal.
- Dark is not a new entry in D-012's colour language. It is the absence of one,
  used to make the next entry legible.
- The rule generalises past LEDs: it applies to any signal whose whole job is to
  tell a human that their turn has started.

### Evidence

- Live run 2026-08-18, R2 on the floor: 120 s armed, **0 reactions, 0 touches**.
  The operator confirmed they never petted him — they were waiting for a cue
  that was indistinguishable from the state before it.
- Verified in the test suite by asserting the *ordering* of LED writes, not
  their presence: `test_the_arm_cue_is_a_visible_edge_not_a_colour` fails when
  the dark phase is removed, which the presence-only assertion did not.

### Rejected

- **A chirp as the arming cue.** Unmissable, and available under the `dome`
  ceiling. Rejected because sound is the loudest character channel R2 has, and
  a service event that sounds like him talking blurs exactly the boundary
  CLAUDE.md draws. Light is the sanctioned affordance; use it properly instead
  of reaching past it.
- **Printing `ARMED` more loudly.** This was the original failure. The observer
  cannot see the console — the whole premise of `/survey-session` — and no
  amount of console formatting reaches a person whose hands are on the robot.
