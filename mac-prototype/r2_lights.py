"""The LED animation player — docs/behaviour-states.md, made executable.

WHAT THIS IS
    A pure function of time. `sample()` says what a pattern shows at instant
    t; `frames()` turns a state into the (time, channels) writes that render
    it. Nothing here sleeps, connects, or sends. That is deliberate: the whole
    thing is dry-testable against synthetic time, so the frame maths is proven
    before a single packet leaves the Mac.

WHY A PLAYER AND NOT A LOOP
    The rate ceiling is 8.3 writes/s (`cmd_safe_interval` is 120 ms and R2
    drops commands sent faster). A naive render loop ticking at some fixed
    rate spends that budget on frames that change nothing. So the player
    computes the instants at which a channel actually CHANGES, and writes only
    there. A steady state costs zero writes per second, which is what makes an
    idle that runs for hours affordable.

    The same reasoning forces batching: if the front and back PSI change at
    the same instant, that is ONE write carrying both, not two. Merging by
    instant is not a tidiness measure, it is how the budget is met.

WHAT IT DELIBERATELY DOES NOT DO
    No fades on the PSIs. Every value change flickers on this hardware --
    R2's own LED update path, so it survives the ESP32 port -- which means an
    interpolated ramp reads as flicker rather than as a fade. Design discrete
    high-contrast frames. The holo (bit 7) is the one channel that dims
    cleanly, and it is where anything continuous belongs.
"""
from __future__ import annotations

from dataclasses import dataclass, field

from r2_behavior import (
    BASE_NEUTRAL, BASE_ENGAGED, BASE_SUCCESS, BASE_PENDING, BASE_DANGER,
    BASE_REST, STATUS_COLOURS,
    LED_FRONT_R, LED_FRONT_G, LED_FRONT_B, LED_LOGIC,
    LED_BACK_R, LED_BACK_G, LED_BACK_B, LED_HOLO,
)

# `cmd_safe_interval` in r2_probe.py. R2 drops commands sent faster than this,
# so it is a hardware property and not a tuning knob.
CMD_SAFE_INTERVAL_S = 0.12
MAX_WRITES_PER_S = 1.0 / CMD_SAFE_INTERVAL_S          # 8.333...

# A blink goes colour <-> DARK. Distinct from "off" as a level, because a
# fixture that is dark for half a period is still ON in the sense that it has
# a pattern; a fixture set to None is not participating at all.
DARK = "dark"

KINDS = ("steady", "blink", "alternate", "sweep")


