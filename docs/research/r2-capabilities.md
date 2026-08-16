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
| Dome / head | `set_head_position(float°)`, `get_head_position()` | Range from `extended_sensors`: **-162° … +182°** (`r2d2.py:471`) |
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

### Sensors in

`extended_sensors` (`r2d2.py:470-477`): `r2_head_angle` (-162…182), gyroscope
x/y/z (±20000). Inherited from BB9E: accelerometer, orientation, locator.
Notifications: animation-complete, leg-action-complete, head-reset-to-zero,
battery-state-changed.

**INFERRED** — head angle + gyro + accelerometer is enough for a decent
"something happened to me" sense (picked up, bumped, tipped) without any
camera. That is the right first perception layer for a pet.

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

> [!danger] `R2_CHATTY_*` is NOT neutral talking — S1b, 2026-08-16
> The label called it *"neutral talking — the workhorse for idle/ambient"*.
> Five sampled ids, five distinct emotional readings, none of them neutral:
>
> | Id | Name | Reads as |
> |---|---|---|
> | 1950 | `CHATTY_1` | quick success |
> | 1959 | `CHATTY_10` | inquisitive, a question |
> | 1966 | `CHATTY_11` | an answer / completion |
> | 2007 | `CHATTY_15` | "huh?" — surprise |
> | 2010 | `CHATTY_16` | "well, that's disappointing" |
>
> **Consequence for idle.** These are conversational turn-shapes. A question
> implies someone to ask; an answer implies something was asked. Played into an
> empty room they read as R2 talking to nobody, or expecting a reply he will
> not get — the opposite of comfortable ambient presence. **The largest family
> in R2's vocabulary cannot be the idle workhorse**, and `## 4`'s behavior table
> needs a different source for `idle()`.
>
> Where they ARE valuable: interaction. Emotionally-loaded conversational
> fragments are exactly right for back-and-forth with a person.
>
> **UNKNOWN — do nearby ids cohere as exchanges?** Suggestive, under-powered,
> recorded so it is not lost. 1959+1966 (7 apart) and 2007+2010 (3 apart) each
> read as a coherent exchange **in either order** — so there is no
> question→answer direction. But 1950+1959 (9 apart) and 1950+2132 (182 apart)
> both read generic, so distance alone does not explain it; `CHATTY_1` looks
> like a tonal outlier. Four pairs is not a rule. Worth re-testing with a
> wider sample if dialogue design needs it.
>
> **UNKNOWN — do the id gaps encode clip length?** Ids are spaced 3-22 apart,
> not sequentially. If the gap is duration, we get lengths for all 212 sounds
> without playing them, which is what S1d needs for event-backed choreography
> timing. Untested.

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
| `R2_EXCITED_*` | 16 | High-energy delight |
| `R2_ALARM_*` | 15 | Warning, urgency |
| `R2_HEY_*` | 12 | Attention-getting, greeting |
| `R2_HIT_*` | 11 | Reaction to impact |
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

Four groups (`r2d2.py:417-468`):

| Group | Ids | Meaning |
|---|---|---|
| `CHARGER_1…7` | 0-6 | Charging-dock sequences |
| `EMOTE_*` | 7-24 | 16 authored emotional beats |
| `IDLE_1…3` | 25-27 | Ambient idle |
| `WWM_*` | 31-54 | 23 "Watch With Me" reaction animations |
| `MOTOR` | 55 | Motor sound/behaviour |

Ids **20, 23, 28, 29, 30 are absent** from the enum. UNKNOWN whether they are
invalid, or valid-but-unnamed. Probe them and watch for error `0x02`
(bad_command_id) / `0x07` (bad_parameter_value).

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

### ⚠️ Live conflict: animation IDs

`claude-r2d2-buddy/main/translator.c:93-108` labels the low ids from
experimentation and **disagrees with spherov2**:

| Id | spherov2 (`r2d2.py`) | translator.c |
|---|---|---|
| 0 | `CHARGER_1` | "blinks red/blue — avoid" |
| 3 | `CHARGER_4` | `ANIM_ALERT` (impatient) |
| 4 | `CHARGER_5` | `ANIM_SAD` (denied) |
| 7 | `EMOTE_ALARM` | `ANIM_HAPPY` (approved) |
| 9 | `EMOTE_ATTENTION` | `ANIM_WIGGLE` |
| 13 | `EMOTE_SEARCH` | `ANIM_CHATTY` |
| 15 | `EMOTE_LAUGH` | `ANIM_HAHAHA` ✓ |

Only id 15 agrees. **Do not resolve this by reasoning.** spherov2's names very
likely came from the official app's asset table and are more trustworthy, but
`translator.c` was written by someone watching a real droid. Possible
explanations: firmware-version differences, an off-by-N in one table, or
`translator.c` simply guessing. **Resolution requires playing each id on our
unit and writing down what it does.** That survey is the single highest-value
first hardware session — see `initial-findings.md`.

---

## 4. Proposed semantic behavior vocabulary

Mapping the target states from the brief onto real capabilities. Status column:
`READY` = every ingredient traced; `NEEDS-SURVEY` = depends on resolving the
animation-id conflict; `DEFERRED` = requires locomotion.

| Behavior | Animation candidate | Sound family | Dome | Lights | Status |
|---|---|---|---|---|---|
| `idle()` | `IDLE_1/2/3` (robot-native idle does not exist — REFUTED) | `R2_CHATTY_*` (sparse, mode 1) | slow small drift | logic **blink** pattern (bit 3 is on/off; use bit 7 for anything that fades) | READY |
| `sleep()` | — | — | 0°, hold | all off | READY |
| `wake()` | `EMOTE_ATTENTION` | `R2_HEY_*` | ±20° travel from rest | logic on (bit 3), holo ramp up (bit 7) | NEEDS-SURVEY |
| `listen()` | — (composed) | — | small tilt, hold | holo on | READY |
| `express_curious()` | `WWM_CURIOUS` | `R2_CHATTY_*` rising | ±15° alternating, pause between | holo flicker | NEEDS-SURVEY |
| `express_happy()` | `WWM_HAPPY` | `R2_POSITIVE_*` | quick ±30° | front warm | NEEDS-SURVEY |
| `express_excited()` | `WWM_EXCITED` / `EMOTE_EXCITED` | `R2_EXCITED_*` | fast sweep | front bright | NEEDS-SURVEY |
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
