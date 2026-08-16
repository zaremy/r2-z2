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
| Front LEDs | RGB, LED bits 0/1/2 | `r2d2.py:18-20` |
| Rear LEDs | RGB, LED bits 4/5/6 | `r2d2.py:22-24` |
| Logic displays | bit 3, **single channel** | `r2d2.py:21` — brightness only, not RGB |
| Holo projector | bit 7, **single channel** | `r2d2.py:25` |
| Audio | 388 sound ids + volume | `io.py:60-72` |
| Authored animations | 51 ids | `r2d2.py:417-468` |
| Idle animations | `enable_idle_animations(bool)` | `animatronic.py:73` — **the robot has its own idle loop** |

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

**Use only the `R2_*` family for the character.** BB-8/BB-9E sounds are a
different droid's voice and will break the illusion; the test tones are
factory diagnostics. That single filter cuts 388 down to a workable 212.

R2-native families:

| Family | Count | Reads as |
|---|---|---|
| `R2_CHATTY_*` | 62 | Neutral talking — the workhorse for idle/ambient |
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
| `idle()` | `IDLE_1/2/3`, or robot-native idle | `R2_CHATTY_*` (sparse, mode 1) | slow small drift | logic dim pulse | READY |
| `sleep()` | — | — | 0°, hold | all off | READY |
| `wake()` | `EMOTE_ATTENTION` | `R2_HEY_*` | 0° → ±20° → 0° | logic up | NEEDS-SURVEY |
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

**OBSERVED, and a decision we owe ourselves** — the robot ships with its own
idle loop (`enable_idle_animations`). Leaving it **on** gives free ambient life
but makes R2's behaviour non-deterministic and un-attributable; turning it
**off** means every motion is ours and legible in logs. Recommendation: off
during bring-up (so `docs/decisions.md` can record what caused what), then
re-evaluate as a *feature* once our own idle engine exists.

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