@dataclass(frozen=True)
class Pattern:
    """One fixture's behaviour over time.

    `values` are whatever the fixture takes: an RGB triple for a PSI, an int
    level for the holo or the logic panel. The player never interprets them,
    which is what lets one implementation drive both.

    The four kinds are the vocabulary from D-012 Amendment A section 4, and
    three of them are ours:

      steady    one value held                      a status claim
      blink     value <-> dark                      escalation
      alternate value <-> value                     THE DROID'S OWN IDIOM
      sweep     ordered walk, ONE SHOT, then holds  a transition

    `alternate` is listed because the vocabulary is not ours to prune, not
    because we may spend it freely: at rest R2 alternates red/blue on the
    front uncommanded, so a STATUS layer that alternates cannot be told apart
    from him simply being himself. Expression may alternate; status may not.
    `StateLights.validate()` is where that is enforced.
    """
    kind: str
    values: tuple
    period_s: float = 0.0
    phase: float = 0.0
    scale: float = 1.0           # per-FIXTURE brightness, 0..1

    def __post_init__(self):
        if self.kind not in KINDS:
            raise ValueError(f"unknown pattern kind {self.kind!r}")
        n = len(self.values)
        if self.kind == "steady" and n != 1:
            raise ValueError("steady takes exactly one value")
        if self.kind == "blink" and n != 1:
            raise ValueError("blink takes one value; the other half is DARK")
        if self.kind == "alternate" and n != 2:
            raise ValueError("alternate takes exactly two values")
        if self.kind == "sweep" and n < 3:
            raise ValueError(
                "a sweep of fewer than 3 values is an alternate or a step; "
                "say which, because they cost different budgets and mean "
                "different things")
        if self.kind == "steady":
            if self.period_s:
                raise ValueError("a steady pattern has no period")
        elif self.period_s <= 0:
            raise ValueError(f"{self.kind} needs a period")
        if not 0.0 <= self.phase < 1.0:
            raise ValueError("phase is a fraction of a period, in [0, 1)")
        if not 0.0 < self.scale <= 1.0:
            # Zero is not a dim setting, it is "off", and off is a fixture set
            # to None. Conflating them hides a state that renders as nothing.
            raise ValueError("scale is a fraction in (0, 1]")

    @property
    def segments(self) -> int:
        """How many times a period this pattern changes what it shows.

        This IS its cost. Two for a blink (on, off) and for an alternate; one
        per member for a sweep; none at all for steady.
        """
        return {"steady": 0, "blink": 2, "alternate": 2,
                "sweep": len(self.values)}[self.kind]

    @property
    def writes_per_s(self) -> float:
        if self.kind == "steady":
            return 0.0
        if self.kind == "sweep":
            return 0.0          # one shot; it does not recur
        return self.segments / self.period_s

    def sample(self, t: float):
        """What this fixture shows at time t. `DARK` means off this half-beat.

        A sweep is ONE SHOT: it walks its members once and then holds the last
        one forever. That is what a transition means -- wake sweeps blue to
        cyan to green and then *stays* engaged. A looping sweep would be a
        state that never arrives anywhere.
        """
        if self.kind == "steady":
            return self.values[0]
        if self.kind == "sweep":
            i = int((t / self.period_s) + self.phase)
            return self.values[min(i, len(self.values) - 1)]
        q = ((t / self.period_s) + self.phase) % 1.0
        if self.kind == "blink":
            return self.values[0] if q < 0.5 else DARK
        return self.values[0] if q < 0.5 else self.values[1]

    def transitions(self, until_s: float) -> list[float]:
        """Every instant in [0, until_s) at which this pattern changes.

        NOTE the two meanings of `period_s`, which is the one place this class
        is not uniform and the one place it bit:

          blink / alternate  a full CYCLE; it changes twice per period
          sweep              the DWELL on each member; the walk takes
                             period_s * len(values) in total

        `sample()` has always read it the second way for sweeps. This method
        read it the first way, so a wake sweep produced three instants inside
        one dwell, every one of them sampling to the same colour, and the
        whole transition collapsed to a single frame. It rendered as "jump
        straight to blue and stay there" while reporting success.
        """
        if self.kind == "steady":
            return [0.0]
        if self.kind == "sweep":
            step, limit = self.period_s, self.period_s * len(self.values)
        else:
            step, limit = self.period_s / 2, float("inf")
        out, t = [], 0.0
        while t < until_s and t < limit:
            out.append(round(t, 6))
            t += step
        return out or [0.0]


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

FRONT_BITS = (LED_FRONT_R, LED_FRONT_G, LED_FRONT_B)
BACK_BITS = (LED_BACK_R, LED_BACK_G, LED_BACK_B)


def _psi_channels(bits, pattern, t: float, quiet: float) -> dict[str, int]:
    value = pattern.sample(t)
    rgb = (0, 0, 0) if value is DARK else value
    k = pattern.scale * quiet
    return {str(b): int(round(v * k)) for b, v in zip(bits, rgb)}


def _level_channels(bit: int, pattern, t: float, quiet: float) -> dict[str, int]:
    value = pattern.sample(t)
    level = 0 if value is DARK else value
    return {str(bit): int(round(level * pattern.scale * quiet))}


