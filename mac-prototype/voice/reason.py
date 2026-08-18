"""voice/reason.py — heard text in, a behaviour out (#46, S2 V5).

WHERE THIS SITS
    capture -> wake -> transcribe -> REASON -> (V6) speak / behaviour -> r2
                                     ^^^^^^
    The last stage that is pure thought. It emits a `Beat` from the
    choreography layer and does not touch BLE.

THE MODEL NEVER NAMES AN OP, AND THAT IS THE WHOLE SAFETY ARGUMENT
    The obvious design lets the model return a timeline of ops and validates
    it. This does not. The tool schema offers exactly three closed choices --
    a sound NAME from `R2_SOUNDS`, a dome angle, a mood -- and this module
    composes the Beat from primitives.

    So `animation` and `set_stance` are not filtered out of model output; they
    are **unreachable from it**. A jailbroken, confused or malicious response
    cannot express them, because the vocabulary has no word for them. That is
    a structural guarantee rather than a validation rule, and validation rules
    are the ones that get bypassed by the next caller (D-013's own lesson:
    `FORBIDDEN_OPS` was enforced on the construction path, not the send path).

    `CLAUDE.md` says the reasoning layer calls `express_curious()`, not
    `set_dome_position(-27)`. This is that boundary, drawn at the model.

VALIDATION IS STILL REQUIRED, BECAUSE THE CLOSED SETS ARE NOT ENOUGH
    MEASURED 2026-08-17, five OpenAI models on this exact schema with
    "under 12 does not move" written into the parameter description: they
    returned dome angles of **0, 8, 12 and 20 degrees**. Two of four are below
    `MIN_DOME_TRAVEL_DEG`, where the dome SILENTLY ignores the command and
    still reports ok (D-013). Stating a constraint in a prompt is not
    enforcement -- the model is a suggestion engine, and every number it
    returns is checked here.

WHY MOOD DOES NOT PICK A COLOUR
    It would be the obvious mapping and it is forbidden. D-012 Amendment A:
    all six reachable RGB corners already carry a STATUS meaning, so a beat
    that painted "annoyed" would be overloading one of them. Expression is
    carried by motion, sound, the holo and the logic panel -- never by hue.
    Mood here selects a sound and a gesture; the rest colour stays a status
    claim.

TIMEOUT IS A CHARACTER REQUIREMENT, NOT AN ENGINEERING ONE
    A hung call leaves R2 lit in Processing forever, having visibly failed
    with no signal (#46 AC3). MEASURED across candidate models: p50 latency
    ranged from 0.63 s to 14.7 s for the same task, so this is not
    theoretical. The timeout is enforced here and `error_beat()` is the
    defined path back to neutral.
"""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import r2_behavior as B                                    # noqa: E402
from r2_assets import R2_SOUNDS                            # noqa: E402

# Every model call is bounded by this. See the module docstring: the fastest
# candidate ran 0.63 s p50 and the slowest 14.7 s, so a generous-looking bound
# is still a real one.
DEFAULT_TIMEOUT_S = 6.0

# Legal moods. Closed set -- the model cannot invent a seventh.
MOODS = ("curious", "happy", "annoyed", "sad", "alert")

# `bounded_head_move` caps travel at 45 deg; below MIN_DOME_TRAVEL_DEG the
# dome does not move at all and still reports success (D-013).
MAX_DOME_DEG = 45.0


class ReasonError(RuntimeError):
    """The model could not be consulted, or said something unusable."""


@dataclass
class Behaviour:
    """What the reasoning layer decided. Validated by construction."""

    heard: str
    mood: str
    sound_id: int
    dome_deg: float
    rejections: tuple[str, ...] = ()
    latency_s: float = 0.0
    model: str = ""

    def to_beat(self) -> B.Beat:
        """Compose primitives. Never an animation, never a stance."""
        steps = [B.Step("leds", {"channels": B.front(B.BASE_ENGAGED)}),
                 B.Step("sound", {"id": self.sound_id})]
        if self.dome_deg:
            steps.append(B.Step("dome", {"delta": self.dome_deg}))
        # The base layer is STATE and survives the link dropping, so the last
        # thing any beat does is restore it. `Beat.validate()` enforces this
        # and caught the first version of this method, which ended on `dome`
        # and would have left him lit cyan until something else changed it.
        steps.append(B.Step("leds", {"channels": B.front(B.BASE_NEUTRAL)}))
        beat = B.Beat(
            name=f"reason:{self.mood}",
            phrases=(B.Phrase(tuple(steps), gap_s=0.0),),
            rest_colour=B.BASE_NEUTRAL,
            interruptible=True,
            energy="low",
            cooldown_s=2.0,
            evidence=f"V5 reason(), model={self.model or 'stub'}",
            notes=f"heard: {self.heard!r}",
            return_to_start=bool(self.dome_deg),
        )
        beat.validate()
        if B.tier_rank(beat.required_tier()) > B.tier_rank("dome"):
            raise ReasonError(f"composed beat exceeds the dome tier: "
                              f"{beat.required_tier()}")
        return beat

    def describe(self) -> str:
        """#46 AC5 — heard text, intent, sound id, timeline, in the terminal."""
        beat = self.to_beat()
        lines = [f'  heard    : "{self.heard}"',
                 f"  mood     : {self.mood}",
                 f"  sound    : {self.sound_id} ({_name_of(self.sound_id)})",
                 f"  dome     : {self.dome_deg:+.1f} deg" if self.dome_deg
                 else "  dome     : (no move)",
                 f"  model    : {self.model or 'stub'}  in {self.latency_s:.2f}s",
                 f"  tier     : {beat.required_tier()}",
                 "  timeline :"]
        for p in beat.phrases:
            for s in p.steps:
                lines.append(f"      {s.op:<6} {s.params}")
        for r in self.rejections:
            lines.append(f"  REJECTED : {r}")
        return "\n".join(lines)


