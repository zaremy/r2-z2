"""r2_behavior.py — the choreography layer. First composition of dome + sound
+ light into a single semantic beat.

WHERE THIS SITS
    brain -> behavior -> CHOREOGRAPHY -> r2/commands -> r2/protocol -> r2/ble
                         ^^^^^^^^^^^^
    The reasoning layer calls `express_curious()`. It never calls
    `set_head_position(-27)`. This file is the whole of that translation for
    the Mac prototype; nothing above it exists yet.

WHY THE SHAPE IS WHAT IT IS — the bridge is SERIAL
    The daemon's queue loop is `for req in reqs: await handle_request(...)`
    (r2_probe.py:1443). One request finishes before the next begins. There is
    no way to make two channels fire at the same instant from out here, so
    "choreography" cannot mean parallel tracks.

    Two facts rescue it:

    1. `sorted(REQ_DIR.glob("*.json"))` drains everything already queued
       back-to-back, skipping the 0.2 s idle poll. Writing a phrase's files in
       ONE batch therefore paces its steps at CMD_SAFE_INTERVAL (0.12 s)
       rather than the ~0.39 s measured floor of one `./r2 send` round-trip.
    2. `_op_dome` with `settle=0` returns as soon as the move is COMMANDED.
       The dome is still physically turning while the next op runs. The
       overlap is mechanical, not concurrent — motion outlives its command.

    So: steps inside a Phrase are batch-written and read as one gesture; gaps
    BETWEEN phrases are real pauses, and are the only timing we actually
    control. Any beat wanting a deliberate pause must split phrases there.

WHAT THIS DELIBERATELY DOES NOT DO
    No `animation`, no `set_stance`. Both sit at the `stance` tier because an
    authored animation is a leg command whose contents cannot be inspected
    before sending, and EMOTE_YES — a *nod* — put R2 on the floor (#11).
    Every beat here is composed from primitives and tops out at `dome`.
"""

from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass
from pathlib import Path

from r2_assets import R2_SOUNDS

# Mirrors r2_probe.py:177-180. Duplicated rather than imported because
# importing r2_probe drags in bleak and the BLE stack, and this module must be
# testable — and dry-runnable — with no radio present.
LED_FRONT_R, LED_FRONT_G, LED_FRONT_B = 0, 1, 2
LED_LOGIC = 3
LED_BACK_R, LED_BACK_G, LED_BACK_B = 4, 5, 6
LED_HOLO = 7

# The permission ladder, low to high (D-009). `motion` is a deprecated alias
# that resolves DOWN to `dome`; it is not a rung.
TIERS = ("read", "leds", "audio", "dome", "stance")

# MEASURED 2026-08-17, n=4 over 8-22 degrees of travel. A dome move takes
# ~2.0-2.2 s REGARDLESS of distance — 8.35 deg took 2.19 s and 22.61 deg took
# 2.07 s — so this is a fixed-duration move, not a slew rate. Anything issued
# into that window is SILENTLY DISCARDED: `ok: true`, no error, no motion.
# Two beats drifted -7.7 and -7.4 degrees per invocation before this was found.
DOME_MOVE_S = 2.2

# MEASURED, same run: every move stops ~2.4-3.7 deg SHORT of its commanded
# target, in the direction of travel. Relative deltas therefore never sum back
# to zero, which is why a "balanced" gesture still walks the dome.
DOME_UNDERSHOOT_DEG = 3.0

# MEASURED 2026-08-17. THE DOME CANNOT MAKE SMALL MOVEMENTS. Commanded travel
# below ~10.5 deg is SILENTLY IGNORED — the firmware answers `ok: true`, the
# response carries a plausible `commanded` value, and nothing moves.
#
#     4.0 -> 0.11    6.0 -> 0.00    8.0 -> 0.00   10.0 -> -0.11   (no motion)
#    10.5 -> 7.08   11.0 -> 7.71   12.0 -> 8.45   14.0 -> -10.46  (motion)
#
# The boundary lies between 10.0 and 10.5 with n=1 either side, so 12.0 is the
# working minimum: a measured edge is not a safe constant to sit on.
#
# This was first misdiagnosed as "a move issued while another is still
# travelling gets dropped" — a hypothesis that fitted n=2 and was refuted by a
# correction move that failed with 2.2 s of clear air in front of it. Recorded
# because the wrong explanation was the more interesting one, and it survived
# two runs before the boundary test killed it.
#
# Design consequence, and it is not small: a subtle 5 deg tilt is NOT
# available on this hardware. Every dome gesture must be at least 12 deg, so
# "small dome movement" in the bring-up order means 12 deg, not 3.
MIN_DOME_TRAVEL_DEG = 12.0

# Above this deficit a touch gets the full beat; below it, a brief
# acknowledgement. One number, named, because #85 AC1 asserts the two sides
# differ and a magic literal in the middle of a beat is not a decision anyone
# can find later.
FULL_DELIGHT_INTENSITY = 0.5

# Which tier each op we are allowed to emit needs. A subset of r2_probe.OPS on
# purpose: this layer may not reach for `animation` or `set_stance`, and the
# safest way to express "may not" is to have no name for them.
OP_TIER = {
    "leds": "leds",
    "sound": "audio",
    "stop_audio": "audio",
    "dome": "dome",
    "head": "read",
    "stance": "read",
    "stop": "read",
    # Read-tier additions for the S1e sensor probe (#29). `events` drains the
    # notification ring and `sensors` configures a notification stream; neither
    # can move him, which is the whole test for this tier.
    "events": "read",
    "sensors": "read",
}

