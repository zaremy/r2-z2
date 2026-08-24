"""r2_mood.py — one scalar: how well he has been treated lately (#85).

WHY ONE SCALAR AND NOT A MOOD MODEL
    `CLAUDE.md` lists identity, mood, energy, boredom, relationship, memories
    and goals as persistent state. This file implements exactly ONE of them,
    because exactly one behaviour needs it. The rest arrives when a behaviour
    needs it; a model built ahead of its first consumer is a model nobody can
    falsify.

THE RISE IS SUB-SATURATING, AND THAT IS THE WHOLE POINT
    A touch on a lonely R2 moves him a lot. The same touch thirty seconds
    later barely moves him at all. If any single touch pinned happiness to
    100, the scalar would become a constant with a decay bolted on, and every
    behaviour that reads it would be reading nothing — the number would carry
    no information about how he has been treated, only about whether anyone
    has touched him since the last decay window.

    So the rise is a fraction of the DEFICIT, scaled by how recently he was
    last touched:

        delta = (CEILING - value) * GAIN * recency

    Both factors are strictly below 1, so `value` approaches the ceiling and
    never arrives. That is the invariant AC2 tests, and it is a property of
    the formula rather than of a clamp — a clamp would produce the same
    numbers and none of the meaning.

DECAY IS WALL CLOCK, COMPUTED ON READ
    Not a ticking timer. A timer means the value is only correct while the
    process runs, and this state is supposed to survive a restart: R2 left
    alone overnight should come back lonely, not come back exactly as he was
    when the process died.

    That forces `time.time()` and not `time.monotonic()`, which matters more
    than it looks. `r2_reactive.Reactive` runs on `time.monotonic` because it
    measures durations, and monotonic clocks are not comparable across
    process restarts — the epoch is arbitrary. Persisting a monotonic
    timestamp and subtracting it after a reboot yields a meaningless number
    that is often negative. AC4 exists to keep these two clocks apart.
"""

from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass, replace
from pathlib import Path

# The scale. 0 is "nobody has been near me for a long time", 100 is an
# asymptote the model deliberately cannot reach.
FLOOR = 0.0
CEILING = 100.0

# Time for an untouched R2 to lose half his remaining happiness. Chosen so a
# lunch break is noticeable and an overnight is total; NOT measured, and
# nothing downstream should treat it as though it were.
DECAY_HALF_LIFE_S = 20 * 60.0

# Fraction of the deficit a maximally-welcome touch closes. Strictly below 1
# so no touch can saturate.
GAIN = 0.35

# How fast the welcome recovers after a touch. A second touch immediately
# after the first gets RECENCY_FLOOR of the gain; after a few half-lives it
# is back to nearly all of it.
RECENCY_HALF_LIFE_S = 45.0
RECENCY_FLOOR = 0.08

DEFAULT_PATH = Path(__file__).parent / ".state" / "mood.json"


def _clamp(v: float) -> float:
    return max(FLOOR, min(CEILING, float(v)))