def _name_of(sound_id: int) -> str:
    for k, v in R2_SOUNDS.items():
        if v == sound_id:
            return k
    return "?"


def error_beat(reason: str = "reasoning unavailable") -> B.Beat:
    """The defined path back to neutral (#46 AC3).

    Yellow, not red. `BASE_DANGER` is "danger and stop, ONLY" (D-012
    Amendment A); a reasoning call that timed out is "needs monitoring", which
    is exactly what BASE_PENDING means. Mis-signalling a network hiccup as
    danger would spend the one colour reserved for stopping him.
    """
    beat = B.Beat(
        name="reason:error",
        phrases=(
            B.Phrase((B.Step("leds", {"channels": B.front(B.BASE_PENDING)}),),
                     gap_s=0.8),
            # Show the fault, then hand him back neutral. Ending on PENDING
            # would leave the household looking at a warning indefinitely.
            B.Phrase((B.Step("leds", {"channels": B.front(B.BASE_NEUTRAL)}),),
                     gap_s=0.0),
        ),
        rest_colour=B.BASE_NEUTRAL,
        interruptible=True,
        energy="low",
        cooldown_s=0.0,
        evidence="V5 reason() failure path",
        notes=reason,
    )
    beat.validate()
    return beat


# --------------------------------------------------------------- the schema

def tool_schema() -> dict:
    """The model's entire vocabulary. Three closed choices, no ops."""
    return {
        "type": "function",
        "function": {
            "name": "react",
            "description": ("Choose how R2-D2 reacts to what was said. He never "
                            "speaks words; he chirps and moves his dome."),
            "parameters": {
                "type": "object",
                "properties": {
                    "sound": {
                        "type": "string",
                        "enum": sorted(R2_SOUNDS),
                        "description": "A sound id name from R2's native table.",
                    },
                    "dome_deg": {
                        "type": "number",
                        "description": (
                            f"Dome turn in degrees, negative left. Must be 0 or "
                            f"at least {B.MIN_DOME_TRAVEL_DEG:g}; anything "
                            f"smaller does not move the dome at all. Max "
                            f"{MAX_DOME_DEG:g}."),
                    },
                    "mood": {"type": "string", "enum": list(MOODS)},
                },
                "required": ["sound", "dome_deg", "mood"],
            },
        },
    }


SYSTEM_PROMPT = (
    "You are the reasoning service for R2-D2, an astromech droid who lives in a "
    "household. He NEVER speaks words — he chirps, whistles and turns his dome. "
    "Given what a person said to him, choose his reaction by calling the react "
    "tool. Pick the sound whose name best fits the mood of the moment. Keep him "
    "understated: he is a presence in the house, not a performer."
)


def _validate(raw: dict, heard: str, *, model: str, latency: float) -> Behaviour:
    """Everything the model said, checked before it can become BLE traffic."""
    rejections: list[str] = []

    name = raw.get("sound")
    sound_id = R2_SOUNDS.get(name) if isinstance(name, str) else None
    if sound_id is None and isinstance(name, (int, float)):
        sound_id = int(name) if int(name) in set(R2_SOUNDS.values()) else None
    if sound_id is None:
        raise ReasonError(
            f"model chose sound {name!r}, which is not in R2_SOUNDS. "
            f"Rejected before any BLE traffic (#46 AC2).")
    if sound_id not in set(R2_SOUNDS.values()):                # belt and braces
        raise ReasonError(f"sound id {sound_id} is not a value in R2_SOUNDS")

    mood = raw.get("mood")
    if mood not in MOODS:
        rejections.append(f"mood {mood!r} not in {MOODS}; using 'curious'")
        mood = "curious"

    try:
        deg = float(raw.get("dome_deg", 0.0))
    except (TypeError, ValueError):
        rejections.append(f"dome_deg {raw.get('dome_deg')!r} is not a number; using 0")
        deg = 0.0
    if abs(deg) > MAX_DOME_DEG:
        rejections.append(f"dome_deg {deg:g} exceeds {MAX_DOME_DEG:g}; clamped")
        deg = MAX_DOME_DEG if deg > 0 else -MAX_DOME_DEG
    if 0 < abs(deg) < B.MIN_DOME_TRAVEL_DEG:
        # MEASURED: models return 0 and 8 despite the constraint in the schema.
        rejections.append(
            f"dome_deg {deg:g} is below MIN_DOME_TRAVEL_DEG "
            f"({B.MIN_DOME_TRAVEL_DEG:g}); the dome would silently ignore it "
            f"and report ok, so it is dropped to 0")
        deg = 0.0

    return Behaviour(heard=heard, mood=mood, sound_id=sound_id, dome_deg=deg,
                     rejections=tuple(rejections), latency_s=latency, model=model)