# ---------------------------------------------------------------------------
# States
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class StateLights:
    """One row of docs/behaviour-states.md, executable.

    `value` is the quiet-hours scale. It multiplies, never shifts hue: scaling
    a saturated corner keeps it saturated, so (0,255,0) -> (0,64,0) is still
    unambiguously green. Safety and privacy states are exempt and carry
    `dimmable=False`.
    """
    name: str
    front: Pattern
    back: Pattern
    holo: Pattern | None = None
    logic: Pattern | None = None
    sound_family: str | None = None
    status: bool = True          # False for expression, which may alternate
    dimmable: bool = True
    # Whether this state may be RESTORED on a fresh connect, or must be
    # re-derived. DEFAULT FALSE: restoring is the exception.
    #
    # The test is whether the claim is about the SYSTEM or about an
    # INTERACTION. A pending issue is still pending in the morning, so
    # `attention` restores. Listening, thinking and misheard are all claims
    # about an exchange in progress, and no exchange survives a disconnect --
    # restoring one asserts a conversation that is not happening. `waiting` is
    # about the backpack rather than a conversation, and is still not
    # restorable: the link is re-derived live on every connect, so a
    # remembered wait would assert a block that may have cleared. `danger`
    # and `offline` are live conditions we cannot vouch for: we do not know
    # he is still on the floor, and we know for a fact we are not offline
    # while talking to him.
    restorable: bool = False
    note: str = ""

    def fixtures(self):
        for f in (self.front, self.back, self.holo, self.logic):
            if f is not None:
                yield f

    @property
    def writes_per_s(self) -> float:
        """Sustained write rate, counting MERGED instants.

        Summing per-fixture rates is wrong and wrong in the dangerous
        direction: `danger` blinks front and back together at 0.25 s, which is
        8 writes/s as one merged stream and a budget-busting 16 if you add the
        fixtures up. A guard that over-counts rejects a state that renders
        perfectly, and the fix would be to slow the blink -- making the most
        urgent signal on the droid less urgent, to satisfy an arithmetic
        error.

        One-shot sweeps are excluded: they do not recur, so they cost nothing
        sustained. Their peak is checked separately.
        """
        recurring = [f for f in self.fixtures()
                     if f.kind in ("blink", "alternate")]
        if not recurring:
            return 0.0
        window = max(f.period_s for f in recurring) * 4
        instants = {t for f in recurring for t in f.transitions(window)}
        return len(instants) / window

    def validate(self) -> None:
        for psi, label in ((self.front, "front"), (self.back, "back")):
            for v in psi.values:
                if v not in STATUS_COLOURS:
                    raise ValueError(
                        f"state {self.name!r} paints {v} on the {label} PSI, "
                        f"which carries no status meaning. Every reachable "
                        f"corner is spent (D-012 Amendment A section 5); there "
                        f"is no colour left to invent a meaning for.")
            # The droid's own idiom. A status layer that alternates is
            # indistinguishable from R2 simply being himself at rest.
            if self.status and psi.kind == "alternate" and self.front is psi:
                if not self.note:
                    raise ValueError(
                        f"state {self.name!r} alternates on the front PSI, "
                        f"which is R2's own resting idiom. If that is "
                        f"deliberate, say why in `note`.")
        # The budget. This is the guard that a render loop would have needed
        # anyway, moved to construction where it costs nothing at runtime.
        if self.writes_per_s > MAX_WRITES_PER_S:
            raise ValueError(
                f"state {self.name!r} needs {self.writes_per_s:.1f} writes/s; "
                f"the ceiling is {MAX_WRITES_PER_S:.1f} and R2 DROPS the "
                f"excess, so this would render as something other than what "
                f"it says")

    def frames(self, until_s: float, *, value: float = 1.0
               ) -> list[tuple[float, dict[str, int]]]:
        """The writes that render this state over [0, until_s).

        Returns (timestamp, channels) merged BY INSTANT: two fixtures changing
        together are one write carrying both, which is how a state that would
        blow the budget as separate writes fits inside it.
        """
        if not self.dimmable:
            value = 1.0
        instants = sorted({t for f in self.fixtures()
                           for t in f.transitions(until_s)})
        out = []
        for t in instants:
            ch: dict[str, int] = {}
            ch.update(_psi_channels(FRONT_BITS, self.front, t, value))
            ch.update(_psi_channels(BACK_BITS, self.back, t, value))
            if self.holo is not None:
                ch.update(_level_channels(LED_HOLO, self.holo, t, value))
            if self.logic is not None:
                ch.update(_level_channels(LED_LOGIC, self.logic, t, value))
            if not out or ch != out[-1][1]:
                out.append((t, ch))
        return out