# Ops that can end with R2 on the floor. Named explicitly so the guard below
# is a positive assertion about a known list, not the absence of a match.
FORBIDDEN_OPS = ("animation", "set_stance")


def tier_rank(tier: str) -> int:
    return TIERS.index(tier)


# ---------------------------------------------------------------------------
# Colour
# ---------------------------------------------------------------------------

# D-012 Amendment A: the LED base layer is system truth, and the palette is the
# SEVEN CORNERS OF THE RGB CUBE. There is no eighth colour and no shade
# variant: low saturation reads grey, and blending would need 24-30 Hz against
# a 8.3 writes/s ceiling. Six of the corners are spent on status.
BASE_NEUTRAL = (0, 0, 255)      # blue    — idle, nothing engaged
BASE_ENGAGED = (0, 255, 255)    # cyan    — engaged with you
BASE_SUCCESS = (0, 255, 0)      # green   — wake-sweep terminus, success
BASE_PENDING = (255, 255, 0)    # yellow  — needs monitoring
BASE_DANGER  = (255, 0, 0)      # red     — danger and stop, ONLY
BASE_REST    = (255, 0, 255)    # magenta — rest, low power

# The colours a beat may come to rest on. Every one of them is a status claim,
# because rest IS the status layer showing through.
STATUS_COLOURS = (BASE_NEUTRAL, BASE_ENGAGED, BASE_SUCCESS,
                  BASE_PENDING, BASE_DANGER, BASE_REST)

# There is no expression palette, and that is a finding rather than an
# omission. All six reachable corners carry status, so a beat that invented a
# seventh meaning would be overloading one that already has one. **Expression
# is carried by motion, sound, the holo (bit 7) and the logic panel (bit 3) —
# never by hue.** See docs/behaviour-states.md.
#
# The deleted `PULSE_PALE = (120,190,255)` is why this rule is stated so
# flatly: it was the expression colour, and it is the exact pale blue
# Amendment A cites as its evidence that low saturation reads grey. The one
# beat ever built painted the one colour measured not to work.


def front(rgb: tuple[int, int, int]) -> dict[str, int]:
    r, g, b = rgb
    return {str(LED_FRONT_R): r, str(LED_FRONT_G): g, str(LED_FRONT_B): b}


def back(rgb: tuple[int, int, int]) -> dict[str, int]:
    r, g, b = rgb
    return {str(LED_BACK_R): r, str(LED_BACK_G): g, str(LED_BACK_B): b}


def holo(level: int) -> dict[str, int]:
    """Holo projector, bit 7. BRIGHTNESS ONLY — it has no colour (r2d2.py:17-25).
    Same for the logic displays on bit 3. Writing a triple to either is a
    category error that the daemon will happily accept and R2 will render as
    whatever the last of the three writes happened to be."""
    return {str(LED_HOLO): level}


def logic(level: int) -> dict[str, int]:
    return {str(LED_LOGIC): level}


# ---------------------------------------------------------------------------
# Sound selection
# ---------------------------------------------------------------------------

class SoundPool:
    """A curated pool of sound ids with recent-pick tracking.

    NOT a family. The Behavior Library rule says "pick sounds from a family
    with recent-pick tracking, never a fixed id" — and S1b REFUTED the
    assumption underneath it. `R2_CHATTY_*` has 62 members, and the five that
    were actually heard read as five DIFFERENT things: success, inquisitive,
    answer/completion, "huh?", disappointment. Picking uniformly from the
    family name would let `express_curious()` say "disappointment".

    So a pool is an explicit, verified list. Unheard ids do not go in one. The
    cost is that pools start tiny (two ids for curious) and R2 repeats himself
    until #9 finishes the remaining 15 of 40; the alternative is a character
    that occasionally says the wrong thing, which is worse than one that
    occasionally repeats.
    """

    def __init__(self, name: str, ids: list[int], *, evidence: str):
        if not ids:
            raise ValueError(f"sound pool {name!r} is empty")
        self.name = name
        self.ids = list(ids)
        self.evidence = evidence
        self._recent: list[int] = []

    def pick(self, *, avoid_last: int = 2) -> int:
        """Least-recently-used, deterministic. Deterministic on purpose: a
        behaviour that picks randomly cannot be dry-tested, and `Math.random`
        in a choreography engine means a bug reproduces one run in eight."""
        window = self._recent[-avoid_last:] if avoid_last else []
        fresh = [i for i in self.ids if i not in window] or list(self.ids)
        choice = fresh[0]
        self._recent.append(choice)
        return choice


def sound_id(name: str) -> int:
    """Public alias for `_sid`. Other modules need to name a sound, and
    reaching across a module boundary for an underscore name is a private API
    by accident rather than by design."""
    return _sid(name)


def _sid(name: str) -> int:
    """Look up a sound id by name, failing loudly. A typo'd name silently
    becoming `None` would reach `int(p["id"])` in the daemon and raise there,
    one layer too late to say which behaviour was at fault."""
    if name not in R2_SOUNDS:
        raise KeyError(f"{name!r} is not an R2_* sound id")
    return R2_SOUNDS[name]


# The two CHATTY ids whose heard reading is curiosity-shaped. CHATTY_11 read
# as "inquisitive", CHATTY_15 as '"huh?"'. The other three sampled members
# (1 success, 10 answer, 16 disappointment) are deliberately excluded.
CURIOUS_SOUNDS = SoundPool(
    "curious",
    [_sid("R2_CHATTY_11"), _sid("R2_CHATTY_15")],
    evidence="OBSERVED S1b — heard as inquisitive / 'huh?'; "
             "the other 60 CHATTY ids are UNHEARD and excluded",
)

