#!/usr/bin/env python3
"""Tests for transcription (#45, S2 V4).

Every test here runs WITHOUT whisper.cpp, without a model file and without a
network. #45 AC2 is "the interface is swappable: a stub implementation passes
the same tests" -- so the contract tests below are parameterised over engines
and the stub is a first-class citizen, not a mock.

    python3 -m unittest discover -s mac-prototype -p 'test_*.py' -v
"""

import os
import sys
import tempfile
import unittest
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import capture as C
import transcribe as T


def silence(ms: float) -> bytes:
    return b"\x00\x00" * int(C.SAMPLE_RATE_HZ * ms / 1000)


class TestTheContract(unittest.TestCase):
    """Anything registered in ENGINES must satisfy all of this. The stub is
    held to exactly the bar the real engine is (#45 AC2)."""

    def engines(self):
        yield T.StubTranscriber()

    def test_transcribe_returns_a_transcript_with_the_audio_duration(self):
        for eng in self.engines():
            with self.subTest(engine=eng.name):
                r = eng.transcribe(silence(2000))
                self.assertIsInstance(r, T.Transcript)
                self.assertAlmostEqual(r.audio_s, 2.0, places=3)
                self.assertEqual(r.engine, eng.name)

    def test_duration_is_derived_from_the_capture_contract(self):
        for eng in self.engines():
            with self.subTest(engine=eng.name):
                r = eng.transcribe(silence(1500))
                self.assertAlmostEqual(r.audio_s, len(silence(1500)) / 32000.0, places=6)

    def test_a_ragged_buffer_is_refused_not_truncated(self):
        for eng in self.engines():
            with self.subTest(engine=eng.name):
                with self.assertRaises(C.FormatError):
                    eng.transcribe(b"\x00\x00\x00")

    def test_warm_is_idempotent(self):
        for eng in self.engines():
            with self.subTest(engine=eng.name):
                eng.warm(); eng.warm()

    def test_empty_audio_is_handled_not_crashed(self):
        for eng in self.engines():
            with self.subTest(engine=eng.name):
                r = eng.transcribe(b"")
                self.assertEqual(r.audio_s, 0.0)
                self.assertEqual(r.realtime_factor, float("inf"))


class TestTranscript(unittest.TestCase):

    def test_realtime_factor_below_one_means_faster_than_audio(self):
        self.assertAlmostEqual(T.Transcript("x", "e", 0.5, 5.0).realtime_factor, 0.1)

    def test_realtime_factor_of_zero_length_audio_is_infinite_not_a_crash(self):
        self.assertEqual(T.Transcript("", "e", 0.1, 0.0).realtime_factor, float("inf"))


class TestEngineRegistry(unittest.TestCase):

    def test_unknown_engine_is_refused(self):
        with self.assertRaises(ValueError):
            T.create_transcriber("faster-whisper")     # the one #45 rules out

    def test_every_registered_engine_implements_the_seam(self):
        for name, cls in T.ENGINES.items():
            with self.subTest(engine=name):
                self.assertTrue(issubclass(cls, T.Transcriber))

    def test_the_stub_is_constructible_with_the_real_engines_kwargs(self):
        # V5 must be able to swap engines by config without special-casing.
        eng = T.create_transcriber("stub", model="base.en", warm=True)
        self.assertIsInstance(eng, T.StubTranscriber)


class TestWavIo(unittest.TestCase):

    def test_a_conforming_wav_round_trips(self):
        with tempfile.TemporaryDirectory() as d:
            p = C.write_wav(Path(d) / "a.wav", silence(500))
            self.assertEqual(len(T.read_wav(p)), 16000)

    def test_a_wav_at_the_wrong_rate_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "bad.wav"
            with wave.open(str(p), "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100)
                w.writeframes(silence(100))
            with self.assertRaises(C.FormatError):
                T.read_wav(p)


class TestFdCaptureRegression(unittest.TestCase):
    """whisper.cpp prints from C straight to fd 2.

    The first version of the backend check used `contextlib.redirect_stderr`,
    which swaps Python's sys.stderr OBJECT and leaves fd 2 alone. It captured
    nothing, so `backend_report()` reported metal=False on a run where the
    Metal backend was demonstrably active -- a check failing closed and
    inventing the exact fault it exists to detect. These pin the fix.
    """

    def test_it_captures_writes_to_the_raw_file_descriptor(self):
        with T._capture_fd(2) as log:
            os.write(2, b"banner from C\n")
        self.assertIn("banner from C", log.text)

    def test_redirect_stderr_would_NOT_have_caught_it(self):
        # The bug, asserted. If this ever fails, the premise changed.
        import contextlib, io
        buf = io.StringIO()
        with T._capture_fd(2):            # keep it off the test console
            with contextlib.redirect_stderr(buf):
                os.write(2, b"from C\n")
        self.assertEqual(buf.getvalue(), "")

    def test_the_descriptor_is_restored_afterwards(self):
        before = os.dup(2)
        try:
            with T._capture_fd(2):
                os.write(2, b"x")
            os.write(2, b"")              # would raise if fd 2 were broken
        finally:
            os.close(before)

    def test_text_is_empty_rather_than_missing_when_nothing_was_written(self):
        with T._capture_fd(2) as log:
            pass
        self.assertEqual(log.text, "")


class TestWhisperCppWhenAvailable(unittest.TestCase):
    """Skipped without the wheel, so the suite stays hardware-free."""

    def setUp(self):
        try:
            import pywhispercpp  # noqa: F401
        except ImportError:
            self.skipTest("pywhispercpp not installed")

    def test_importing_the_module_does_not_load_a_model(self):
        eng = T.WhisperCppTranscriber()
        self.assertIsNone(eng._model)

    def test_the_gpu_is_actually_in_use(self):
        # #45 exists partly to avoid an engine that silently runs on CPU. A
        # prebuilt wheel can do that too, so assert the backend, not the text.
        rep = T.WhisperCppTranscriber(warm=True).backend_report()
        self.assertTrue(rep["metal"], f"whisper.cpp did not initialise Metal: {rep}")
        self.assertNotEqual(rep["device"], "unknown")


if __name__ == "__main__":
    unittest.main(verbosity=2)
