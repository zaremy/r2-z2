"""voice/wake.py — "was that my name?", behind ONE interface (#42, S2 V1).

WHY THIS IS AN INTERFACE AND NOT A PORCUPINE WRAPPER
    #42 says "Picovoice Porcupine for the wake word, stock phrase. Check
    personal-use licensing before committing and record what the terms
    actually are." Checked, 2026-08-17:

      OBSERVED (picovoice.ai/docs/faq/general/, fetched 2026-08-17), verbatim:
        "Picovoice is a B2B company focused on on-device AI tools for
         enterprises. At this time, there are no dedicated free or paid plans
         for personal or non-commercial use."

      OBSERVED (Picovoice notice, reported via Home Assistant community
      thread 1012744): the Free Tier was discontinued on 2026-06-30 and
      existing Free Tier AccessKeys stop working. Before that it allowed up to
      three active users a month, and custom wake words were personal-use only
      and expired after 30 days.

    So the premise behind "porcupine sounds like safe to start with" expired
    seven weeks before this file was written. The SDK is still Apache-2.0 and
    the code below still works -- it needs an AccessKey this project does not
    have and currently cannot obtain on a personal plan.

    The response is not to pick a winner unilaterally. It is to make the
    engine a seam: `WakeEngine` is four methods, and every caller upstream
    talks to that. Swapping vendors is a one-line change in `create_engine`,
    and the ESP32 port swaps in ESP-SR's on-chip detector behind the same
    seam.

THE ALTERNATIVE, MEASURED AGAINST THE SAME BAR
    openWakeWord (github.com/dscripka/openWakeWord): code Apache-2.0, no key,
    no account, runs on CPU via onnxruntime. Pre-trained models ship for
    "alexa", "hey mycroft", "hey jarvis", "hey rhasspy". Two things to know
    before choosing it: the MODELS are CC BY-NC-SA 4.0 (non-commercial -- fine
    for a household droid, not fine if this is ever sold), and the last
    release is v0.6.0, Feb 2024. Its native frame is 1280 samples / 80 ms
    against Porcupine's 512 / 32 ms, which is exactly why `capture.Chunker`
    exists and why nothing upstream may assume a frame size.

    UNKNOWN: neither engine's accuracy has been measured in Room B. #42 AC1
    and AC3 are the measurement, and they need a human in the room.

THE PHRASE IS "z2", AND ITS RISK IS RECORDED RATHER THAN ARGUED
    Operator decision, 2026-08-17, stated twice: the wake phrase is "z2"
    ("zee-two"). No stock model exists for it in any free engine, so it has to
    be trained -- see voice/README.md.

    The known risk, recorded so the measurement can settle it: "z2" is TWO
    syllables. ESPHome's guidance for microWakeWord -- the framework that will
    run this on the backpack -- is a phrase of "3-4 syllables that is not
    commonly used so that it does not trigger Assist by mistake"
    (esphome.io/components/micro_wake_word/). Two syllables is under that, and
    "zee-two" is a near neighbour of ordinary speech: "these two", "he's two",
    "she's due". Every one of those is a false accept under exactly the
    condition AC1 tests in -- a room with a TV on.

    This is NOT a reason to override the decision. It is the reason AC3 exists:
    a 30-minute ambient sample turns "I think this will false-fire" into a
    number, and the sensitivity is then set from that number. If the rate comes
    back unusable, the cheapest fix is a longer phrase, and the seam below
    means swapping the model is a config change, not a rewrite.
"""

from __future__ import annotations

import os
import time
from dataclasses import dataclass
from pathlib import Path

from capture import FORMAT, AudioFormat, FormatError

# Sensitivity is a placeholder until AC3's 30-minute ambient sample sets it.
# Recorded as a starting point rather than a tuned value; see #42 AC3.
DEFAULT_SENSITIVITY = 0.5