# EXCITED_1/10/11 were heard as "quick thinking / analyzing / quick reply".
# The family label ("high-energy delight") was REFUTED, and the true reading
# is cognition — which is exactly the LLM round-trip wait.
THINKING_SOUNDS = SoundPool(
    "thinking",
    [_sid("R2_EXCITED_1"), _sid("R2_EXCITED_10"), _sid("R2_EXCITED_11")],
    evidence="OBSERVED S1b — heard as quick thinking / analyzing / quick reply",
)

# Delight. The EXCITED family was the obvious candidate and S1b REFUTED it:
# those ids read as cognition, and `thinking()` above already owns them.
# Using EXCITED here would make delight and deliberation indistinguishable by
# ear, which is worse than having no delight beat at all.
#
# All four LAUGH ids go in because the family name and the reading agree for
# once. Four is also the smallest pool that can satisfy #85 AC7 — four
# consecutive fires, four different ids — and it only does so if the caller
# asks for `avoid_last=3`. The default of 2 yields [1, 2, 3, 1]: the fourth
# fire repeats the first, and a laugh that repeats every fourth time reads as
# a recording rather than a reaction.
DELIGHT_SOUNDS = SoundPool(
    "delight",
    [_sid("R2_LAUGH_1"), _sid("R2_LAUGH_2"),
     _sid("R2_LAUGH_3"), _sid("R2_LAUGH_4")],
    evidence="family label and heard reading agree; EMOTE_LAUGH (id 15) is "
             "hardware-confirmed as a laugh but is an ANIMATION and excluded "
             "(D-010) — these are sound ids, not stance commands",
)


# ---------------------------------------------------------------------------
# Beat structure
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Step:
    """One bridge op. `params` is passed through untouched."""
    op: str
    params: dict

    def tier(self) -> str:
        if self.op in FORBIDDEN_OPS:
            raise ValueError(
                f"op {self.op!r} is a stance-tier command and the choreography "
                f"layer may not emit it: an authored animation drives leg "
                f"actions and can fell him (#11). Compose from primitives.")
        if self.op not in OP_TIER:
            raise ValueError(f"unknown op {self.op!r}")
        return OP_TIER[self.op]


@dataclass(frozen=True)
class Phrase:
    """Steps that must read as ONE gesture, plus the pause that follows.

    Everything in `steps` is batch-written, so the daemon drains it
    back-to-back at ~0.12 s and R2 renders it as a single motion. `gap_s` is
    the only timing this layer genuinely controls — see the module docstring.
    """
    steps: tuple[Step, ...]
    gap_s: float = 0.0

    def __post_init__(self):
        if self.gap_s < 0:
            raise ValueError("gap_s must be >= 0")


