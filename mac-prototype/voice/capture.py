"""voice/capture.py — audio in. The first half of the input path (#42, S2 V1).

WHERE THIS SITS
    mic -> CAPTURE -> wake -> (V4) transcribe -> (V5) reason -> behavior -> r2
           ^^^^^^^
    Nothing below `wake` knows the robot exists. This module opens no BLE
    link, imports no `r2_*`, and runs with R2 powered off. That isolation is
    the deliverable, not a side effect: when the composed behaviour fails we
    need to know whether it was the microphone or the choreography.

THE FORMAT IS A CONTRACT, NOT A PREFERENCE
    16 kHz / 16-bit signed LE / mono. It is what the ES8311 on the
    ESP32-S3-Touch-AMOLED-1.8 produces, what ESP-SR expects, and what every
    ESP32 voice satellite streams. Picking anything else here makes the
    eventual backpack port a rewrite rather than a swap. `AudioFormat.assert_`
    exists so the device default is CHECKED rather than trusted.

    OBSERVED 2026-08-17 on this Mac: the built-in "MacBook Pro Microphone"
    reports `default_samplerate 48000.0`; CoreAudio accepts a 16 kHz / int16 /
    mono request anyway, and the opened stream then reports
    `stream.samplerate == 16000.0`.

    So be precise about what `assert_` buys, because the obvious reading is
    wrong. PortAudio reports the NEGOTIATED rate, not the hardware rate --
    16000 here means "you will be handed 16 kHz", NOT "the microphone runs at
    16 kHz". Transparent resampling is therefore INVISIBLE to this check and
    always will be.

    What the check does catch is a host that quietly SUBSTITUTES a different
    rate or channel count instead of honouring the request, which is a real
    and silent failure on some devices. What it cannot do is prove the audio
    was never resampled. Recorded rather than assumed, because an assertion
    trusted for more than it can do is worse than no assertion: it converts an
    unknown into a false sense of a verified one.

    On the Mac the resample is fine -- CoreAudio's 48k->16k is transparent for
    wake-word purposes. The contract matters because the ES8311 on the backpack
    produces 16 kHz natively, and code written against 48 kHz would have to be
    rewritten rather than swapped.

WHY CAPTURE DOES NOT PICK A FRAME SIZE
    The two candidate wake engines disagree, and neither is wrong:
      - Porcupine  wants exactly 512 samples (32 ms) per call.
      - openWakeWord wants multiples of 1280 samples (80 ms).
    So this module streams BYTES and `Chunker` re-blocks them into whatever
    frame length the engine asks for. The engine owns its frame size; capture
    owns the sample format. Hardwiring 512 here would have quietly locked the
    project to one vendor at the exact moment that vendor's free tier closed.

PURE BY DEFAULT
    Everything above `MicSource` is pure Python over `bytes` and `array` --
    no numpy, no sounddevice, no PortAudio. The whole segmentation state
    machine is therefore dry-testable against synthetic PCM with no hardware
    and no wheels installed, which is how the R2 driver's rubric bugs were
    caught before the link came up. `sounddevice` is imported lazily, inside
    `MicSource.__enter__`, so importing this module never needs a microphone.
"""

from __future__ import annotations

import array
import math
import sys
import time
import wave
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

# ---------------------------------------------------------------- the contract

SAMPLE_RATE_HZ = 16_000
SAMPLE_WIDTH_BYTES = 2          # signed 16-bit little-endian
CHANNELS = 1
BYTES_PER_SECOND = SAMPLE_RATE_HZ * SAMPLE_WIDTH_BYTES * CHANNELS

# Full-scale amplitude for signed 16-bit. Used to normalise RMS to 0.0-1.0 so
# thresholds in this file mean the same thing regardless of sample width.
FULL_SCALE_S16 = 32768.0


class FormatError(ValueError):
    """The audio device did not give us the contract above."""


