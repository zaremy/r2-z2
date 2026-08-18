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
import time
from dataclasses import dataclass, field
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
# to zero, which is why a "balanced" gesture still walks the dome. Any beat
# that must end where it started has to close on an ABSOLUTE angle.
DOME_UNDERSHOOT_DEG = 3.0

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
}

# Ops that can end with R2 on the floor. Named explicitly so the guard below
# is a positive assertion about a known list, not the absence of a match.
FORBIDDEN_OPS = ("animation", "set_stance")


def tier_rank(tier: str) -> int:
    return TIERS.index(tier)


# ---------------------------------------------------------------------------
# Colour
# ---------------------------------------------------------------------------

# D-012: the LED base layer is system truth. Colour carries meaning,
# steady-vs-blink carries mode (status between interactions, expression during
# one). These are the three status colours, and they are the ONLY colours a
# beat may come to rest on.
BASE_NEUTRAL = (0, 0, 255)      # blue  — on / waiting / neutral
BASE_SUCCESS = (0, 255, 0)      # green — success
BASE_PENDING = (255, 0, 0)      # red   — issue pending resolution

# Expression colours. Reachable mid-beat, never at rest.
#
# OBSERVED, S1e: low saturation reads as grey on this hardware — (120,190,255)
# rendered genuinely pale but could not be told apart from other pastels. So
# an expression palette must CONTRAST, not shade. These two are far enough
# apart in hue to read as a change rather than a flicker.
PULSE_PALE = (120, 190, 255)
PULSE_CYAN = (0, 255, 255)


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
    _resolved: list = field(default_factory=list, repr=False)

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
        return round(steps * 0.12 + sum(p.gap_s for p in self.phrases), 3)

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
        if tier_rank(self.required_tier()) >= tier_rank("stance"):
            raise ValueError(f"beat {self.name!r} reaches the stance tier")
        if self.rest_colour not in (BASE_NEUTRAL, BASE_SUCCESS, BASE_PENDING):
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
    share this interface so a beat can be flown end-to-end with no robot."""

    def send_batch(self, steps, timeout: float) -> list[dict]:
        raise NotImplementedError


class FileBridge(Bridge):
    """The live path. Batch-writes a phrase so the daemon drains it
    back-to-back instead of one poll cycle per step."""

    def __init__(self, bridge_dir: Path | None = None):
        base = bridge_dir or (Path(__file__).parent / ".bridge")
        self.req = base / "requests"
        self.resp = base / "responses"

    def running(self) -> bool:
        return self.req.exists()

    def send_batch(self, steps, timeout: float = 12.0) -> list[dict]:
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

    def __init__(self, fail_on: str | None = None, head_deg: float = -12.5):
        self.sent: list[Step] = []
        self.batches: list[list[Step]] = []
        self.fail_on = fail_on
        self.head_deg = head_deg

    def _reply(self, step: Step) -> dict:
        out = {"ok": self.fail_on != step.op, "op": step.op}
        # A `head` read must answer with a real angle. Returning a bare ok
        # made return_to_start silently no-op, so the correction path — the
        # entire fix for the dome drift — was never exercised by any test
        # while every test still passed.
        if step.op == "head":
            out["data"] = {"degrees": self.head_deg}
        return out

    def send_batch(self, steps, timeout: float = 12.0) -> list[dict]:
        self.batches.append(list(steps))
        self.sent.extend(steps)
        return [self._reply(s) for s in steps]


# ---------------------------------------------------------------------------
# Performing
# ---------------------------------------------------------------------------

def perform(beat: Beat, bridge: Bridge, *, ceiling: str,
            sleep=time.sleep) -> dict:
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

    for i, phrase in enumerate(beat.phrases):
        responses.extend(bridge.send_batch(phrase.steps, timeout=12.0))
        if phrase.gap_s and i < len(beat.phrases) - 1:
            sleep(phrase.gap_s)

    if beat.return_to_start and start_angle is not None:
        # Absolute, not relative. Every move undershoots by ~3 degrees in the
        # direction of travel, so a sequence of deltas that sums to zero on
        # paper still walks. Only an absolute target closes the loop. The
        # daemon still bounds the TRAVEL from a fresh reading, so this cannot
        # become a large uncommanded swing.
        sleep(DOME_MOVE_S)
        responses.extend(bridge.send_batch((
            Step("dome", {"angle": start_angle, "settle": 0}),
            Step("leds", {"channels": front(beat.rest_colour)}),
        ), timeout=12.0))

    failed = [r for r in responses if not r.get("ok")]
    return {"ok": not failed, "beat": beat.name, "refused": False,
            "tier": need, "steps": len(responses),
            "start_angle": start_angle,
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
            # 1. Attention. Holo up, front to the pale expression colour.
            Phrase((
                Step("leds", {"channels": {**front(PULSE_PALE), **holo(180)}}),
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
            # 3. Tilt the other way, shorter — a second look, not a repeat.
            Phrase((
                Step("dome", {"delta": -travel * 1.5, "settle": 0}),
                Step("leds", {"channels": {**front(PULSE_CYAN), **holo(120)}}),
            ), gap_s=DOME_MOVE_S),
            # 4. Holo down and base colour restored. The dome correction is
            #    NOT here: perform() closes it against the angle it read before
            #    the beat began, because deltas undershoot and never sum back.
            Phrase((
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
    a pale-blue / cyan pulse, which is the modulation the operator specified
    after ruling that low-saturation pastels read as grey and that hues must
    contrast. Blinking rather than steady because this happens DURING an
    interaction (D-012).

    OBSERVED, S1e: every LED value change flickers on this hardware. That is
    R2's own update path, not our timing jitter, and it survives the port to
    the ESP32 — so the stutter is a material property to lean into, not a
    defect to smooth out. The pulse count is deliberately low.
    """
    phrases = [
        Phrase((
            Step("sound", {"id": THINKING_SOUNDS.pick(), "volume": 200}),
            Step("leds", {"channels": {**front(PULSE_PALE), **logic(255)}}),
        ), gap_s=0.45),
    ]
    for i in range(pulses):
        phrases.append(Phrase((
            Step("leds", {"channels": front(PULSE_CYAN)}),
        ), gap_s=0.45))
        phrases.append(Phrase((
            Step("leds", {"channels": front(PULSE_PALE)}),
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
        evidence="COMPOSED. Sound OBSERVED (S1b). Pulse palette OBSERVED "
                 "(S1e: pastels read grey, value changes flicker). Dome "
                 "intentionally unused.",
        notes="No dome and no stance — runs at the `audio` ceiling.",
    )
    beat.validate()
    return beat


VOCABULARY = {"express_curious": express_curious, "thinking": thinking}