@dataclass
class Beat:
    """A semantic behaviour, fully specified.

    The metadata fields are not decoration. The Behavior Library requires
    every behaviour to carry duration, interruptibility, energy and cooldown,
    because those four are what let the brain schedule R2 without making him
    twitchy — and none of them need the cloud.
    """
    name: str
    phrases: tuple[Phrase, ...]
    rest_colour: tuple[int, int, int]
    interruptible: bool
    energy: str                     # low | medium | high
    cooldown_s: float
    evidence: str
    notes: str = ""
    # Close the beat by driving the dome back to the ABSOLUTE angle it started
    # at. Without this a beat drifts by its accumulated undershoot every single
    # time it runs, and nothing ever corrects it because the dome has no
    # resting position. Measured drift before this existed: ~7.5 deg per
    # invocation, i.e. 150 deg after twenty curious beats.
    return_to_start: bool = False

    def required_tier(self) -> str:
        """The lowest daemon ceiling that can run this beat. Computed from the
        steps rather than declared, so it cannot drift away from what the beat
        actually does."""
        tiers = [s.tier() for p in self.phrases for s in p.steps]
        return max(tiers, key=tier_rank) if tiers else "read"

    def estimated_duration_s(self) -> float:
        """Lower bound: inter-step pacing plus the declared gaps. Real
        duration is longer — a sound plays for as long as it plays, and S1b
        found id-gap spacing predicts duration only as a bucket, not a value.
        Treat this as "not shorter than", never as a schedule."""
        steps = sum(len(p.steps) for p in self.phrases)
        gaps = sum(p.gap_s for p in self.phrases)
        # The LAST dome move has no gap after it inside the phrase list, and
        # return_to_start adds two more DOME_MOVE_S waits plus three head
        # reads. Ignoring both under-reported express_curious as 5.51 s
        # against a measured 9.98 s, which is not a lower bound anyone can
        # plan against.
        # Count ONLY what always happens: one settle wait and two head reads.
        # Do NOT add a separate tail for the last dome move -- that wait IS
        # the closing settle -- and do NOT assume the correction fires, since
        # it usually does not. Counting both produced 12.47 s against a
        # measured 9.98 s, i.e. an over-estimate, which breaks the lower-bound
        # contract this method exists to keep.
        closing = (DOME_MOVE_S + 2 * 0.12) if self.return_to_start else 0.0
        return round(steps * 0.12 + gaps + closing, 3)

    def validate(self) -> None:
        """Every structural guarantee this layer makes, asserted in one place.

        Called by the constructor helpers below AND by the dry test, so a beat
        cannot be built wrong and discovered wrong on hardware."""
        if not self.phrases:
            raise ValueError(f"beat {self.name!r} has no phrases")
        for p in self.phrases:
            for s in p.steps:
                s.tier()                       # raises on forbidden/unknown
        # A dome move needs ~2.2 s to land, and a second move issued into that
        # window is discarded with no error at all. A gap too short is
        # therefore not a timing imperfection — it deletes a step, and the
        # deletion is invisible in the responses. Checked here so the mistake
        # cannot reach hardware, since hardware reports it as success.
        for i, p in enumerate(self.phrases[:-1]):
            if any(s.op == "dome" for s in p.steps) and p.gap_s < DOME_MOVE_S:
                raise ValueError(
                    f"beat {self.name!r} phrase {i} moves the dome then waits "
                    f"only {p.gap_s}s; a move takes ~{DOME_MOVE_S}s and the "
                    f"next one would be silently dropped")
        # Sub-threshold moves are the sharper failure, because the daemon
        # reports them as successes. A beat asking for an 8 deg tilt is not
        # slightly imprecise — it does nothing at all, and says it worked.
        for i, p in enumerate(self.phrases):
            for s in p.steps:
                if s.op == "dome" and "delta" in s.params:
                    if abs(s.params["delta"]) < MIN_DOME_TRAVEL_DEG:
                        raise ValueError(
                            f"beat {self.name!r} phrase {i} commands "
                            f"{s.params['delta']:+.1f} deg; anything under "
                            f"{MIN_DOME_TRAVEL_DEG} deg is silently ignored by "
                            f"the firmware and still reports ok")
        if tier_rank(self.required_tier()) >= tier_rank("stance"):
            raise ValueError(f"beat {self.name!r} reaches the stance tier")
        # Accepts a status corner OR that corner uniformly dimmed: quiet
        # hours scale value and never hue, and a dimmed blue is still blue.
        # Requiring an exact corner forced the status layer to hand beats a
        # full-brightness rest during quiet hours, so every beat ended on a
        # bright flash. Imported lazily -- r2_lights imports this module.
        from r2_lights import is_status_colour
        if not is_status_colour(self.rest_colour):
            raise ValueError(
                f"beat {self.name!r} rests on {self.rest_colour}, which is not "
                f"a D-012 status colour. The colour we leave him in is what "
                f"the household sees until something changes it.")
        # The last thing a beat does must be to restore the base layer.
        # An LED colour we set is STATE and survives the link dropping, so a
        # beat that ends mid-expression leaves R2 permanently mid-expression.
        # (And a beat that ends dark is indistinguishable from a dead link —
        # that exact confusion cost a session several minutes.)
        last = self.phrases[-1].steps[-1]
        if last.op != "leds":
            raise ValueError(
                f"beat {self.name!r} ends with {last.op!r}, not a colour "
                f"reset; every beat must return to its rest colour")
        want = front(self.rest_colour)
        if {k: v for k, v in last.params["channels"].items()
                if k in want} != want:
            raise ValueError(
                f"beat {self.name!r} does not end on its declared rest colour")
        if self.energy not in ("low", "medium", "high"):
            raise ValueError(f"beat {self.name!r} has energy {self.energy!r}")


# ---------------------------------------------------------------------------
# The bridge
# ---------------------------------------------------------------------------

class Bridge:
    """Writes request files and reads responses. The real one and the fake one
    share this interface so a beat can be flown end-to-end with no robot.

    `send_batch` is deliberately CONCRETE and final-ish: it guards, then
    delegates to `_send`. Subclasses override `_send`, never `send_batch`.
    """

    def send_batch(self, steps, timeout: float = 12.0) -> list[dict]:
        """Guard THEN send.

        The forbidden-op check used to live only in `Beat.validate()`, i.e. on
        the CONSTRUCTION path. A guard on the construction path is bypassed by
        any caller that skips construction — `bridge.send_batch([Step(
        "animation", {"id": 21})])` reached the daemon queue with nothing in
        this module objecting, while D-013 claimed those ops were
        "unrepresentable". They were merely unrepresentable *in a Beat*.

        Physical safety belongs on the path the packets actually take, so the
        check is here. `Step.tier()` raises on both forbidden and unknown ops.
        """
        for s in steps:
            s.tier()
        return self._send(tuple(steps), timeout)

    def _send(self, steps, timeout: float) -> list[dict]:
        raise NotImplementedError


