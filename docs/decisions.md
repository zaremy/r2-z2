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

### Amendment A — 2026-08-18: the BSP was read; it is V2-only

The "provisional" caveat above rested on the BSP source being unread. It has now
been read — `01_project_template` builds clean on ESP-IDF v5.5.5 and fetches it.

**D-005 HOLDS, conditionally on the board being V2.** The reversal condition as
written ("the BSP failing on V2 hardware") is **not** met: BSP v2.0.3 is
V2-native. It calls `esp_lcd_new_panel_co5300()` unconditionally and declares no
SH8601 dependency at all, so there is no V1 display driver in the tree.

The real exposure is the mirror image of the one recorded. If the board arrives
as **V1** (SH8601 + FT3168), the BSP will drive its panel with the CO5300 init
sequence, and switching is not a config change — there is nothing to switch to.
Touch is unaffected: the BSP probes CST816S at `0x15` then FT5x06 at `0x38` at
runtime, and applies the `0x10` panel X offset only in the CST816S case.

**So this decision now has a hardware precondition, not just a software one.**
Confirm the revision before writing firmware against the BSP, and treat a V1
result as a live reversal trigger for D-005 rather than a detail. Evidence and
the per-variant table: [research/embedded-path.md](research/embedded-path.md).

### Amendment B — 2026-09-01: coexistence was measured, and D-005 survives it

The reversal condition has two halves. Amendment A settled the BSP half. This
settles the other: **"Wi-Fi/BLE coexistence proving unworkable under IDF" is
NOT met.** Issue #103 A1, three one-hour arms on the real board holding a real
link to the real droid:

| Arm | Wi-Fi | Duration | BLE disconnects | Worst keepalive gap |
|---|---|---|---|---|
| 1 | off | 64 min | **0** | 3.0 s |
| 2 | connected, idle | 63 min | **0** | 3.1 s |
| 3 | connected, 2.04 Mbit/s sustained | 62 min | **0** | 3.1 s |

Pass/fail was fixed in #103 *before* any arm ran: PASS requires zero disconnects
and no gap over 10 s (three missed keepalives). Nothing came near it — the worst
gap in any arm is 3.1 s against a 3 s keepalive period, i.e. **once the link was
up, not one keepalive was missed**, and coexistence at full load cost 0.1 s over
the standalone baseline. (Arms 2 and 3 each log two failed keepalives from
before the first `LINK UP`, while Wi-Fi association delayed the BLE connect;
there is no link to measure then, so scoring starts there.) Verdict re-derivable from the committed evidence with
`firmware/coex_check/results/verdict.py`.

Two things this does NOT establish, worth stating so the result is not stretched:

- **It is one radio environment, one AP, one droid, one morning.** The field
  report that motivated the experiment (loss rising to 20%+, streaks of 100%)
  was presumably measured somewhere with different congestion. A clean result
  here does not repeal that report; it says our hardware under our traffic in
  our house is fine.
- **Our traffic is the friendly shape for time-slicing** — a 3 s keepalive plus
  short bursts. A future design that streams audio continuously over BLE is
  outside what was measured and would need its own arm.

**D-005 HOLDS.** Both halves of its reversal condition have now been tested on
hardware rather than argued about, and neither is met.

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

**Weakened 2026-08-15, then RESTORED IN FULL 2026-08-23.**

The 2026-08-15 note read: the retailer listing for the unit actually purchased
(Amazon B0DSVK5576) names **SH8601 + FT3168** in its title — the V1 stack. If
that held, `vthinkxie`'s 1.8" env would be the *correct* firmware for this board
and this decision's premise void. It concluded "the direction of the hazard is
now unknown, not known", and named its own reversal condition: `0x38` (FT3168)
answering on the probe.

**That condition did not occur. The probe ran on 2026-08-23 and `0x38` was
silent; `0x15` (CST816) answered.** The board is V2. So:

- **The retailer listing was wrong.** This is the sharpest evidence yet for the
  rule in [board-revision.md](research/board-revision.md) that the revision must
  be probed, never read off a product page — the listing named both V1 parts and
  both were wrong.
- **This decision's original premise is restored, not void.** `vthinkxie`'s 1.8"
  target is V1-only; this board is V2. Flashing it would send an SH8601 init
  sequence to a CO5300 panel and probe touch at `0x38` where nothing answers —
  exactly the hazard originally described.
- **The direction of the hazard is known again, and it is the original
  direction.** The intermediate "unknown" state is closed.

Leaving the weakened wording standing would have been the dangerous outcome: it
reads as "`vthinkxie` might be the right firmware for this board", which is now
known to be false.

*Reversed by:* nothing outstanding. The probe that could have reversed this
confirmed it instead. A different board would need its own probe.

Evidence: [board-capabilities.md](research/board-capabilities.md); transcript at
`R2Z2-vault/Experiments/data/board-check-20260823-000316.log`.

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
**Most of this list is settled by Amendment A below — read it before relying on
anything here.**

---

### Amendment A — the hardware overrules the scheme
**2026-08-17 · operator rulings + measured on `D2-6F6B`**

D-012 above was written from three measurements and a ruling. Driving the
channel properly since then has settled most of what it left open, and
**refuted one thing it assumed**. The scheme's shape survives — colour carries
meaning, modulation carries mode — but the primitives it named do not.

**1. There are seven colours, not a continuum.** Low saturation reads as grey
(a `(120,190,255)` pale blue rendered "almost grey"), so colour semantics must
use hard-contrasting hues. In practice that is the **corners of the RGB cube** —
each channel fully on or fully off — which is also the only set every successful
hardware test has used. **Orange and amber are therefore unreachable**: there is
no corner between red and yellow, and the mixed value is both desaturated enough
to drift grey and too close to yellow to separate at lamp size. Two hues that
differ only in shade are one hue on this hardware.

**2. Fades are not available on the RGB fixtures.** Every value change flickers —
isolated by writing the identical colour twenty times (rock steady) against any
change at all, where a 30-unit step flickers as much as a full swap. An
interpolated ramp therefore reads as flicker, not as a fade. The cause is R2's
own LED update path, so **it survives the ESP32 port**. Design discrete
high-contrast frames; never interpolate a PSI.

**3. The fade relocates to bit 7.** The holo projector dims cleanly across
255 → 64 → 16 and is the one genuinely continuous channel. Bit 3, the logic
displays, has a curve so steep it must be designed as on/off — which suits a
"thinking" indicator anyway, since real logic displays flicker rather than fade.
This reverses the *Does not decide* line below: those two channels are no longer
excluded, they are load-bearing.

**4. The pattern vocabulary is four primitives, and only three are ours.**

| primitive | whose | carries |
|---|---|---|
| **steady** — one corner held | ours, except red | a status claim |
| **blink** — colour ↔ dark | ours | escalation: slow attention, fast danger |
| **alternate** — colour ↔ colour | **the droid's** | expression |
| **sweep** — ordered walk through 3+ corners | ours | transition |

At rest, uncommanded, the front alternates red/blue and the back green/yellow;
animation id 0 *speeds up* the front alternation rather than starting it. So
**alternation is his idiom, and a status layer that alternates cannot be told
apart from him simply being himself.** Steady is very nearly ours alone — the
exception being animation id 4, the refusal, which breaks its alternation to
hold **red steady**, making a steady red ambiguous between "fault pending" and
"he just refused you". That is why danger is carried as a fast blink, not a hold.

Alternation is also a real expansion where it belongs: seven corners become
twenty-one distinguishable pairs, and the flicker that ruins fades is free for a
corner-to-corner swap. You cannot mix your way to an eighth colour, though —
blending would need 24–30 Hz and the ceiling is 8.3.

**5. Colour assignments.**

| colour | meaning |
|---|---|
| **blue** | idle — nothing engaged |
| **cyan** | engaged with you |
| **green** | wake-sweep terminus; success |
| **yellow** | needs monitoring — *this is D-012's old steady red* |
| **red** | danger and stop, only |
| **magenta** | rest, low power |

**Red is narrowed.** D-012 assigned steady red to "issue pending resolution".
Both IEC 60073 and every shipping consumer device put pending-attention on
**yellow** and reserve red for danger and privacy — and amber, which the
original scheme would have wanted, is not reachable anyway. This also answers
*what yellow means*, which D-012 declined to decide.

**Blue is idle because it is the dimmest corner** — 0.072 relative luminance,
about a tenth of green — and idle is the state that runs for hours. Cyan is the
engaged baseline that interaction states hold underneath, so the back PSI
answers "is he with me" at a glance while the front says what he is doing.

**6. Urgency rides on rate, not brightness.** The corners are nowhere near
equally bright: yellow 0.93, cyan 0.79, green 0.72, magenta 0.28, red 0.21,
blue 0.07. Trimming them to a common level was tried and **failed** — anchored
to red and blue, the two dimmest corners, it crushed the interaction states
until they stopped reading. Red at full is only 0.21, so **danger can never be
the brightest thing on this droid**; it is the *fastest*, at a 0.25 s blink
against 2.4 s for a pending issue. Only yellow keeps a trim (0.60), being both
the brightest corner and a sustained state.