# ---------------------------------------------------------------------------
# The table
# ---------------------------------------------------------------------------
#
# docs/behaviour-states.md, row for row. Where a row here disagrees with that
# file, the file is right and this is a bug.
#
# The holo is the only channel that dims cleanly (255 -> 64 -> 16 measured),
# so anything continuous lives here. It is still STEPPED, not ramped: the step
# rate is bounded by the same 120 ms interval as everything else, and whether
# it still reads as smooth at that rate is UNMEASURED. See the module note in
# docs/behaviour-states.md.
_BREATHE = (16, 64, 160, 255, 160, 64)

STATES: dict[str, StateLights] = {}


def _add(state: StateLights) -> StateLights:
    state.validate()
    STATES[state.name] = state
    return state


_add(StateLights(
    name="idle",
    front=Pattern("steady", (BASE_NEUTRAL,)),
    back=Pattern("steady", (BASE_NEUTRAL,)),
    restorable=True,
    note="Blue because it is the DIMMEST corner -- 0.072 relative luminance, "
         "about a tenth of green -- and idle is the state that runs for "
         "hours. Costs zero writes/s, which is the other half of the same "
         "argument.",
))

_add(StateLights(
    name="wake",
    front=Pattern("sweep", (BASE_NEUTRAL, BASE_ENGAGED, BASE_SUCCESS), 0.45),
    back=Pattern("steady", (BASE_NEUTRAL,)),
    holo=Pattern("sweep", (16, 96, 255), 0.45),
    logic=Pattern("steady", (255,)),
    sound_family="R2_HEY_*",
    note="NO DOME. Operator ruling 2026-08-17: the sweep finishes in 1.35 s "
         "and every dome move takes ~2.0-2.2 s regardless of distance, so a "
         "dome move would still be travelling after the light had settled and "
         "the acknowledgement would land twice. 0.45 s per step is 6.7 "
         "writes/s at its peak, deliberately under the 8.3 ceiling; 0.36 s "
         "would sit exactly ON it.",
))

_add(StateLights(
    name="listen",
    front=Pattern("steady", (BASE_ENGAGED,)),
    back=Pattern("steady", (BASE_ENGAGED,)),
    holo=Pattern("sweep", _BREATHE, 2.2),
    logic=Pattern("blink", (255,), 0.9),
    note="The PSIs hold the claim and cost nothing; the holo and the logic "
         "panel carry the activity. That split is what lets a state run for "
         "an unbounded time inside the budget.",
))

_add(StateLights(
    name="thinking",
    front=Pattern("alternate", (BASE_ENGAGED, BASE_NEUTRAL), 0.9),
    back=Pattern("alternate", (BASE_ENGAGED, BASE_NEUTRAL), 0.9, phase=0.5),
    holo=Pattern("sweep", _BREATHE, 1.2),
    logic=Pattern("blink", (255,), 0.45),
    sound_family="R2_EXCITED_*",
    note="ALTERNATES ON THE FRONT, which is R2's own resting idiom, and that "
         "is a knowing exception. Two defences: the corners are cyan/blue "
         "where his are red/blue, and thinking is the one status state that "
         "is also mid-interaction. docs/behaviour-states.md flags this as the "
         "genuine gap in D-012's steady-vs-modulated axis -- it is a fixture "
         "answer to a semantic question, and it is still open.",
))

_add(StateLights(
    name="answering",
    front=Pattern("steady", (BASE_ENGAGED,)),
    back=Pattern("steady", (BASE_ENGAGED,)),
    holo=Pattern("steady", (255,)),
    logic=Pattern("steady", (255,)),
    note="The reply (operator ruling 2026-09-19, E2E v0 slice 3.0). The PSIs "
         "stay engaged cyan, the listen/think family; the holo and logic are "
         "held ON, where listen breathes and blinks them, so the two cyan "
         "states differ on the body. The feeling is carried by the reply's "
         "own chirp and dome turn, not by a hue: expression gets no colour. "
         "No sound family: the sound is whatever the reply chose. Not "
         "restorable -- it is a claim about an exchange.",
))