class FileBridge(Bridge):
    """The live path. Batch-writes a phrase so the daemon drains it
    back-to-back instead of one poll cycle per step."""

    def __init__(self, bridge_dir: Path | None = None):
        base = bridge_dir or (Path(__file__).parent / ".bridge")
        self.base = base
        self.req = base / "requests"
        self.resp = base / "responses"

    def daemon(self) -> dict | None:
        """The live daemon's lock record, or None if nothing holds the bridge.

        `self.req.exists()` used to stand in for this, and it is not the same
        question: the queue directories SURVIVE the daemon that made them, so
        a dead daemon reads as running. A session then starts, briefs the
        operator, and only discovers the truth ~15 s later when the first op
        times out with "no response" -- which reads as a protocol bug rather
        than as "nothing is listening". Cost one live run when the link
        dropped as R2 was carried to another room.

        The lock is the daemon's own liveness record (r2_probe.py:921) and it
        carries the CEILING too, which is why this returns the record rather
        than a bool: what the daemon will actually permit is knowable, and
        should never be taken on trust from a command-line flag.
        """
        try:
            held = json.loads((self.base / "daemon.lock").read_text())
        except (OSError, ValueError):
            return None
        if not isinstance(held, dict) or not isinstance(held.get("pid"), int):
            return None
        try:
            os.kill(held["pid"], 0)
        except ProcessLookupError:
            return None
        except OSError:
            # Alive but not ours to signal. Still alive.
            pass
        return held

    def running(self) -> bool:
        return self.daemon() is not None

    def _send(self, steps, timeout: float = 12.0) -> list[dict]:
        if not self.running():
            raise RuntimeError(
                "bridge not running. Only the operator can start it:\n"
                "  cd ~/Projects/R2Z2/mac-prototype && ./r2 daemon --allow dome")
        # Ids must sort in emission order — the daemon drains
        # `sorted(glob("*.json"))`. Derive them from one base stamp plus an
        # index rather than calling time.time() per step: two steps written
        # inside the same microsecond would otherwise collide and one would
        # overwrite the other, silently dropping a step from the gesture.
        base = int(time.time() * 1_000_000)
        names = [f"{base + i:016d}.json" for i in range(len(steps))]
        for name, step in zip(names, steps):
            (self.req / name).write_text(
                json.dumps({"op": step.op, "params": step.params}))
        out: list[dict] = []
        deadline = time.monotonic() + timeout
        for name in names:
            path = self.resp / name
            resp = None
            while time.monotonic() < deadline:
                # `exists()` is NOT "readable". The daemon writes the response
                # with a plain write_text, so there is a window where the file
                # exists and is still empty or half-written. Polling at 0.02 s
                # lands in that window regularly — `./r2 send` only gets away
                # with the naive check because it polls at 0.15 s and usually
                # misses it. Treat a failed parse as "not ready yet", not as a
                # failed request: the alternative is a crash mid-beat that
                # leaves R2 stopped halfway through an expression.
                if path.exists():
                    try:
                        resp = json.loads(path.read_text())
                        break
                    except (json.JSONDecodeError, OSError):
                        pass
                time.sleep(0.02)
            if resp is None:
                (self.req / name).unlink(missing_ok=True)
                out.append({"ok": False, "error": f"no response for {name}"})
                continue
            out.append(resp)
            path.unlink(missing_ok=True)
        return out


class FakeBridge(Bridge):
    """Records what would have been sent. This is what makes the whole layer
    dry-testable: build the driver and prove the sequence while the link is
    DOWN, because every edit made with the link up costs a relaunch and only
    the operator can relaunch."""

    def __init__(self, fail_on: str | None = None, head_deg: float = -12.5,
                 head_seq: list[float] | None = None):
        self.sent: list[Step] = []
        self.batches: list[list[Step]] = []
        self.fail_on = fail_on
        self.head_deg = head_deg
        # Successive answers to `head`, so a test can simulate a dome that
        # actually drifted. Without this every read returns the same angle,
        # the residual is always 0, and the correction branch never runs.
        self.head_seq = list(head_seq) if head_seq else None

    def _reply(self, step: Step) -> dict:
        out = {"ok": self.fail_on != step.op, "op": step.op}
        # A `head` read must answer with a real angle. Returning a bare ok
        # made return_to_start silently no-op, so the correction path — the
        # entire fix for the dome drift — was never exercised by any test
        # while every test still passed.
        if step.op == "head":
            if self.head_seq:
                self.head_deg = self.head_seq.pop(0)
            out["data"] = {"degrees": self.head_deg}
        return out

    def _send(self, steps, timeout: float = 12.0) -> list[dict]:
        self.batches.append(list(steps))
        self.sent.extend(steps)
        return [self._reply(s) for s in steps]


# ---------------------------------------------------------------------------
# Performing
# ---------------------------------------------------------------------------

class DomeHome:
    """A persistent reference angle for the dome, held across beats.

    This exists because of a mistake worth keeping. `return_to_start` read the
    dome at the start of EACH beat, so the anchor was wherever the dome
    happened to be — and the residual measured against it was always ~3 deg,
    always below the 12 deg the firmware needs to act on. The correction
    therefore never fired once, and the dome still walked, just more slowly:
    measured -24.58 -> -32.40 -> -34.93 -> -37.58 -> -41.17 over five runs.

    A drift reference has to be FIXED to be a reference at all. Anchoring to
    "where this beat began" measures the last beat's error and then forgets
    it, which is precisely how a slow leak survives a fix aimed at it.
    """

    def __init__(self, angle: float | None = None):
        self.angle = angle

    def anchor(self, angle: float) -> float:
        """Set the home once, on the first beat that has a reading."""
        if self.angle is None:
            self.angle = angle
        return self.angle

    def correction(self, here: float) -> float | None:
        """How far to go to get home, or None if it cannot be commanded.
        Accumulated drift crosses the threshold eventually, and that is the
        point: the error is allowed to build against a fixed mark until it is
        large enough for the hardware to act on."""
        if self.angle is None:
            return None
        delta = self.angle - here
        return delta if abs(delta) >= MIN_DOME_TRAVEL_DEG else None


# One robot, one dome, therefore one home. A process-wide default so the
# CORRECTED drift behaviour is what a caller gets without knowing to ask for
# it. The opt-in version shipped first and every call site forgot it, which
# made the fix inert: the anchor moved with the dome, the residual stayed
# ~3 deg, and the correction never fired. A fix that depends on remembering an
# optional keyword is not a fix.
_DEFAULT_HOME = DomeHome()


