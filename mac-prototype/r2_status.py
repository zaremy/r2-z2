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
from r2_behavior import Bridge, Step, perform

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
                 *, quiet_value: float = 1.0):
        self.bridge = bridge
        self.store = store or StatusStore()
        self.quiet_value = quiet_value
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
        self.bridge.send_batch([
            Step("leds", {"channels": LG.assertion(
                self.current, value=self._value_for(self.current))})])
        self.store.write(self.current)
        return self.current, self.dropped

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
        """Drive the current state's animation for `seconds`.

        `sleep` and `now` are injected so the whole runner is testable on
        synthetic time with no robot and no waiting. That is not a convenience:
        a scheduler tested only against a real clock is tested only against
        the timings that happened to occur.

        Frames are sent one batch each, in order. The player has already
        merged fixtures changing at the same instant into a single write, so
        one frame IS one write, and the state's declared writes/s is the rate
        this loop produces.
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
        """
        ch = LG.assertion(self.current)
        return (ch["0"], ch["1"], ch["2"])
