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
| **listen** | cyan steady | cyan steady | breathe 2.2 s | blink | — | **none** — see below |
| **thinking** | cyan↔blue alt, 0.9 s | cyan↔blue alt | breathe 1.2 s | blink | `R2_EXCITED_*` ✅ | still |
| **answering** | cyan steady | cyan steady | on | on | the reply's own | the reply's, 12-45 deg or none |
| **attention** | yellow blink 2.4 s | yellow blink 2.4 s | off | off | **unassigned** | none |
| **danger** | red blink 0.25 s | red blink 0.25 s | off | off | `R2_ALARM_*` ✅ | none |
| **misheard** | yellow blink 1.2 s | yellow blink 1.2 s | off | off | **unassigned** | none |
| **offline** | red blink 1.0 s | red blink 1.0 s | off | off | **unassigned** | none |
| **waiting** | blue blink 3.0 s | blue steady @0.25 | off | off | — | none |
| **sleep** | blue steady @0.2 | blue steady @0.2 | off | off | — | none |

**Rate is the discriminator within a hue**, because luminance cannot carry
urgency here — red at full is 0.213 relative luminance, so danger can never be
the brightest thing on the droid (Amendment A §6). Yellow separates attention
(2.4 s) from misheard (1.2 s); red separates a physical fault (0.25 s) from a
lost link (1.0 s). Only one of those means go and pick him up.

> [!note] **sleep** is **conceptually unblocked by D-023 and still blocked in
> code.** Both halves are true and the distinction matters.
> The keepalive *is* the wake command (`DID 0x13 / CID 0x0D`, every 3 s), so any
> sleep we *send* is undone within three seconds — that part of #38 stands. What
> changed is the realisation that we do not need to send one: R2 has his own
> idle sleep and our keepalive is what suppresses it.
>
> **Assert `sleep` as the last write, then stop the keepalive.** He holds this
> dim blue while he is awake-and-released, and his own idiom returns once he
> goes under. A colour we set survives a link drop; it did **not** survive the
> one sleep cycle anybody watched — but `CLAUDE.md` is careful that **sleep is
> the likely destroyer, not the proven one**, since that 22.1 h window also
> contains duration. The goodnight is *expected* to lapse when he sleeps, and
> nothing depends on it lapsing for that particular reason.
>
> It is still a session-lifecycle change before it is a lighting one, and that
> change **does not exist yet**. The code still refuses this state on purpose:
> `r2_lights.BLOCKED = ("sleep",)` (`r2_lights.py:466`), `StatusLayer.set()`
> raises on it (`r2_status.py:207-210`), and two tests pin both facts
> (`test_r2_lights.py:192`, `test_r2_status.py:145`). **Nothing here unblocks
> them**, and it should not until there is a release path to assert it from —
> a state you can enter but never leave is worse than one you cannot enter.
>
> To actually unblock: build the release action (stop the keepalive), then in
> one change drop `sleep` from `BLOCKED`, assert it as the final write on that
> path, and update those two tests to pin the new behaviour rather than the old
> prohibition. The lifecycle change is a **subtraction**: stop sending, do not
> send more.

> [!info] **wake has no dome move.** Operator ruling, 2026-08-17.
> The sweep finishes in 1.35 s; every dome move takes ~2.0-2.2 s regardless of
> distance (D-013), so the dome would still be travelling after the light had
> settled — the acknowledgement would arrive twice, late the second time.
> Wake is now **light and sound only**, which is also the cheapest state to
> enter and the one that fires most often.
>
> This drops `EMOTE_ATTENTION` (id 9) from wake, and with it one of the two
> `inconclusive` animation labels this table was resting on.

> [!warning] **listen had "small tilt, hold" and that was unbuildable.**
> Commanded dome travel under ~10.5 deg is silently ignored and still reports
> `ok: true`. This file marks `idle()`'s "slow small drift" and
> `impatient()`'s "twitch" as BROKEN for exactly that reason and missed its own
> listen row. Either the tilt clears 12 deg -- at which point it is not small,
> and it costs ~2.0-2.2 s -- or listening carries no dome at all. Set to
> **none** pending an operator call.

> [!important] **`withholding` is gone. R2 never decides not to answer.**
> Operator ruling. The state meant *deliberate silence* — he heard you and
> chose not to reply — and that is not a thing this character does. Removing
> the meaning freed frames that were already the right shape for something
> else: present, unmistakably alive, and not saying anything.
>
> They are now **`waiting`** — blocked on something the backpack has not
> produced yet: a link that is up but unresponsive, a request still
> unanswered, a board still coming up. Nothing was redesigned; the meaning
> was.
>
> **It is distinct from `thinking`, which is also a wait.** Thinking looks
> BUSY — holo breathing, logic blinking, an interaction in flight. Waiting
> looks PATIENT — both channels dark, because he is not working on anything,
> he is blocked. If those two ever converge visually they are one state with
> two names.
>
> **It only renders while the backpack can still reach him.** A fully dead
> link cannot be announced on the droid by the thing that failed. That case
> needs an indicator on the backpack itself, and
> `research/board-capabilities.md` records none — so it is currently
> unsignalled, and that is a gap rather than a decision.

## This table is normative; the panel renders views of it