def reset_default_home() -> None:
    """Forget the process-wide home so the next beat re-anchors.

    Needed because the default anchors to the FIRST reading this process ever
    took and then holds it forever. That is right for one continuous session
    and wrong the moment the premise breaks: someone lifts him onto a shelf,
    a new session inherits a dome parked 90 deg from yesterday's home, or a
    test suite runs many beats in one process. Call it at session start."""
    _DEFAULT_HOME.angle = None


def perform(beat: Beat, bridge: Bridge, *, ceiling: str,
            home: "DomeHome | None" = None, sleep=time.sleep) -> dict:
    """Run one beat. Returns a record of what happened.

    `ceiling` is the daemon's --allow level and is checked BEFORE anything is
    sent. Refusing here rather than letting the daemon refuse mid-beat matters:
    a beat half-executed is R2 left mid-expression, which is a worse state than
    a beat never started.
    """
    beat.validate()
    need = beat.required_tier()
    if tier_rank(need) > tier_rank(ceiling):
        return {"ok": False, "beat": beat.name, "refused": True,
                "error": f"{beat.name!r} needs tier {need!r}; daemon ceiling "
                         f"is {ceiling!r}. Relaunch with --allow {need}.",
                "responses": []}

    responses: list[dict] = []
    started = time.monotonic()

    # Read where the dome actually is BEFORE anything moves, so the beat can
    # be closed out against a real number rather than an assumed one. The dome
    # has no home position — it has been found at 103, 3.3 and -0.06 degrees
    # across sessions — so "where it started" is the only meaningful anchor,
    # and it is only knowable by asking.
    start_angle = None
    if beat.return_to_start:
        r = bridge.send_batch([Step("head", {})], timeout=12.0)[0]
        responses.append(r)
        start_angle = (r.get("data") or {}).get("degrees")
        # Anchor to a PERSISTENT home, not to this beat's start. Defaults to
        # the process-wide home so drift is bounded even when the caller
        # passes nothing; tests inject their own to stay isolated.
        anchor_to = home if home is not None else _DEFAULT_HOME
        if start_angle is not None:
            start_angle = anchor_to.anchor(start_angle)

    aborted_at = None
    try:
        for i, phrase in enumerate(beat.phrases):
            out = bridge.send_batch(phrase.steps, timeout=12.0)
            responses.extend(out)
            # STOP on a failed step. This loop used to run to completion
            # regardless, which is the thing this function's own docstring
            # says it exists to prevent: phrase 3 was sent on top of a dome
            # position phrase 2 never confirmed reaching.
            if any(not r.get("ok") for r in out):
                aborted_at = i
                break
            if phrase.gap_s and i < len(beat.phrases) - 1:
                sleep(phrase.gap_s)
    finally:
        # Whatever happened above — clean finish, aborted phrase, or an
        # exception out of the bridge (FileBridge raises RuntimeError when the
        # daemon has gone away, and OSError if the queue directory vanished) —
        # R2 must not be left holding an EXPRESSION colour. An LED colour we
        # set is state that survives the link dropping, so the last thing we
        # wrote is what the household sees until something changes it.
        # CLAUDE.md: "failure must result in stop, not last-command."
        #
        # This also fixes a narrower bug: the reset used to live inside the
        # `return_to_start` block, so `thinking()` — which does not set it —
        # ended every run holding an expression colour.
        try:
            responses.extend(bridge.send_batch((
                Step("leds", {"channels": front(beat.rest_colour)}),
            ), timeout=12.0))
        except Exception:
            # Best effort only. Never mask the original failure with a
            # secondary one raised while tidying up.
            pass

    residual = None
    if aborted_at is None and beat.return_to_start and start_angle is not None:
        sleep(DOME_MOVE_S)
        here = (bridge.send_batch([Step("head", {})], timeout=12.0)[0]
                .get("data") or {}).get("degrees")
        if here is not None:
            residual = round(start_angle - here, 2)
            if abs(residual) >= MIN_DOME_TRAVEL_DEG:
                # Absolute, not relative: undershoot means a sum-to-zero set
                # of deltas still walks. The daemon bounds the TRAVEL from a
                # fresh reading, so this cannot become a large uncommanded
                # swing.
                responses.extend(bridge.send_batch((
                    Step("dome", {"angle": start_angle, "settle": 0}),
                ), timeout=12.0))
                sleep(DOME_MOVE_S)
                here = (bridge.send_batch([Step("head", {})], timeout=12.0)[0]
                        .get("data") or {}).get("degrees")
                # `is not None`, NOT truthiness: 0.0 deg is a real position
                # inside the usable range, and `if here` would discard it and
                # silently report the PRE-correction residual as the final one.
                if here is not None:
                    residual = round(start_angle - here, 2)
            # else: the correction is BELOW the threshold and therefore
            # unachievable. Say so rather than emitting a command that would
            # report success and do nothing. Residual under ~12 deg is the
            # floor of what this hardware can hold, not a bug to fix.

    failed = [r for r in responses if not r.get("ok")]
    return {"ok": not failed, "beat": beat.name, "refused": False,
            "tier": need, "steps": len(responses),
            "aborted_at_phrase": aborted_at,
            "start_angle": start_angle,
            "residual_deg": residual,
            "elapsed_s": round(time.monotonic() - started, 3),
            "failed": failed, "responses": responses}


# ---------------------------------------------------------------------------
# The vocabulary
# ---------------------------------------------------------------------------