@dataclass(frozen=True)
class Happiness:
    """Immutable snapshot. `decayed()` and `touched()` return new instances.

    Frozen on purpose: a mutable mood object invites a caller to read the
    value, do something slow, and write back a number computed against a
    timestamp that has since moved. Every transition here takes the clock as
    an argument so the elapsed time is explicit at the call site.
    """

    value: float = 0.0
    updated_at: float = 0.0        # WALL clock (time.time), see module docstring
    last_touch_at: float | None = None

    def __post_init__(self) -> None:
        object.__setattr__(self, "value", _clamp(self.value))

    # -- reads ---------------------------------------------------------------

    def decayed(self, now: float) -> "Happiness":
        """The state as of `now`. Decay is applied here, never on a timer."""
        if self.updated_at <= 0.0 or now <= self.updated_at:
            # No baseline, or a clock that went backwards. Do not invent decay
            # from a negative interval — that would RAISE happiness, silently,
            # every time an NTP correction landed.
            return replace(self, updated_at=max(now, self.updated_at))
        elapsed = now - self.updated_at
        factor = 0.5 ** (elapsed / DECAY_HALF_LIFE_S)
        return replace(self, value=_clamp(self.value * factor), updated_at=now)

    def level(self, now: float) -> float:
        """Current happiness, decay included. The number behaviours read."""
        return self.decayed(now).value

    def intensity(self, now: float) -> float:
        """How much a touch right now would MEAN to him, 0..1.

        This is the DEFICIT, not the happiness. A starved R2 gets the full
        beat because the contact matters; one petted a minute ago gets a
        short acknowledgement. The issue leaves the direction implicit, so it
        is stated here: intensity answers "how much did he need this", which
        is the same quantity that drives the size of the rise.

        Reading it is side-effect free, which is what lets `react()` take the
        value BEFORE applying the rise — so the response reflects the state
        the touch found him in, not the state it produced.
        """
        return round((CEILING - self.level(now)) / (CEILING - FLOOR), 6)

    # -- the transition ------------------------------------------------------

    def recency(self, now: float) -> float:
        """How welcome a touch is right now, RECENCY_FLOOR..1."""
        if self.last_touch_at is None:
            return 1.0
        dt = max(0.0, now - self.last_touch_at)
        recovered = 1.0 - 0.5 ** (dt / RECENCY_HALF_LIFE_S)
        return RECENCY_FLOOR + (1.0 - RECENCY_FLOOR) * recovered

    def touched(self, now: float) -> tuple["Happiness", float]:
        """Apply one touch. Returns (new state, delta actually applied).

        Decay first, then rise. The order is load-bearing: rising from a
        stale value would credit him for happiness he has already lost, and
        an R2 touched once an hour would ratchet upward forever.
        """
        base = self.decayed(now)
        delta = (CEILING - base.value) * GAIN * base.recency(now)
        new = replace(base, value=_clamp(base.value + delta),
                      last_touch_at=now)
        return new, round(new.value - base.value, 6)


# ---------------------------------------------------------------------------
# Persistence — character state, not device state
# ---------------------------------------------------------------------------

def load(path: Path | str = DEFAULT_PATH, *, now: float | None = None) -> Happiness:
    """Read the stored mood. A missing or corrupt file is a fresh R2.

    Deliberately not an error. This is character state: the worst case for a
    bad read is that he starts lonely, which is exactly what a robot with no
    history should be, and refusing to start because a JSON file is truncated
    would trade a cosmetic loss for an outage.
    """
    now = time.time() if now is None else now
    try:
        raw = json.loads(Path(path).read_text())
        h = Happiness(
            value=float(raw["value"]),
            updated_at=float(raw["updated_at"]),
            last_touch_at=(None if raw.get("last_touch_at") is None
                           else float(raw["last_touch_at"])),
        )
    except (OSError, ValueError, KeyError, TypeError):
        return Happiness(value=0.0, updated_at=now)
    return h


def save(h: Happiness, path: Path | str = DEFAULT_PATH) -> Path:
    """Write atomically. A half-written mood file reads as a fresh R2 on the
    next load, which is a silent loss of state rather than a visible one."""
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(p.suffix + ".tmp")
    tmp.write_text(json.dumps({
        "value": round(h.value, 6),
        "updated_at": h.updated_at,
        "last_touch_at": h.last_touch_at,
    }))
    os.replace(tmp, p)
    return p


class Mood:
    """Load-modify-save around `Happiness`, with the clock injected.

    The clock is a constructor argument rather than a module-level default so
    AC4 can simulate elapsed wall time without sleeping, and so a test cannot
    accidentally pass by measuring process uptime.
    """

    def __init__(self, path: Path | str = DEFAULT_PATH, *,
                 clock=time.time, persist: bool = True) -> None:
        self.path = Path(path)
        self.clock = clock
        self.persist = persist
        self.state = load(self.path, now=clock()) if persist else \
            Happiness(value=0.0, updated_at=clock())

    def level(self) -> float:
        return self.state.level(self.clock())

    def intensity(self) -> float:
        return self.state.intensity(self.clock())

    def touch(self) -> float:
        """Apply one touch; return the delta. Persists if configured."""
        self.state, delta = self.state.touched(self.clock())
        if self.persist:
            save(self.state, self.path)
        return delta

    def settle(self) -> None:
        """Write the decayed value without a touch. For clean shutdown, so a
        long quiet period is recorded rather than inferred on next load."""
        self.state = self.state.decayed(self.clock())
        if self.persist:
            save(self.state, self.path)
