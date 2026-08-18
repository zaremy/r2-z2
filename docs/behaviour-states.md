# Behaviour states — the one table everything implements against

**Status:** proposal, 2026-08-17. Supersedes nothing; *reconciles* two lists
that were drifting apart.

## Why this file exists

Two catalogues existed, on two different axes, with overlapping names and
conflicting definitions:

- **The LED language** (D-012 Amendment A) — idle, wake, listen, thinking,
  curious, excited, attention, danger, sleep. A **status** vocabulary: is he
  with me, is something wrong.
- **The behaviour table** (`research/r2-capabilities.md:1136`) — ~23
  `express_*()` rows. A **character-expression** catalogue.

Where the names overlap — curious, excited, thinking — the two disagreed on
colour, and three expression rows contradicted the LED ruling outright. This
file settles the relationship rather than picking a winner.

## The rule: status rests, expression passes through

They are not competing lists. They are **layers**, and the layering is already
latent in the code — `Beat` validates that every beat returns to a declared
rest colour (`mac-prototype/r2_behavior.py:356`).

- **The status layer is the resting state.** Exactly one is active at a time.
  It owns the PSI colour, and it is what the household sees when nothing is
  happening. An LED colour we set is state, not a command: it survives
  animations played over it, the link dropping, and a fresh connect.
- **Expression beats are transient.** They ride on top, run for seconds, and
  **must return to the status colour underneath.** A beat is not allowed to
  leave a colour behind.

### Consequence: expression does not get a colour

The seven reachable corners are **entirely spent on status** (Amendment A §5).
There is no eighth colour — blending needs 24-30 Hz against a 8.3 writes/s
ceiling — and no shade variant, because low saturation reads grey.

So **expression is carried by motion, sound, the holo (bit 7) and the logic
panel (bit 3), not by hue.** This is the rule that kills the three broken rows,
and it is a constraint, not a preference.

## Status states

Front and back PSI unless noted. Sound column is the family, not an id.

| state | front | back | holo (7) | logic (3) | sound | dome |
|---|---|---|---|---|---|---|
| **idle** | blue steady | blue steady | off | off | — | none |
| **wake** | sweep blue→cyan→green, 0.45 s | blue steady | ramp up | on | `R2_HEY_*` ✅ | **none** |
| **listen** | cyan steady | cyan steady | breathe 2.2 s | blink | — | small tilt, hold |
| **thinking** | cyan↔blue alt, 0.9 s | cyan↔blue alt | breathe 1.2 s | blink | `R2_EXCITED_*` ✅ | still |
| **attention** | yellow blink 2.4 s | yellow blink 2.4 s | off | off | **unassigned** | none |
| **danger** | red blink 0.25 s | red blink 0.25 s | off | off | `R2_ALARM_*` ✅ | none |
| **misheard** | yellow blink 1.2 s | yellow blink 1.2 s | off | off | **unassigned** | none |
| **offline** | red blink 1.0 s | red blink 1.0 s | off | off | **unassigned** | none |
| **withholding** | blue blink 3.0 s | blue steady @0.25 | off | off | — | none |
| **sleep** | blue steady @0.2 | blue steady @0.2 | off | off | — | none |

**Rate is the discriminator within a hue**, because luminance cannot carry
urgency here — red at full is 0.213 relative luminance, so danger can never be
the brightest thing on the droid (Amendment A §6). Yellow separates attention
(2.4 s) from misheard (1.2 s); red separates a physical fault (0.25 s) from a
lost link (1.0 s). Only one of those means go and pick him up.

> [!warning] **sleep** is specified and **blocked.** The keepalive *is* the wake
> command (`DID 0x13 / CID 0x0D`, every 3 s), so any sleep is undone within
> three seconds. Session-lifecycle change before it is a lighting one — #38.

> [!info] **wake has no dome move.** Operator ruling, 2026-08-17.
> The sweep finishes in 1.35 s; every dome move takes ~2.0-2.2 s regardless of
> distance (D-013), so the dome would still be travelling after the light had
> settled — the acknowledgement would arrive twice, late the second time.
> Wake is now **light and sound only**, which is also the cheapest state to
> enter and the one that fires most often.
>
> This drops `EMOTE_ATTENTION` (id 9) from wake, and with it one of the two
> `inconclusive` animation labels this table was resting on.

