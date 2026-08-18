"""voice/speak.py — Threepio's voice (#47, S2 V6).

R2 NEVER SPEAKS. THAT IS NOT A LIMITATION BEING WORKED AROUND
    He chirps, from his own body, from a table of 212 native ids. C-3PO is a
    SECOND CHARACTER who rides in the backpack and does the talking. This is
    canon -- a protocol droid translates for an astromech -- and it dissolves
    the speaker-displacement problem in D-011: Threepio's voice *should* sound
    like it comes from somewhere other than R2's body, because it is somebody
    else's voice.

    Amazon reached the same place from the other direction: they gave Astro
    the Alexa voice, found it "strange and creepy", and shipped a non-verbal
    body with speech demoted to a visibly separate character -- concluding the
    constraint "actually helped us create a better character".

THE CHARACTER LIVES IN A PROMPT, NOT IN A VOICE FILE
    `gpt-4o-mini-tts` takes plain-language direction, which is the entire
    reason #47 picked it for the MVP: the register is iterated by editing
    `STEERING` below rather than by tuning a synthesiser or commissioning a
    performance. And designing *a* protocol droid rather than cloning a
    specific performer sidesteps a likeness question we have no business
    picking up.

QUIET HOURS ARE ENFORCED HERE, NOT ONLY UPSTREAM
    #47 AC3 assigns quiet hours to V7. But V7 not calling `speak()` is a
    policy held by a caller, and `CLAUDE.md` is explicit that a guard belongs
    where the effect happens: `FORBIDDEN_OPS` was once enforced on the
    construction path and any caller that skipped construction skipped the
    guard (D-013). Sound is the one output that reaches a sleeping household
    through a closed door, so the gate is *also* here, defaulting to on. V7
    may still decide earlier and more cleverly; this is the floor.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime, time as dtime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from reason import ReasonError, error_beat                  # noqa: E402,F401

DEFAULT_TIMEOUT_S = 12.0
DEFAULT_VOICE = "ash"
DEFAULT_MODEL = "gpt-4o-mini-tts"

# The character, in plain language. Edit THIS to change who he is; do not add
# a second prompt somewhere else.
STEERING = (
    "You are a protocol droid: fussy, formal and over-precise, with a "
    "permanent undercurrent of anxiety. You speak in complete, slightly "
    "over-constructed sentences and you are unable to stop yourself supplying "
    "an unnecessary qualification. You are unfailingly polite, faintly "
    "put-upon, and you take mild catastrophes extremely seriously. Never "
    "shout. Never be cool. Deliver lines briskly but with careful diction, as "
    "though the listener might otherwise misunderstand something important."
)

# The line-writing prompt shares the character above so there is ONE
# description of who he is. #47 AC1 requires the ten test lines be generated
# from a steering prompt rather than read from a fixed script -- otherwise the
# criterion tests my writing rather than the prompt's.
LINE_SYSTEM = (
    STEERING + "\n\nWrite exactly one spoken line for the situation given. "
    "One or two sentences. Output the line only, with no quotation marks, no "
    "stage directions and no preamble. You are speaking aloud in a household, "
    "often about the astromech droid you accompany, who does not speak."
)


class SpeakError(RuntimeError):
    """Speech could not be produced."""


class QuietHoursError(SpeakError):
    """Refused because the household is asleep. Not a fault."""


@dataclass(frozen=True)
class QuietHours:
    """A nightly window in which nothing is voiced.

    Defaults chosen to be obviously conservative rather than tuned; V7 owns
    the real policy and the user's actual schedule.
    """

    start: dtime = dtime(22, 0)
    end: dtime = dtime(8, 0)
    enabled: bool = True

    def active(self, now: datetime | None = None) -> bool:
        if not self.enabled:
            return False
        t = (now or datetime.now()).time()
        if self.start <= self.end:
            return self.start <= t < self.end
        return t >= self.start or t < self.end        # window crosses midnight


@dataclass(frozen=True)
class Utterance:
    text: str
    audio: bytes
    fmt: str
    engine: str
    voice: str
    latency_s: float

    def write(self, path) -> Path:
        p = Path(path)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(self.audio)
        return p


# ------------------------------------------------------------------ the seam

class Speaker:
    """AC4: the seam. No caller outside this module names a vendor."""

    name = "abstract"
    voice = "none"

    def synthesize(self, text: str, timeout: float) -> Utterance:
        raise NotImplementedError


class StubSpeaker(Speaker):
    """No network, no key, no sound. Every test below runs on this."""

    name = "stub"
    voice = "stub"

    def __init__(self, error: Exception | None = None, delay: float = 0.0,
                 audio: bytes = b"RIFFstub") -> None:
        self.error, self.delay, self._audio = error, delay, audio
        self.calls: list[str] = []

    def synthesize(self, text: str, timeout: float) -> Utterance:
        self.calls.append(text)
        if self.delay > timeout:
            raise SpeakError(f"stub exceeded timeout {timeout}s")
        if self.error:
            raise self.error
        return Utterance(text, self._audio, "wav", self.name, self.voice, 0.0)


class OpenAITtsSpeaker(Speaker):
    """The only vendor-aware code for speech (AC4)."""

    name = "openai"
    URL = "https://api.openai.com/v1/audio/speech"

    def __init__(self, voice: str | None = None, model: str | None = None,
                 api_key: str | None = None, instructions: str = STEERING,
                 fmt: str = "wav") -> None:
        self.voice = voice or os.environ.get("TTS_VOICE") or DEFAULT_VOICE
        self.model = model or os.environ.get("TTS_MODEL") or DEFAULT_MODEL
        self.instructions = instructions
        self.fmt = fmt
        self._key = api_key or _tts_key()
        if not self._key:
            raise SpeakError(
                "No TTS key. Set TTS_API_KEY in .env, or set TTS_PROVIDER to "
                "the same value as LLM_PROVIDER to reuse LLM_API_KEY.")

    def synthesize(self, text: str, timeout: float) -> Utterance:
        body = {"model": self.model, "voice": self.voice, "input": text,
                "instructions": self.instructions, "response_format": self.fmt}
        req = urllib.request.Request(
            self.URL, data=json.dumps(body).encode(),
            headers={"Authorization": f"Bearer {self._key}",
                     "Content-Type": "application/json"})
        t0 = time.perf_counter()
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                audio = r.read()
        except urllib.error.HTTPError as e:
            raise SpeakError(f"TTS returned HTTP {e.code}") from e
        except Exception as e:
            raise SpeakError(f"TTS unreachable: {type(e).__name__}") from e
        return Utterance(text, audio, self.fmt, self.name, self.voice,
                         time.perf_counter() - t0)


def _tts_key() -> str:
    """TTS_API_KEY, or LLM_API_KEY when both slots name the SAME provider.

    Deliberately keyed on provider equality rather than hardwired: it saves
    pasting one key twice in the single-vendor case without ever assuming
    which vendor that is.
    """
    key = os.environ.get("TTS_API_KEY", "").strip()
    if key:
        return key
    tts_p = os.environ.get("TTS_PROVIDER", "").strip().lower()
    llm_p = os.environ.get("LLM_PROVIDER", "").strip().lower()
    if tts_p and tts_p == llm_p:
        return os.environ.get("LLM_API_KEY", "").strip()
    return ""


SPEAKERS = {"openai": OpenAITtsSpeaker, "stub": StubSpeaker}


def create_speaker(name: str | None = None, **kwargs) -> Speaker:
    name = name or os.environ.get("TTS_PROVIDER") or "stub"
    try:
        cls = SPEAKERS[name]
    except KeyError:
        raise ValueError(f"unknown TTS provider {name!r}; have {sorted(SPEAKERS)}") from None
    if cls is StubSpeaker:
        for k in ("voice", "model", "api_key", "instructions", "fmt"):
            kwargs.pop(k, None)
    return cls(**kwargs)


# --------------------------------------------------------------- line writing

def compose_line(situation: str, *, timeout: float = 15.0,
                 model: str | None = None, api_key: str | None = None) -> str:
    """Write one in-character line for a situation (#47 AC1).

    The lines are GENERATED so the taste criterion tests the steering prompt
    rather than my prose. A fixed script would pass AC1 and prove nothing.
    """
    key = api_key or os.environ.get("LLM_API_KEY", "").strip()
    if not key:
        raise SpeakError("LLM_API_KEY is not set; cannot compose lines")
    body = {"model": model or os.environ.get("LLM_MODEL") or "gpt-5.4-mini",
            "messages": [{"role": "system", "content": LINE_SYSTEM},
                         {"role": "user", "content": situation}]}
    req = urllib.request.Request(
        "https://api.openai.com/v1/chat/completions",
        data=json.dumps(body).encode(),
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            payload = json.load(r)
    except Exception as e:
        raise SpeakError(f"line composition failed: {type(e).__name__}") from e
    return payload["choices"][0]["message"]["content"].strip().strip('"')


# ------------------------------------------------------------------ speaking

def play(path: Path) -> None:
    """Mac audio out for the MVP; the ES8311 speaker on the backpack (D-011)."""
    subprocess.run(["afplay", str(path)], check=True)


def speak(text: str, *, speaker: Speaker | None = None,
          timeout: float = DEFAULT_TIMEOUT_S,
          quiet_hours: QuietHours | None = None,
          out_dir: Path | None = None,
          play_audio: bool = False,
          now: datetime | None = None) -> Utterance:
    """Say a line. Raises QuietHoursError rather than waking the house.

    `play_audio` defaults to FALSE. Generating audio and playing it are
    separate decisions, and the one that makes noise should be the one you
    have to ask for.
    """
    gate = quiet_hours if quiet_hours is not None else QuietHours()
    if gate.active(now):
        raise QuietHoursError(
            f"quiet hours {gate.start:%H:%M}-{gate.end:%H:%M}; not voicing "
            f"{text[:40]!r}")
    speaker = speaker or create_speaker()
    utt = speaker.synthesize(text, timeout)
    if utt.latency_s > timeout:
        raise SpeakError(f"TTS took {utt.latency_s:.1f}s, over the {timeout}s bound")
    if out_dir is not None:
        utt.write(Path(out_dir) / f"line.{utt.fmt}")
    if play_audio:
        with tempfile.NamedTemporaryFile(suffix=f".{utt.fmt}", delete=False) as f:
            f.write(utt.audio)
            tmp = Path(f.name)
        try:
            play(tmp)
        finally:
            tmp.unlink(missing_ok=True)
    return utt