# ------------------------------------------------------------- the providers

class Provider:
    """The seam. AC4: no code outside this module names a vendor."""

    name = "abstract"

    def react(self, heard: str, timeout: float) -> dict:
        raise NotImplementedError


class StubProvider(Provider):
    """No network, no key. Lets every test above run offline."""

    name = "stub"

    def __init__(self, response: dict | None = None, delay: float = 0.0,
                 error: Exception | None = None) -> None:
        self.response = response or {"sound": "R2_CHATTY_1", "dome_deg": 15.0,
                                     "mood": "curious"}
        self.delay = delay
        self.error = error
        self.calls = 0

    def react(self, heard: str, timeout: float) -> dict:
        self.calls += 1
        if self.delay:
            # Honour the contract the real provider honours.
            if self.delay > timeout:
                raise ReasonError(f"stub exceeded timeout {timeout}s")
            time.sleep(self.delay)
        if self.error:
            raise self.error
        return dict(self.response)


class OpenAIProvider(Provider):
    """The only vendor-aware code in the project (AC4)."""

    name = "openai"
    URL = "https://api.openai.com/v1/chat/completions"

    def __init__(self, model: str | None = None, api_key: str | None = None) -> None:
        self.model = model or os.environ.get("LLM_MODEL") or "gpt-5.4-mini"
        self._key = api_key or os.environ.get("LLM_API_KEY", "")
        if not self._key:
            raise ReasonError(
                "LLM_API_KEY is not set. Put it in .env (see .env.example); "
                "it is never read or logged by this code.")

    def react(self, heard: str, timeout: float) -> dict:
        body = {"model": self.model,
                "messages": [{"role": "system", "content": SYSTEM_PROMPT},
                             {"role": "user", "content": heard}],
                "tools": [tool_schema()],
                "tool_choice": {"type": "function", "function": {"name": "react"}}}
        req = urllib.request.Request(
            self.URL, data=json.dumps(body).encode(),
            headers={"Authorization": f"Bearer {self._key}",
                     "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                payload = json.load(r)
        except urllib.error.HTTPError as e:
            raise ReasonError(f"provider returned HTTP {e.code}") from e
        except Exception as e:
            raise ReasonError(f"provider unreachable: {type(e).__name__}") from e
        calls = payload["choices"][0]["message"].get("tool_calls")
        if not calls:
            raise ReasonError("model returned no tool call")
        return json.loads(calls[0]["function"]["arguments"])


PROVIDERS = {"openai": OpenAIProvider, "stub": StubProvider}


def create_provider(name: str | None = None, **kwargs) -> Provider:
    name = name or os.environ.get("LLM_PROVIDER") or "stub"
    try:
        cls = PROVIDERS[name]
    except KeyError:
        raise ValueError(f"unknown provider {name!r}; have {sorted(PROVIDERS)}") from None
    return cls(**kwargs)


# ------------------------------------------------------------------ the call

def reason(heard: str, state: dict | None = None, *,
           provider: Provider | None = None,
           timeout: float = DEFAULT_TIMEOUT_S) -> Behaviour:
    """Heard text -> a validated Behaviour. Raises ReasonError on any failure.

    `state` is accepted now and unused: V7 owns persistent state, and taking
    the parameter here means V7 does not change this signature.
    """
    provider = provider or create_provider()
    t0 = time.perf_counter()
    raw = provider.react(heard, timeout)
    latency = time.perf_counter() - t0
    if latency > timeout:
        raise ReasonError(f"provider took {latency:.1f}s, over the {timeout}s bound")
    return _validate(raw, heard, model=getattr(provider, "model", provider.name),
                     latency=latency)


def main(argv: list[str] | None = None) -> int:
    """AC5: print heard text, intent, sound id and timeline."""
    import argparse
    p = argparse.ArgumentParser(prog="python3 voice/reason.py")
    p.add_argument("text", nargs="+", help="what was heard")
    p.add_argument("--provider", default=None)
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_S)
    a = p.parse_args(argv)
    heard = " ".join(a.text)
    try:
        b = reason(heard, provider=create_provider(a.provider), timeout=a.timeout)
    except ReasonError as e:
        print(f"reason() FAILED: {e}")
        print("  -> error beat:", error_beat(str(e)).name,
              "resting on", error_beat(str(e)).rest_colour)
        return 1
    print(b.describe())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