@dataclass(frozen=True)
class AudioFormat:
    rate_hz: int = SAMPLE_RATE_HZ
    width_bytes: int = SAMPLE_WIDTH_BYTES
    channels: int = CHANNELS

    @property
    def bytes_per_second(self) -> int:
        return self.rate_hz * self.width_bytes * self.channels

    def frame_bytes(self, samples: int) -> int:
        return samples * self.width_bytes * self.channels

    def ms_to_bytes(self, ms: float) -> int:
        """Byte count for `ms`, rounded DOWN to a whole frame.

        Rounding down matters: a byte count that is not a multiple of
        width*channels splits a sample across a boundary, and every downstream
        `array('h', buf)` then raises instead of merely sounding wrong.
        """
        raw = int(self.bytes_per_second * ms / 1000.0)
        frame = self.width_bytes * self.channels
        return raw - (raw % frame)

    def bytes_to_ms(self, n: int) -> float:
        return 1000.0 * n / self.bytes_per_second

    def assert_(self, rate_hz: float, width_bytes: int, channels: int) -> None:
        """Fail loudly when the device disagrees with the contract.

        AC2 of #42 is "assert it in a test rather than trusting the device
        default". This is that assertion, and it is deliberately a hard error:
        a silently resampled 48 kHz stream still transcribes well enough to
        look fine on the Mac and then fails on the ES8311, which is the worst
        possible place to discover it.
        """
        actual = (int(rate_hz), int(width_bytes), int(channels))
        wanted = (self.rate_hz, self.width_bytes, self.channels)
        if actual != wanted:
            raise FormatError(
                f"audio format mismatch: device gave "
                f"{actual[0]} Hz / {actual[1] * 8}-bit / {actual[2]}ch, "
                f"contract requires "
                f"{wanted[0]} Hz / {wanted[1] * 8}-bit / {wanted[2]}ch"
            )


FORMAT = AudioFormat()


# ------------------------------------------------------------------- measuring

def rms(pcm: bytes) -> float:
    """Normalised RMS (0.0-1.0) of signed-16 mono PCM.

    Pure `array` rather than numpy so the segmentation logic below stays
    importable in a bare venv. n=16000 (one second) costs ~1 ms here, which is
    three orders of magnitude under the frame budget.
    """
    if not pcm:
        return 0.0
    if len(pcm) % 2:
        raise FormatError(f"odd byte count {len(pcm)} is not whole 16-bit samples")
    samples = array.array("h")
    samples.frombytes(pcm)
    if sys.byteorder == "big":       # the contract is little-endian
        samples.byteswap()
    total = 0
    for s in samples:
        total += s * s
    return math.sqrt(total / len(samples)) / FULL_SCALE_S16


def dbfs(level: float) -> float:
    """RMS as dBFS, for terminal output. Silence floors at -120 rather than -inf."""
    return 20.0 * math.log10(level) if level > 1e-6 else -120.0


# -------------------------------------------------------------------- plumbing

class Chunker:
    """Re-blocks an arbitrary byte stream into exact `size`-byte frames.

    The mic hands over whatever PortAudio felt like delivering; the wake engine
    demands an exact frame length and raises on anything else. This sits
    between them and holds the remainder. Feeding it a partial frame yields
    nothing until the rest arrives, which is the entire point -- dropping the
    remainder loses up to 31 ms of speech per block and silently degrades
    detection in a way no test would notice.
    """

    def __init__(self, size: int) -> None:
        if size <= 0:
            raise ValueError("chunk size must be positive")
        self.size = size
        self._buf = bytearray()

    def feed(self, data: bytes) -> Iterator[bytes]:
        self._buf.extend(data)
        while len(self._buf) >= self.size:
            yield bytes(self._buf[: self.size])
            del self._buf[: self.size]

    @property
    def pending(self) -> int:
        return len(self._buf)


