"""The status layer: what R2 is, between moments.

WHY THIS EXISTS
    `r2_lights` describes ten states and could render none of them: nothing
    imported it but its own test. Meanwhile `r2_behavior` beats wrote LEDs
    directly, so there were two systems describing one droid and only the
    older one ran. This is the missing half -- the thing that HOLDS a status
    and puts it on the hardware.

THE THREE JOBS
    1. Remember which status is current, across process restarts. A colour we
       set does not survive a sleep cycle (MEASURED 2026-08-18), so the store
       is our side of that: he forgets, we do not.
    2. Assert it on connect, before anything else runs. The state after a
       discontinuity must be one we chose.
    3. Render it -- walk the frames and send them on schedule, under the
       8.3 writes/s ceiling.

WHAT IT DOES NOT DO
    It does not decide WHEN to change status. That is the brain's job. This
    layer is told, and remembers, and shows.
"""
from __future__ import annotations

import json
import time
from pathlib import Path

import r2_lights as LG
from r2_behavior import Bridge, Step, perform, sound_id

# The connect chirp. R2_CHATTY_1 is the best-evidenced short sound we have:
# HEARD in S1b and read as "quick success", the most neutral-leaning member of
# the family, and already carrying an operator ruling that it is acceptable
# despite CHATTY being refuted as "neutral talking" (they are conversational
# turn-shapes, and "I am back" is a conversational turn).
#
# R2_STEP_3/4/5 are the only sounds CONFIRMED short -- but they were sampled
# for DURATION only and never rated for character, so we know how long they
# are and not what they sound like. Short and unknown is worse than short and
# rated.
CONNECT_CHIRP = sound_id("R2_CHATTY_1")
CHIRP_VOLUME = 200          # 80 was too quiet to evaluate across a room (S1b)

# The optional connect nod. 15 deg clears the ~10.5 deg floor below which the
# firmware SILENTLY ignores the command and still returns ok: true, with margin
# for the curve -- that floor is one point on it, not a constant.
GREET_TRAVEL_DEG = 15.0

# The rate ceiling lives in r2_lights, which owns the light layer. r2_probe
# has the same measured value under a different name (CMD_SAFE_INTERVAL); a
# test asserts the two cannot drift apart, because a duplicated hardware
# constant that silently disagrees would be a rate ceiling nobody is enforcing.
SAFE_INTERVAL_S = LG.CMD_SAFE_INTERVAL_S

DEFAULT_STORE = Path(__file__).parent / ".bridge" / "status.json"


class StatusStore:
    """Which status is current, persisted.

    Deliberately a plain JSON file with one key. Persistent state lives in OUR
    store and survives swapping anything underneath it (CLAUDE.md), and the
    smallest honest version of that for one string is a file.

    Every read is defensive. A wake path that raises on a corrupt store leaves
    R2 showing the firmware default, which is the exact outcome this module
    exists to prevent -- so a bad file reads as "no memory", never as an error.
    """

    def __init__(self, path: Path | None = None):
        self.path = Path(path) if path else DEFAULT_STORE

    def read(self) -> str | None:
        try:
            data = json.loads(self.path.read_text())
        except (OSError, ValueError):
            return None
        name = data.get("state") if isinstance(data, dict) else None
        return name if isinstance(name, str) else None

    def write(self, name: str) -> None:
        if name not in LG.STATES:
            raise KeyError(f"{name!r} is not a status state")
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps({"state": name, "at": time.time()}))
        tmp.replace(self.path)          # atomic; a half-written store reads as
                                        # no memory, and no memory is safe