**Ruled 2026-09-06 (D-017 Amendment B, resolving #101's prerequisite P3).**
This file defines what states exist. The backpack panel is a *view* of them and
may not invent one.

The concrete case that forced the ruling: the panel spec listed
`offline_net`, `offline_r2` and `offline_llm` where this table has a single
**`offline`**. Those three are **display modes, not states**. The droid is
offline; the panel says which thing is unreachable. That is the split
`CLAUDE.md` already draws — the LED is glanceable and says *that* something is
up, the screen is the detail view and says *what* — so the distinction belongs
on the screen precisely because it is detail. As three states it would live in
the body, where nothing can render it.

Two naming corrections fell out of the same ruling:

- **`wake` is a state here; `waking` is a panel view** (D-023). One word apart,
  and most of why the two documents looked contradictory.
  **`waking` is bounded**: past `PANEL_WAKING_BOUND_MS` (4.1 s — P2's common
  2650 ms reconnect plus two missed ~500 ms scan windows, plus ~450 ms margin;
  `firmware/platform/panel_state/include/panel_state.h`)
  the panel shows `offline`, R2 view. D-023 leaves `waking` unranked because it
  "resolves itself in seconds"; the bound is what makes that true on a panel
  whose link never holds DOWN, where it had read `waking` for as long as he
  was gone.
- **`listen`, not `listening`.** The latter was the panel spec's own coinage.

So a panel may split one row of this table into several screens, and must not
add a row. If a state genuinely needs to exist that this file does not have,
it is added *here* first.

## On waking: assert, never inherit

**MEASURED 2026-08-18.** A colour we set survives a link drop and a fresh
connect. It does **not** survive a sleep cycle. Magenta was written at
23:52:47, confirmed on the droid by eye, and 22.1 h later -- after he had
slept -- he came back showing the firmware's own red/blue alternation with no
trace of it.

Nothing in the codebase re-established it, so the household would have seen
R2's own resting idiom every morning regardless of what this file said. Worse
than wrong for idle: a droid left in **attention** came back showing nothing
about it, and a pending issue silently stopped being pending.

**On connect, assert the status colour before anything else runs.** It is the
LED equivalent of *default to STOP* -- the state after a discontinuity must be
one we chose, not one we inherited. `r2_lights.assertion()` writes all eight
bits, explicitly zeroing every fixture the state does not use, because a
partial write leaves the rest holding exactly what the assertion exists to
displace.

### What may be restored, and what must be re-derived

The test is whether the claim is about the **system** or about an
**interaction**. Interactions do not survive a disconnect.

| state | restored? | why |
|---|---|---|
| **idle** | yes | trivially still true |
| **attention** | yes | a pending issue is still pending in the morning |
| listen, thinking, answering, misheard | no | claims about an exchange that has ended |
| waiting | no | the link is re-derived live on every connect |
| wake | no | an instant, not a state |
| danger | no | a live physical condition we cannot vouch for a day later |
| offline | no | demonstrably false — we are talking to him |
| **sleep** | **no** | it was the last thing we asserted before letting go, and by the time anything reconnects the claim is either stale or self-refuting — connecting *wakes him*, so a restored `sleep` would be false at the instant it was written (D-023) |

A dropped claim is **returned to the caller**, never swallowed. Silently
asserting a stale danger and silently clearing one are both wrong; reporting
it lets the brain re-derive it.

## Expression beats

Reconciled against the LED ruling and the dome measurements. Rows marked
**BROKEN** contradict a measurement and are not buildable as written.

| beat | was | now | why |
|---|---|---|---|
| `express_curious()` | front `PULSE_PALE` → `PULSE_CYAN`, ±15° dome, `R2_CHATTY_*` | **motion + sound + holo only**; no PSI change | `PULSE_PALE` is `(120,190,255)` — the exact pale blue Amendment A §1 cites as reading "almost grey". Shipped code uses the one colour proven not to work (`r2_behavior.py:137,704`). |
| `express_annoyed()` | front **red** | **BROKEN** — red is danger/stop only | Amendment A §5. Wrong in the dangerous direction: it makes red ambiguous. |
| `complain()` | **red** | **BROKEN** — same | as above |
| `express_concerned()` | **amber** | **BROKEN** — amber unreachable | Amendment A §1: no corner between red and yellow. |
| `express_excited()` | `R2_EXCITED_*`, "fast sweep" | **RETIRED** — superseded by `express_delight()` | S1b refuted `EXCITED` as delight: it reads as *cognition*, and `thinking()` already owns those ids. Never implemented in code; the name is retired rather than repaired, because "excited" and "delighted" were one row pretending to be two. The delight it was reaching for is `express_delight()` below. The "fast sweep" half was unbuildable regardless — no dome gesture can feel fast at a fixed ~2 s. |
| `express_delight()` | — | **BUILT** (#85) — `DELIGHT_SOUNDS` (`R2_LAUGH_1..4`), one dome gesture at 14°, holo ramp, logic on, **no PSI change** | The replacement, and the first behaviour whose *size* depends on state: `intensity` is the happiness deficit read before the touch is applied, so a starved R2 gets the full beat and a recently-petted one a brief acknowledgement. `EMOTE_LAUGH` (id 15) is a hardware-confirmed laugh and is still excluded — it is an *animation*, i.e. a stance command whose contents cannot be inspected before sending (D-010). A laugh cannot feel quick here and this one does not try. |
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