## Expression beats

Reconciled against the LED ruling and the dome measurements. Rows marked
**BROKEN** contradict a measurement and are not buildable as written.

| beat | was | now | why |
|---|---|---|---|
| `express_curious()` | front `PULSE_PALE` → `PULSE_CYAN`, ±15° dome, `R2_CHATTY_*` | **motion + sound + holo only**; no PSI change | `PULSE_PALE` is `(120,190,255)` — the exact pale blue Amendment A §1 cites as reading "almost grey". Shipped code uses the one colour proven not to work (`r2_behavior.py:137,704`). |
| `express_annoyed()` | front **red** | **BROKEN** — red is danger/stop only | Amendment A §5. Wrong in the dangerous direction: it makes red ambiguous. |
| `complain()` | **red** | **BROKEN** — same | as above |
| `express_concerned()` | **amber** | **BROKEN** — amber unreachable | Amendment A §1: no corner between red and yellow. |
| `express_excited()` | `R2_EXCITED_*`, "fast sweep" | **sound family wrong AND motion unbuildable** | S1b refuted `EXCITED` as delight — it reads as *cognition*, and `thinking()` already owns it. Excited and thinking would be indistinguishable by ear. Candidate: `R2_LAUGH_*` / `R2_POSITIVE_*`. Separately, no dome gesture can feel fast at a fixed ~2 s. |
| `express_surprised()` | "snap turn" | **BROKEN** — no snap exists | fixed-duration move, not a slew rate (D-013) |
| `impatient()` | "small repeated twitch" | **BROKEN** — below the floor | commanded travel under ~10.5° is silently ignored and returns `ok: true` (`research/r2-capabilities.md:1036`) |
| `idle()` | "slow small drift" | **BROKEN** — same floor | as above |
| `agree()` | `EMOTE_YES` | **hazard** | `EMOTE_YES` — a *nod* — emitted WADDLE three times and put R2 on the floor |

**The register the dome cannot do.** Twitches, bobs, drifts and snaps are all
gone: 12° minimum travel, ~2.0-2.2 s per move regardless of distance. Anything
that must read as *quick* has to come from lights or sound.

## Preconditions no row currently states

- **Authored animations drive leg actions.** Every animation-backed row is
  `stance` tier, not `dome` tier, and cannot be inspected before it runs.
- **R2 parks himself in bipod ~1 minute after the link drops.** Nothing may
  assume a tripod at wake.
- **The tripod is needed to MOVE, not to STAND.** Locomotion rows need an
  explicit stance step; bipod is a stable standing posture.
- **Audio onset is slower than the 0.12 s batch spacing**, so sound must be
  queued *before* the dome move it accompanies.
- **One animation label this table leans on is `inconclusive`** —
  `look_around()` on `EMOTE_SEARCH` (id 13). `EMOTE_ATTENTION` (id 9) was the
  other; dropping the dome from wake retired it.

## What this invalidates in code

`mac-prototype/r2_behavior.py`, all against fresh `main`:

1. **`BASE_PENDING = (255,0,0)`** (line 129) — Amendment A §5 moved pending to
   **yellow** and narrowed red to danger and stop.
2. **The rest-colour validator** (line 356) allows only the old trio, so a beat
   correctly resting on yellow is **rejected at construction**.
3. **`PULSE_PALE`** (line 137) is the grey-reading pale blue, used by
   `express_curious()` at line 704.
4. **Cyan is miscategorised and two colours are missing.** `PULSE_CYAN`
   (line 138) sits in the *expression* palette, but cyan is now a **status**
   colour — "engaged with you". Yellow and magenta have no constant at all.

## Unmeasured — this table rests on three assumptions

1. The holo still dims cleanly at a 120 ms step rate (measured only at rest).
2. The rate of the firmware's own alternation — ours is judged beside it.
3. Whether PWM duty is linear enough for the quiet-hours dim.

Each can invalidate part of this file. None needs more than an LED-tier session.