class RingBuffer:
    """Fixed-capacity PCM ring, used for PRE-ROLL.

    A wake engine reports the phrase only once it has heard all of it, so by
    the time we are told to start recording, the first ~200 ms of what the
    speaker said next is already past. Keeping a rolling window means the
    utterance handed downstream begins BEFORE the detection instant instead of
    clipping its own first syllable.
    """

    def __init__(self, capacity_bytes: int) -> None:
        if capacity_bytes <= 0:
            raise ValueError("ring capacity must be positive")
        self.capacity = capacity_bytes
        self._buf = deque(maxlen=capacity_bytes)

    def write(self, data: bytes) -> None:
        self._buf.extend(data)

    def read(self) -> bytes:
        return bytes(self._buf)

    def clear(self) -> None:
        self._buf.clear()

    def __len__(self) -> int:
        return len(self._buf)


# ---------------------------------------------------------------- segmentation

@dataclass(frozen=True)
class Utterance:
    """One captured utterance, ready to hand to transcription (V4)."""

    pcm: bytes
    started_at: float          # time.time() at the wake event
    reason: str                # "silence" | "max_duration"
    peak_level: float
    fmt: AudioFormat = FORMAT

    @property
    def duration_s(self) -> float:
        return self.fmt.bytes_to_ms(len(self.pcm)) / 1000.0


class Segmenter:
    """Collects audio from a wake event until the speaker stops.

    End-of-speech is RMS-and-hangover, not a VAD model. That is a deliberate
    floor, not a shortcut: it has no license, no wheel, and no accuracy claim
    to defend, and V4 will replace it with the transcriber's own endpointing.
    What it must not do is end the utterance on the natural pause between two
    words, which is why `silence_ms` defaults to 700 -- comfortably past an
    inter-word gap and still under a second of dead air.

    `threshold` MUST be calibrated per device. This was measured, not assumed,
    and the result was stronger than "the default was wrong":

      MEASURED 2026-08-17, two microphones, same room, same speaker.
        MacBook Pro Microphone  noise floor -68.6 dBFS, speech -39 to -53 dBFS
        Logitech BRIO           noise floor -47.6 dBFS, speech -31 to -34 dBFS

      The BRIO's SILENCE is louder than the MacBook mic's SPEECH. No absolute
      constant separates the two on both devices: -40 dBFS works on the BRIO
      and classifies most MacBook speech as silence; -60 dBFS works on the
      MacBook and makes BRIO silence read as speech, so an utterance never
      ends. The original 0.010 default landed within 0.2 dB of the MacBook's
      measured speech peak -- every frame of a real sentence read as silence.

    So the threshold is derived from a measured noise floor via `calibrate()`,
    and `DEFAULT_THRESHOLD` survives only as the fallback for callers that
    cannot sample the room first. Prefer `calibrate()`.
    """

    # Fallback only. Correct for the BRIO, wrong for the built-in mic -- which
    # is exactly why it must not be relied on. See calibrate().
    DEFAULT_THRESHOLD = 0.010          # ~-40 dBFS
    DEFAULT_SILENCE_MS = 700
    DEFAULT_MAX_MS = 12_000
    DEFAULT_PREROLL_MS = 500

    def __init__(
        self,
        fmt: AudioFormat = FORMAT,
        threshold: float = DEFAULT_THRESHOLD,
        silence_ms: int = DEFAULT_SILENCE_MS,
        max_ms: int = DEFAULT_MAX_MS,
    ) -> None:
        self.fmt = fmt
        self.threshold = threshold
        self.silence_ms = silence_ms
        self.max_ms = max_ms
        self._collected = bytearray()
        self._silent_bytes = 0
        self._peak = 0.0
        self._started_at = 0.0
        self._active = False

    # How far above the measured noise floor a frame must sit to count as
    # speech. 12 dB = 4x in amplitude. Both measured mics clear this
    # comfortably: the BRIO ran ~15 dB floor-to-speech, the MacBook ~16 dB.
    NOISE_MARGIN_DB = 12.0

    @classmethod
    def threshold_for(cls, noise_rms: float) -> float:
        """Speech threshold for a measured noise floor."""
        return max(noise_rms * (10 ** (cls.NOISE_MARGIN_DB / 20.0)), 1e-5)

    @classmethod
    def calibrate(cls, quiet_pcm: bytes, **kwargs) -> "Segmenter":
        """Build a Segmenter from a sample of the room's own silence.

        Pass ~500 ms recorded before anyone speaks. This is the supported
        path; constructing with a bare `threshold` assumes a device.
        """
        return cls(threshold=cls.threshold_for(rms(quiet_pcm)), **kwargs)

    @property
    def active(self) -> bool:
        return self._active

    def begin(self, preroll: bytes = b"", now: float | None = None) -> None:
        self._collected = bytearray(preroll)
        self._silent_bytes = 0
        self._peak = 0.0
        self._started_at = time.time() if now is None else now
        self._active = True

    def feed(self, frame: bytes) -> Utterance | None:
        """Add one frame. Returns the Utterance once speech has ended."""
        if not self._active:
            raise RuntimeError("Segmenter.feed before begin()")
        self._collected.extend(frame)
        level = rms(frame)
        self._peak = max(self._peak, level)

        if level < self.threshold:
            self._silent_bytes += len(frame)
        else:
            self._silent_bytes = 0

        if self._silent_bytes >= self.fmt.ms_to_bytes(self.silence_ms):
            return self._finish("silence")
        if len(self._collected) >= self.fmt.ms_to_bytes(self.max_ms):
            return self._finish("max_duration")
        return None

    def _finish(self, reason: str) -> Utterance:
        self._active = False
        return Utterance(
            pcm=bytes(self._collected),
            started_at=self._started_at,
            reason=reason,
            peak_level=self._peak,
            fmt=self.fmt,
        )