def express_curious(*, rest=BASE_NEUTRAL, travel: float = 15.0) -> Beat:
    """"What was that?" — R2 notices something and asks about it.

    The canonical example from CLAUDE.md, and the first behaviour ever
    composed on this project. Spec from the behaviour table in
    docs/research/r2-capabilities.md: sound rising from the CHATTY family,
    dome +/-15 degrees alternating with a pause between, holo flicker.

    NO PSI COLOUR CHANGE. Every reachable corner is spent on status
    (D-012 Amendment A), so expression rides on motion, sound and the holo.
    The front PSI moves exactly once, at the end, restoring the rest colour.

    Reading of the gesture, which is what the phrasing encodes:
      1. Light shifts first  — attention lands before the body moves.
      2. Dome turns AND the chirp fires 0.12 s later, while it is still
         travelling. The question is asked mid-turn, not after it.
      3. A held pause. This is the whole behaviour: curiosity is the pause,
         not the motion. Without it this is just a twitch.
      4. Dome comes back most of the way and the base colour returns.

    Dome moves are expressed as `delta`, never `angle`. The dome has no
    resting position — observed at 103, 3.3 and -0.06 degrees across three
    sessions — so a destination is meaningless and the daemon bounds travel
    from a freshly read position.
    """
    beat = Beat(
        name="express_curious",
        phrases=(
            # 1. Attention — carried by the HOLO, not by hue. The front PSI
            #    is left alone for the whole gesture: it is the status layer,
            #    and curiosity is not a status claim. This phrase used to
            #    paint PULSE_PALE here.
            Phrase((
                Step("leds", {"channels": holo(180)}),
            ), gap_s=0.15),
            # 2. Sound FIRST, then the turn. The original order was
            #    dome-then-sound on the theory that 0.12 s of head start would
            #    put the chirp inside the turn. REFUTED on hardware: the
            #    operator heard the chirp land in the pause, after the dome had
            #    settled. Audio onset is slower than the batch spacing, so the
            #    only way to overlap them is to start the sound first.
            Phrase((
                Step("sound", {"id": CURIOUS_SOUNDS.pick(), "volume": 200}),
                Step("dome", {"delta": travel, "settle": 0}),
                Step("leds", {"channels": holo(255)}),
            ), gap_s=DOME_MOVE_S),              # the pause that IS the curiosity
            # 3. Swing the other way, PAST centre. Twice the travel because it
            #    has to cross back over the start and still clear the 12 deg
            #    floor on its own.
            Phrase((
                Step("dome", {"delta": -travel * 2, "settle": 0}),
                Step("leds", {"channels": holo(120)}),
            ), gap_s=DOME_MOVE_S),
            # 4. The return is AUTHORED, not left to a correction. A gesture
            #    that ends near where it began needs a final move under 12 deg
            #    to tidy up, and that move is exactly the one the firmware
            #    ignores. So the last leg of the gesture IS the return, at full
            #    travel, and perform() only has to mop up what undershoot left.
            Phrase((
                Step("dome", {"delta": travel, "settle": 0}),
                Step("leds", {"channels": {**front(rest), **holo(0)}}),
            ), gap_s=0.0),
        ),
        rest_colour=rest,
        interruptible=True,
        energy="low",
        cooldown_s=20.0,
        return_to_start=True,
        evidence="COMPOSED. Sound OBSERVED (S1b: CHATTY_11 inquisitive, "
                 "CHATTY_15 'huh?'). Dome timing MEASURED 2026-08-17 "
                 "(~2.0-2.2 s per move, ~3 deg undershoot). Sound-before-dome "
                 "ordering follows a REFUTED first attempt at the reverse. "
                 "The pause reading as curiosity is OBSERVED (operator).",
        notes="Volume 200: 80 was too quiet to evaluate across a room (S1b).",
    )
    beat.validate()
    return beat


def thinking(*, rest=BASE_NEUTRAL, pulses: int = 3) -> Beat:
    """The LLM round-trip wait. R2 is processing and you can see that he is.

    The behaviour with the best evidence behind it, which is why it is the
    second one built: S1b heard EXCITED_1/10/11 as "quick thinking /
    analyzing / quick reply", refuting the family's own label. Before that
    reading this behaviour had no sound at all.

    Dome is still by design — thinking is not motion. The light does the work:
    a CYAN <-> BLUE alternation, which is exactly what docs/behaviour-states.md
    specifies for this state. Both are status corners — cyan is "engaged with
    you", blue is idle — so the pulse reads as attention coming and going
    rather than as a colour nobody has a meaning for. It used to alternate
    PULSE_PALE against cyan; PULSE_PALE was the pale blue measured to read
    grey. Blinking rather than steady because this happens DURING an
    interaction (D-012).

    OBSERVED, S1e: every LED value change flickers on this hardware. That is
    R2's own update path, not our timing jitter, and it survives the port to
    the ESP32 — so the stutter is a material property to lean into, not a
    defect to smooth out. The pulse count is deliberately low.
    """
    phrases = [
        Phrase((
            Step("sound", {"id": THINKING_SOUNDS.pick(), "volume": 200}),
            Step("leds", {"channels": {**front(BASE_NEUTRAL), **logic(255)}}),
        ), gap_s=0.45),
    ]
    for i in range(pulses):
        phrases.append(Phrase((
            Step("leds", {"channels": front(BASE_ENGAGED)}),
        ), gap_s=0.45))
        phrases.append(Phrase((
            Step("leds", {"channels": front(BASE_NEUTRAL)}),
        ), gap_s=0.45))
    # Rest. Logic displays back down and the base colour restored, so what the
    # household sees afterwards is a defined status and not the tail of a wait.
    phrases.append(Phrase((
        Step("leds", {"channels": {**front(rest), **logic(0)}}),
    ), gap_s=0.0))

    beat = Beat(
        name="thinking",
        phrases=tuple(phrases),
        rest_colour=rest,
        interruptible=True,
        energy="low",
        cooldown_s=0.0,          # it is a wait state; it may repeat immediately
        evidence="COMPOSED. Sound OBSERVED (S1b). Pulse palette follows "
                 "D-012 Amendment A (seven corners; pastels read grey, value "
                 "changes flicker). Dome intentionally unused.",
        notes="No dome and no stance — runs at the `audio` ceiling.",
    )
    beat.validate()
    return beat