class StatusLayer:
    """Holds the current status and puts it on the hardware.

    One instance per session. `connect()` first, then `set()` whenever the
    brain decides something changed, then `render()` to drive the animation.
    """

    def __init__(self, bridge: Bridge, store: StatusStore | None = None,
                 *, quiet_value: float = 1.0, chirp: bool = True,
                 chirp_in_quiet_hours: bool = False, greet: bool = False):
        self.bridge = bridge
        self.store = store or StatusStore()
        self.quiet_value = quiet_value
        self.chirp = chirp
        self.chirp_in_quiet_hours = chirp_in_quiet_hours
        self.greet = greet
        self.current: str = LG.DEFAULT_STATE
        self.dropped: str | None = None

    # -- lifecycle ---------------------------------------------------------

    def connect(self) -> tuple[str, str | None]:
        """Assert a status on the hardware. Call this FIRST, on every connect.

        Returns `(asserted, dropped)`. `dropped` is a remembered claim that
        could not be restored -- handed back rather than swallowed, so the
        brain can re-derive it. Both silently asserting a stale danger and
        silently clearing one are wrong.

        MEASURED 2026-08-18: a colour survives a link drop and a fresh
        connect, but NOT a sleep cycle. Magenta written at 23:52:47 was gone
        22.1 h later, back to the firmware's own red/blue alternation. Without
        this call the household sees R2's own idiom every morning, whatever
        the light language says.
        """
        self.current, self.dropped = LG.resume(self.store.read())
        # Store first, hardware second -- the same order `set()` argues for,
        # and for the same reason. If the send fails or the process dies, the
        # store must already say what we intended. This used to send first,
        # which meant a crash between the two left a dropped claim still
        # sitting in the store to be dropped again on the next connect.
        self.store.write(self.current)
        self.bridge.send_batch([
            Step("leds", {"channels": LG.assertion(
                self.current, value=self._value_for(self.current))})])
        self._chirp()
        self._greet()
        return self.current, self.dropped

    def _greet(self) -> bool:
        """An optional dome nod on connect. OFF BY DEFAULT.

        Default off is the safety rule, not timidity: each actuator is
        individually opt-in and never bundled (CLAUDE.md). Lights and a chirp
        cannot knock anything over; the dome is a moving part, and asserting a
        status is not a moment anybody asked for motion.

        Its own batch, for the same reason the chirp gets one: this needs the
        `dome` tier while the assertion needs only `leds`, and a refusal here
        must never cost us the assertion.

        Expressed as `delta`, never `angle`. The dome has NO resting position
        -- observed at 103, 3.3 and -0.06 degrees across three sessions -- so a
        destination is meaningless, and the daemon bounds travel from a freshly
        read position.

        OUT AND BACK, because a single move would displace the dome a little
        further on every connect and nothing would ever put it back. Two moves
        at ~2.0-2.2 s each makes a connect greeting a ~4.4 s event, not a
        flourish. That is the hardware, not the choreography.
        """
        if not self.greet:
            return False
        for delta in (GREET_TRAVEL_DEG, -GREET_TRAVEL_DEG):
            self.bridge.send_batch([
                Step("dome", {"delta": delta, "settle": 0})])
        return True

    def _chirp(self) -> bool:
        """The short "I am back" on connect. Returns whether it was sent.

        SENT AS ITS OWN BATCH, AFTER the assertion, and never bundled with it.
        The chirp needs the `audio` tier and the assertion needs only `leds`,
        so a daemon started at `--allow leds` refuses the sound -- and if the
        two shared a batch, a refused chirp could cost us the assertion, which
        is the part that actually matters. The louder, less important half must
        never be able to take the quieter, more important half down with it.

        Silence is therefore an acceptable outcome and not an error.

        Suppressed during quiet hours. A chirp is the one part of a connect
        that carries into another room, and quiet hours exist precisely so the
        droid can come back without announcing it at 2am. Overriding that is
        explicit -- pass `chirp=True` to the constructor -- because the whole
        point of a quiet-hours rule is that it is not bypassed by accident.
        """
        if not self.chirp:
            return False
        if self.quiet_value < 1.0 and not self.chirp_in_quiet_hours:
            return False
        self.bridge.send_batch([
            Step("sound", {"id": CONNECT_CHIRP, "volume": CHIRP_VOLUME})])
        return True

    def set(self, name: str) -> None:
        """Change status. Persists BEFORE rendering.

        Order matters and is not arbitrary: if the write to the droid fails or
        the process dies mid-render, the store must already say what we
        intended, so the next connect asserts the right thing. A store that
        lags the hardware is a store that forgets the last thing that
        happened -- which is the case worth surviving.
        """
        if name not in LG.STATES:
            raise KeyError(f"{name!r} is not a status state")
        if name in LG.BLOCKED:
            raise ValueError(
                f"{name!r} is specified but BLOCKED and cannot be entered; "
                f"see docs/behaviour-states.md")
        self.store.write(name)
        self.current = name
        self.bridge.send_batch([
            Step("leds", {"channels": LG.assertion(
                name, value=self._value_for(name))})])

    def _value_for(self, name: str) -> float:
        """The quiet-hours scale to request. Quiet hours scale VALUE, never
        hue -- scaling a saturated corner keeps it saturated.

        NO EXEMPTION CHECK HERE, deliberately. `StateLights.frames()` already
        clamps a non-dimmable state back to full, so a guard here would be a
        second copy of one rule: mutating it away changed no test, which is
        how it was found. One enforcement point, living next to the flag it
        reads.
        """
        return self.quiet_value

    # -- rendering ---------------------------------------------------------

    def render(self, seconds: float, *, sleep=time.sleep,
               now=time.monotonic) -> list[dict]:
        """Drive the current state's animation FOR `seconds`, then return.

        `sleep` and `now` are injected so the whole runner is testable on
        synthetic time with no robot and no waiting. That is not a
        convenience: a scheduler tested only against a real clock is tested
        only against the timings that happened to occur.

        Frames are sent one batch each, in order. The player has already
        merged fixtures changing at the same instant into a single write, so
        one frame IS one write, and the state's declared writes/s is the rate
        this loop produces.

        IT RUNS FOR THE FULL DURATION, including when there is nothing left to
        do. That sounds obvious and was not: this returned after the LAST
        TRANSITION, so a steady state -- idle, the one that runs for hours --
        emitted frame zero and returned in 0.0000 s. The obvious main loop,

            while running:
                layer.render(10)

        then measured at 368,780 writes/s against a ceiling of 8.3, with a
        pegged core and a flooded queue. The caller was not wrong; `render`
        was, and it was wrong in the shape most likely to be trusted.
        """
        state = LG.STATES[self.current]
        frames = state.frames(seconds, value=self._value_for(self.current))
        started = now()
        sent = []
        for at, channels in frames:
            behind = at - (now() - started)
            if behind > 0:
                sleep(behind)
            sent.append(channels)
            self.bridge.send_batch([Step("leds", {"channels": channels})])
        # Hold out the remainder. A steady state spends its whole duration
        # here, which is the point: idle costs one write and `seconds` of
        # quiet, not one write and an instant return.
        remaining = seconds - (now() - started)
        if remaining > 0:
            sleep(remaining)
        return sent

    # -- expression --------------------------------------------------------

    def express(self, beat_factory, *, ceiling: str, sleep=time.sleep,
                **kwargs) -> dict:
        """Run an expression beat ON TOP of the current status, then hand the
        status back.

        This is the wiring that stops the two systems disagreeing. Before it,
        `express_curious()` took `rest=BASE_NEUTRAL` as a default argument and
        every call site accepted it -- so a beat run while status was
        `attention` ended by painting blue over a pending issue, and the issue
        stopped being pending. The beat was not wrong; nothing had ever told
        it what the status was.

        The rest colour is taken FROM the status layer rather than passed to
        it, so a caller cannot supply one that contradicts what R2 is.

        `sleep` is threaded through to `perform`, which has always accepted
        it. Not passing it would have left every caller -- and every test --
        waiting out a real 2.2 s dome move, and quietly dropped a capability
        the layer underneath already offered.

        Then the status is RE-ASSERTED, which matters for any state that is
        not steady. A beat ends on a single frame, so a beat run during
        `attention` would leave yellow held steady and the blink would never
        resume -- the claim would still be visible but its urgency would be
        silently gone. Restoring the colour is not the same as restoring the
        state.
        """
        beat = beat_factory(rest=self.rest_colour(), **kwargs)
        result = perform(beat, self.bridge, ceiling=ceiling, sleep=sleep)
        # The caller schedules against the beat's cooldown -- one of the four
        # fields the behaviour library requires precisely so R2 can be run
        # without becoming twitchy. Building the beat happens in here now, so
        # the number has to come back out; a caller that cannot see it would
        # silently drop the cooldown and re-fire as fast as the sensor allows.
        # The VALUE, not the object: the beat itself does not need to cross
        # this boundary.
        result["cooldown_s"] = beat.cooldown_s
        # Unconditional: a beat that failed part-way is exactly when the
        # status most needs re-establishing, and `perform` stops on failure
        # without necessarily having reached its own colour reset.
        self.bridge.send_batch([
            Step("leds", {"channels": LG.assertion(
                self.current, value=self._value_for(self.current))})])
        return result

    def rest_colour(self) -> tuple[int, int, int]:
        """The colour an expression beat must come to rest on.

        Frame zero of the current status -- for a blinking state that is the
        lit half, so a beat never rests on a status's dark phase and reads as
        "off".

        HONOURS QUIET HOURS. It did not, and could not: the beat validator
        demanded an exact corner, so this returned full brightness while the
        status layer asserted dimmed. Every beat during quiet hours ended on a
        bright flash before the re-assert pulled it back down -- at 2am, which
        is precisely what quiet hours exist to prevent.
        """
        ch = LG.assertion(self.current,
                          value=self._value_for(self.current))
        return (ch["0"], ch["1"], ch["2"])