# The wake model we actually ship. Anchored to this package rather than the
# cwd so `python3 -m voice` works from anywhere.
#
# THE OPERATOR'S PHRASE IS "z2" AND THIS IS NOT IT.
#   "z2" was chosen (#42) and no free engine ships it, so it needs training --
#   README "Training your own" is the recipe. That training never happened, so
#   `models/z2.onnx` has never existed on disk. Pointing the default at it made
#   the default unusable: every caller that did not pass `keywords=` hit the
#   does-not-exist guard below. `converse.py` happened to pass its own path, so
#   the whole voice loop ran while this constant was dead -- the same shape as
#   the CLI that re-declared NOISE_MARGIN_DB and made the class constant inert.
#   Until z2 is trained, the shipped default is the community r2d2 model and the
#   spoken phrase is "R2-D2". Swap this line when z2.onnx lands; do not swap it
#   back to a filename that is not in `models/`.
MODELS_DIR = Path(__file__).resolve().parent / "models"
DEFAULT_KEYWORD = str(MODELS_DIR / "r2d2.onnx")


@dataclass(frozen=True)
class WakeEvent:
    keyword: str
    score: float
    at: float          # time.time() of the frame that triggered it


class WakeEngine:
    """The seam. Four members, and upstream may rely on nothing else.

    `frame_samples` is the engine's, not ours -- see the module docstring.
    """

    name = "abstract"
    frame_samples = 512
    fmt: AudioFormat = FORMAT

    @property
    def frame_bytes(self) -> int:
        return self.fmt.frame_bytes(self.frame_samples)

    def process(self, frame: bytes) -> WakeEvent | None:
        raise NotImplementedError

    def close(self) -> None:
        pass

    def __enter__(self) -> "WakeEngine":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _check(self, frame: bytes) -> None:
        if len(frame) != self.frame_bytes:
            raise FormatError(
                f"{self.name} wants {self.frame_bytes} bytes "
                f"({self.frame_samples} samples), got {len(frame)}. "
                f"Feed frames through capture.Chunker(engine.frame_bytes)."
            )


class PorcupineEngine(WakeEngine):
    """Picovoice Porcupine. Needs PICOVOICE_ACCESS_KEY; see the licensing note above."""

    name = "porcupine"

    def __init__(
        self,
        keywords: list[str] | None = None,
        sensitivity: float = DEFAULT_SENSITIVITY,
        access_key: str | None = None,
    ) -> None:
        try:
            import pvporcupine
        except ImportError as exc:                       # pragma: no cover
            raise RuntimeError(
                "pvporcupine is not installed. In mac-prototype:\n"
                "    .venv/bin/pip install pvporcupine"
            ) from exc

        key = access_key or os.environ.get("PICOVOICE_ACCESS_KEY", "").strip()
        if not key:
            raise RuntimeError(
                "PICOVOICE_ACCESS_KEY is not set. Note that Picovoice "
                "discontinued its free tier on 2026-06-30 and states it has no "
                "plan for personal or non-commercial use -- see the licensing "
                "note at the top of voice/wake.py. Try `--engine openwakeword`."
            )

        self.keywords = keywords or ["computer"]
        self._p = pvporcupine.create(
            access_key=key,
            keywords=self.keywords,
            sensitivities=[sensitivity] * len(self.keywords),
        )
        self.frame_samples = self._p.frame_length
        FORMAT.assert_(self._p.sample_rate, FORMAT.width_bytes, FORMAT.channels)

    def process(self, frame: bytes) -> WakeEvent | None:
        import struct

        self._check(frame)
        pcm = struct.unpack_from(f"<{self.frame_samples}h", frame)
        idx = self._p.process(pcm)
        if idx < 0:
            return None
        # Porcupine reports a match, not a confidence. Reporting 1.0 rather
        # than inventing a number keeps "score" honest across engines.
        return WakeEvent(keyword=self.keywords[idx], score=1.0, at=time.time())

    def close(self) -> None:
        if getattr(self, "_p", None) is not None:
            self._p.delete()
            self._p = None