**7. The rate ceiling is 8.3 writes per second.** `cmd_safe_interval` is 120 ms
and R2 drops commands sent faster. A 4 Hz blink is 8 writes/s and therefore has
no margin. Today's harness is slower still at ~390 ms per set, but that is two
bridge poll loops rather than the LED; the true LED ceiling is **unmeasured**
(#17).

**8. Quiet hours scale value, not hue** — scaling a saturated corner keeps it
saturated, so `(0,255,0)` → `(0,64,0)` is still unambiguously green. Step to the
dim frame; do not ramp into it. Safety and privacy states are exempt.

> [!warning] The sleep state is specified but **blocked**
> R2 cannot be put to sleep while the daemon holds the link, because the
> keepalive **is** the wake command (`DID 0x13 / CID 0x0D`, every 3 s). Any
> sleep is undone within three seconds. Quiet-hours behaviour is a
> session-lifecycle change before it is a lighting decision — tracked on #38.

**Now decided, that D-012 deferred:** brightness (§6, §8), the logic-display and
holo channels (§3), what yellow means (§5), and whether the two fixtures show
different colours (§5 — yes: back holds status, front carries expression).

**Still open:** whether the holo still dims cleanly at a 120 ms step rate, and
what rate the firmware's own alternation runs at — ours is judged beside it.

*Reversed by:* a firmware or hardware revision in which a PSI value change no
longer flickers, which would reopen fades and with them the whole modulation
question.

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
- ~~**Board revision — confirm, do not assume.**~~ **RESOLVED 2026-08-23 —
  V2 (CO5300 + CST816), OBSERVED.** `board_check` on the bus: `0x15` answered,
  `0x38` silent, `0x20` present. This confirmed a prior inference from the
  factory-image comparison, by an independent method. **D-005 Amendment A's
  reversal is NOT triggered.** Evidence and the full I²C + PMU inventory:
  [board-capabilities.md](research/board-capabilities.md); transcript in the
  vault at `Experiments/data/board-check-20260823-000316.log`.
- ~~**Flash size** — 8 MB (`vthinkxie`'s claim) vs 16 MB (Waveshare's
  `sdkconfig.defaults`).~~ **RESOLVED 2026-08-22 — 16 MB, OBSERVED.**
  `esptool.py flash_id` read it directly from the board and the full-image dump
  is exactly 16,777,216 bytes. `vthinkxie`'s 8 MB is REFUTED for this unit.
- **Persistence media** — NVS / LittleFS / microSD split.
- **Keepalive period** — 3 s is inherited, not measured.
- **Backpack mounting.** Dimensions and weight are unaddressed and not a
  software problem. **Power is answered** (2026-09-01): there is no cell on the
  board — AXP2101 `STATUS1` battery-present bit clear across ten samples, see
  D-017 Amendment A and `research/board-capabilities.md`.

### Amendment A — 2026-09-07: the panel inherits the semantics, not the values

**Status:** accepted · **Operator ruling** · unblocks #101 child 5, whose AC2
asks for states rendered "with correct colour" and had no normative source for
what correct meant.

#### The question

Every word of the colour reasoning above is about **LED fixtures glanced from
across a room** — blue is idle *because it is the dimmest corner at 0.072
relative luminance*, cyan is the engaged baseline the back PSI holds. None of
it is about a 29 mm panel read deliberately at arm's length, and
`firmware/panel/main/panel_ui.c` had in fact chosen its own palette with a
comment arguing exactly that.

D-017 says the wake frame "carries the state colour", implying one shared
notion. Two readings, no ruling, and building either way was inventing.

#### The ruling

**The panel inherits D-012's colour SEMANTICS. It does not inherit the
values.**

One vocabulary: `yellow` means needs-monitoring on the body and on the glass,
`red` means danger in both places. What a colour *means* is the shared language
and must not fork. What luminance and hue best deliver that meaning is a
property of the medium — an OLED read at arm's length is not a diffused lens
seen across a room, and forcing the panel to 0.072-luminance blue because that
suited an LED would be obeying the letter of a decision against its purpose.

This is the same split D-017 Amendment B drew for states two days ago: one
model, several views.

#### The consequence that changes shipped code

**Red is narrowed here too, and the panel was over-using it.** The section
above reserves red for "danger and stop, only" and puts pending-attention on
yellow — a correction D-012 made deliberately, citing IEC 60073.

`panel_ui.c` rendered `no link` in **red**. A dropped link is not danger:
nothing is going to hurt him or anyone because the radio stopped answering. It
is the definition of needs-monitoring, so it becomes **yellow**, and red is
kept for a state that genuinely warrants alarm.

That is worth noticing as a pattern rather than a fix: the panel reached for
red because the row felt bad, which is exactly the reasoning D-012 rejected
when it took red away from "issue pending resolution".

#### What this ruling does NOT give the panel

**A colour for "we cannot say".** D-012 assigns six colours to six meanings and
none of them is absence of knowledge. The panel needs one — `STORAGE` and
`BRAIN` are not wired, and a reading whose link has dropped is not stale but
unvouchable.

It uses a neutral grey, and that is deliberately **not** a colour claim: it is
the absence of one. Adding a seventh meaning to the light language would be a
new decision about the body, made for the convenience of a screen, and this
amendment does not make it.

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
   **dark** for the entire hands-off stretch, then to the armed colour at the
   instant it arms. The edge is the signal; the colour only says *which*
   state was entered.
2. **Never assume the prior state.** An LED colour we set is storage we own and
   survives the link dropping, so the state before a signal is whatever some
   earlier session left. A signal defined only by its destination is a no-op
   whenever the destination is already current — which is exactly the case
   after a clean teardown, i.e. the *normal* case.
3. **This is a service signal, and service signals on the body are fine.** The
   character boundary forbids *rendering* on the body, not signalling. A
   colour change spells nothing and shows no face.

### Amendment A — 2026-08-18: the armed colour is cyan, not blue

Shipped as dark → **blue** (`BASE_NEUTRAL`), which was correct under D-012's
original table where blue read "on / waiting / neutral". `docs/behaviour-states.md`
landed the same day and reassigned it: **blue steady is now `idle` — nothing
engaged**, and **`listen` is cyan steady, front and back**. An armed loop
painted `idle` is the opposite of what it is doing, and unreadable against a
genuinely idle robot from across the room — which is this decision's own
failure mode, reintroduced by a colour that got redefined underneath it.

`r2_reactive` now arms with `BASE_ENGAGED` on both PSIs, matching the `listen`
row. Two consequences worth stating:

- **The disarm edge is now free.** `_tidy` ends on `BASE_NEUTRAL`, so exiting
  reads cyan → blue: `listen` → `idle`, two hues and two rows of one table.
  The explicit dark frame that used to mark the disarm is gone — it existed
  only because armed and session-over were both blue.
- **The general rule survives the specific colour.** This decision was never
  about blue. It is about defining the *before* state, and it now has a worked
  example of the colour itself moving while the rule held.

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

---

## D-015 — Voice's off-board compute is cloud APIs for now; a home server is deferred, not rejected

**Date:** 2026-08-24 · **Status:** accepted · **Operator ruling** ·
**Extends:** `intent.md` (cloud is for the unusual), D-011 (the backpack speaker
may carry character audio)

### Context

Speech recognition does not fit on an ESP32-S3. That is not a tuning problem:
Espressif's own AFE wake-word and AEC paths are hard-gated on PSRAM
(`USE_AFE_WAKE_WORD`, `USE_AUDIO_PROCESSOR` → `depends on … && SPIRAM`), and
they buy a *wake word plus a fixed command set* — not conversation. Every
shipping ESP32 voice assistant, including the `xiaozhi` firmware that came on
this board from the factory, streams audio off-device and does STT, the LLM and
TTS elsewhere.

So "the ESP32 is the brain" (`architecture.md`) is true of the character loop
and cannot be true of the language path. Something else runs speech, and there
were only ever two shapes:

| | Cost |
|---|---|
| **Cloud APIs** | no new hardware; a live third-party dependency in the voice path |
| **Home server** | survives any vendor; ~4 vCPU / 8 GB always-on, and `CLAUDE.md` says no Raspberry Pi without a documented forcing limitation |

Full evidence and sources: vault `Reference/Assistant Landscape Research.md`.

### Decision

1. **Voice uses cloud APIs when it arrives.** Provider-agnostic behind the
   existing cloud-client interface — this buys nothing if it hardwires a vendor.
2. **Voice stays out of S7.** S7 is the behaviour engine on device and the point
   at which the Mac stops being required. Voice remains at **S9**, where the
   roadmap already had it.
3. **A home server is deferred, not rejected.** The self-hosted path is known to
   work and to be provider-swappable end to end (local ASR, local TTS, local
   LLM). It is the intended destination, not a fallback.

### The scoping that makes (1) acceptable

Choosing cloud accepts the **T3 / Jibo exposure** on purpose — the failure mode
that killed Jibo and Anki Vector was a server being switched off. It is
acceptable *only* because it is confined:

- **The local behaviour loop may never depend on it.** Idle, mood, boredom,
  sleep/wake, quiet hours and reflexes run with no network, unchanged. This is
  already `intent.md`'s rule; D-015 is why it is load-bearing rather than a
  performance preference.
- **Losing the cloud must degrade R2 to mute, not to dead.** If a voice outage
  ever makes him less alive, this decision has failed and the home server is no
  longer deferred.
- **Voice here means character voice** — he hears you and Threepio answers
  (#41, #47). It is **not** the assistant job. Timers, weather and smart-home
  control remain out of scope; that job fails the character test and, on one
  analog microphone with no DSP, fails on hardware anyway.

### Consequences

- **`CLAUDE.md`'s no-Pi rule is untouched today and will need amending, not
  excepting, when the server lands.** The rule exists to stop a Pi becoming the
  brain. A speech box is not the brain — but that distinction must be written
  down at the time, as an amendment, rather than assumed by whoever is holding
  the soldering iron. A rule quietly broken once stops being believed.
- **The cloud client is now on the critical path for S9** and must be
  provider-agnostic from its first line. Retrofitting that is how vendors become
  identities.
- **T2 headroom is a precondition and is still unmeasured.** BLE central +
  Wi-Fi + TLS + LVGL on a PSRAM framebuffer + a continuous audio uplink has
  never been run together. Cloud does not relieve this; it *is* the continuous
  uplink. Measure before scheduling S9.

> [!warning] The existence proof on this board does not cover this.
> `xiaozhi` ran Wi-Fi, TLS, Opus streaming, on-device wake word and an LVGL
> display together on this exact hardware — and it has **no Bluetooth at all**
> (upstream #1426, #129 are open feature requests). The single hardest thing we
> need, a shared 2.4 GHz radio serving a BLE central link *and* an audio
> uplink, is precisely what the reference implementation never had to solve.

### Reversed by

- Any measurement showing BLE + Wi-Fi + TLS + audio cannot coexist with
  acceptable R2 control latency — which makes the shape of the off-board
  compute moot until the radio problem is solved.
- A cloud provider outage that reaches the *character*, not just the language.
  That is the trigger to stop deferring the home server.
- Deciding voice should be usable with no internet at all, which is a different
  product decision and would force the server immediately.

### Rejected

- **A home server now.** Correct destination, wrong time — see Amendment A for
  what "wrong time" actually means. It would let the voice decision hold the
  Mac-retirement decision hostage, and retiring the Mac and adding voice are
  separate projects that should stay that way.
- **Waiting for on-device STT to become viable.** MultiNet gives ~200 fixed
  commands, which is a remote control, not a conversation. Designing toward it
  would produce exactly the commandable appliance the character test forbids.

### Amendment A — 2026-08-24: the trigger is ship, not an outage

Operator correction, same day, before this landed. Three things above are
stated more weakly than the facts support.

**1. Cloud APIs are not a future choice. They are already running.**
`mac-prototype/voice/` is built: `reason.py` (#46) turns heard text into a
`Beat` through a closed tool schema, and `speak.py` (#47) is Threepio's voice
on `gpt-4o-mini-tts`. So the decision above does not *select* an architecture
for later — it **ratifies the one already in the tree** and says it continues.
What remains future is voice **on the board**, which is S9. Voice on the Mac is
S2 and partly done.

**2. The home server exists, and the trigger is validation, not failure.**
The body of this ADR frames the server as deferred with a cloud outage as the
trigger to stop deferring. That is too passive and it inverts the real plan:

> **Cloud is the development path. The home server is the production path.
> The trigger is "validated, ship it" — not "the cloud broke".**

The operator already owns the hardware. It is not a purchase, a design
question, or a new dependency on the household; it is a box in the garage
waiting for a reason to be switched on.

Two consequences worth stating:

- **The `CLAUDE.md` no-Pi concern largely dissolves.** That rule guards against
  a Pi quietly becoming the brain in a project that has not earned one. It does
  not apply to hardware already owned, running a stage of the pipeline that
  provably cannot run on the MCU. The amendment obligation in *Consequences*
  stands — write it down when it happens — but the bar it has to clear is much
  lower than that section implies.
- **The T3 exposure is time-boxed by design**, not merely tolerated. A cloud
  dependency we intend to remove at ship is a different risk from one we intend
  to keep. The outage trigger is still worth keeping as a *floor*: if the cloud
  reaches the character before validation is done, the server comes forward.

**3. The xiaozhi-and-BLE note in the warning box is badly worded.** "xiaozhi has
no Bluetooth" reads as a claim about hardware and is not one — the ESP32-S3
plainly has BLE, and the board plainly has the codec. The claim is about the
**firmware**: xiaozhi never uses BLE, so it never had to schedule one radio
against a continuous BLE central link *and* a continuous audio uplink at the
same time. The board's capability is not in question. Only the "proven
together" claim is, and that is the claim we would be borrowing.

---

## D-016 — Three surfaces, and the web app is not a remote control

**Date:** 2026-08-24 · **Status:** accepted · **Extends:** `CLAUDE.md` character
boundary, D-012 · **Prompted by:** the northstar showing the whole product at once

### Context

`CLAUDE.md` draws the character boundary across **two** surfaces: R2's body is the
character interface, and the backpack touchscreen is a service panel that is not a
face. That boundary has held for every decision so far.

Building the northstar put the entire product on one page for the first time, and a
**third** surface walked in without ever having been ruled on: a web app. It had been
implied for a while — `intent.md` lists provisioning and settings, the frame named a
builder's job, and the MCP work assumes something calls the tools — but nothing said
what it is, and more importantly nothing said what it must never be.

An unruled surface is how the character leaks. The screen got an explicit prohibition
precisely because it is the obvious place to put a face; the web app is the obvious
place to put a **joystick**, and that is the more dangerous of the two.

### Decision

Three surfaces, each answering a different question, and the questions are what keep
them apart:

| Surface | Answers | Read | Forbidden |
|---|---|---|---|
| **His body** — dome, lights, sound, stance | *Is he alright?* and everything with personality | at a glance, across the room | anything with a face or text |
| **The backpack panel** | *What is wrong, and can I fix it here?* | up close, deliberately, when the light already sent you | rendering him; anything needing a glance |
| **The web app** | *Who is he becoming?* | sitting down, occasionally, by the owner | **being a remote control** |

**The web app carries:** his state (mood, energy, boredom), his diary in his own
terms rather than log lines, his memory of people and routine, the set of things he
is able to notice, and housekeeping — Wi-Fi, quiet hours, how bold he may be.

**The web app must never carry:** a drive pad, a "do X now" button, a behaviour
trigger, or any control that makes him perform on demand.

### Why the prohibition is the load-bearing half

Two independent failures, and the web app is the only surface exposed to both:

1. **It would make him commandable.** A button that reliably produces a behaviour
   teaches the household that R2 obeys, and appliance-ness is not a mode you get to
   leave. The body is safe from this because it has no buttons; the panel is safe
   because everything on it is a *diagnostic* fired by an operator who is debugging,
   not a household member who wants a trick.
2. **It would move the character onto a screen.** Watching R2 do something *because
   you pressed a thing in a browser* puts the interesting part in the browser. That
   is the same failure `CLAUDE.md` forbids on the touchscreen, arriving through a
   door nobody had thought to lock.

The hardware-test surface is not an exception to this — it is the reason the panel
exists rather than the web app. Firing an actuator is a **repair** action, performed
at the droid, by someone holding him, at the place the safety ladder can see.

### Consequences

- **The access control already exists and is free.** MCP distinguishes tools exposed
  to the model from tools registered with `AddUserOnlyTool`, which are hidden from it
  and surfaced only to a client that asks for them. Service and housekeeping tools go
  in the second set; the web app is the client that asks. The character/service split
  becomes an authorisation boundary rather than a convention people have to remember.
- **Provisioning and settings move off the panel**, which is the right home for them
  anyway at 29 × 35 mm. This removes the single structural objection to the
  instrument-panel launcher (nowhere to put settings) and the launcher choice should
  be re-run with settings out of the panel's job description.
- **The panel's job shrinks to one question**, which is what makes a watch-sized
  screen viable at all.
- **A new obligation on every future surface:** state what it must never do, not only
  what it is for. The screen's prohibition was written down and held for months; the
  web app's absence of one is why this ADR exists.

### Reversed by

- A household member wanting to ask R2 for something directly and finding no way to,
  in a way that reads as a missing feature rather than as character. That is a real
  signal, and the answer would be a *request he can decline*, not a button.
- Discovering the panel genuinely needs settings on it — e.g. Wi-Fi provisioning that
  cannot bootstrap without a local UI, which is plausible and unmeasured.

### Rejected

- **A phone app.** Adds a platform, a store, a signing identity and a review process
  to a project whose whole durability argument is that nobody else can switch it off.
  A local web page served to any browser has none of that.
- **Settings on the panel.** Three or four touch targets fit on it. Spending them on
  configuration rather than on the fault in front of you is the wrong trade.
- **A "run behaviour" developer button, even hidden behind a debug flag.** Debug
  affordances become product affordances; the panel's hardware-test surface already
  covers the legitimate need, under the safety ladder, at the droid.

---

## D-017 — The panel is an instrument, and its wake frame is the top row drawn large

**Date:** 2026-08-24 · **Status:** accepted · **Operator ruling** ·
**Extends:** D-016 (three surfaces), `CLAUDE.md` character boundary

### Context

Five launcher designs were explored for the backpack panel. Two were eliminated by
the operator early (a long scroll and a permanent log tail). Of the three left, the
shotgun narrowed to two: a **status face** showing one thing with drill-in, and an
**instrument panel** showing four fixed rows.

Two things settled it, and neither existed when the options were drawn.

**1. D-016 moved settings and provisioning to the web app.** The single structural
objection to fixed rows was that four of them leave nowhere for configuration. That
objection is gone, and the panel gets to answer one question properly.

**2. There may be two batteries — and this argument is CONDITIONAL on that.**

> [!warning] **Corrected by review before landing. An UNKNOWN had graduated to a
> premise.**
> An earlier draft asserted two batteries as fact. `research/board-capabilities.md:430`
> records the opposite: *"how power is supplied (own battery via AXP2101, or tapped
> from R2) are entirely unaddressed"*, and this file's own open-items list still
> carries **"Backpack power and mounting. Entirely unaddressed."** The AXP2101 is a
> power-management IC and is present either way; it does not evidence a cell.
>
> `CLAUDE.md` requires every claim to be labelled OBSERVED / INFERRED / UNKNOWN and
> forbids an INFERRED claim silently graduating. This one graduated. It is relabelled
> here rather than quietly deleted, because the failure is more instructive than the
> fact.

**If** the backpack carries its own cell, then there are two batteries failing in
opposite directions — if his dies the panel still reports it, if the pack's dies the
panel dies with it — which makes the most common real question **comparative**, and a
one-thing-at-a-time face is structurally unable to answer a comparative question.

**If** the backpack is tapped from R2, there is one battery, the failure modes are not
opposite, and this argument contributes nothing.

**The decision does not depend on which it is**, which is why it still stands: reason
1 alone — settings leaving the panel — already removes the only structural objection to
fixed rows. What changes is that D-017 rests on **one** leg rather than two until the
power question is answered. The `PACK` row is provisional on the same condition; if it
turns out there is one battery, the row is dropped and storage returns to it.

**Tracked in #93**, because a condition with no resolution path stays conditional
forever and the `PACK` row keeps propagating as though it were settled. It is minutes
of work: look at the board for a cell, or read the PMU's battery registers on and off
USB.

### Decision

**The panel is the instrument: a fixed header plus four rows that never scroll and
never reorder.** Position is how you find things.

**Its wake frame is the most severe row, drawn large and untappable.** Not a separate
screen, not a summary, not a composed message — the same row, enlarged. The full rows
arrive on the second beat, once the operator has clearly decided to look.

Rows: **LINK · R2 · PACK · BRAIN**. Storage moves to the menu.
*(Superseded by Amendment A: there is no second battery, so the rows are
**LINK · R2 · STORAGE · BRAIN** and `PACK` is removed.)*

### Why the wake frame is an enlargement and not a face

The watch pattern is right and we take it: the first frame after a wake owes you the
condition and nothing else — no controls, nothing tappable. It is a safety property
here as much as a design one, because a glance that lands a thumb on a row is a glance
that can arm something which moves him.

But a *face* needs a priority function: something that decides what "most wrong"
means at runtime, is right every time, and gives the operator no way to see what it
discarded. **An enlargement needs only a static rank over a closed set**, which is a
different and much smaller thing.

**That rank is defined here, because it did not previously exist.** An earlier draft
of this ADR claimed "severity ordering already exists" — it does not.
`behaviour-states.md` gives every state a colour and a rate and never orders them
against each other, and a review caught the claim before this landed. The ordering is
therefore *introduced* by this decision rather than inherited by it, and saying so is
the difference between a justification and a rationalisation:

```
danger  >  offline  >  attention  >  misheard  >  waiting  >  thinking  >  listen  >  idle  >  sleep
```

Nine states, fixed at author time, no inputs. It is a constant, not a computation —
which is what keeps it on the enlargement side of the line. **If it ever needs to
consider anything at runtime — recency, how many rows are bad, what the operator was
last looking at — it has become the face this decision rejected, and that is a new
decision.**

The failure mode is therefore **predictable rather than clever**: with two things
wrong it still shows one, and it shows the higher-ranked one, every time, for a reason
anyone can read off the list.

### Consequences

- **Two of the states are already faces and stay that way.** Offline and danger throw
  the rows away entirely and show one condition with one action — with no link there
  are no readings, and four dashes would be theatre. This decision makes that the
  documented exception rather than an inconsistency.
- **No priority function gets built.** If one is ever wanted, it is a new decision with
  its own reversal case, not a quiet extension of this one.
- **The menu button lives in the header on every screen**, including the two that
  replace the rows. An escape hatch that disappears in the states you most need it is
  not an escape hatch. This is also the bootstrap route D-016 depends on: Wi-Fi cannot
  be configured from a web app that needs Wi-Fi.

  **This is D-016's own reversal condition firing, and it is named here rather than
  left implicit.** D-016 moved settings off the panel and listed as a reversal
  *"Wi-Fi provisioning that cannot bootstrap without a local UI"*. That is exactly
  what the menu is. It is a **scoped** reversal — Wi-Fi and pairing only, everything
  else stays in the browser — not a quiet re-opening of the panel to configuration.
- **Hold-to-arm survives only while it is labelled.** Apple retired Force Touch — an
  entire sensor — because the gesture was undiscoverable, and replaced it with visible
  controls. Ours is legible: an armed row prints `HOLD 1.2s` where its value would be,
  so the affordance occupies space it was already using. **If that label is ever
  dropped for tidiness, this becomes Force Touch and should be deleted.**

### Reversed by

- A priority function turning out to be needed for a real state we have not modelled —
  in which case the wake frame becomes a face and this decision is superseded, not
  amended.
- The wake measurement going badly. What wakes the screen is **unmeasured**: touch
  always works, but lift-to-wake assumes a device that only moves when a person moves
  it, and this one is strapped to something that drives itself. If wake turns out to be
  unreliable, a two-beat reveal may be one beat too many.

### Rejected

- **The status face as the default.** It wins the two questions asked most — *is he
  alright* and *what is wrong* — at zero taps. But those are the questions **the LED
  already answered** before anyone walked over. By the time the panel is being read at
  all, the question has become comparative, and that is the face's structural weakness.
- **No wake frame at all.** Simplest, and it loses the property the whole thing is for:
  a glance should not be able to arm an actuator.
- **Settings on the panel.** Three or four touch targets fit. Spending them on
  configuration rather than on the fault in front of you is the wrong trade — D-016.


### Amendment A — 2026-09-01: there is no second battery, so the PACK row goes

The condition this ADR was left hanging on is resolved. **There is no cell on
the backpack board.** #93 / gap B2, measured rather than inspected: the AXP2101
was asked directly, and its battery-present bit (`STATUS1` bit 3) is clear on
all ten samples while VBUS reads good, with battery detection verified enabled
(`BAT_DET_CTRL` read back `0x01`, and already on at boot). The voltage ADC
ranged **0 mV to 8183 mV** across those reads — a floating pin, not a cell. Evidence and the instrument
guard: `research/board-capabilities.md`, raw capture in
`firmware/pack_check/results/`.

**This ADR prescribed its own consequence, so it is applied rather than
re-argued:** *"if it turns out there is one battery, the row is dropped and
storage returns to it."*

**The rows are now LINK · R2 · STORAGE · BRAIN.** `PACK` is removed. It was
never a measured row — it was drawn in three published design artifacts on the
strength of an assumption, which is exactly what #93 was filed to stop.

**And the second leg of the argument above falls with it.** The "two batteries
fail in opposite directions, so the real question is comparative" reasoning
required a cell in the backpack. There is none on the board, so that argument
contributes nothing and should not be cited again. **D-017 stands on reason 1
alone** — settings leaving the panel removes the only structural objection to
fixed rows.

Scoped honestly: what was measured is that **no cell is attached to this board**,
USB-powered and not mounted. It does not prove the finished assembly can never
carry one, because the mount is not designed yet. If a mounting is later chosen
that adds a cell, the comparative argument and the `PACK` row both come back —
and that is a cheap thing to notice, because the row's absence will be
conspicuous the moment a second battery exists.

Worth keeping for the pattern: the corrected draft above was right to be
conditional, and right about which way to hedge. The failure it caught was an
UNKNOWN graduating to a premise; the fix was not to guess better but to file
#93 so the condition had somewhere to be resolved. It took one register read.

### Amendment B — 2026-09-06: the body table is normative; the panel renders views of it

**Status:** accepted · **Operator ruling** · resolves prerequisite **P3** of #101,
which gates that epic's child 5 wholesale.

#### The problem

Two normative documents had drifted, and #101 could not be built against the
disagreement:

| Panel spec / #101 | `behaviour-states.md:49` |
|---|---|
| `listening` | **`listen`** |
| `offline_net`, `offline_r2`, `offline_llm` | a single **`offline`** |
| — | **`wake`**, omitted by the panel list entirely |

Meanwhile the severity rank above is a **closed nine-state set with one
`offline`**, and D-023 added `released`, `waking` and `unprovisioned` outside it
by design. So "all states render per the severity rank" was unimplementable as
written: the rank does not cover the states the panel names.

#### The ruling

**`docs/behaviour-states.md` is the single source of truth for what states
exist. The panel renders VIEWS of those states and may not invent one.**

It follows that:

1. **The three `offline_*` are display modes of one canonical `offline`.** Not
   states. The droid is offline; the panel says *which thing* is unreachable.
   This is exactly the split `CLAUDE.md` already draws — the LED is glanceable
   and says *that* something is up, the screen is the detail view and says
   *what* — so "no route to R2" versus "no Wi-Fi" versus "no reasoning service"
   belongs on the screen precisely because it is detail. Three states would
   have put that distinction in the body, where nothing can render it.
2. **`wake` stays a behaviour state and is not a panel state.** The collision
   with D-023's `waking` is most of why this looked contradictory: `wake` is
   what he does, `waking` is what the panel shows while the link comes up. Two
   different things that were one word apart.
3. **`listen` is the name.** `listening` was the panel spec's own coinage.
4. **The severity rank is unchanged — still nine states, still one `offline`.**
   That matters: the rank stays a constant over a closed set, which is what
   keeps this on the enlargement side of the line drawn above. Ranking eleven
   states, three of them views, would have made it a computation over a
   membership that changes with the UI, and that is the *face* this ADR
   rejected.

#### Why this direction

The body model describes what the droid IS; the panel is a view of it. A view
that invents states is how the two drifted in the first place, and reversing
the dependency — expanding the rank to match a screen — would make the
character model answer to a rendering. The cheaper fix is also the one that
stops it recurring.

The rejected alternative worth naming: keeping both normative with an explicit
mapping table. It is more honest about the two layers genuinely differing, but
it is a third document to keep in sync, and this repo already has the scar —
D-012 and `r2-capabilities.md` were each individually honest and jointly
misleading, which cost a design built on an LED fade the fixtures cannot render.

#### Consequences

- #101's child 5 is unblocked and its AC3 can stop being conditional. It must
  render the states `behaviour-states.md` defines, with `offline` free to
  present in three ways.
- `behaviour-states.md` gains a short section recording that the panel may
  split a state into several views, so the next reader does not re-derive this.
- **This is a documentation ruling with no code behind it yet.** Nothing
  currently renders any of these states on the panel — child 5 is unstarted.
  The reconciliation is real; the implementation is not, and the two should not
  be confused when this is cited.

---

## D-018 — He always answers. What changes is how, never whether

**Date:** 2026-08-25 · **Status:** accepted · **Operator ruling** ·
**Supersedes:** the unruled "can he refuse?" proposal · **Implemented by:** #85

### Context

A rule was needed for the question every future feature raises: does this make R2 feel
like a creature, or like an appliance? Three candidates were put up — *can he refuse*,
*does it fire every time*, and *no rule*. All three were wrong, and the first two were
wrong in the same way: **they made unreliability the marker of character.**

That was my framing and it does not survive contact with the evidence this project
already gathered. The habituation evidence recorded in
`Plan/Product Frame.md` (§HMW) found **predictability** drives trust and bonding,
while unpredictable behaviour suppressed both. That was recorded, an HMW
asking for unpredictability was struck because of it, and then the same instinct came
straight back as a proposed *rule*. A droid that might ignore you is not mysterious.
He is broken, and you cannot tell the difference from outside.

### Decision

**R2 always responds. The variation is in HOW he responds, never in WHETHER.**

Which gives the test its real axis, and it is not determinism:

> **An appliance's response is a function of the trigger alone.
> A creature's response is a function of the trigger AND its own state.**

A feature is **character** if his mood, energy, boredom, recent history or posture
change what comes back. It is a **gadget** if the same trigger produces the same output
regardless of how he has been.

### Worked cases

| Feature | Verdict | Why |
|---|---|---|
| Touch → delight | **character** | Always fires. Magnitude depends on how starved he was (#85) |
| "Hey R2" wake word | **character** | Always answers. Asleep he is slow and grumpy; mid-something he finishes first |
| Walk past → he looks up | **character** | Always notices. How much depends on how long he has been alone |
| Door sensor → perk up, identically | **gadget** | Output is a pure function of the door |
| 3pm dentist reminder | **gadget** | Must fire identically regardless of mood — that is what a reminder *is* |
| 7am wake chirp | **character**, if it varies | Same clock trigger; grumpy before coffee, brighter later |

Note what the axis does that the rejected ones could not: it **allows a wake word**,
which "nothing fires every time" banned outright, and it **blocks a reminder**, which
"can he refuse" let through because a delayed action is not obviously obedience.

### How mood shows up: it picks the flavour, never the presence

Operator, sharpening this the moment it was written: *"the thing that makes him
interesting isn't whether he responds — it's HOW he responds. If in low mood he can
use a grumpy sound, but the response is still required."*

So mood is a **selector over renderings of a mandatory response**, not a gate on
whether one happens:

```
trigger  ->  response IS REQUIRED  ->  mood picks which rendering
                                        happy   -> R2_LAUGH_*, holo up, full gesture
                                        neutral -> shorter acknowledgement
                                        grumpy  -> a put-upon variant, still an answer
```

**The binding rule is just this: he always reacts, and the response varies with his
state.** Nothing more is required.

**#85 has landed and it does NOT yet satisfy the rule.** `express_delight` takes an
intensity read from the mood *before* the touch is applied, and `react()` passes it —
the shape is right. But #86 merged (`db59f0a`) with the feature **inert on the live
path**, and it is inert on `main` as this is written: `r2_reactive.py:796` builds
`Reactive(...)` without `mood=` or `beat_factory=`, so the defaults win and nothing
calls any of it. `:714` still computes the required tier from `express_curious`, the
wrong behaviour.

A review posted on #86 named both before it merged. They were not fixed. That makes
this the **second** time the project has shipped a correct, tested, reviewed layer that
nothing calls — #83 wrote the rule after the first one, from the LED epic, and its line
holds: *correctness does not detect deadness.*

So the rule stated in this ADR currently has **zero** working implementations, and the
nearest one is three lines away. Tracked in #97.

**Distinct willing/grumpy renderings are DEFERRED to #92**, not adopted here. They
would put a two-rendering cost on every behaviour authored from now on, and the
library is small enough that the tax would exceed the benefit. Intensity variation
carries the rule today; richer performance is worth doing when a playtest shows it is
needed, and D-018's own reversal clause already notes that whether state variation is
*perceptible* has never been tested.

One constraint recorded now because it will be forgotten later: **"grumpy" will be
composed, not an authored animation.** Authored animations are
  `FORBIDDEN_OPS` — they drive leg actions whose contents cannot be inspected before
  sending, and `EMOTE_YES`, a *nod*, put him on the floor (D-010). A grumpy rendering
  is a sound family, a slower or smaller dome gesture above the 12° floor, and the
  brightness channels — the same vocabulary as the willing one, performed differently.

The failure mode this forecloses is the one that would otherwise creep in: a low mood
quietly becoming a reason not to answer. It is not. It is a reason to answer *badly*,
which is a completely different and much more characterful thing.

### Consequences

- **Every feature needs his state consulted before it responds.** #85's happiness
  scalar is the first instance, and it is enough. The rest of the mood model arrives
  as behaviours need it — not before.
- **"He might not bother" is retired as a design device.** Anywhere that phrasing
  appears — the northstar's walk-past scene, the six-moments comparison — it is now
  wrong and needs correcting.
- **Reliability is not the enemy of character; state-independence is.** A droid can be
  perfectly dependable and still feel alive, which is a much easier product to live
  with and a much easier one to debug.
- **The rule is mechanical enough to end arguments**, which the earlier candidates were
  not: ask whether the output could differ tomorrow given identical input. If not, it is
  a gadget.

### Reversed by

- A behaviour that plainly reads as alive while being a pure function of its trigger,
  which would mean the axis is wrong rather than the behaviour.
- **Nothing implementing the rule ever landing.** The rule currently has zero shipped
  instances. If it is still at zero when the next few behaviours are authored, it is
  governance without practice and should be deleted rather than kept as decoration.
- Finding that state-dependence is imperceptible in practice — that the variation is
  real in the state store and invisible in the room. That is a playtest, and it has not
  been run.

### Rejected

- **"Can he refuse?"** — makes ignoring you a feature. Contradicts the habituation
  evidence, and an unresponsive droid is indistinguishable from a broken one.
- **"Nothing that fires every time."** — bans a wake word, since being deterministic is
  what a wake word is for, while catching nothing this rule misses.
- **No rule.** The argument then gets relitigated per feature, which is what writing it
  down was for.

---

## D-019 — The spoken voice is R2's, not a second character's
**2026-08-27** · *operator ruling* · **amends D-011, supersedes the framing in #47**

**Decision.** The synthesised voice speaks **as R2, in the first person**. It is
him rendered into speech by a protocol droid, not a companion standing beside
him. Asked its name, it answers R2-D2. It never refers to him in the third
person.

Operator ruling, verbatim: *"c3po is voice of r2d2, not next to r2d2."*

### What this reverses

#47 built `speak()` on the opposite premise, and said so explicitly: C-3PO as a
**second character** who rides in the backpack and does the talking, with the
argument that his voice *should* sound displaced from R2's body **because it
belongs to somebody else**. That framing was load-bearing — it is what let
D-011 treat the speaker-displacement question as dissolved rather than
answered, and it is why Amazon's Astro precedent (a non-verbal body with speech
demoted to a visibly separate character) read as supporting evidence.

It no longer applies. If the voice is R2's own, then sound emitted a few
centimetres behind and above his dome is **his** voice coming from not-quite
his mouth, and the displacement question D-011 set aside is open again.

### What survives from D-011

The decision that the backpack speaker *may* carry character audio stands, and
so do its three constraints: R2's 212 native ids stay primary, authored audio is
co-timed with dome motion, and the screen boundary is untouched.

The co-timing constraint gets **more** load-bearing, not less. D-011 called a
still dome during backpack audio "the configuration most likely to break the
illusion" when the voice belonged to someone else. Now that it is R2's own
voice, a still dome is the configuration that makes it sound like a speaker
taped to a robot.

### What is now UNKNOWN again

- **Whether a first-person voice survives the displacement.** D-011's evidence
  ran both ways and was settled by a taste judgement made about a *second*
  character. That judgement does not transfer, and it has not been re-made
  against the new framing. No playtest has been run since the change.
- **Whether the `ballad` voice still fits.** It was auditioned across all
  thirteen candidates against the OLD steering prompt (#66). The character
  brief has changed; the audition is stale by the rule recorded with it. The
  full set is retained at
  `R2Z2-vault/.../Experiments/data/threepio-audition-20260818/` so
  re-judging costs nothing.
- **Whether "C-3PO" is still the right description at all.** The prompt
  deliberately names no performer, and the model volunteers "C-3PO" from its
  own knowledge. Under the old framing that was a character; under this one it
  is a *register*, and the naming may want revisiting.

### Why this is recorded rather than left in the prompt

The change reached production as two edited strings in `voice/speak.py` during
a live demo. Nothing else in the repo knew. A future session reading D-011 and
#47 would have found the two-character premise stated as settled, in two
places, and rebuilt it — the same failure this repo has already recorded as
*"a doc can go wrong without being edited."*

*Reversed by:* a playtest where the first-person voice reads as ventriloquism
rather than as him — the cheap version is the same line delivered first person
and third person, back to back, to someone who has not seen this file.

---

## D-020 — The voice is gated on R2 being reachable
**2026-08-28** · *operator ruling* · **follows D-019** · implements the gate in
`voice/speak.py`

**Decision.** Synthesised speech is **refused unless R2 is reachable**. The gate
lives inside `speak()`, next to quiet hours, and defaults to refusing. The only
override is `--audition`, the same escape hatch quiet hours already has.

### What forced it

OBSERVED 2026-08-27: the voice spoke **three times, unprompted, with R2 powered
down and no animation**. Reported by the operator.

Operator ruling: the voice must be gated on R2 readiness.

Under D-019 the voice is **his**, in the first person. A first-person line with
no droid behind it is not a degraded feature, it is a different and worse one —
a disembodied voice in a room with nothing visible producing it.

### What this reverses

An earlier session decoupled the speech path from BLE **deliberately**, and
reported it as a feature: *"only Threepio was still working, because he needs no
robot."* That was the right instinct for keeping the module severable and the
wrong outcome for the household. It is reversed.

### Why the gate is in `speak()` and not in `converse.py`

The same reason quiet hours is (`speak.py`, "QUIET HOURS ARE ENFORCED HERE"):
a guard held by a caller is not a guard. `FORBIDDEN_OPS` was once enforced on
the construction path and every caller that skipped construction skipped the
check. Sound is the output that reaches a household through a closed door, so
both gates sit where the sound is made.

`Embodiment.present` therefore defaults to **False**. The asymmetry is the
point: assuming presence is wrong in the direction that reaches a household,
assuming absence is wrong only in the direction of silence.

**Say what this is, precisely: a safe DEFAULT, not an unbypassable guard.**
`speak()` has no bridge and cannot see BLE, so it takes the caller's word via
`Embodiment(present=...)`. A caller can still pass `present=True` and make
noise — exactly as it can pass `QuietHours(enabled=False)`. What the placement
buys is that a caller who passes *nothing* is refused. An earlier draft of
this ADR called it a guard outright; a cross-model review was right that the
word claims more than the code does.

### Readiness is asked per turn, not per run

`converse.py` resolves `bridge` once at startup and never revisits it, so
`bridge is not None` only means a daemon held the bridge **when the run
started**. `send_r2` and `send_animation` already re-check liveness before
sending; the speech path did not check at all. `r2_is_present()` asks the same
question the same way — including the `hasattr(bridge, "daemon")` guard, so a
`FakeBridge` in a dry test is not read as a dead droid.

### The readiness signal was wrong. FIXED — the daemon now records the link

**MEASURED 2026-08-28, in review, before this ever ran in the house.**
`daemon.lock` proves a daemon PROCESS is alive. It does not prove R2 is
CONNECTED, and those come apart by design:

- `acquire_daemon_lock()` is called at `r2_probe.py:1641`, **before**
  `_run_daemon` reaches the BLE scan at `r2_probe.py:1690`.
- The lock record is `{"pid", "ceiling", "started"}` — nothing about the link.
- The scan's `--timeout` defaults to **10.0 s**, and the lock is released only
  by `cmd_daemon`'s `finally`, i.e. AFTER a failed scan returns.

So with R2 powered off, there is a window of at least ten seconds in which the
lock exists, `r2_is_present()` returns True, and the voice speaks anyway — the
exact reported symptom. Worse, `talk.command:41-45` polls for that lock file,
breaks the moment it appears, sleeps 2 s and launches `converse.py`, which
puts the primary user path INSIDE the window.

**The fix, operator ruling 2026-08-28: the daemon records it.**
`mark_daemon_connected()` rewrites the lock with `connected: true` — and does
it only after `await r2.wake()` returns, i.e. after the link is genuinely up.
`r2_is_present()` now requires that flag, so holding the lock is no longer
enough. The rewrite goes through `os.replace`, because a torn read parses as
"no daemon" and would cut the voice off mid-session for no reason, and it
preserves `pid`, because `release_daemon_lock` refuses to drop a lock that is
not its own.

**MEASURED ON HARDWARE 2026-08-29, first run after the fix landed.** The
window is not a reading of the source, it was watched live:

```
09:34:00  lock taken   {"pid": 59098, "ceiling": "dome", "started": ...}   <- no "connected"
09:34:12  connected    {..., "connected": true, "droid": "D2-6F6B"}
          BLIND WINDOW 11.95 s
```

At t+5 s the lock existed and carried no `connected` field, which is exactly
the state the old check read as "R2 is present". `talk.command:41-45` polls for
that file every second, breaks on first sight and sleeps 2 s, so it would have
launched `converse.py` at roughly t+3 s — **about nine seconds inside the
window**. The 10 s figure inferred from `--timeout` was close; the measured
value is ~12 s because the scan is followed by connect and `wake()`.

> [!warning]
> **A daemon already running when this lands writes no `connected` field, so
> the voice stays silent until it is restarted.** Safe direction, but it looks
> exactly like a regression if you meet it unprepared. Only the operator can
> restart the daemon (macOS gives Bluetooth to the responsible process), so
> this is a step in the upgrade, not something the code can paper over. It was
> a deliberate choice to fail closed rather than treat a missing field as
> "old daemon, assume connected" — that reading would preserve the exact hole
> being closed.

Two mutations survived the first battery and both were this shape: deleting
the call site, and marking connected BEFORE `wake()`. Neither could be caught
by a behavioural test, because `_run_daemon` needs a real droid — so the call
site is pinned by a source-order assertion instead. Weak, and stated as weak;
it is the only thing that fails when someone deletes the call, and a weak test
on a live path beats a strong one on a dead layer.

### `--send none` makes the loop mute, and on reflection that is CORRECT

`--send` defaults to `"none"`, `open_bridge("none")` returns `None`, so a bare
`python voice/converse.py` refuses every line. VERIFIED by execution, not read.
`open_bridge` collapses two different states into `None` — *the operator opted
out of driving R2* and *R2 is unreachable*. The first reading suggested this
was a defect to fix by letting `--send none` speak. It is not: with no bridge
at all there is no way to ask whether he is connected, so speaking would be
speaking blind, which is the thing D-019 forbids. It stays mute, and the
startup banner now names which of the two it is and what to pass. The cost is
real — the bare `python voice/converse.py` dev loop is silent unless you pass
`--send` or `--audition` — and it is accepted knowingly.

`talk.command` is unaffected; it passes `CEILING=stance`.

### What this does NOT fix

- **The false-accept rate is still UNKNOWN.** This gate stops the *symptom*;
  the *frequency* is unmeasured. #42 AC3 (a 30-minute ambient sample) has been
  deferred twice and never run. The `r2d2` model publishes **4.69 false
  accepts/hour**, so three in a day is well inside expected — the gate must not
  be read as evidence the wake word improved.
- **A false wake while R2 IS up still produces a full reply to nothing** —
  animation, chirp and speech. `misheard` already exists in `r2_lights.STATES`
  and is the honest signal. Not done here.

### Evidence

5 mutations, 0 survivors: flipping the default to present, deleting the gate,
hardcoding presence at the caller, and reading both an absent bridge and a dead
daemon as present each turn the suite red. 611 tests pass.

**That first battery proved the gate was load-bearing and NOT that it asked
the right question** — all five mutations took `daemon.lock` as ground truth
for "R2 is here", the very assumption that turned out to be false. A green
suite around a wrong premise is the sibling of *a guard on the construction
path is not a guard*, one layer up, and it is why the review caught this and
the tests did not.

After the fix: **6 mutations, 0 survivors** — reverting to the old
lock-only check, writing `connected` as False, deleting the ownership check,
dropping the pid on rewrite, deleting the call site, and moving the mark
before `wake()`. **620 tests pass.** Still Mac-prototype only, still no CI,
and the daemon path itself is unexercised by any test because it needs a
droid: the first real evidence will be the operator restarting the daemon and
watching the voice stay silent until R2 answers.

*Reversed by:* a decision that the voice is a companion rather than R2 himself,
which would reopen D-019 first.

---

## D-021 — The idle state of the backpack screen is ours
**2026-08-30** · *operator ruling* · **amends the scope of D-017**

**Decision.** R2Z2 owns what the backpack screen shows **at rest**, not merely
what it shows when summoned. The host OS's own resting face is replaced, not
coexisted with.

Operator ruling: *"we need to own idle."*

### What forced it

The board's factory firmware is **ESP-Brookesia**, and its idle state is an
animated emoji face. OBSERVED in the factory image, which carries an
`emoji_collection` of six expressions:

```
neutral.png · happy.png · sad.png · angry.png · surprised.png · sleepy.png
```

Brookesia calls this **AI Expression** (`brookesia_expression_emote`) —
"expression switching and animation playback control". It is a first-class
feature of the framework, aimed at exactly this class of device.

### Why this is a character decision, not a layout one

**Whatever owns idle owns the character.** The resting state is what the
household sees by default, without asking for anything. If we ship as an app
that must be launched, then the droid's backpack spends almost all of its life
showing a *generic assistant's* emoji face — a second character, louder than
ours because it is always there, and not ours.

That is worse than either alternative. Two characters on one droid is a failure
the whole character boundary exists to prevent.

### What this amends in D-017

D-017 ruled the panel is an instrument and never a face. That was written about
**our own rendering**, and it did not anticipate a host OS drawing a face of its
own on the same glass. The constraint is hereby scoped to **the device's
screen**, not to our frames within it:

> No face is rendered on the backpack screen. Not ours, and not the host's.

The instrument stays the instrument. The resting frame is the STATUS face from
the Panel Spec — state word, power, dome — not a character.

### Consequences, and they bind

- We **cannot ship as a passive app** on the vendor image. Owning idle requires
  either suppressing AI Expression, replacing the shell, or booting our own
  image.
- The resting frame becomes the most-seen surface in the project. It should be
  designed as the thing people glance at for months, not as a fallback.
- The LED keeps its job unchanged (D-012): it carries what is worth noticing
  from across the room. The idle screen is still the detail view, read up close.

### What is UNKNOWN, and gates the how

This ADR records the **requirement**, not a proven mechanism. None of the
following is established:

- Whether AI Expression can be suppressed or replaced without forking Brookesia.
- Whether the OS reserves screen area (`navigation_bar`, `Recents` both appear
  in the image) that would constrain the idle frame.
- Whether the board can hold a BLE link to R2 at all under this OS — the factory
  image ships **no BLE stack** (`esp_wifi` yes, `nimble`/`bluedroid` absent),
  which is the coexistence risk D-005 named as its own reversal condition.

An OS integration study answers these before any implementation is specced.

### Amendment A — 2026-08-30, same day: the premise above is MISATTRIBUTED

Partition forensics run hours after this ADR was filed show the emoji face does
**not** belong to ESP-Brookesia. The board carries **two complete applications**:

```
factory  @0x110000  esp-brookesia  v1      built 2026-05-27  IDF v5.5.4
ota_0    @0x690000  xiaozhi        v2.2.6  built 2026-05-26  IDF v5.5.4
```

String counts across the two images, `brookesia` vs `xiaozhi`:

```
emoji_collection    0 / 1      WakeNet   0 / 8      afe_   0 / 50
MultiNet            0 / 4      Opus      0 / 46
```

The face, and the entire speech stack with it, are **xiaozhi's**. And
`otadata` is fully erased, so the bootloader falls through to `factory`:
**the device boots the Brookesia launcher, and xiaozhi does not run by default.**

**What that does to the argument.** This ADR justified owning idle by saying a
rival *character* owns the screen at rest. That is not what happens. At rest the
device shows a **launcher home screen** — neutral chrome, not a competing
character. The decision may still be right, but the reason given for it above is
not the reason.

Two claims made elsewhere on the strength of the original premise are also wrong
and are withdrawn here:

- *"Building on Brookesia gives us an on-device wake word."* It does not. The
  speech stack is xiaozhi's, in a different application.
- *"The OS's idle state is an animated emoji face."* It is the launcher.

**Status: the ruling stands as recorded, the rationale does not.** Owning idle is
still defensible — a launcher grid is not what a droid's back should show, and
D-017 still wants an instrument there. But that is a weaker and different claim
than the one filed, and it deserves the operator's re-confirmation rather than a
quiet rewrite by the author of the mistake.

### Amendment B — 2026-08-30: re-confirmed on the corrected premise

The operator re-confirmed after reading Amendment A. **The decision holds.**

So the record should be read as: we own idle **not** because a rival character
was there, but because a launcher grid is the wrong resting face for a droid,
and because D-017 already wants an instrument on that glass. The original
justification was wrong on the facts; the ruling survives on a narrower and more
honest one.

This makes the burn-in question urgent rather than academic. Owning idle means a
**static instrument panel lit for months** on an OLED, and nothing in this
project has ever considered image retention. That is now the first design
constraint on the resting frame, not an afterthought — see the gap register on
#101 (E1).

*Reversed by:* the study showing idle cannot be owned without forking the
framework, at a cost the operator judges worse than living with the host's
launcher — or by the operator deciding a neutral launcher at rest is acceptable,
which the corrected premise makes a much more reasonable position than it looked
when this was filed.

---

## D-022 — We ship our own partition table; the dual-image option is dead on arithmetic
**2026-09-01**

Closes gap **B4** of #101, which had been deferred repeatedly as a taste call.
It is not one. The numbers decide it.

**Decision: replace the stock partition layout with our own table.** Do not ship
our panel as a second OTA image living beside the stock launcher.

### The stock layout, measured from the verified backup

Read out of `~/esp/r2z2-board-backups/factory-backup.bin`, whose sha256 was
re-checked against its sidecar on 2026-09-01 and matches. **That file is not in
this repo** — it is a 16 MB local artifact — so every figure in this table is
reproducible only on a machine holding it. What the repo itself supports is that
the backup exists and restored to a working system
(`research/board-capabilities.md`), and that section is explicit that the
restore was hash-checked *before* writing and never read back and re-hashed
afterwards. Treat the table as measured-locally, not as committed evidence:

| Partition | Size | Used | What it holds |
|---|---|---|---|
| `nvsfactory` | 0.20 MB | **0.00 MB** | empty |
| `nvs` | 0.82 MB | **0.00 MB** | empty |
| `otadata` | 0.01 MB | erased | so the bootloader falls through to `factory` |
| `factory` | 5.50 MB | 4.46 MB | esp-brookesia v1 — the launcher that boots |
| `ota_0` | 3.00 MB | 2.80 MB | xiaozhi v2.2.6 — **never boots** |
| `assets` | 3.00 MB | 2.72 MB | stock app assets |
| `storage` | 3.44 MB | 3.43 MB | stock app data |

### Why dual-image is dead

`ota_0` is **3.00 MB and already 93.5% full**. Our panel app has to carry
ESP-Brookesia *plus* NimBLE *plus* Wi-Fi. Two measurements bound that:

- Espressif's own shipping Brookesia build, with no BLE stack at all, is
  **4.46 MB** — it is the `factory` image above.
- `firmware/coex_check` — NimBLE central, Wi-Fi STA and an HTTP client, with
  **no display stack whatsoever** — was **1.04 MB** (#103 A1).

Those two are **not additive** — the `factory` image carries vendor apps we
would not ship, and `coex_check` duplicates IDF runtime that a combined build
shares once. So this is a strong bound, not a proof: a Brookesia panel carrying
both radios is very unlikely to fit in 3.00 MB, and **no combined build exists
that shows otherwise.** Calling it arithmetically impossible would overstate it.
What can be said flatly is that the option was never weighed against a measured
size in the year it sat open, and the two bounds available make it the losing
side of the trade rather than a close call.

### The framing was also wrong, which is why it kept sliding

"Replace `factory` **or** dual-image into `ota_0`" assumes we keep the stock
partition table. **We never have.** Every experiment in #103 flashed its own
table — `touch_check` and `board_check` on the IDF default, `coex_check` on a
3 MB custom table, `brookesia_check` and `tap_target_check` on the vendor
example's 8 MB + 4 MB layout. The stock table has been overwritten many times
over; it survives only in the backup. So the real question was never which stock
slot to occupy, but what our own table should look like.

### What this gains and costs

**Gains 9.44 MB** currently committed to things we do not use: `ota_0` (3.00,
an image that never boots), `assets` (3.00) and `storage` (3.44), both holding
stock-app data. Sizing is then ours to choose rather than inherited.

**Costs the stock launcher on-device.** After the next flash the Brookesia
launcher and its apps exist only in the backup. The operator assessed those apps
and judged only MusicPlayer interesting, for a visualisation resembling our own
`thinking` state. **D-021** already commits us to owning idle, so a bootable
stock launcher was of limited value even before the size argument.

Both NVS partitions read **0.00 MB used**, so no provisioning or calibration
data is lost with them.

**Rollback is the full 16 MB image**, restored successfully on 2026-08-29 with
the stock UI confirmed by eye, and re-verified by hash today.

### The replacement table

Deciding to own the layout is not a decision until the layout exists. The
contract it must satisfy, and an initial table meeting it:

| Partition | Type | Size | Why |
|---|---|---|---|
| `nvs` | data/nvs | 64 KB | settings and Wi-Fi credentials. The stock `nvs` was 0.82 MB and **empty**, so nothing is inherited and the size is ours to pick |
| `otadata` | data/ota | 8 KB | selects the active slot |
| `phy_init` | data/phy | 4 KB | RF calibration |
| `ota_0` | app | 6 MB | the panel app. Stock Brookesia alone is 4.46 MB and ours adds two radios |
| `ota_1` | app | 6 MB | **reserved**, so a bad update can roll back |
| `storage` | data/spiffs | ~3.4 MB | our own assets, sized from what we ship rather than inherited |

Two constraints worth stating as constraints, not sizes:

- **The app slot is at least 6 MB.** 4.46 MB is what a Brookesia build with *no*
  BLE already costs. Sizing to today's binary would guarantee a repartition.
- **The second app slot is reserved even though OTA is not implemented.**
  Reserving costs nothing now; adding it later means repartitioning, which means
  a full erase and reflash of a device that by then lives in a household rather
  than on the desk. Implementing OTA is explicitly **not** in scope here — only
  leaving room for it is.

It fits, with room: **15.47 MB of the 15.97 MB** available after the bootloader
and table (partitions start at `0x9000`), leaving 0.49 MB spare. Checked rather
than assumed — a published layout that overflows would be a poor way to close a
decision that turned on arithmetic.

This table is a starting point, not a further decision: it can move freely until
the first non-experimental flash, and nothing above depends on the exact sizes.

### Stated uncertainty

**Our final app size has not been measured.** No build combining Brookesia, BLE
and Wi-Fi exists yet; the 3 MB verdict rests on the two bounds above rather than
on our own binary. If a future panel drops Brookesia for a lean LVGL app, the
size argument weakens — but so does dual-image's only attraction, since what it
preserves is the Brookesia launcher.

*Reversed by:* a release build of the panel, with every shipping feature
compiled in, measuring **≤ 2.70 MB** (3.00 MB less the ~10% headroom the stock
images leave) **and** a named requirement that needs the stock launcher
bootable. Both are checkable: the first is `ls -l build/*.bin`, the second has
to be written down as a requirement rather than felt. Absent both, this stands.

---

## D-023 — Powering him down is us stopping, so the panel offers RELEASE, not OFF
**2026-09-01**

Closes the hard half of gap **C1** in #101: what *"power everything except the
backpack down"* actually means. It had stayed open because it looked like a
missing feature. It is not — it is a **missing subtraction**.

### The constraint, restated until it stops being awkward

R2 has no off switch, and **he has his own idle sleep** — OBSERVED 2026-08-18,
the first time the keepalive was ever stopped and he was watched. He reverted to
his own resting alternation and faded out, on the charger at full battery.

Our keepalive **is** the wake command (`DID 0x13 / CID 0x0D`, every ~3 s), so
any `sleep` we send is undone within three seconds (#38). The reason he never
sleeps during a session is not firmware. **It is us, every three seconds.**

So the action is not a command. **To power him down we stop.** The
implementation of the panel's most physical-feeling control is an absence.

### Three consequences, and the third is the interesting one

**1. It is a release, not an off.** No switch exists and we cannot promise an
instant. The panel must not say `OFF`, because that names a state we do not
control and cannot deliver on demand.

**2. It is not instantaneous, and by an unmeasured amount.** After we stop, he
holds whatever we last set, reverts to his own idiom, and fades out after an
interval **nobody has measured** — the only bound is a useless 22.1 h because
nobody was watching in between. So at the moment of release the panel cannot
honestly say he is asleep.

**3. Confirming he slept wakes him — but weaker evidence is free.** To *confirm*
his state we must reconnect, and connecting sends `wake`, so the confirmation
destroys the thing confirmed. A first draft of this ADR stopped there and
declared the state unobservable. That is too absolute: **a passive BLE scan
takes no connection** (`r2_probe.py:10`), and a droid that has gone under should
stop advertising. So absence-of-advertisement is real, non-destructive evidence.

What it cannot do is *discriminate*. Not advertising is equally consistent with
asleep, out of range, or failed — and telling those apart is exactly what needs
a connection. So the honest position is narrower than "unobservable" and still
rules out the display we wanted to avoid:

**The panel shows `released`, a claim about what WE did, not about what he is.**
It is knowable, it stays true regardless of where he is in his own timeout, and
it never has to be retracted. A passive scan may *corroborate* it — "released,
not advertising" is a stronger line than "released" — but the panel must never
promote that to `asleep`, because the same observation is what a failure looks
like. Same rule the LED language already runs on: state what you can vouch for.

### This unblocks the `sleep` row without adding a command

`behaviour-states.md` carries **sleep** as *"specified and blocked"* — blocked
because a sleep command is undone by the next keepalive. **The reasoning that
blocked it is retired here** — though the code prohibition stays until there is
a release path to assert it from (see below). And it is retired without finding
a command:

**Assert `sleep` (blue steady @ 0.2) as the last write, then stop the
keepalive.** He holds that dim blue while he is awake-and-released, and
his own idiom returns once he goes under. A colour we set survives a link drop;
it did **not** survive the one sleep cycle anybody watched — though `CLAUDE.md`
is careful that **sleep is the likely destroyer, not the proven one**, since
that 22.1 h window also contains duration. This ADR inherits that caveat rather
than rounding it up: the goodnight is *expected* to lapse when he sleeps, and
nothing here breaks if it lapses for a different reason.

The blocked row unblocks by **removing** a command, not adding one. That is the
same shape as the decision above, and it is why this gap resisted being specced
as a feature.

**This is a semantic change to `sleep`, and it should be named rather than
smuggled.** D-012 has status colours as claims about the system that can be
verified. Asserted at release, `sleep` is shown while he is still *awake* — so
it stops being "he is asleep" and becomes **"we have let go"**, a goodnight
marker. That is deliberate and it is the body's honest counterpart to the
panel's `released`: both state what we did, neither claims what he is. It does
mean `sleep` is the one status row whose name no longer describes its
condition, and renaming it to `released` on the body is a reasonable follow-up
this ADR does not take.

**What this does NOT do is unblock the code.** `sleep` remains refused by
`r2_lights.BLOCKED` (`r2_lights.py:466`) and by `StatusLayer.set()`
(`r2_status.py:207-210`), with two tests pinning it. That is correct for now:
the release path does not exist, and a state you can enter but never leave is
worse than one you cannot enter. This ADR decides *what release means*; the
prohibition comes out in the same change that builds it. Saying "unblocked"
while the code still blocks it would be exactly the doc-versus-repo drift this
project keeps catching.

### The panel needs three states it does not have

| State | What it means | Why it cannot be folded into an existing one |
|---|---|---|
| `released` | we have stopped holding him awake | distinct from `offline`: nothing failed, and it is not `sleep`, which is a claim about him |
| `waking` | reconnect in progress, **~12 s on the Mac daemon path** | measured there (scan + connect + `wake()`, the 11.95 s blind window). Without it the panel is indistinguishable from broken for twelve seconds |
| `unprovisioned` | first run: no network, no droid paired | `offline` says a known link died; this says there was never a link to lose |

`waking` should show **progress**, not a spinner — twelve seconds is long enough
that an unbounded animation reads as a hang. But note the figure is **borrowed**:
11.95 s was measured on the Mac daemon's scan-connect-wake path, and the panel's
own reconnect has never been timed. Use it to justify *having* a progress state,
not to calibrate the bar; re-measure on the panel before any duration is drawn
to scale.

**None of the three enters D-017's severity ordering**, and that ordering stays
the closed nine-state constant it was declared to be. The wake frame exists to
surface *severity* on a running system; `released` is deliberate rather than
wrong, `waking` is a transition that resolves itself in seconds, and
`unprovisioned` is a mode the panel is persistently in rather than an event to
be woken by. Adding any of them to the rank would mean the panel could wake the
household to announce that it is doing what it was told.

### The two-axis model this exposes

C1 asked about *"everything except the backpack"*, which reads as one control.
It is a point in a two-axis space, and naming the axes is most of the work:

| | backpack display | R2 |
|---|---|---|
| axis | `resting` · `UI asleep` · `off` | `held` · `released` · `waking` |

*"Power everything except the backpack down"* is **R2 `released` × backpack
`resting` or `UI asleep`**. The reason it felt like one switch is that only one
of the two axes had ever been named.

### Decision

**The panel offers a `RELEASE` action whose implementation is stopping the
keepalive**, preceded by asserting `sleep` on the body as the goodnight. It
reports `released`, never `asleep`. It gains `released`, `waking` and
`unprovisioned` as first-class states, and `waking` shows bounded progress. The
duration it is drawn against must be **measured on the panel's own reconnect**;
the ~12 s from the Mac daemon path justifies having the state, not the length of
its bar.

### ~~What is deliberately not decided here~~ — DECIDED 2026-09-07

Both open items are now ruled, by the operator.

- **The word is `GOODNIGHT`.** `RELEASE` was this ADR's placeholder and a
  structural claim, not a CX one. `GOODNIGHT` is honest about what actually
  happens — he goes to sleep, he is not switched off — and it suits a droid
  that is meant to feel continuously present rather than operated. It is also
  the only one of the four candidates that says the true thing: this ADR's
  whole finding is that there is no off, only us stopping.
- **No confirm.** The action is not dangerous, and the cost of coming back is
  now measured rather than assumed: **P2 timed the panel's own reconnect at
  2600-3101 ms** (#142), not the ~12 s guessed from the Mac path. A confirm on
  a harmless, quickly-reversible action teaches people to dismiss confirms,
  which is a real cost paid on the day one guards something that matters.

  The counter-argument was recorded and rejected knowingly: the vault warns
  that a gesture on a 29 mm panel held one-handed while steadying the robot is
  easy to fire by accident. That is an argument for the control being hard to
  hit, not for a dialogue after it.

### The measurement this now depends on

**His idle timeout is PARTLY measured now (2026-09-07), and the answer is
awkward.** A 67-minute scan with nothing connected never saw him stop
advertising, but the operator observed him back in his own red/blue
alternation — and a colour we set survives a link drop and NOT a sleep cycle,
so he had slept. **He sleeps while continuing to advertise**, which means no
radio-side signal marks the transition and `released` cannot converge on
`asleep` by watching the air. Evidence: `firmware/panel/results/p4-idle-2026-09-06.txt`.

The original wording of this section follows, and its concern still stands
because the TIMING remains unknown -- only the mechanism is now understood.

**His idle timeout is unmeasured, and how honest `released` feels depends on
it.** If it is five minutes, `released` converges on asleep quickly and the
distinction is academic. If it is hours, `released` is a long limbo and the
panel is carrying a genuinely uncertain state for most of a day. `CLAUDE.md`
already records this as cheap and unmeasured: stop the keepalive, then look at
5, 15, 30 and 60 minutes. It needs no code.

*Reversed by:* a firmware-level off being found that does not depend on our
silence — which would make this a command after all — or the idle timeout
measuring long enough that `released` is useless to show.

## D-024 — Every frame is built by `r2_packet`, because the hand-built ones were silently wrong

**Status:** accepted, 2026-09-05
**Supersedes nothing. Closes the loop opened by #114 slice 1.**

### The decision

No code outside `firmware/platform/r2_packet/` may construct a Sphero frame.
Sends go through `r2_gate_send()`, which encodes via `r2_packet_encode()`. This
is a structural rule, not a style preference, and the reason is that the
alternative was tried and failed measurably for weeks without anyone noticing.

### What was wrong

`coex_check/main/r2_central.c` built its packets by hand in three places and
escaped none of them. The Sphero V2 framing reserves `0x8D` (SOP), `0xD8` (EOP)
and `0xAB` (ESC); any of those appearing in a frame body must be escaped. The
sequence byte is a counter that walks through all three, and the checksum
computed over it can land on them too — so **six of every 256 packets were
malformed**, not three. (The checksum route is the half that is easy to miss,
and it doubles the rate.)

### Why nobody saw it

`coex_stats` recorded the ATT write status, and **the ATT write succeeds**. R2
accepts the bytes at the link layer and discards the packet at the Sphero layer.
A keepalive that silently fails to wake him is indistinguishable, to that
instrument, from one that worked. The A1 arms reported `fail=0`, `fail=2`,
`fail=2` and `disconnects=0` across 3,778 keepalives, and every one of those
numbers was true.

This is the same shape as the entries already in `CLAUDE.md` about validating an
instrument: the measurement was real, it just measured a different layer than
the one that was broken.

### The evidence, which was in the repo the whole time

`firmware/coex_check/results/escaping_forensics.py` re-derives this from the
committed A1 logs. Consecutive battery replies sit exactly 21 sequence numbers
apart, so a gap of 42 is one lost reply and the missing sequence number is
arithmetic:

| arm | replies | lost | lost to a mangled sequence number |
|---|---|---|---|
| `arm2-wifi-idle` | 58 | 4 | **4 / 4** |
| `arm3-wifi-loaded` | 58 | 4 | **4 / 4** |

**Eight for eight, across two independent arms, with no false positives.** The
four sequence numbers are the same four in both arms because the counter is
deterministic. Nothing else in either log needs explaining.

### The fix, measured rather than argued

`firmware/link_check` sends a battery read at each of the six mangled sequence
numbers deliberately, instead of waiting for the counter to reach one by chance:

```
ESCAPE GAUNTLET: battery reads at the 6 sequence numbers the old
                 unescaped encoder always lost
battery 4.42 V (seq=0x8D)   battery 4.42 V (seq=0xAB)   battery 4.42 V (seq=0xD8)
battery 4.42 V (seq=0x07)   battery 4.42 V (seq=0x34)   battery 4.42 V (seq=0x52)
ESCAPE GAUNTLET: 6/6 answered
```

Full log: `firmware/link_check/results/first-contact-2026-09-05.txt`.

### Consequences

- `r2_uuids.h` in `platform/r2_link/` carries BLE identity only. The packet
  constants were deleted from it; two copies of a protocol constant is how they
  drift apart.
- `coex_check` keeps its own copy and its bug. It is a finished experiment whose
  results are already recorded and whose logs are the evidence above — editing it
  now would invalidate the artefact without improving anything.
- The rule has teeth because of the gate's send counter: a caller that builds
  its own frame cannot move `r2_gate_stats()`, and `link_check` asserts that the
  gate admitted exactly as many sends as the link transmitted.

### What this does not claim

That escaping was the only thing wrong. It explains 8 of 8 *lost battery
replies*; it says nothing about the keepalives, whose failures are unobservable
by construction — a mangled wake is silently dropped and the next one is three
seconds behind. The keepalive loss rate for A1 is **unmeasurable after the
fact** and is best estimated as the same 6-in-256, roughly 89 of 3,778.