_add(StateLights(
    name="attention",
    front=Pattern("blink", (BASE_PENDING,), 2.4),
    back=Pattern("blink", (BASE_PENDING,), 2.4),
    restorable=True,
    note="Yellow, not red. D-012 assigned steady red to 'issue pending'; both "
         "IEC 60073 and every shipping consumer device put pending-attention "
         "on yellow and reserve red for danger.",
))

_add(StateLights(
    name="danger",
    front=Pattern("blink", (BASE_DANGER,), 0.25),
    back=Pattern("blink", (BASE_DANGER,), 0.25),
    sound_family="R2_ALARM_*",
    dimmable=False,
    note="The FASTEST thing on the droid, because it cannot be the brightest: "
         "red at full is 0.213 relative luminance against yellow's 0.928. "
         "Urgency rides on rate here or it does not ride at all. A blink and "
         "not a hold, because animation id 4 (the refusal) breaks its own "
         "alternation to hold red steady -- so a steady red is ambiguous "
         "between 'fault' and 'he just refused you'. Exempt from quiet hours.",
))

_add(StateLights(
    name="misheard",
    front=Pattern("blink", (BASE_PENDING,), 1.2),
    back=Pattern("blink", (BASE_PENDING,), 1.2),
    note="Recoverable, so yellow -- the same register as a pending issue, at "
         "twice the rate. 'Say it again' is a request for attention, not a "
         "fault report.",
))

_add(StateLights(
    name="offline",
    front=Pattern("blink", (BASE_DANGER,), 1.0),
    back=Pattern("blink", (BASE_DANGER,), 1.0),
    dimmable=False,
    note="Red at a quarter the rate of a physical fault. Both are faults; the "
         "rate says which, and only one of them means go and pick him up.",
))

_add(StateLights(
    name="waiting",
    front=Pattern("blink", (BASE_NEUTRAL,), 3.0),
    back=Pattern("steady", (BASE_NEUTRAL,), scale=0.25),
    note="Blocked on something the backpack has not produced yet -- a link "
         "that is up but unresponsive, a request still unanswered, a board "
         "still coming up. The back holds a dim steady claim while the front "
         "blinks once every three seconds: an acknowledgement, not a "
         "conversation, and what stops a wait reading as a dead droid. "
         "0.7 writes/s, so it can hold for hours.\n\n"
         "THIS WAS `withholding` -- deliberate silence, R2 choosing not to "
         "answer. OPERATOR RULING: he should never decide not to answer. That "
         "removed the state's only reason to exist and freed frames that were "
         "already the right shape for a wait: present, unmistakably alive, and "
         "not saying anything. Nothing was redesigned; the meaning was.\n\n"
         "Distinct from `thinking`, which is also a wait. Thinking looks BUSY "
         "-- holo breathing, logic blinking, an interaction in flight. Waiting "
         "looks PATIENT -- both dark, because he is not working on anything, "
         "he is blocked.\n\n"
         "Only renderable while the backpack can still reach him. A fully "
         "dead link cannot be announced on the droid by the thing that "
         "failed; that case needs an indicator on the backpack, and "
         "docs/research/board-capabilities.md records none.",
))

_add(StateLights(
    name="sleep",
    front=Pattern("steady", (BASE_NEUTRAL,), scale=0.2),
    back=Pattern("steady", (BASE_NEUTRAL,), scale=0.2),
    note="BLOCKED, not merely unimplemented: R2 cannot be put to sleep while "
         "the daemon holds the link, because the keepalive IS the wake "
         "command (DID 0x13 / CID 0x0D, every 3 s). Any sleep is undone "
         "within three seconds. Session-lifecycle change before it is a "
         "lighting one -- #38. The row is here so the language is complete, "
         "and it must not be read as available.",
))

