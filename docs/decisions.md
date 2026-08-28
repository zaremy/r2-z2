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