class OpenWakeWordEngine(WakeEngine):
    """openWakeWord. No key, no account. Models are CC BY-NC-SA 4.0."""

    name = "openwakeword"
    frame_samples = 1280        # 80 ms, the engine's native step

    def __init__(
        self,
        keywords: list[str] | None = None,
        sensitivity: float = DEFAULT_SENSITIVITY,
    ) -> None:
        try:
            import numpy
            from openwakeword.model import Model
        except ImportError as exc:                       # pragma: no cover
            raise RuntimeError(
                "openwakeword is not installed. In mac-prototype:\n"
                "    .venv/bin/pip install openwakeword\n"
                "then once, to fetch the pre-trained models:\n"
                "    .venv/bin/python -c "
                "'import openwakeword; openwakeword.utils.download_models()'"
            ) from exc

        self._np = numpy
        # Each entry is EITHER a stock model name ("hey_jarvis") OR a path to a
        # trained .onnx/.tflite. openWakeWord resolves an existing path
        # directly and takes the event name from the basename, so a trained
        # `models/z2.onnx` reports itself as "z2" with no extra mapping
        # (openwakeword/model.py:86-99).
        self.keywords = keywords or [DEFAULT_KEYWORD]
        for k in self.keywords:
            looks_like_path = k.endswith((".onnx", ".tflite")) or os.sep in k
            if looks_like_path and not os.path.exists(k):
                raise RuntimeError(
                    f"wake-word model {k!r} does not exist yet.\n"
                    f"The phrase 'z2' has no stock model in any free engine -- "
                    f"it has to be trained once.\n"
                    f"See mac-prototype/voice/README.md for the ~1 hour "
                    f"training step, or pass --keyword hey_jarvis to smoke-test "
                    f"the pipeline with a stock model first."
                )
        # openWakeWord scores 0..1 per model; sensitivity is our threshold on
        # that score rather than an engine parameter.
        self.threshold = sensitivity
        self._model = Model(wakeword_models=list(self.keywords))

    def process(self, frame: bytes) -> WakeEvent | None:
        self._check(frame)
        samples = self._np.frombuffer(frame, dtype="<i2")
        scores = self._model.predict(samples)
        best, score = max(scores.items(), key=lambda kv: kv[1])
        if score < self.threshold:
            return None
        # The model keeps internal state across frames; without a reset it
        # re-fires on the decaying tail of the same utterance for ~1 s.
        self._model.reset()
        return WakeEvent(keyword=best, score=float(score), at=time.time())


class ScriptedEngine(WakeEngine):
    """Fires on a fixed schedule. Exists so the pipeline is testable with no
    wheels, no models and no microphone -- the same reason the R2 driver's
    rubric was dry-tested on synthetic data before the link came up."""

    name = "scripted"

    def __init__(self, fire_on: set[int] | None = None, frame_samples: int = 512) -> None:
        self.frame_samples = frame_samples
        self.fire_on = fire_on or {0}
        self.seen = 0

    def process(self, frame: bytes) -> WakeEvent | None:
        self._check(frame)
        idx = self.seen
        self.seen += 1
        if idx in self.fire_on:
            return WakeEvent(keyword="scripted", score=1.0, at=time.time())
        return None


ENGINES = {
    "porcupine": PorcupineEngine,
    "openwakeword": OpenWakeWordEngine,
    "scripted": ScriptedEngine,
}


def create_engine(name: str, **kwargs) -> WakeEngine:
    """The whole vendor decision, in one place, by design."""
    try:
        cls = ENGINES[name]
    except KeyError:
        raise ValueError(
            f"unknown wake engine {name!r}; have {sorted(ENGINES)}"
        ) from None
    if cls is ScriptedEngine:
        kwargs.pop("sensitivity", None)
        kwargs.pop("keywords", None)
    return cls(**kwargs)