BLOCKED = ("sleep",)


# ---------------------------------------------------------------------------
# Waking up
# ---------------------------------------------------------------------------
#
# MEASURED 2026-08-18: a colour we set survives a link drop and a fresh
# connect, but NOT a sleep cycle. Magenta was written at 23:52:47, confirmed on
# the droid, and 22.1 h later -- after he had slept -- he came back showing the
# firmware's own red/blue alternation with no trace of it.
#
# `CLAUDE.md` says the base layer is storage we own. It is, within a session.
# Across a sleep it is not, and nothing in this codebase re-established it, so
# the household would have seen R2's own resting idiom every morning no matter
# what the language said. Worse than wrong for idle: a droid left in
# `attention` came back showing nothing about it, and a pending issue silently
# stopped being pending.
#
# This is the LED equivalent of "default to STOP" -- the state after a
# discontinuity must be one we chose, not one we inherited.

DEFAULT_STATE = "idle"


def is_status_colour(rgb) -> bool:
    """True for a status corner, or that corner uniformly dimmed.

    Quiet hours scale VALUE and never hue, so (0,0,255) at 0.2 becomes
    (0,0,51) -- still unambiguously blue, still the same claim, and NOT a
    member of STATUS_COLOURS. Something had to know that a dimmed corner is
    the same colour as the corner.

    A scaled corner is recognisable without knowing the scale: the zero /
    non-zero PATTERN is preserved and every lit channel shares one value. That
    admits exactly the dimmed corners and rejects everything else -- (0,40,51)
    has unequal lit channels and is a colour nobody chose.

    Without this, `rest_colour()` had to hand beats a FULL-brightness colour to
    get past the beat validator, so every beat during quiet hours ended on a
    bright flash before the status re-assert dimmed it back. At 2am, which is
    the one thing quiet hours exist to prevent.
    """
    rgb = tuple(rgb)
    if rgb in STATUS_COLOURS:
        return True
    lit = {v for v in rgb if v}
    if len(lit) != 1:
        return False
    pattern = tuple(255 if v else 0 for v in rgb)
    return pattern in STATUS_COLOURS


def resume(remembered: str | None) -> tuple[str, str | None]:
    """What to show on a fresh connect, and what was dropped getting there.

    Returns `(state_to_assert, dropped)`. `dropped` is the remembered state
    when it could not be restored, so the caller can RE-DERIVE it rather than
    have it silently vanish -- which is the failure mode this whole function
    exists to prevent. Silently clearing a danger claim and silently asserting
    a stale one are both wrong; reporting it is not.

    Unknown or unparseable names fall back to the default rather than raising.
    A wake path that crashes on a corrupt store leaves R2 showing the firmware
    default, which is exactly the outcome being fixed.
    """
    if remembered is None:
        return DEFAULT_STATE, None
    state = STATES.get(remembered)
    if state is None or not state.restorable:
        return DEFAULT_STATE, remembered
    return remembered, None


def assertion(state_name: str, *, value: float = 1.0) -> dict[str, int]:
    """The ONE write that re-establishes a status colour on connect.

    A single frame, not a running animation: this is a bootstrap, and its job
    is that R2 is showing something we chose before anything else happens. The
    player takes over afterwards and animates from there.

    Taking frame zero is deliberate rather than incidental -- for a blink that
    is the lit half, so a state asserts itself visibly instead of starting on
    its own dark phase and reading as "off" until the first tick.

    ALL EIGHT BITS ARE WRITTEN, including an explicit zero for every fixture
    the state does not use. Emitting only the channels a state mentions leaves
    the others holding whatever was there before, and "whatever was there
    before" is precisely what an assertion exists to stop. Asserting `idle` --
    which uses neither the holo nor the logic panel -- would otherwise leave
    yesterday's holo glowing over it.
    """
    ch = dict.fromkeys(
        (str(b) for b in (*FRONT_BITS, *BACK_BITS, LED_HOLO, LED_LOGIC)), 0)
    ch.update(STATES[state_name].frames(0.001, value=value)[0][1])
    return ch