# ------------------------------------------------------------------- the mic

class MicSource:
    """The Mac's default input, as a blocking iterator of PCM bytes.

    `sounddevice` is imported HERE rather than at module scope so that
    importing `voice.capture` needs no PortAudio, no wheel and no microphone.
    The tests below rely on that.
    """

    def __init__(
        self,
        fmt: AudioFormat = FORMAT,
        block_ms: int = 32,
        device: int | str | None = None,
    ) -> None:
        self.fmt = fmt
        self.block_samples = int(fmt.rate_hz * block_ms / 1000)
        self.device = device
        self._stream = None

    def __enter__(self) -> "MicSource":
        try:
            import sounddevice
        except ImportError as exc:                       # pragma: no cover
            raise RuntimeError(
                "sounddevice is not installed. In mac-prototype:\n"
                "    .venv/bin/pip install sounddevice"
            ) from exc

        if self.fmt.width_bytes != 2:                    # pragma: no cover
            raise FormatError("MicSource only speaks 16-bit; the contract is s16le")

        self._stream = sounddevice.RawInputStream(
            samplerate=self.fmt.rate_hz,
            blocksize=self.block_samples,
            device=self.device,
            channels=self.fmt.channels,
            dtype="int16",
        )
        self._stream.start()
        # AC2: check what we actually got. CoreAudio will resample a 48 kHz
        # device down without saying so, and `dtype="int16"` is a request.
        self.fmt.assert_(
            rate_hz=self._stream.samplerate,
            width_bytes=self.fmt.width_bytes,
            channels=self._stream.channels,
        )
        return self

    def __exit__(self, *exc) -> None:
        if self._stream is not None:
            self._stream.stop()
            self._stream.close()
            self._stream = None

    def blocks(self) -> Iterator[bytes]:
        if self._stream is None:
            raise RuntimeError("MicSource used outside its `with` block")
        while True:
            data, overflowed = self._stream.read(self.block_samples)
            if overflowed:
                print("  [warn] input overflow — dropped audio", file=sys.stderr)
            yield bytes(data)


def write_wav(path: Path, pcm: bytes, fmt: AudioFormat = FORMAT) -> Path:
    """Write PCM out so a human can actually listen to what was captured."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(fmt.channels)
        w.setsampwidth(fmt.width_bytes)
        w.setframerate(fmt.rate_hz)
        w.writeframes(pcm)
    return path
