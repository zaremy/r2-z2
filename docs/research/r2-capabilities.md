# R2-D2 expressive capability inventory

Source: `sphero-r2d2/spherov2/toy/r2d2.py` @ `5401f31`, plus the command
surface in [r2-protocol.md](r2-protocol.md). Counts computed by enumerating the
`IntEnum`s directly, not by reading the README.

**Nothing in this document has been confirmed on the physical robot.** Every
mapping below is a *hypothesis to test*, and the whole point of the first
hardware session is to find out which ones actually read as the intended
emotion.

---

## 1. Output channels

| Channel | Control | Notes |
|---|---|---|
| Dome / head | `set_head_position(float°)`, `get_head_position()` | Declared **-162° … +182°** (`r2d2.py:471`); **OBSERVED usable -145.2° … +170.2°**. See S1c below |
| Stance | `perform_leg_action(R2LegActions)` | `STOP/THREE_LEGS/TWO_LEGS/WADDLE` (`animatronic.py:16`) |
| Leg position | `set_leg_position(float)`, `get_leg_position()` | Finer than the stance enum; semantics UNKNOWN |
| Locomotion | Drive DID `0x16` | Deliberately untouched until dome/LED/audio pass |
| Front LEDs | RGB, LED bits 0/1/2 | **OBSERVED** — one round lens on the dome face |
| Rear LEDs | RGB, LED bits 4/5/6 | **OBSERVED** — rectangular panel on the **back of the dome** |
| Logic displays | bit 3, single channel | **OBSERVED** — the two square grid panels, blue/cyan. Treat as **on/off** |
| Holo projector | bit 7, single channel | **OBSERVED** — separate clear lens on the dome face, white, **properly dimmable** |
| Audio | 388 sound ids + volume | `io.py:60-72` |
| Authored animations | 51 ids | `r2d2.py:417-468` |
| ~~Idle animations~~ | ~~`enable_idle_animations(bool)`~~ | **REFUTED on hardware 2026-08-16 — R2-D2 answers `bad_command_id`. See below.** |

### S1a LED survey — OBSERVED 2026-08-16 on `D2-6F6B`

All 8 channels driven individually to 255 and back to 0, observed by eye and
photographed. **`r2d2.py:17-25` is correct on every bit** — a much better
result than the idle command gave us.

| Bit | Fixture | Colour | Verdict |
|---|---|---|---|
| 0 | dome-face round lens | red | rgb |
| 1 | dome-face round lens | green | rgb |
| 2 | dome-face round lens | blue | rgb |
| 3 | two square grid panels | blue/cyan | **brightness_only, but effectively on/off** |
| 4 | back-of-dome panel | red | rgb |
| 5 | back-of-dome panel | green | rgb |
| 6 | back-of-dome panel | blue | rgb |
| 7 | dome-face clear lens | white | **brightness_only, genuinely dimmable** |

Four physical fixtures: two RGB triples, two single-colour.

**The rear light is on the back of the DOME, not the body.** Easy to assume
otherwise from the name; recording it so nobody guesses wrong later.

> [!important] Bits 3 and 7 are both "brightness" but not equally useful
> Stepped through 255 → 64 → 16, five seconds each:
> - **Bit 7 dims cleanly** across all three steps. Usable as a continuous
>   expressive channel — fades, slow pulses, intensity as a mood signal.
> - **Bit 3 responds, but its curve is brutally steep** — 255 → 64 already
>   drops most of the way, so the range between is not worth addressing.
>   **Design for it as on/off.**
>
> This does NOT rule out the "thinking" indicator the behavior table wants
> from the logic displays — patterned **blinking** delivers that, and is
> arguably truer to how real logic displays behave (they flicker, they do not
> fade). What is ruled out is the smooth *ramp*, not the intent.

**AC4 confirmed:** a single 16-bit-mask write (`CID 0x0E`) setting all 8
channels at once returned `success`, on hardware. Repo decision D-004 stands.

**UNKNOWN, spotted in passing:** in the bit-1 photograph the holo lens appears
to glow faintly alongside the front RGB lens. Could be reflection off adjacent
optics rather than crosstalk. Not chased — worth one look during S1d, since
authored animations may drive several fixtures at once.

> [!danger] `enable_idle_animations` does not exist on R2-D2
> **REFUTED** 2026-08-16 against `D2-6F6B`: `DID 0x17 CID 0x2C` returns
> `bad_command_id` (`0x02`), reproducibly.
>
> **The library and the firmware genuinely disagree. No source reading predicts
> this.** `spherov2` **does** expose `enable_idle_animations` on R2D2:
> `r2d2.py:14` is `class R2D2(BB9E)`, and `bb9e.py:121` assigns
> `enable_idle_animations = Animatronic.enable_idle_animations`, so the MRO
> (`R2D2 → BB9E → ToyV2 → Toy`) resolves it and `hasattr(R2D2,
> 'enable_idle_animations')` is `True`. The library claims the capability; the
> robot rejects the command.
>
> Also measured that session: `enable_leg_action_notify` (`0x2A`) and
> `enable_head_reset_to_zero_notify` (`0x39`) — both named directly in
> `r2d2.py`'s own class body — returned `success`. So the library got two of
> three right and one wrong. **A capability the library exposes is a claim to be
> tested, not a fact.**
>
> **Correction, recorded deliberately.** An earlier version of this note said
> the opposite: that `r2d2.py` omitted the command and "the source predicted
> this and we misread it." That was wrong — it was written after grepping
> `r2d2.py`'s class body without checking its base class. The hardware result
> was never in doubt; the *explanation* was, and it was published before being
> verified. See `docs/decisions.md` on why absence-from-a-file is not absence.
>
> **Consequence: there is no way to turn R2's idle behaviour off.** Every plan
> that treated idle-disable as the precondition for trustworthy survey data
> needs rewriting — see [[S1 Capability Survey]] in the vault.
>
> **Open, and now the important question: does R2-D2 have a native idle loop at
> all?** A command that does not exist is weak evidence that the behaviour does
> not either. Measured baseline: over 30 s, connected and awake, the dome did
> not move and R2 emitted zero unsolicited packets. That is suggestive, not
> conclusive — 30 s is short, he may have been charging, and idle may need a
> longer inactivity window.
>
> **How to read `spherov2` capability questions, correctly this time.**
>
> 1. A toy's capability set is its **own class body ∪ every base class**.
>    Resolve it through the MRO (`python3 -c "from spherov2.toy.r2d2 import
>    R2D2; print(hasattr(R2D2, 'x'))"`), never by grepping one file. Reading
>    only `r2d2.py`'s body is what produced the wrong explanation above —
>    `play_animation` and `stop_animation` are *not* missing from R2D2 either;
>    they come from `bb9e.py:119-120`.
> 2. **The resolved set is still only a library claim.** `0x2C` is the
>    counterexample: correctly resolved as present, and refused by the robot.
>    Presence means "worth probing", not "supported".
> 3. Absence is a weaker hint than presence, and neither is evidence. Only a
>    response from the robot is evidence.

### S1c dome range and settling — OBSERVED 2026-08-16 on `D2-6F6B` (issue #10)

Session at `--allow motion`. Every move via `bounded_head_move`, capped at 45°
of travel; 30° steps for the sweeps, 15° for the accuracy phase. Human observer
watching for the stall/strain distinction the log cannot make.

| Measure | Declared | **OBSERVED** |
|---|---|---|
| Maximum angle | +182° (`r2d2.py:471`) | **+170.2°** |
| Minimum angle | −162° (`r2d2.py:471`) | **−145.2°** |
| Usable span | 344° | **315.4°** |
| Moves to traverse it | ~16 (epic estimate) | **20** (7 up, 13 down) |
| `get_head` accuracy | — | **5/5 within 5°** of commanded (AC2 threshold was 4/5) |
| Settling time | — | **≤1.0 s — NOT resolved**, see below |

> [!warning] AC3 is satisfied on paper and unmeasured in fact
> The criterion was "time from ACK until two consecutive `head` reads differ by
> <1°", and that is ~1.0 s. But **every single move settled on sample 2 of 2** —
> the dome was already within 1° by the first read the bridge could take, which
> is one BLE round trip after the command. So 1.0 s is *my polling floor, not
> R2's settling time*. The true figure is somewhere below it and this session
> did not resolve it. Recording the number without this note would have turned a
> measurement artifact into an OBSERVED constant. Resolving it needs the
> event-backed timing in #11, not faster polling.

**Both limits are enforced as a silent refusal, not a mechanical stop.**
Commanded 12° past the top and 17° past the bottom, R2 moved *exactly* 0.0° —
byte-identical `get_head` readings before and after — with **no audible motor
load** at either end (operator observation, asked for specifically and confirmed
on a deliberate repeat at the +170° limit). The firmware discards an
out-of-range target rather than driving toward it and stalling. The declared
range in `r2d2.py:471` is therefore **wrong by 28.6° in total**, and wrong
asymmetrically.

**The dome has a deadband, and it is a gradient, not a constant.** Every move
undershoots its commanded target, and the size depends on where in the range
the dome is — **2.4° near +170°, growing monotonically to 5.7° near −145°**,
measured across 12 consecutive 30° steps. It does *not* scale with move size:
a 15° command and a 30° command lose about the same amount at the same part of
the range. A correction constant fitted at one end will be wrong at the other.

**There is a minimum effective dome increment.** Discovered by accident and
worth more than the range numbers: the return-to-start walk commanded the same
4.2° gap **21 times in a row and the dome never moved**, while every command
returned success. A move smaller than the local deadband produces no motion and
no error.