def express_delight(*, rest=BASE_NEUTRAL, intensity: float = 1.0,
                    travel: float = 14.0) -> Beat:
    """He was touched, and it landed. Composed from primitives (#85).

    `intensity` is 0..1 and comes from `r2_mood`: it is the DEFICIT, how much
    the contact meant to him, read BEFORE the touch is applied. A starved R2
    gets the full beat; one petted a minute ago gets a short acknowledgement.
    Same channels, same sound pool, fewer phrases — one behaviour at two
    lengths, not two behaviours.

    NO PSI COLOUR CHANGE, and this is the constraint that shapes everything
    else. All seven reachable corners are spent on status (D-012 Amendment A),
    so a delight beat that recoloured the PSIs would be asserting a status
    that is not true. The front PSI is touched exactly once, in the last
    phrase, restoring whatever status colour the layer handed in. Expression
    rides on motion, sound, the holo (bit 7) and the logic panel (bit 3).

    NOT `EMOTE_LAUGH`. Id 15 is hardware-confirmed as a laugh and is still
    excluded: an animation is a stance command whose contents cannot be
    inspected before sending, and `EMOTE_YES` — a *nod* — emitted WADDLE three
    times and put R2 on the floor with `perform_leg_action` never called by us
    (D-010). This beat fires with someone's hand on him, unsupervised,
    possibly over a hard floor. `FORBIDDEN_OPS` already makes the animation
    unreachable; #85 AC5 keeps that guard alive.

    A LAUGH CANNOT FEEL QUICK, so this does not try. Every dome move takes
    ~2.0-2.2 s regardless of distance and anything under ~10.5 deg is silently
    ignored while returning ok (D-013). The register the hardware offers is
    a slow rock, not a giggle — designing a fast one would be designing for
    hardware we do not have.
    """
    if not 0.0 <= intensity <= 1.0:
        raise ValueError(f"intensity {intensity} outside 0..1")
    full = intensity >= FULL_DELIGHT_INTENSITY

    # ONE pick per beat, whatever the length. Two picks in the full beat would
    # halve the pool's effective period and make AC7's "four fires, four ids"
    # depend on which pick you counted.
    laugh = DELIGHT_SOUNDS.pick(avoid_last=3)

    # Phrase 1 — the holo comes up and the logic panel lights. Brightness
    # only: both are on/off-ish channels whose curve is too steep for
    # anything subtle (D-012 Amendment A section 3).
    opening = Phrase((
        Step("leds", {"channels": {**holo(160 if full else 200),
                                   **logic(255)}}),
    ), gap_s=0.15)

    # Phrase 2 — sound FIRST, then the turn. Audio onset is slower than the
    # batch spacing, so starting the dome first puts the laugh in the pause
    # after it rather than inside it. Learned the expensive way on
    # express_curious, which had this backwards until hardware refuted it.
    swing_out = Phrase((
        Step("sound", {"id": laugh, "volume": 200}),
        Step("dome", {"delta": travel, "settle": 0}),
        Step("leds", {"channels": holo(255)}),
    ), gap_s=DOME_MOVE_S)

    closing_channels = {**front(rest), **holo(0), **logic(0)}

    if full:
        phrases = (
            opening,
            swing_out,
            # Back across centre. Twice the travel so the return leg still
            # clears the 12 deg floor on its own — a gesture that ends where
            # it began needs a sub-threshold tidy-up move, and that is exactly
            # the move the firmware discards.
            Phrase((
                Step("dome", {"delta": -travel * 2, "settle": 0}),
                Step("leds", {"channels": holo(120)}),
            ), gap_s=DOME_MOVE_S),
            Phrase((
                Step("dome", {"delta": travel, "settle": 0}),
                Step("leds", {"channels": closing_channels}),
            ), gap_s=0.0),
        )
    else:
        phrases = (
            opening,
            swing_out,
            Phrase((
                Step("dome", {"delta": -travel, "settle": 0}),
                Step("leds", {"channels": closing_channels}),
            ), gap_s=0.0),
        )

    beat = Beat(
        name="express_delight" + ("" if full else ":brief"),
        phrases=phrases,
        rest_colour=rest,
        interruptible=True,
        energy="low" if not full else "medium",
        # Shorter than curious: this answers a hand, and a creature that
        # cannot answer twice in a minute reads as broken rather than paced.
        cooldown_s=12.0 if full else 6.0,
        return_to_start=True,
        evidence="COMPOSED (#85). Sound: R2_LAUGH_* family label and heard "
                 "reading agree. Dome timing MEASURED (D-013): ~2.0-2.2 s per "
                 "move, 12 deg floor. No PSI change (D-012 Amendment A).",
        notes=f"intensity={intensity:.2f} -> {'full' if full else 'brief'}",
    )
    beat.validate()
    return beat


VOCABULARY = {"express_curious": express_curious, "thinking": thinking,
              "express_delight": express_delight}
