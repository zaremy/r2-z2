"""voice/transcribe.py — audio to text (#45, S2 V4).

WHERE THIS SITS
    mic -> capture -> wake -> TRANSCRIBE -> (V5) reason -> (V6) speak
                              ^^^^^^^^^^
    Takes the PCM buffer `capture.Utterance` hands over and returns words.
    Still no BLE, still no robot.

THE BACKPACK WILL NEVER RUN THIS, AND THAT IS THE POINT
    ESP-SR on the ESP32-S3 gives WakeNet (one wake phrase) and MultiNet (<=200
    FIXED phrases). Neither is open-vocabulary. So unlike `capture`, whose
    16 kHz contract exists to make the port a swap, this stage is deliberately
    Mac-only and the engine choice carries no portability weight. What it does
    carry is a seam: `Transcriber` is two methods, and V5 talks to that.

WHY THE MODEL IS RESIDENT, AND WHY THAT RULED OUT THE CLI
    #45 requires `base.en` warm-cached and a p50 under 1.5 s. whisper.cpp ships
    a perfectly good CLI, but shelling out reloads a 147 MB model on every
    utterance -- the spawn alone would eat the budget before a sample is read.
    So this uses the Python bindings and holds one `Model` for the process
    lifetime.

METAL IS VERIFIED AT RUNTIME, NOT ASSUMED
    #45 exists partly to avoid `faster-whisper`, which is CPU-only on Apple
    Silicon and forfeits the GPU silently. A prebuilt wheel can do exactly the
    same thing, so `WhisperCppTranscriber.backend_report()` reads back what
    whisper.cpp actually initialised. OBSERVED 2026-08-17 on an M3 Pro:

        whisper_backend_init_gpu: using MTL0 backend
        ggml_metal_init: found device: Apple M3 Pro

    A wheel that quietly fell back to CPU would still pass every functional
    test in this file, which is why the check is on the backend and not on the
    output.
"""

from __future__ import annotations

import contextlib
import os
import tempfile
import time
import wave
from dataclasses import dataclass

from capture import FORMAT, AudioFormat, FormatError

DEFAULT_MODEL = "base.en"


class _CapturedFd(list):
    """Holds captured text. A list so it can be filled on context exit —
    reading inside the block would race the C library's own flush."""

    @property
    def text(self) -> str:
        return self[0] if self else ""


@contextlib.contextmanager
def _capture_fd(fd: int):
    """Capture output written to a raw file descriptor by C code.

    Needed because whisper.cpp prints from C straight to fd 2 and never
    touches Python's sys.stderr. The captured text is available on the yielded
    object AFTER the block exits, once the fd has been restored and flushed.
    """
    out = _CapturedFd()
    tmp = tempfile.TemporaryFile(mode="w+b")
    saved = os.dup(fd)
    try:
        os.dup2(tmp.fileno(), fd)
        yield out
    finally:
        os.dup2(saved, fd)
        os.close(saved)
        try:
            tmp.seek(0)
            out.append(tmp.read().decode("utf-8", "replace"))
        finally:
            tmp.close()


@dataclass(frozen=True)
class Transcript:
    text: str
    engine: str
    elapsed_s: float
    audio_s: float

    @property
    def realtime_factor(self) -> float:
        """<1.0 means faster than the audio it transcribed."""
        return self.elapsed_s / self.audio_s if self.audio_s else float("inf")


class Transcriber:
    """The seam. V5 may rely on nothing but these."""

    name = "abstract"
    fmt: AudioFormat = FORMAT

    def warm(self) -> None:
        """Load whatever is expensive. Safe to call more than once."""

    def transcribe(self, pcm: bytes) -> Transcript:
        raise NotImplementedError

    def _check(self, pcm: bytes) -> float:
        """Validate against the capture contract; return duration in seconds."""
        if len(pcm) % (self.fmt.width_bytes * self.fmt.channels):
            raise FormatError(
                f"{len(pcm)} bytes is not whole "
                f"{self.fmt.width_bytes * 8}-bit/{self.fmt.channels}ch frames"
            )
        return self.fmt.bytes_to_ms(len(pcm)) / 1000.0


class StubTranscriber(Transcriber):
    """No model, no wheel, no network. #45 AC2: the stub passes the same tests
    the real engine does, which is what makes the seam a seam rather than a
    decoration."""

    name = "stub"

    def __init__(self, text: str = "stub transcript") -> None:
        self._text = text
        self.calls = 0

    def transcribe(self, pcm: bytes) -> Transcript:
        audio_s = self._check(pcm)
        self.calls += 1
        return Transcript(self._text, self.name, 0.0, audio_s)


class WhisperCppTranscriber(Transcriber):
    """whisper.cpp via its Python bindings, model held resident."""

    name = "whisper.cpp"

    def __init__(self, model: str = DEFAULT_MODEL, warm: bool = False) -> None:
        self.model_name = model
        self._model = None
        self._init_log = ""
        if warm:
            self.warm()

    def warm(self) -> None:
        if self._model is not None:
            return
        try:
            from pywhispercpp.model import Model
        except ImportError as exc:                       # pragma: no cover
            raise RuntimeError(
                "pywhispercpp is not installed. In mac-prototype:\n"
                "    .venv/bin/pip install pywhispercpp"
            ) from exc
        # whisper.cpp writes its banner from C, straight to FILE DESCRIPTOR 2.
        # `contextlib.redirect_stderr` swaps Python's sys.stderr object and does
        # NOT touch fd 2, so it captures nothing here -- the first version of
        # this method used it and reported `metal: False` on a run where the
        # Metal backend was demonstrably active. A check that silently fails
        # closed is worse than no check, since it invents the exact problem it
        # exists to detect. Capture at the fd level instead.
        with _capture_fd(2) as log:
            self._model = Model(
                self.model_name, print_realtime=False, print_progress=False
            )
        self._init_log = log.text

    def backend_report(self) -> dict:
        """What whisper.cpp actually initialised. See the module docstring."""
        if self._model is None:
            self.warm()
        log = self._init_log
        return {
            "metal": "using MTL0 backend" in log or "ggml_metal_init" in log,
            "device": next(
                (l.split("found device:")[1].strip()
                 for l in log.splitlines() if "found device:" in l),
                "unknown",
            ),
            "model": self.model_name,
        }

    def transcribe(self, pcm: bytes) -> Transcript:
        audio_s = self._check(pcm)
        self.warm()
        import numpy as np

        # whisper wants float32 in [-1, 1]; capture hands over int16 LE.
        samples = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0
        t0 = time.perf_counter()
        with _capture_fd(2):
            segments = self._model.transcribe(samples)
        elapsed = time.perf_counter() - t0
        text = " ".join(s.text.strip() for s in segments).strip()
        return Transcript(text, self.name, elapsed, audio_s)


ENGINES = {"whisper.cpp": WhisperCppTranscriber, "stub": StubTranscriber}


def create_transcriber(name: str = "whisper.cpp", **kwargs) -> Transcriber:
    try:
        cls = ENGINES[name]
    except KeyError:
        raise ValueError(
            f"unknown transcriber {name!r}; have {sorted(ENGINES)}"
        ) from None
    if cls is StubTranscriber:
        kwargs.pop("model", None)
        kwargs.pop("warm", None)
    return cls(**kwargs)


def read_wav(path) -> bytes:
    """Read a wav, asserting it matches the capture contract."""
    with wave.open(str(path), "rb") as w:
        FORMAT.assert_(w.getframerate(), w.getsampwidth(), w.getnchannels())
        return w.readframes(w.getnframes())