> [!danger] The "roughly 5°" figure below was SUPERSEDED on 2026-08-17
> This session saw 4.2° fail and inferred a threshold of ~5°. That was a
> **lower bound mistaken for a boundary** — nothing between 4.2° and 15° was
> ever commanded. The boundary was measured directly on 2026-08-17 and sits
> between **10.0° and 10.5°** near −32°, twice the inferred figure. See
> [S2a — composing a behaviour](#s2a-first-composed-behaviour--observed-2026-08-17-on-d2-6f6b-issue-43).
> The rest of this subsection stands; only the number is wrong.

> [!warning] Consequence for the behavior layer
> `express_curious()` cannot do a subtle 2° dome tilt — it will do nothing,
> silently, and report success. **The smallest legible dome gesture is ~12°**
> (corrected 2026-08-17; this said ~5° and that was an inference, not a
> measurement), and choreography that chains many small moves loses ~3-5° per
> command with no feedback that it happened. Prefer fewer, larger moves;
> re-read the angle rather than integrating commanded deltas.

Raw per-move data (before/commanded/landed, settle samples) is in the vault at
[[Experiments/Dome Survey]]. **UNKNOWN:** whether the deadband gradient is
gravity/load related or a control-loop property — distinguishing them needs the
dome tested on its side, which is not worth a session yet.

### S1f notifications — OBSERVED 2026-08-16 on `D2-6F6B` (issue #11)

One animation played: **id 21 `EMOTE_YES`**. Stopped there — see the safety note.

**`play_animation_complete_notify` FIRES, unprompted.** It has no enable command
anywhere upstream, and it arrived anyway. This was the open question the whole
sub-stage existed for.

| t from command | Event | `(did, cid)` | Payload |
|---|---|---|---|
| +329 ms | `leg_action_complete` | `0x17, 0x26` | `03` |
| +747 ms | `leg_action_complete` | `0x17, 0x26` | `03` |
| +778 ms | `leg_action_complete` | `0x17, 0x26` | `03` |
| +779 ms | `leg_action_complete` | `0x17, 0x26` | **`04`** |
| **+1112 ms** | **`animation_complete`** | `0x17, 0x11` | **`0015`** |
| +1171 ms | `leg_action_complete` | `0x17, 0x26` | `02` |

**The notification carries the animation id.** `0x0015` = 21 = the id that was
requested. A scheduler can match a completion to the specific animation it
dispatched, rather than assuming the next completion is its own.

> [!warning] `animation_complete` does NOT mean motion has stopped
> A `leg_action_complete` arrived **59 ms after** `animation_complete`. The
> animation reported done while a leg action was still finishing. Choreography
> that chains the next move immediately on `animation_complete` will overlap
> with residual motion from the previous one. Sequence on it, but do not treat
> it as "the body is now at rest".

**AC3 verdict: `event_driven_viable`.** Events fire, they are attributable via
the payload, and latency is ~1.1 s wall-clock for a short animation — usable for
sequencing. The 59 ms overlap above is a caveat on the scheduler's design, not a
reason to fall back to fixed sleeps.

**AC4 flips from `untested_by_design` to verified.** `leg_action_complete` fires
— six of them — **without `perform_leg_action` ever being called.** An authored
animation drives leg actions on its own.

**The payload is a STANCE STATE, not a command.** It decodes against
`R2DoLegActions` (`animatronic.py:8-13`: `UNKNOWN`/`THREE_LEGS`/`TWO_LEGS`/
`WADDLE`/`TRANSITIONING` = 0-4), the enum `get_leg_action` (CID `0x25`) returns
— **not** `R2LegActions` (`animatronic.py:16-20`), the enum `perform_leg_action`
takes. The two overlap on 1/2/3 and diverge at 0 and 4, so reading the wrong one
is silently plausible.

So the sequence is: `WADDLE`, `WADDLE`, `WADDLE`, **`TRANSITIONING`**,
animation_complete, **`TWO_LEGS`**.

> [!important] It is WADDLE that topples him, not the bipod end-state
> `EMOTE_YES` ends in `TWO_LEGS` — stabiliser retracted, never restored — and
> R2 fell during it. The obvious reading is "bipod is unstable". **That reading
> is wrong**, and was corrected the same session by the cheapest possible test:
> after the fall he was stood back up and **left standing in bipod, stable and
> upright, indefinitely** (OBSERVED).
>
> So being on two legs is fine. What is in the event sequence besides the
> end-state is **`WADDLE` ×3** — translation on two tracks with the stabiliser
> up. **INFERRED:** the tripod is required to *move*, not to *stand*; on two
> legs he can rotate in place but translating topples him. This matches the
> operator's independent hypothesis, formed from watching, before the event
> payloads were decoded.
>
> **Consequence for #12: the hazard is an animation that waddles, not one that
> ends bipod.** A classifier keying on the final stance would pass a
> waddle-in-the-middle animation as safe. Key on whether `WADDLE` appears in
> the leg-event sequence at all.

> [!warning] `get_leg_action` does not SENSE the stance
> **OBSERVED:** R2 standing visibly and definitely in bipod, freshly connected,
> answered `raw: 0` = `UNKNOWN`. It reports tracked state that a reconnect
> wipes, not a measurement of where the legs are.
>
> **So a stance read cannot establish ground truth at session start.** The
> obvious safety protocol — "read stance, confirm tripod, then play" — blocks on
> the first item and never opens, because the honest answer at connect is always
> `UNKNOWN`. `stable` is correctly `false` there; the op is not lying, it simply
> cannot bootstrap.
>
> Establishing a known stance needs `perform_leg_action` (a write, and stance
> bring-up — see #22) or a human eye. What the read IS good for: detecting a
> *change* within a session, once a baseline exists. Same discipline as the
> dome — read it, do not assume it, and never trust it across handling.

> [!danger] An authored animation knocked the robot over
> `EMOTE_YES` — a *nod* — commanded `WADDLE` three times and then `TWO_LEGS`,
> and **R2 fell.** Not damaged, but down.
>
> **`may_drive` is not a safety classification.** `r2_survey.py:135` flags ids
> 8/9/11 from `translator.c:101-104` as able to drive the body. That says
> nothing about stance transitions, and a stance transition is what put him
> over. Driving and falling are different failure modes; filtering for one does
> not filter the other.
>
> **Consequence for #12:** the animation survey plays **all 56 ids**. If a nod
> does this, a sequential sweep repeats it. #12 cannot run as specified. It
> needs a fall-safe protocol — surface, support, tripod state verified between
> items, and an abort on stance events rather than after a fall — and **#22
> stance should land first**.
>
> Operator hypothesis worth testing in #22, consistent with the event sequence:
> the third leg must be down to translate, and on two legs he can only rotate
> in place (opposite-direction track drive). Three `WADDLE`s followed by
> `TWO_LEGS` fits a retracted tripod that never came back down.

### Stance bring-up — OBSERVED 2026-08-17 on `D2-6F6B` (issue #22)

First session in which the legs were commanded deliberately. Ran at
`--allow stance`. **All ten acceptance criteria met.** No falls.

| Measure | **OBSERVED** |
|---|---|
| Deploy from `UNKNOWN` | **works** — tripod comes down, confirmed by eye |
| `get_leg_action` after the write | **`UNKNOWN` → `TRANSITIONING` → `THREE_LEGS`** |
| Transitions attempted | **6/6 completed**, none stalled or refused |
| Settling | **2.28 - 2.56 s**, tight across all six |
| `TRANSITIONING` | present on **every** transition, never skipped |
| Ack latency | 0.69 - 0.87 s |
| `leg_pos` at `three_legs` | **2.90** (3.03 on a second deploy) |
| `leg_pos` at `two_legs` | **230.13** |

**The stance IS bootstrappable, and that is what unblocks #12.** The write
updates the state the read exposes, so a session can establish a known
baseline instead of being stuck at `UNKNOWN` forever. `TWO_LEGS → THREE_LEGS`
works from a standing bipod (3/3), so an animation that leaves him bipod can
be recovered **in software** — no human needed to stand him up.

**`leg_pos` is the third leg's position**, confirmed by the operator watching
it move at the moment the float jumped 227 units. Cleanly bimodal, ~2.9
deployed and ~230.1 retracted, repeatable to ~0.13 between deployments. That
makes `set_leg_position` interpretable enough to write safely later — which is
exactly why #22 required reading it first.

**The deploy pushes him slightly FORWARD, not backward** (operator
observation). Clearance is needed in front. An earlier briefing of mine said
the opposite; it was a guess and it was wrong.

> [!warning] `stop` wipes the stance baseline
> `perform_leg_action(STOP)` does **not** retract the leg — `leg_pos` was
> byte-identical either side of a `stop` and the operator confirmed the leg
> stayed down. But it **resets `get_leg_action` to `UNKNOWN`**.
>
> `stop` runs on every exit, so **no session can hand off a known stance to
> the next one.** The bootstrap above has to be redone every time. Not
> dangerous — he is not destabilised — but it means "what stance is he in?"
> is unanswerable at the start of every session by construction.

> [!important] R2 retracts the third leg by himself, a short time after the link drops
> **OBSERVED**, and watched happening. Three candidates were tested in one
> session and the first two were eliminated outright:
>
> - **`stop` does not retract it.** Tested with the link still up: `leg_pos`
>   byte-identical either side, operator confirmed the leg stayed down.
> - **Disconnect does not retract it.** Tested with Ctrl-C: leg stayed down,
>   operator confirmed nothing happened.
> - **Time does.** With R2 sitting disconnected in tripod, the leg raised on
>   its own a short while later, watched by the operator. No command was in
>   flight and nothing was touching him.
>
> **The delay was about one minute** (operator, watching). That number rules
> out the first explanation reached for: it is too quick for an inactivity or
> sleep timer, and too slow to be part of the disconnect itself.
>
> **INFERRED:** this is R2's own *link-loss parking* behaviour — controller
> gone, no keepalive arriving, so retract to the compact state. The retraction
> and the delay are OBSERVED; the reason is not, and n=1 on the timing.
>
> Note the direction it parks in. **R2's idea of a safe default is bipod, not
> tripod** — the opposite of what "default to STOP" implies to a reader of
> `CLAUDE.md`'s safety section. Our stop leaves him standing on three legs;
> a minute later he takes one away, and nothing in our code is involved.
>
> **He therefore has no resting posture** — exactly like the dome having no
> home position, and for the household-presence goal in `CLAUDE.md` it is the
> same class of problem: a persistent companion is always *discovered* in
> bipod, whatever stance you left him in. Any behaviour that assumes a tripod
> at wake-up is assuming something false.
>
> It matters for the goal in `CLAUDE.md` — a persistent household presence
> that is always discovered in bipod has effectively no resting posture, the
> same way the dome has no home position.

### Sensors in

`extended_sensors` (`r2d2.py:470-477`): `r2_head_angle` (-162…182), gyroscope
x/y/z (±20000). Inherited from BB9E: accelerometer, orientation, locator.
Notifications: animation-complete, leg-action-complete, head-reset-to-zero,
battery-state-changed.

**INFERRED** — head angle + gyro + accelerometer is enough for a decent
"something happened to me" sense (picked up, bumped, tipped) without any
camera. That is the right first perception layer for a pet.

### S1e sensor streaming — OBSERVED 2026-08-18 on `D2-6F6B` (issue #29)

**R2 has proprioception, and it answers the question this project is pointed
at: he can tell when you touch him.** `DID 0x18` has been in our constants
since the start and had never once been switched on.

Driver: `mac-prototype/sensor_probe.py`, rubric dry-tested (22 cases) before
the link came up. Session at `--allow dome`.

#### AC1 — the stream enables

`set_sensor_streaming_mask` is **DID 0x18 / CID 0x00**, payload
`>HBI` = interval(2) + count(1) + mask(4) (`sensor.py:84-85`); the extended
mask is **CID 0x0C**, a bare 4-byte mask (`sensor.py:95-96`). The notify is
`(24, 2, 0xff)`, which our `CID_SENSOR_STREAM_NOTIFY` already matched.

All three commands returned `success`, and the read-back
(`CID 0x01`) confirmed the mask. **Unlike `enable_idle_animations`, this
library-exposed capability is real.**

> [!important] The enable is a THREE-call sequence and the order matters
> Copied from spherov2's `SensorControl.__update` (`controls/v2.py`), not
> invented: **interval 0 first**, then the extended mask, then the real
> interval. Setting the extended mask while a stream is already running does
> not reliably take. Nothing in the packet spec implies this.

#### AC2 — what arrives

| Property | **OBSERVED** |
|---|---|
| Wire format | big-endian **float32 per enabled component**, `struct.unpack('>Nf')` |
| Scaling | none — values arrive as floats. Only `locator`/`velocity` carry a x100 modifier |
| Rate | **4.00 Hz**, exactly the requested 250 ms interval |
| Channels confirmed | accelerometer x/y/z, attitude pitch/roll/yaw, gyroscope x/y/z, `r2_head_angle` |
| Decode errors | **0** across 240 baseline samples |

**Field order is load-bearing and is NOT the order you request.** The payload
is decoded by walking `sensors` (bb9e.py:80-112, inherited) then
`extended_sensors` (r2d2.py:470-477, R2D2's own) in **declaration** order,
taking one float per enabled component. `attitude` precedes `accelerometer`
even when the caller names accelerometer first. A wrong order yields plausible
numbers under wrong names with no error anywhere.

> [!tip] The decode was validated against three independent knowns
> Not "it parsed", but "it agrees with physics and with another command path":
> `accelerometer.z` = **0.9988 g** (gravity, upright); stream `r2_head_angle`
> = **23.61°** against a `get_head` read of **23.72°** on a completely
> separate command path, **0.11° apart**; max gyro at rest **0.17**.

#### AC3 — the rest baseline is very quiet

240 samples over 60 s. Per-channel standard deviation
at rest: `accelerometer.z` **0.0036 g**,
`attitude.pitch` **0.0340°**,
`r2_head_angle` **0.0193°**,
`gyroscope.x` **0.2548**.

> [!note] Three baselines were taken; these are the numbers from the LAST one
> The first was voided with the 6-sigma rubric, the second predates the
> event-ring flush fix and so over-counts. Every figure above and every
> threshold used to score the trials comes from the third, and the trials were
> re-scored against it from stored raw. Mixing figures across the three is
> exactly the error a mechanical re-derivation caught here.

#### AC4 / AC6 — touch is detectable, on the dome AND the body

**Verdict: `touch_detectable`.**

| | result |
|---|---|
| Sensitivity | **10/10** (5 dome, 5 body) |
| Specificity, settled | **0/6** false positives |
| Specificity, immediately after handling | **2/2 fired** |
| Discriminating channels | `gyroscope.x`, `gyroscope.z`, `attitude.yaw`, `accelerometer.z` |

Detection rule: **peak-to-peak within a 5 s window exceeding what rest produces
in an equal-length window x1.5, corroborated across at least two channels.**

> [!warning] "No touch" cannot be asserted until he settles
> Both specificity failures were fired IMMEDIATELY after a touch trial.
> **R2 keeps moving after contact stops**, so a detector needs a settle delay,
> not merely a threshold. This is not in #29's acceptance criteria; it came out
> of running negative controls at two different times.
>
> A tidier hypothesis — *"the rest baseline goes stale"* — was proposed when
> controls ran 0/6 before the block and 2/2 after, and is **REFUTED**:
> re-scoring the stored raw against a fresh baseline fires the same two
> windows. It was residual motion, not calibration drift.

#### The instrument was wrong four times before it was right

Recorded because each bug produced a **confident, unanimous 10/10**, and
because one control run would have caught any of them:

| Bug | What it did |
|---|---|
| 6-sigma threshold on max-over-window | max of 85–220 draws clears 6σ by chance; every trial fired regardless of touch |
| statistic measured deviation from the baseline MEAN | a channel merely parked somewhere new fired; killed by switching to peak-to-peak |
| event ring never flushed | a 5 s window swept in minutes of history — 220 samples against ~20 real ones. **This also inflated the apparent sample rate to 7.3 Hz; the true rate is 4.00 Hz, exactly as requested.** A harness artifact was nearly published as a device property |
| single-channel firing | one spurious encoder blip was enough; fixed by requiring 2 channels |

**The generalisable lesson: the instrument was only ever validated against a
known positive.** It was never asked to say *no* until ten trials had already
been collected and thrown away. A negative control belongs BEFORE the first
real trial, not after the last.

**NOT covered:** whether touch can be localised (dome vs body were both
detected, never distinguished); the settle delay was observed but not
measured; no
sustained-contact vs tap distinction was attempted.

### S1e dome as a force sensor — OBSERVED 2026-08-17 on `D2-6F6B` (issue #29 AC5)

There is no force, torque, current or touch sensor in the protocol. The test
is indirect: command a small dome move, read where it actually landed, and
compare the error against the unobstructed baseline. All moves were ±15° at
`--allow dome`, around +2…+27°, far from either range limit.

**Free-move baseline, hands off (n=5):**

| | error vs commanded |
|---|---|
| min | 3.48° |
| max | **4.54°** |
| mean | 3.99° |

This independently reproduces the S1c gradient. Interpolating the 2.4° @ +170
→ 5.7° @ −145 curve to +10° predicts **4.08°**; measured mean was **3.99°** at
an angle the curve was never fitted on.

**Sustained hold, operator resisting the move (n=5):**

| trial | commanded | actually moved | error | operator saw |
|---|---|---|---|---|
| 1 | −15° | 0.0° | 15.00° | partly blocked |
| 2 | +15° | +5.6° | 9.40° | moved |
| 3 | −15° | −9.4° | 5.59° | moved |
| 4 | +15° | +1.2° | 13.73° | moved |
| 5 | −15° | 0.0° | 14.89° | partly blocked |

**AC5 verdict: `detects_resistance`.** Worst free trial 4.54°, best held trial
5.59° — the distributions do not overlap, 5/5.

**Light continuous stroking — actual petting (n=2):**

| trial | commanded | actually moved | error |
|---|---|---|---|
| 1 | −15° | −13.4° | 1.58° |
| 2 | +15° | +12.2° | 2.85° |

**Verdict: `pet_not_detectable`.** Both strokes landed not merely inside the
free-move band but *below its minimum* (3.48°) — the two cleanest moves of the
session. There is no trend to chase with more trials; n=2 is small but the
result is not marginal in the direction that would matter.

> [!warning] Detection scales with force, and petting is below the floor
> Error fell 15.00 → 9.40 → 5.59 as the hold softened, then to 1.58 with a
> stroke. A threshold on dome error can catch **someone holding his head
> still**; it cannot catch **someone petting him**. Touch-triggered behaviour
> — the thing this was asked for — needs AC1 sensor streaming, not this.

> [!info] `get_head` SENSES; `get_leg_action` only TRACKS
> Settled by trial 1: a 15° command against a blocked dome reported **0.0°
> moved**. Had the encoder been echoing the commanded value, as
> `get_leg_action` does for stance, the error would have read ≈0 and the whole
> method would be worthless. It does not. Position reads can be trusted as
> physical observations, and the two readbacks are **not** the same kind of
> thing — do not generalise from one to the other.

Consequence for the behavior layer: a `react_to_touch()` semantic behavior is
**not** buildable on dome error alone. What *is* buildable today is
"something is stopping my head" — a distinct and narrower signal, and one that
only exists while a move is already in flight.

---

## 2. Sound vocabulary — 388 ids, clustered

The ids are **not** R2-only. Roughly a third are borrowed voices:

| Voice | Count |
|---|---|
| **R2-D2 native** | 212 |
| BB-8 | 125 |
| BB-9E | 33 |
| R2-Q5 | 11 |
| Test tones (`TEST_*Hz`) | 7 |

### S1b sound survey — OBSERVED 2026-08-16 on `D2-6F6B`

**25 of 40 sampled ids played and rated by ear**, at volume 200 (80 was too
quiet to evaluate). Stopped early by operator decision: *"in general, these
labels seem to be accurate, I don't think I need to do any more."*

| Family / id | Label promised | Read as | Verdict |
|---|---|---|---|
| `ACCESS_PANELS`, `ALARM_1/10/12`, `ANNOYED` | warning, urgency, irritation | as labelled | ✅ |
| `BURNOUT` | — | burnout / fatigue | ✅ |
| `ENGAGE_HYPER_DRIVE` | — | preparing something important | ✅ |
| `CHATTY_1/10/11/15/16` | neutral talking | success, inquisitive, answer, "huh?", disappointment | ❌ **REFUTED** |
| `EXCITED_1/10/11` | high-energy delight | quick thinking, analyzing, quick reply | ❌ **REFUTED** |
| `FALL` | — | damaged metal / impact | ✅ |
| `HEAD_SPIN` | — | mechanical movement | ✅ |
| `HEY_1/10/11` | attention-getting | whistles, expressive | ✅ |
| `HIT_1/10/11` | reaction to impact | mechanical thunks | ⚠️ see below |
| `LAUGH_1/2/3` | amusement | happy, high-pitched, one raspberry | ✅ |
| `MOTOR` | — | long mechanical lift with clocklike ticking | ✅ |

> [!danger] `R2_CHATTY_*` is NOT neutral talking
> Five samples, five distinct emotional readings, none neutral. These are
> **conversational turn-shapes** — a question implies someone to ask, an answer
> implies something was asked. That is 62 of 212 ids, and the doc had assigned
> them to idle/ambient.
>
> **Operator ruling:** an early CHATTY (`CHATTY_1`, the most neutral-leaning,
> "quick success") is acceptable for `idle()` anyway. Recorded as a decision,
> not a measurement — the readings above stand, and this is a taste call about
> whether that colouring matters in practice.
>
> Where the family clearly belongs: **interaction**. Emotionally-loaded
> conversational fragments are right for back-and-forth with a person.

> [!danger] `R2_EXCITED_*` is cognition, not delight
> "Quick thinking", "analyzing", "quick reply" — not high-energy excitement.
> 16 ids. **This is the family for R2 waiting on an LLM round-trip**, which is
> a need the behavior table had no sound for.

> [!warning] `R2_HIT_*` may be foley, not vocalisation
> They read as mechanical thunks — the sound of *being* hit. A character
> reacting to being bumped wants a yelp, not an impact sample. Only the second
> is expressive. Untested distinction; matters before wiring a bump reflex.

**Duration: the id gap is a bucket, not a value.** Ids are spaced 2-131 apart,
not sequentially, so the gap was suspected to encode clip length. Tested with
the three shortest gaps and the three longest:

| Id | Gap | Predicted | Heard |
|---|---|---|---|
| `STEP_3/4/5` | 2 | very short | short ✅ |
| `BURNOUT` | 35 | long | long ✅ |
| `SAD_5` | 36 | long | **medium** ❌ |
| `MOTOR` | 131 | longest | longest ✅ |

Gaps 35 and 36 gave different perceived lengths, so **it is not a linear
duration**. Usable to bucket short/medium/long and to flag outliers; not usable
to time choreography. S1d still needs measured or event-backed durations.

**NOT SAMPLED — six families have no reading at all:** `NEGATIVE`, `POSITIVE`,
`SAD`, `SCREAM`, `SHORT_OUT`, `STEP`. Their labels are concrete, and every
concrete label held; the two that failed (`CHATTY` "neutral talking",
`EXCITED`) were the two vaguest. Reasonable to trust them and revisit if a
behaviour built on one feels wrong — but they are **UNVERIFIED**, and #9's
acceptance criterion of ≥3 rated ids per family is **not met**.

**Use only the `R2_*` family for the character.** BB-8/BB-9E sounds are a
different droid's voice and will break the illusion; the test tones are
factory diagnostics. That single filter cuts 388 down to a workable 212.

R2-native families:

| Family | Count | Reads as |
|---|---|---|
| `R2_CHATTY_*` | 62 | **REFUTED — not neutral.** See S1b below |
| `R2_NEGATIVE_*` | 28 | Refusal, complaint, disagreement |
| `R2_SAD_*` | 25 | Dejected, lonely |
| `R2_POSITIVE_*` | 23 | Agreement, satisfaction |
| `R2_EXCITED_*` | 16 | **REFUTED — reads as cognition/processing.** See S1b |
| `R2_ALARM_*` | 15 | Warning, urgency |
| `R2_HEY_*` | 12 | Attention-getting, greeting |
| `R2_HIT_*` | 11 | Mechanical thunks — possibly foley, not vocalisation. See S1b |
| `R2_STEP_*` | 6 | Movement foley |
| `R2_LAUGH_*` | 4 | Amusement |
| `R2_SCREAM`, `R2_SCREAM_2` | 2 | Fear/pain |
| singletons | 8 | `R2_ANNOYED`, `R2_BURNOUT`, `R2_FALL`, `R2_MOTOR`, `R2_HEAD_SPIN`, `R2_SHORT_OUT`, `R2_ACCESS_PANELS`, `R2_ENGAGE_HYPER_DRIVE` |

**INFERRED, and the key design lever:** the large families
(`CHATTY` 62, `NEGATIVE` 28, `SAD` 25, `POSITIVE` 23) exist so a droid can say
the *same kind of thing* many times without repeating itself. A pet should pick
a **random unheard member of a family**, tracking recent picks, rather than
binding one sound id per emotion. Repetition is what makes a toy feel like a
toy.

**OBSERVED** — `play_audio_file` takes an `AudioPlaybackModes` flag
(`io.py:8-11`): `PLAY_IMMEDIATELY=0`, `PLAY_ONLY_IF_NOT_PLAYING=1`,
`PLAY_AFTER_CURRENT_SOUND=2`. Mode 1 is the correct default for ambient
behavior — it makes idle chatter self-suppressing instead of stuttering over
itself. Mode 0 is right for interruptions that *should* cut in.

---

## 3. Animation vocabulary — 51 ids

### S1d animation survey — OBSERVED 2026-08-17 on `D2-6F6B` (issue #12), 56/56 ids

Ran at `--allow stance` with R2 **free-standing on a table, unassisted** — no
hand catching him, so a recorded fall is a real fall and a recorded "did not
fall" is a genuine unassisted survival — neither label is contaminated by
intervention.

**Fall observations cover 37 of 56 ids** (7 fell, 30 did not). The other 19 —
`0-13, 16, 17, 21, 52, 55` — carry **no fall observation at all** and are
excluded from both columns rather than counted as survivors. Most were swept in
early blocks before per-id fall attribution was being recorded; 52 and 55 sit
in the final block, whose single fall could not be attributed to a specific id
and is left UNATTRIBUTED. So the fall column is uncontaminated, not complete.

> [!danger] Most authored animations retract the stabiliser and waddle
> **36 of 56 ids emit `WADDLE`.** It is the norm, not an outlier.
>
> Every one was started from a **verified** `THREE_LEGS` baseline — the driver
> asserted it and read it back before each id — and every one **retracted the
> leg itself**. Pre-deploying the tripod does not make an authored animation
> safe; the animation overrides it. See D-010.

| classification | n | ids |
|---|---|---|
| `waddles` — unsafe while standing | 36 | 2, 3, 4, 5, 7, 8, 9, 10, 12, 13, 14, 15, 19, 21, 22, 24, 31, 32, 33, 35, 36, 37, 38, 39, 40, 41, 42, 43, 45, 46, 48, 49, 50, 51, 53, 54 |
| `leaves_bipod` — stable standing, recoverable | 4 | 11, 18, 20, 28 |
| `returns_to_tripod` — cleans up after itself | 2 | 29, 30 |
| `no_leg_activity` — no stance hazard observed | 14 | 0, 1, 6, 16, 17, 23, 25, 26, 27, 34, 44, 47, 52, 55 |

**Usable on a standing droid: 20 of 56.** The other 36 are the emotional core
of the library — `EMOTE_*` and most of `WWM_*`, including `WWM_CURIOUS` (35),
the animation `architecture.md` names for `express_curious()`.

#### The fall outcome is stochastic; the event sequence is not

Seven ids were **observed** to fell him: **14, 15, 22, 31, 37, 38, 51**. That
list is not a safe/unsafe boundary, and this is the most important result here.

Repeat trials on ids 53 and 54 replayed their leg-event sequences almost
exactly — 53 gave 6 waddles in 3.29 s then 3.16 s; 54 gave 2 waddles in 1.86 s
then 1.69 s — while the **fall outcome did not reproduce**. One fall in the
52-55 block could not be attributed to either on a repeat and is recorded
UNATTRIBUTED rather than guessed.

> [!warning] A per-id empirical safe-list is not achievable *for the household case*
> Falling depends on starting pose, residual momentum from the previous item,
> and the surface — none of which the animation id determines.
>
> The evidence for non-reproducibility is **two ids re-run once each**, which is
> thin, and does not prove impossibility in general: a safe-list might well be
> establishable under tightly controlled pose, surface and rest-between-items.
> **But those are exactly the conditions a droid living in a house cannot be
> guaranteed**, so a safe-list built under them would not transfer to the case
> we care about. This is why D-010 treats `WADDLE` emission, not observed
> falls, as the safety boundary — the emission is a property of the animation,
> the fall is a property of the situation.

No feature we examined separates fallers from survivors. These are three scalar
summaries, not an exhaustive search — event order, first-waddle timing,
inter-event spacing and multivariate combinations are **untested**:

| | fell (n=7) | stayed up (n=30) |
|---|---|---|
| waddle count | 4 – 12 | 0 – 10 |
| longest consecutive run | 2 – 5 | 0 – 8 |
| duration | 3.00 – 9.11 s | 1.51 – 21.30 s |

Every range overlaps. Id 42 has 10 waddles and a run of 8 and stayed up; id 22
has 4 and a run of 2 and went down. **`leg_action_complete` reports state
transitions only** — never direction, distance or force — so a two-waddle lurch
and a six-waddle shuffle are the same symbol. The signal that would predict a
fall (accelerometer, gyro) is sensor streaming, #29 AC1.

#### A reactive stance guard cannot work — REFUTED on hardware

Tested on id 37: play the animation and re-deploy the stabiliser the moment it
retracts. The re-deploy was **accepted**, not refused — the firmware permits
commanding `three_legs` mid-animation. It arrived far too late.

The arithmetic rules it out permanently. Detection costs a bridge round-trip
(~0.3-0.5 s) and deployment settles in **2.28-2.56 s** (#22). A fall completes
in well under a second. **Even with zero detection latency the leg lands more
than a second after he is already down.** No polling rate fixes this.

Two assumptions this rests on, stated rather than buried: that a stabiliser is
only load-bearing once *settled* (a partially-extended leg may help sooner),
and that the guard cannot act *before* the retraction it is reacting to. The
hardware test was one animation. What is firmly refuted is the reactive guard
as built; what remains open is a predictive one, which would need the
animation's leg track known in advance — the thing D-010 says we cannot get.

#### The unnamed gaps are valid — all five

Ids **20, 23, 28, 29, 30** are absent from the `spherov2` enum, and this doc
previously expected `0x02 bad_command_id`. **All five play normally.** They are
valid-but-unnamed, not invalid. Two of them (29, 30) are the *only* ids in the
entire library that touch the legs and put the stabiliser back — behaviourally
the most valuable class found, and unnamed upstream.

#### Durations are event-backed and free

`animation_complete` fires with no enable command and carries the id, so every
duration here is measured rather than stopwatched. Range **0.88 s** (id 55
`MOTOR`) to **21.30 s** (id 27 `IDLE_3`).

> [!warning] Two method traps, both hit in this session
> **Leg notifications must be enabled or every id looks safe.**
> `animation_complete` fires unprompted; `leg_action_complete` does **not**. A
> session that forgets the enable still sees completions and still measures
> durations, and silently sees zero leg events. The first run of this survey
> did exactly that and classified `EMOTE_YES` — the known waddler — as
> `no_leg_activity`, the safest bucket. Prove the channel live by commanding a
> leg move and seeing the event before trusting any silence.
>
> **Deploy is stable; retract is not.** A diagnostic that retracted the
> stabiliser to prove the channel was live knocked him over backwards. Any
> forced leg movement must be a deploy.

> [!warning] `may_drive` is refuted as a safety filter, empirically
> It flags ids 8/9/11 (`translator.c:101-104`). Measured: **8 and 9 waddle; 11
> does not.** One of three, in the wrong direction. It describes driving, not
> stance, and stance is what topples him.

**Not covered by this pass:** AC4's seven conflict verdicts (0, 3, 4, 7, 9, 13,
15) need the epic's conflict-resolution protocol with predeclared predicates
and media artifacts; AC6 interruption testing; and the per-id
`energy_cost_class` / `wear_class` / `recommended_cooldown_s` fields.

> **RESOLVED 2026-08-17.** AC4 is adjudicated — see *Animation ID conflict* in
> §3. AC6 is covered below. The three cost fields and `made_sound` remain open.

### S1d AC6 — interruption

Three ids, all from the `no_leg_activity` set so the legs never move and a fall
cannot confound the reading. `stop` issued at ~40 % of the measured length.

| Id | Full length | Stop sent | Completion | Motion ceased | Sound ceased |
|---|---|---|---|---|---|
| 1 | 5.145 s | 2.177 s | 3.587 s | yes | yes |
| 25 | 14.489 s | 5.857 s | 7.259 s | yes | *no audio* |
| 27 | 21.300 s | 8.710 s | 9.939 s | yes | *no audio* |

**A mid-play stop works, 3/3.** Every animation emitted `animation_complete`
early — **1.23, 1.40 and 1.41 s** after the stop went out, against full lengths
of 5–21 s. That timing is machine evidence and does not depend on anyone
watching.

**Motion answered on 3/3; sound on only 1/3.** Two of the three ids produce no
audio at all, so "did the sound cease" was not testable on them. That is a
shortfall in target selection, not a missed reading: the ids were chosen for
fall-safety, and **`made_sound` was never captured in S1d**, so there was no
way to pick sound-producing ids on purpose.

> [!warning] The bridge cannot attribute a sound stop
> `stop` (`r2_probe.py:1102`) is a **composite**: `stop_animation` +
> `stop_audio` + a leg stop. So when sound ceases, it cannot be attributed to
> halting the animation rather than to the audio stop fired alongside it. The
> question underneath AC6 — animations bundle sound (`translator.c:91`), so
> does stopping one stop the other? — needs a `stop_animation`-only op that
> does not exist. **AC6 is met for motion and partially met for sound.**

**OBSERVED — an interrupt leaves the dome wherever it stopped.** Measured at
**−45.92°** after interrupting id 27. Consistent with the standing fact that
the dome has no resting position. Consequence for choreography: any interrupt
must be followed by an explicit dome move if a known orientation is wanted.

### LED colour — the channel is ours to own

Ad-hoc, serving the colour-semantics ruling in **D-012**; not a #12
criterion. (An earlier revision of this line cited D-011, which is the
backpack-speaker decision — wrong ADR.)

- **At rest the droid is not dark.** Front alternates **red/blue**, back
  alternates **green/yellow**. Animation id 0 *speeds up* the front alternation
  rather than starting it.
- **Front and back are true RGB** (bits 0/1/2 and 4/5/6, `r2d2.py:17-25`).
  Logic displays (bit 3) and holo projector (bit 7) are **brightness only — no
  colour**.
- **OBSERVED — a colour we set HOLDS.** Front set to `{0:0, 1:255, 2:0}` and
  back to `{4:0, 5:255, 6:0}` both rendered pure green and stayed; the baseline
  alternation did not resume and overwrite them.
- **OBSERVED — an animation transiently overrides a set colour, and the set
  colour returns when the animation ends.** Green survived a play of id 1
  without being re-issued. So our colour behaves as a **base layer**: masked
  during an animation, reasserted after. **n=1, one animation id** — whether
  every animation behaves this way is UNKNOWN.
- **OBSERVED — the base colour survives a disconnect.** Green set, animations
  played over it, daemon killed, reconnected an hour later: **still green**. The
  base layer persists with nothing maintaining it, so it is storage rather than
  something we refresh.

#### Colour depth: PWM works, but pastels do not read

Every test above sent only `0` or `255` per channel. A `(120, 190, 255)` pale
blue rendered **pale** — "almost grey" — so intermediate levels are real and the
full 24-bit space is available.

**Low saturation reads as grey, though**, which makes pastels useless as distinct
meanings. Any colour semantics must use hard-contrasting hues.

#### The flicker is R2's; the jitter is ours

Two phenomena were being conflated, and only one survives the port to firmware:

| | Cause | Survives the ESP32 port? |
|---|---|---|
| timing jitter, up to **158 ms** | our two bridge poll loops | **no** |
| flicker on every colour change | R2's LED update path | **yes** |

**Isolated by writing the identical colour 20 times with no value change: rock
steady.** So the write path does not blank the LED — the *change* is what is
visible, and a 30-unit step flickers exactly as much as a full swap.

**OBSERVED consequence: smooth interpolation is not available on this
hardware.** An interpolated baby-blue→cyan pulse read as flicker rather than a
fade for this reason, not because the ramp was too coarse. The light language is
inherently **steppy**: design discrete high-contrast frames, do not attempt
fades. Ruled on in **D-012 Amendment A** (§2) — which is where the ruling now actually lives; before 2026-08-17 this pointed at a D-012 that did not yet contain it.

> [!warning] The modulation rate ceiling here is the BRIDGE, not the LED
> ~**0.389 s** median per set, decomposing as ~0.07 s CLI process spawn + a
> 0.2 s daemon request-queue poll (`r2_probe.py:1442`) + a 0.15 s client
> response poll (`r2_probe.py:1517`). Two poll loops account for ~0.35 s of it.
> So nothing faster than ~0.8 s per cycle can be driven evenly **through this
> harness**, and the true ceiling is **unmeasured**. Do not carry 0.4 s forward
> as "the LED update rate". Tracked on #17.

### Enum groups

Four groups (`r2d2.py:417-468`):

| Group | Ids | Meaning |
|---|---|---|
| `CHARGER_1…7` | 0-6 | Charging-dock sequences |
| `EMOTE_*` | 7-24 | 16 authored emotional beats |
| `IDLE_1…3` | 25-27 | Ambient idle |
| `WWM_*` | 31-54 | 23 "Watch With Me" reaction animations |
| `MOTOR` | 55 | Motor sound/behaviour |

Ids **20, 23, 28, 29, 30 are absent** from the enum. **RESOLVED 2026-08-17 —
all five are valid and play normally**; none returned `bad_command_id`. See the
S1d section above. They are valid-but-unnamed, and 29/30 are the only two ids
in the library that restore the stabiliser after using it.

`WMM_FRUSTRATED=39` is a typo for `WWM_` in upstream. Cosmetic; the id is fine.

The full emotional set worth building on:

- **EMOTE (7-24):** ALARM 7, ANGRY 8, ATTENTION 9, FRUSTRATED 10, DRIVE 11,
  EXCITED 12, SEARCH 13, SHORT_CIRCUIT 14, LAUGH 15, NO 16, RETREAT 17,
  FIERY 18, UNDERSTOOD 19, YES 21, SCAN 22, SURPRISED 24.
- **WWM (31-54):** ANGRY 31, ANXIOUS 32, BOW 33, CONCERN 34, CURIOUS 35,
  DOUBLE_TAKE 36, EXCITED 37, FIERY 38, FRUSTRATED 39, HAPPY 40, JITTERY 41,
  LAUGH 42, LONG_SHAKE 43, NO 44, OMINOUS 45, RELIEVED 46, SAD 47, SCARED 48,
  SHAKE 49, SURPRISED 50, TAUNTING 51, WHISPER 52, YELLING 53, YOOHOO 54.

**INFERRED** — `WWM_*` are reaction shots authored for a passive viewing mode.
That is *exactly* the register a household pet needs: short, self-contained,
non-goal-directed reactions. Expect `WWM_*` to be the richest source of
believable ambient behavior, and `EMOTE_*` to be the more declarative,
"answering you" register.

**OBSERVED** — animations bundle motion *and* sound.
`claude-r2d2-buddy/main/translator.c:91` says so explicitly ("sounds bundled
into animations"). **INFERRED consequence:** layering a separate `play_audio`
on top of an animation will double-talk. Choreography must choose: authored
animation *or* hand-composed dome+sound+light, not both at once.

### Animation ID conflict — ADJUDICATED 2026-08-17 (#12 AC4/AC5)

`claude-r2d2-buddy/main/translator.c:93-108` labels the low ids from
experimentation and **disagrees with spherov2** (`r2d2.py:417-468`) on 7 of the
8 overlapping ids. All seven were played on `D2-6F6B` against predicates
written down **before** each id was fired.

| Id | spherov2 | translator.c | **Verdict** | What was observed |
|---|---|---|---|---|
| 0 | `CHARGER_1` | "blinks red/blue — avoid" | **translator** | front light's red/blue alternation **sped up**; no leg motion, 1.53-1.92 s over 3 fires |
| 3 | `CHARGER_4` | `ANIM_ALERT` (impatient) | inconclusive | tone read as **neutral** unprompted; `CHARGER_4` predicts nothing observable |
| 4 | `CHARGER_5` | `ANIM_SAD` (denied) | **translator** | descending tone, read as *"no I don't agree"*; red held **steady** |
| 7 | `EMOTE_ALARM` | `ANIM_HAPPY` (approved) | **spherov2** | urgent repeating tone **and** red-dominant lights; the cheerful reading failed |
| 9 | `EMOTE_ATTENTION` | `ANIM_WIGGLE` | inconclusive | **both** held — assertive chirp + dome motion **and** side-to-side rocking |
| 13 | `EMOTE_SEARCH` | `ANIM_CHATTY` | inconclusive | dome **does** scan **and** he **does** chatter; "longer" is measurably false |
| 15 | `EMOTE_LAUGH` | `ANIM_HAHAHA` ✓ | inconclusive (**control**) | sources agree, so nothing to separate — the shared laugh prediction **HELD** |

**Neither source wins.** translator takes 2, spherov2 takes 1, four are
undecidable — and the four are undecidable for two structural reasons, not for
want of careful watching:

1. **`CHARGER_N` is not a falsifiable label.** It names *where* an animation is
   used, not what it looks or sounds like, so ids 0/3/4 can never return a
   `spherov2` verdict from an observational protocol. Those records carry
   `spherov2_falsifiable: false`, and the rubric refuses to let an untestable
   side win by default.
2. **The competing labels are not mutually exclusive.** id 9 genuinely *is*
   both an attention-getter and a wiggle; id 13 genuinely *is* both a scan and
   a chatter. The disagreement is partly two people describing the same
   animation from different angles.

**The control is what makes the undecidables credible.** id 15 is the one id
where the sources agree; its shared prediction was confirmed, so the four
`inconclusive` results are a property of the labels, not an artifact of a
protocol that cannot detect anything.

> [!warning] The differences are small, and that is a finding
> The operator, unprompted on the first id: *"we're talking about beeps and
> boops here, the differences are not dramatic on many of the interactions."*
> An earlier hypothesis that spherov2 owns the `EMOTE_*` block while its
> `CHARGER_*` block is mislabelled was **refuted** the moment id 9 — an
> `EMOTE_*` id — failed to go spherov2's way.

**Method, so a verdict can be re-checked.** Predicates were frozen in source
before the link came up (`conflict_ac4.py`, `PREDICATES`) and copied into each
record at fire time; `neither` requires **zero** satisfied predicates on both
sides, with any partial match resolving to `inconclusive` per epic #5's rubric.
Raw TX was captured for ids 7 and 15 (e.g. `8d0a17057b000757d8`, DID `0x17`
CID `0x05`, payload `0007`); earlier fires lost it to a buffered `tee`.

**Deliberate deviation from #12 AC4:** the media artifact was **waived** by the
operator, who is the sole reviewer on this project — the reproducibility it
buys is for a third party who does not exist here. Records carry
`evidence_complete: false` and this note stands in place of the video. All
seven event sequences reproduced their S1d durations and leg-event counts
closely (e.g. id 7: 5.31 s / 20 events in S1d, 5.33 and 5.14 s / 20 here),
which is the reproducibility that *is* available without a camera.

---

### S2a first composed behaviour — OBSERVED 2026-08-17 on `D2-6F6B` (issue #43)

The first time dome, sound and light were driven as **one gesture** rather than
as separate survey items. Driver: `mac-prototype/r2_behavior.py`, dry-tested
offline (44 cases) before the link came up. Session at `--allow dome`.

Two behaviours were built: `express_curious()` (all three channels) and
`thinking()` (sound + light, dome deliberately still). Neither plays an
authored animation, so neither can drive a leg action — see D-013.

#### The bridge is serial, so choreography cannot mean parallel tracks

The daemon's queue loop is `for req in reqs: await handle_request(...)`
(`r2_probe.py:1443`). One request completes before the next starts. **True
simultaneity is not reachable from outside the daemon** — which is the concrete
argument for the daemon-side timeline executor in #43.

Two things partly compensate, and both were used:

1. `sorted(REQ_DIR.glob("*.json"))` drains everything already queued
   back-to-back, so writing a phrase's request files in ONE batch paces its
   steps at `CMD_SAFE_INTERVAL` (0.12 s) instead of the ~0.39 s round-trip
   floor of one `./r2 send`.
2. `_op_dome` with `settle=0` returns as soon as the move is *commanded*. The
   dome is still physically turning while the next op runs — the overlap is
   mechanical, not concurrent.

#### Dome mechanics, measured

| Property | **OBSERVED** | Method |
|---|---|---|
| Minimum effective travel | **between 10.0° and 10.5°** near −32° | 10 commands, 3.5 s apart, alternating direction |
| Move duration | **~2.0–2.2 s, independent of distance** | n=4; 8.35° took 2.19 s, 22.61° took 2.07 s |
| Undershoot | **~2.4–3.7°**, in the direction of travel | same run |
| Sub-threshold command | **silently ignored, reports `ok: true`** | 4°/6°/8°/10° all produced ≤0.11° of motion |

```
 4.0° -> 0.11    6.0° -> 0.00    8.0° -> 0.00   10.0° -> -0.11   (no motion)
10.5° -> 7.08   11.0° -> 7.71   12.0° -> 8.45   14.0° -> -10.46  (motion)
```

**A move takes the same ~2 s whether it travels 8° or 22°**, so this is a
fixed-duration move, not a slew rate. Combined with the threshold, the
practical rule is: **12° minimum, 2.2 s apart.**

> [!warning] The threshold is position-dependent and only ONE point was measured
> S1c established that the deadband is a **gradient** — 2.4° near +170° growing
> to 5.7° near −145°. This session measured the *motion threshold* at roughly
> −32° only. It is very likely larger toward the bottom of the range and
> smaller at the top. **10.0–10.5° is one point on a curve, not a constant.**
> 12° is a working minimum with margin near centre, not a verified floor
> everywhere.

#### Two hypotheses raised and REFUTED in the same session

Both are recorded because each fitted the data it was formed on and each was
wrong, and the wrong versions are the more natural readings.

| Hypothesis | Fitted | Killed by |
|---|---|---|
| "A dome move issued while another is still travelling is silently dropped" | n=2 — the third move of two consecutive beats vanished | A correction move that failed with **2.2 s of clear air** in front of it. The real cause is the travel threshold: all three failures were 7.5–7.8° moves. |
| "Anchoring each beat to the angle it started at bounds the drift" | The residual was ~3° every beat, comfortably small | The anchor moved *with* the dome, so the residual was always sub-threshold and the correction **never fired once**. Measured walk: −24.58° → −32.40° → −34.93° → −37.58° → −41.17°. |

The second is the more useful lesson: **a drift reference that follows the
thing it measures is not a reference.** It measures the last step's error and
then forgets it, which is exactly how a slow leak survives a fix aimed at it.

#### Drift, and the fix that works

A gesture built from deltas cannot return to where it started, because every
leg undershoots. The fix is a **persistent home angle held across beats**
(`DomeHome`): error accumulates against a fixed mark until it exceeds the
~12° the firmware will act on, then one absolute-angle move pulls him back.

Verified over five consecutive beats sharing one home at −45.71°:

| beat | drift vs home | dome moves | correction |
|---|---|---|---|
| 1 | −3.59° | 3 | — |
| 2 | −6.97° | 3 | — |
| 3 | −9.62° | 3 | — |
| **4** | **−3.59°** | **4** | **fired** |
| 5 | −6.97° | 3 | — |

A bounded sawtooth between ~−3.6° and ~−12°, repeating, instead of a monotonic
walk. **Residual under ~12° is the floor of what this hardware can hold**, not
a defect to engineer away.

#### Sound must be queued BEFORE the dome move it accompanies

The first build queued `dome` then `sound` 0.12 s later, on the theory that the
dome would still be travelling when the chirp landed. **REFUTED by the
operator**, who heard the chirp land in the pause *after* the dome settled.
Audio onset is slower than the 0.12 s batch spacing and a small dome move
completes in a few hundred ms, so the sound must go first for any overlap at
all. Whether it then genuinely overlaps is **UNVERIFIED** — not yet observed
after the reordering.

#### What the operator confirmed

- The **three-colour light transition reads well** — pale blue → cyan → steady
  blue was described as "a nice transition". This is the first composed
  expression confirmed to work as designed.
- The **mid-gesture pause is clearly perceptible**, which was the intent: the
  hold is what makes the beat read as a question rather than a twitch.

**NOT covered:** whether the reordered chirp overlaps the turn; whether the
whole beat reads as *curiosity* to someone who was not told what it is;
`thinking()` was built and unit-tested but **never fired on hardware**.

### S2b sensor-triggered behaviour — PARTIAL 2026-08-18 on `D2-6F6B`

The first closed loop: the sensor stream (S1e) drives the choreography layer
(S2a) with no human in the middle. `mac-prototype/r2_reactive.py`.

> **RESOLVED 2026-08-18.** This section first shipped saying the loop worked
> end to end but had never once been triggered by a hand — every live trigger
> was ambient vibration. That is no longer true. A deliberate pet is now
> OBSERVED to drive the full loop, and the operator confirmed the gesture
> rendered. Detail in *Deliberate touch* below.

#### What the loop does

`calibrate → negative control → arm → detect → settle → perform → recover →
cooldown → re-arm → closing control`. Detection reuses S1e's validated rubric
verbatim (peak-to-peak against an empirical rest null, two channels required to
corroborate). The behaviour is `express_curious()` from S2a.

Both controls gate: the opening one refuses to arm, and the **closing** one
marks the whole run suspect if the detector fires on nothing at the end though
it was quiet at the start. Thresholds are frozen from one baseline taken in a
pose the dome then walks away from, so specificity drift inside a run is the
expected failure rather than a hypothetical — and without a control at both
ends every reaction between them stays unadjudicated.

Two refusals sit in front of the operator's time and R2's motion:

- **Ceiling pre-flight.** `perform` refuses on tier, but only after
  calibration and the control have both run — 50 s of standing still to learn
  the daemon was launched at the wrong `--allow`. Worse, at `read` the LED
  writes are refused too, so D-014's arming cue silently never happens. The
  tier is now checked against `Beat.required_tier()` before anything starts.
- **Stalled-stream refusal.** A dead stream is the one failure this design
  cannot see by itself: the buffer keeps its last full window, every poll
  re-scores those same samples, and "quiet" comes back forever — so a settle
  completes and the beat performs on data that stopped arriving. Quiet is now
  undeclarable while nothing is being received, and the loop refuses to
  perform on a stalled feed. CLAUDE.md: prove the channel live before trusting
  silence.

#### OBSERVED — the mechanism

| Claim | Evidence |
|---|---|
| A stream disturbance drives a full behaviour with no human step | **3/3** triggers → 3 completed `express_curious` performances, desk run 2026-08-18 |
| The loop does not react to itself | 3/3 recoveries reached measured quiet; no reaction was ever triggered by the beat's own dome motion |
| The negative control gates arming | **3/3** live runs passed a 20 s hands-off control; a synthetic stuck detector refuses to arm (exit 5) |
| Event delivery is lossless at this rate | `dropped=0`, `decode_errors=0` across all live runs |
| The dome drift correction fires in real use | residual walked 4.44° → 9.41°, crossed the 12° floor, and was corrected back to 2.43° — D-013's mechanism working live for the first time, not just in tests |

Timing, desk run (3 reactions): settle **2.61–3.43 s**, beat **10.25–10.31 s**
(13.30 s on the run that included a drift correction), cooldown held
**16.35–16.37 s** of the beat's declared 20 s.

#### OBSERVED — the surface matters more than expected

Rest thresholds are a property of what he is standing on, not of the robot:

| channel | floor run 1 | floor run 2 |
|---|---|---|
| `r2_head_angle.r2_head_angle` | 1.7437 | 0.3170 |
| `gyroscope.x` | 0.5506 | 0.6413 |
| `gyroscope.y` | 0.5496 | 0.5485 |
| `attitude.pitch` | 0.1227 | 0.1134 |
| `gyroscope.z` | **0.0000** | **0.0000** |

Two findings, both load-bearing:

- **`gyroscope.z` reports a constant at rest** and yields a zero limit on both
  floor runs. A zero-limit channel is skipped, so 9 of 10 channels carry the
  two-channel corroboration rule, not 10.
- **`r2_head_angle`'s rest noise swung 5.5× between two consecutive runs on the
  same surface.** A threshold set once and reused is therefore wrong; the
  per-session calibration is not a convenience, it is required.

#### OBSERVED — a rickety desk is indistinguishable from a hand

The desk run's three triggers were **not** petting. Nobody was touching him.
The operator's desk transmits enough vibration to clear a rest threshold
calibrated on that same desk, which is worth stating plainly for a robot
meant to live in a household: **on an unstable surface he will react to people
walking past.** This is a specificity limit of the whole approach, not a bug in
the rubric — and it is why he was moved to the floor.

#### REFUTED — "those were false positives"

Asserted mid-session and wrong. Re-scoring 120 rest samples offline — deriving
thresholds from one half and sliding over the other — gives **0/55 firing
windows at a 1.5 s window, 0/49 at 3.0 s, 0/41 at 5.0 s.** The detector is
clean on quiet data; the desk was the signal source. Recorded because the wrong
explanation (a structural multiple-comparisons flaw) was the more sophisticated
one and fitted every fact available at the time.

The figures above are floor run 1's, which are re-derivable from
`.bridge/s2b-reactive-floor-run1.json`. The original analysis ran on the desk
run and returned the identical counts — the window totals depend only on the
sample count, and both runs captured 120 samples.

#### Provenance — the desk run's raw data was destroyed

`r2_reactive.py` wrote every run to one fixed filename, so **floor run 1
overwrote the desk run it was trying to explain.** The desk figures in the
tables above (3/3 triggers, the settle and beat timings, the residual walk)
come from the session's console output and **cannot be re-derived from stored
data.** Every other number in this section can. Fixed the same session — one
log per run — but the evidence for the headline result is weaker than it
should be, and saying so is cheaper than pretending otherwise.

#### OBSERVED — deliberate touch drives the loop

Settled 2026-08-18 by `monitor` first and a live run second, which is the order
that mattered: two earlier runs had returned a bare "0 reactions" and that
number cannot distinguish an untouched robot from a threshold too high to trip.
Both had zero touches in them — the first because the arming cue was invisible
(D-014), the second because the operator declined to continue.

**A pet is not a marginal signal.** 45 s of `monitor` with the operator petting
the dome, scoring 108 windows against a 20 s rest baseline:

| | |
|---|---|
| peak ratio against its rest limit | `gyroscope.x` at **157.9×** |
| channels over their limit at peak | **9** of 10 (the rule needs 2) |
| windows with ≥2 channels over | **51** of 108 |
| first corroborated window | t = 6.68 s — the moment the hand landed |

Detection was never the constraint. Nothing about the rubric needed loosening.

**The live run, at the S1e-validated 3.0 s window (12 samples at 4.00 Hz):**

| | |
|---|---|
| opening control | silent |
| reaction | t = 39.6 s, triggered on `attitude.pitch` |
| settle | **timeout at 6.1 s** — the disturbance never went quiet, so he answered at the cap |
| beat | `express_curious` completed in 10.30 s, residual 0.63° |
| recovery | quiet |
| closing control | **silent** |
| dropped / decode errors | 0 / 0 |

Controls silent at **both** ends, so the reaction between them is adjudicated
rather than assumed — the first on this project that is. Operator confirmed the
chirp *and* the dome turn, so the gesture rendered, not just the command.

**Two things the run says that the numbers alone do not:**

- **`settle=timeout` is the normal case for petting, not the exception.** A
  hand resting on the dome never produces the quiet the settle waits for, so
  the answer arrives at `SETTLE_MAX_S` (6 s) rather than ~2 s. The cap is
  therefore load-bearing: without it, being held would mean never being
  answered. It also means the response lands ~6 s after contact begins, which
  is a long time to hold attention — the operator registered the chirp first
  and needed prompting to confirm the dome.
- **A 1.5 s window did not survive its own control on this surface.** The first
  attempt refused to arm: the detector fired during the hands-off control. The
  3.0 s window passed both controls on the same surface minutes later. The
  shorter window buys ~1.5 s of latency at the cost of specificity, and on an
  imperfect surface it is the wrong trade.

#### The armed state is `listen`, not `idle`

The loop arms with **cyan on both PSIs** — the `listen` row of
`docs/behaviour-states.md`. It shipped as blue (`BASE_NEUTRAL`), correct under
D-012's original table, and that table's replacement reassigned blue steady to
`idle — nothing engaged`. An armed loop painted `idle` is unreadable against a
genuinely idle robot, which is D-014's own failure mode arriving by a different
route: the rule held, the colour moved. See **D-014 Amendment A**.

The disarm edge comes free of it — `_tidy` ends on `BASE_NEUTRAL`, so exiting
reads cyan → blue, `listen` → `idle`.

#### Method note

`--window` defaults to 1.5 s (6 samples at 4 Hz), which is **half** the 3.0 s
window S1e validated the rubric at. Shortened deliberately for responsiveness:
a corroborated detection cannot be faster than several samples, and several
samples at 4 Hz is the latency floor. The departure is adjudicated empirically
by the negative control before anything arms, and a control failure prints the
3.0 s fallback.

**That fallback has now fired in earnest.** On 2026-08-18 the 1.5 s window
failed its opening control and the 3.0 s window passed both controls on the
same surface minutes later. The default is left at 1.5 s because the control
gates it — the point of the design is that the operator never has to guess
which window is honest today — but **3.0 s is what has actually been proven to
hold**, and it is the right first choice on any surface that is not known
still.

## 4. Proposed semantic behavior vocabulary

Mapping the target states from the brief onto real capabilities. Status column:
`READY` = every ingredient traced; `NEEDS-SURVEY` = depends on resolving the
animation-id conflict; `DEFERRED` = requires locomotion.

> [!warning] The `NEEDS-SURVEY` status in this table is stale as written
> It means "blocked on the animation-id conflict", and that conflict is
> **adjudicated** as of 2026-08-17 (§3). Unblocking these rows did not make
> them ready — it moved the blocker. Two later findings gate them now:
>
> - **D-010** — authored animations are not a behavior library. 36 of 56 emit
>   `WADDLE` and only **20 are usable on a standing droid**, so every row whose
>   Animation candidate is an authored id needs that id re-checked against the
>   S1d safe set before it can be built.
> - **Two of the adjudicated labels are `inconclusive`**, and rows rest on them
>   directly: `wake()` on `EMOTE_ATTENTION` (id 9) and `look_around()` on
>   `EMOTE_SEARCH` (id 13). Neither name is confirmed. `celebrate()` is better
>   off than it looks — id 15's laugh **was** confirmed.
>
> This table is a **proposal from source reading**, not a verified plan. It is
> left standing as the S2 starting point; it should be rebuilt against the S1d
> safe set rather than trusted row by row.

| Behavior | Animation candidate | Sound family | Dome | Lights | Status |
|---|---|---|---|---|---|
| `idle()` | `IDLE_1/2/3` (robot-native idle does not exist — REFUTED) | `R2_CHATTY_1` — operator ruling; family is conversational, not neutral | slow small drift | logic **blink** (bit 3 is on/off; bit 7 for anything that fades) | READY |
| `sleep()` | — | — | 0°, hold | all off | READY |
| `thinking()` | — | **`R2_EXCITED_*`** — reads as "quick thinking / analyzing" | still | logic blink (bit 3) | **NEW from S1b** — covers the LLM round-trip wait, which had no sound before |
| `wake()` | `EMOTE_ATTENTION` | `R2_HEY_*` | ±20° travel from rest | logic on (bit 3), holo ramp up (bit 7) | NEEDS-SURVEY |
| `listen()` | — (composed) | — | small tilt, hold | holo on | READY |
| `express_curious()` | `WWM_CURIOUS` | `R2_CHATTY_*` rising | ±15° alternating, pause between | holo flicker | NEEDS-SURVEY |
| `express_happy()` | `WWM_HAPPY` | `R2_POSITIVE_*` | quick ±30° | front warm | NEEDS-SURVEY |
| `express_excited()` | ~~`WWM_EXCITED` / `EMOTE_EXCITED`~~ | ~~`R2_EXCITED_*`~~ | ~~fast sweep~~ | ~~front bright~~ | **RETIRED** — see `behaviour-states.md`; S1b refuted EXCITED as delight and `express_delight()` (#85) replaces it |
| `express_annoyed()` | `WMM_FRUSTRATED` | `R2_ANNOYED`, `R2_NEGATIVE_*` | sharp turn away | front red | NEEDS-SURVEY |
| `express_sad()` | `WWM_SAD` | `R2_SAD_*` | droop, slow | dim | NEEDS-SURVEY |
| `express_concerned()` | `WWM_CONCERN` | `R2_NEGATIVE_*` soft | slow scan | amber | NEEDS-SURVEY |
| `agree()` | `EMOTE_YES` | `R2_POSITIVE_*` | nod-ish dome bob | — | NEEDS-SURVEY |
| `disagree()` | `EMOTE_NO` / `WWM_NO` | `R2_NEGATIVE_*` | shake | — | NEEDS-SURVEY |
| `express_surprised()` | `WWM_DOUBLE_TAKE`, `WWM_SURPRISED` | `R2_EXCITED_*` | snap turn | flash | NEEDS-SURVEY |
| `express_scared()` | `WWM_SCARED` | `R2_SCREAM` | recoil | flicker | NEEDS-SURVEY |
| `suspicious()` | `WWM_OMINOUS` | `R2_CHATTY_*` low | slow deliberate scan | dim holo | NEEDS-SURVEY |
| `impatient()` | `WWM_JITTERY` | `R2_ALARM_*` | small repeated twitch | — | NEEDS-SURVEY |
| `celebrate()` | `WWM_LAUGH` / `EMOTE_LAUGH` | `R2_LAUGH_*` | spin | full colour | NEEDS-SURVEY |
| `complain()` | `WMM_FRUSTRATED` | `R2_NEGATIVE_*` | turn away | red | NEEDS-SURVEY |
| `look_around()` | `EMOTE_SCAN` / `EMOTE_SEARCH` | quiet | full sweep -90…+90 | holo on | NEEDS-SURVEY |
| `investigate()` | `WWM_CURIOUS` + approach | `R2_CHATTY_*` | track then hold | holo | DEFERRED |
| `approach()` / `retreat()` | `EMOTE_DRIVE` / `EMOTE_RETREAT` | `R2_STEP_*` | — | — | DEFERRED |
| `wander()` | composed | `R2_STEP_*` | — | — | DEFERRED |

Per-behavior metadata each entry will eventually carry (per the brief):
animation, sound family, head choreography, lighting, movement, **duration,
interruptibility, energy cost, cooldown**.

**INFERRED** — interruptibility and cooldown are what separate "alive" from
"twitchy". Two rules to design in from the start: a behavior must be
interruptible by a higher-priority event (so R2 reacts to you mid-idle), and
each behavior needs a per-family cooldown (so the same chirp cannot recur
within N minutes). Neither needs the cloud.

**REFUTED — this was mislabelled OBSERVED and the decision it framed is moot.**
It previously read: *"the robot ships with its own idle loop
(`enable_idle_animations`) … Recommendation: off during bring-up."* Both halves
are wrong. `enable_idle_animations` is rejected by this firmware (see the
callout at the top of this file), so idle **cannot** be turned off, and the
premise that R2 *has* a native idle loop rested entirely on the existence of a
command that does not work.

**UNKNOWN — does R2-D2 idle on his own at all?** Baseline 2026-08-16: over 30 s,
connected and awake, zero dome movement and zero unsolicited packets.
Suggestive, not conclusive. If he does idle, our behaviour engine has to
tolerate non-determinism it cannot switch off, and per-observation
`idle_contaminated` flagging (already in `r2_survey.py`) is the mechanism.

---

## 5. What is missing from the physical vocabulary

- **No display, no camera, no microphone on the robot.** All perception is
  IMU + head angle + our own backpack sensors.
- **Logic displays and holo projector are single-channel** — brightness, not
  colour. Only front/rear are RGB.
- **Sounds and animations are fixed assets.** We cannot author new R2 sounds;
  the expressive range is combinatorial (which sound, which dome move, which
  light, what timing), not generative. That is a hard constraint on how much
  "personality" can live in the robot versus in the timing.
