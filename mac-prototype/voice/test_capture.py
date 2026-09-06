#!/usr/bin/env python3
"""Tests for the voice input path (#42 V1).

Runs WITHOUT a microphone, WITHOUT sounddevice, WITHOUT any wake-word wheel
and WITHOUT the robot. Same discipline as the choreography tests: every branch
that matters is proven against synthetic PCM on the bench, so a live session
is spent measuring in the room rather than debugging.

    python3 -m unittest discover -s mac-prototype -p 'test_*.py' -v
"""

import struct
import sys
import tempfile
import unittest
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import capture as C
import wake as W


# ------------------------------------------------------------------ synthetic

def silence(ms: float) -> bytes:
    return b"\x00\x00" * int(C.SAMPLE_RATE_HZ * ms / 1000)


def tone(ms: float, amplitude: int = 20_000) -> bytes:
    """A square wave. Deliberately not a sine: its RMS is exactly `amplitude`,
    so a threshold assertion below is arithmetic rather than approximate."""
    n = int(C.SAMPLE_RATE_HZ * ms / 1000)
    return b"".join(
        struct.pack("<h", amplitude if (i // 8) % 2 == 0 else -amplitude)
        for i in range(n)
    )


class TestTheFormatContract(unittest.TestCase):
    """AC2 — assert the format, never trust the device default."""

    def test_the_contract_is_16k_16bit_mono(self):
        self.assertEqual(C.FORMAT.rate_hz, 16_000)
        self.assertEqual(C.FORMAT.width_bytes, 2)
        self.assertEqual(C.FORMAT.channels, 1)
        self.assertEqual(C.FORMAT.bytes_per_second, 32_000)

    def test_matching_format_passes(self):
        C.FORMAT.assert_(rate_hz=16_000, width_bytes=2, channels=1)

    def test_a_resampled_device_is_rejected(self):
        # The failure this exists to catch: CoreAudio hands back 48 kHz and
        # resamples silently, it transcribes fine on the Mac, and it breaks on
        # the ES8311 where nobody is watching.
        with self.assertRaises(C.FormatError):
            C.FORMAT.assert_(rate_hz=48_000, width_bytes=2, channels=1)

    def test_stereo_is_rejected(self):
        with self.assertRaises(C.FormatError):
            C.FORMAT.assert_(rate_hz=16_000, width_bytes=2, channels=2)

    def test_eight_bit_is_rejected(self):
        with self.assertRaises(C.FormatError):
            C.FORMAT.assert_(rate_hz=16_000, width_bytes=1, channels=1)

    def test_ms_to_bytes_never_splits_a_sample(self):
        # An odd byte count makes every downstream array('h', buf) raise, so
        # the rounding direction here is load-bearing.
        for ms in (1, 7, 31, 33, 700, 12_000):
            with self.subTest(ms=ms):
                self.assertEqual(C.FORMAT.ms_to_bytes(ms) % 2, 0)

    def test_ms_and_bytes_round_trip(self):
        self.assertEqual(C.FORMAT.ms_to_bytes(1000), 32_000)
        self.assertAlmostEqual(C.FORMAT.bytes_to_ms(32_000), 1000.0)

    def test_a_wav_we_write_reads_back_at_the_contract(self):
        # Verified with the CONSUMING parser rather than by inspecting our own
        # header arithmetic.
        with tempfile.TemporaryDirectory() as d:
            path = C.write_wav(Path(d) / "u.wav", tone(250))
            with wave.open(str(path), "rb") as r:
                self.assertEqual(r.getframerate(), 16_000)
                self.assertEqual(r.getsampwidth(), 2)
                self.assertEqual(r.getnchannels(), 1)
                self.assertEqual(r.getnframes(), 4_000)


class TestRms(unittest.TestCase):

    def test_silence_is_zero(self):
        self.assertEqual(C.rms(silence(100)), 0.0)

    def test_empty_is_zero(self):
        self.assertEqual(C.rms(b""), 0.0)

    def test_a_square_wave_rms_is_its_amplitude(self):
        self.assertAlmostEqual(C.rms(tone(100, 16_384)), 0.5, places=4)

    def test_odd_byte_count_raises(self):
        with self.assertRaises(C.FormatError):
            C.rms(b"\x00\x00\x00")

    def test_dbfs_floors_instead_of_returning_negative_infinity(self):
        self.assertEqual(C.dbfs(0.0), -120.0)
        self.assertAlmostEqual(C.dbfs(1.0), 0.0)


class TestChunker(unittest.TestCase):
    """The re-blocker that lets each engine own its own frame size."""

    def test_exact_frames_come_out_whole(self):
        ch = C.Chunker(1024)
        out = list(ch.feed(b"\x01" * 4096))
        self.assertEqual(len(out), 4)
        self.assertTrue(all(len(f) == 1024 for f in out))
        self.assertEqual(ch.pending, 0)

    def test_a_partial_frame_is_held_not_dropped(self):
        ch = C.Chunker(1024)
        self.assertEqual(list(ch.feed(b"\x02" * 600)), [])
        self.assertEqual(ch.pending, 600)
        out = list(ch.feed(b"\x02" * 424))
        self.assertEqual([len(f) for f in out], [1024])

    def test_no_byte_is_lost_or_reordered_across_ragged_blocks(self):
        # PortAudio delivers what it feels like; dropping the remainder would
        # lose up to one frame of speech per block and no test would notice.
        src = bytes(i % 251 for i in range(10_000))
        ch = C.Chunker(512)
        got = bytearray()
        pos = 0
        for step in (7, 999, 1, 4096, 3000, 1897):
            got.extend(b"".join(ch.feed(src[pos:pos + step])))
            pos += step
        self.assertEqual(pos, len(src))
        whole = len(src) - (len(src) % 512)
        self.assertEqual(bytes(got), src[:whole])
        self.assertEqual(ch.pending, len(src) % 512)

    def test_zero_size_is_refused(self):
        with self.assertRaises(ValueError):
            C.Chunker(0)


class TestRingBuffer(unittest.TestCase):
    """Pre-roll: without it the utterance clips its own first syllable."""

    def test_it_keeps_the_most_recent_bytes(self):
        ring = C.RingBuffer(10)
        ring.write(b"abcdefghij")
        ring.write(b"klmno")
        self.assertEqual(ring.read(), b"fghijklmno")
        self.assertEqual(len(ring), 10)

    def test_it_never_exceeds_capacity(self):
        ring = C.RingBuffer(C.FORMAT.ms_to_bytes(500))
        for _ in range(50):
            ring.write(tone(100))
        self.assertEqual(len(ring), C.FORMAT.ms_to_bytes(500))

    def test_clear_empties_it(self):
        ring = C.RingBuffer(64)
        ring.write(b"x" * 64)
        ring.clear()
        self.assertEqual(ring.read(), b"")

    def test_zero_capacity_is_refused(self):
        with self.assertRaises(ValueError):
            C.RingBuffer(0)


class TestSegmenter(unittest.TestCase):

    def feed_all(self, seg: C.Segmenter, pcm: bytes, frame: int = 1024):
        for i in range(0, len(pcm) - frame + 1, frame):
            out = seg.feed(pcm[i:i + frame])
            if out is not None:
                return out
        return None

    def test_it_ends_on_silence(self):
        seg = C.Segmenter()
        seg.begin()
        utt = self.feed_all(seg, tone(400) + silence(1200))
        self.assertIsNotNone(utt)
        self.assertEqual(utt.reason, "silence")
        self.assertFalse(seg.active)

    def test_it_does_not_end_on_the_gap_between_two_words(self):
        # The failure mode that makes a voice assistant feel broken: cutting
        # the user off mid-sentence. 300 ms is a normal inter-word pause.
        seg = C.Segmenter()
        seg.begin()
        self.assertIsNone(self.feed_all(seg, tone(300) + silence(300) + tone(300)))
        self.assertTrue(seg.active)

    def test_it_ends_on_max_duration_when_the_room_never_goes_quiet(self):
        seg = C.Segmenter(max_ms=1000)
        seg.begin()
        utt = self.feed_all(seg, tone(3000))
        self.assertIsNotNone(utt)
        self.assertEqual(utt.reason, "max_duration")

    def test_preroll_is_prepended_to_the_utterance(self):
        seg = C.Segmenter()
        pre = tone(500)
        seg.begin(preroll=pre)
        utt = self.feed_all(seg, tone(200) + silence(1200))
        self.assertIsNotNone(utt)
        self.assertTrue(utt.pcm.startswith(pre))
        self.assertGreater(utt.duration_s, 0.5)

    def test_peak_level_is_recorded(self):
        seg = C.Segmenter()
        seg.begin()
        utt = self.feed_all(seg, tone(200, 16_384) + silence(1200))
        self.assertAlmostEqual(utt.peak_level, 0.5, places=3)

    def test_duration_is_computed_from_the_contract(self):
        seg = C.Segmenter()
        seg.begin()
        utt = self.feed_all(seg, tone(200) + silence(1200))
        self.assertAlmostEqual(utt.duration_s, len(utt.pcm) / 32_000.0, places=6)

    def test_feeding_before_begin_is_a_programming_error(self):
        with self.assertRaises(RuntimeError):
            C.Segmenter().feed(silence(32))


class TestThresholdCalibration(unittest.TestCase):
    """MEASURED 2026-08-17: the BRIO's noise floor is louder than the MacBook
    mic's speech. A single absolute threshold cannot serve both."""

    # dBFS -> rms, from the survey traces.
    MBP_FLOOR, MBP_SPEECH_QUIETEST = 0.000372, 0.00224     # -68.6, -53.0
    BRIO_FLOOR, BRIO_SPEECH_QUIETEST = 0.00417, 0.0200     # -47.6, -34.0

    def test_calibration_separates_speech_from_silence_on_the_macbook_mic(self):
        t = C.Segmenter.threshold_for(self.MBP_FLOOR)
        self.assertGreater(t, self.MBP_FLOOR)
        self.assertLess(t, self.MBP_SPEECH_QUIETEST)

    def test_calibration_separates_speech_from_silence_on_the_brio(self):
        t = C.Segmenter.threshold_for(self.BRIO_FLOOR)
        self.assertGreater(t, self.BRIO_FLOOR)
        self.assertLess(t, self.BRIO_SPEECH_QUIETEST)

    def test_the_old_absolute_default_fails_on_the_macbook_mic(self):
        # Regression guard for the actual bug: 0.010 sat 0.2 dB above the
        # measured speech peak, so a whole sentence read as silence.
        self.assertGreater(C.Segmenter.DEFAULT_THRESHOLD, self.MBP_SPEECH_QUIETEST)

    def test_no_single_constant_can_serve_both_microphones(self):
        # The finding itself, as an assertion: any threshold below the BRIO's
        # floor breaks utterance-ending there; any above the MacBook's
        # quietest speech breaks detection there. The window is empty.
        self.assertGreater(self.BRIO_FLOOR, self.MBP_SPEECH_QUIETEST)

    def test_calibrate_builds_a_segmenter_from_a_silence_sample(self):
        seg = C.Segmenter.calibrate(silence(500), silence_ms=400)
        self.assertGreater(seg.threshold, 0.0)
        self.assertEqual(seg.silence_ms, 400)

    def test_calibrating_on_true_digital_silence_does_not_yield_zero(self):
        # A zero threshold would make every frame "speech" and no utterance
        # would ever end. Digital silence is what a held-open mic returns.
        self.assertGreater(C.Segmenter.threshold_for(0.0), 0.0)


class TestNoiseMarginOverride(unittest.TestCase):
    """The margin is the dial between "he cannot hear me" and "the room is
    speech". MEASURED live: with a guest in the room and the speaker a metre
    from the built-in mic, speech landed ~11 dB above the floor and the 12 dB
    default classified every frame as silence -- the utterance ended after the
    hangover and the transcriber got [BLANK_AUDIO]."""

    FLOOR = 0.001

    def test_the_default_comes_from_the_class_constant_not_a_literal(self):
        # Asserted against the CONSTANT so a caller that hardcodes the same
        # number cannot silently make the constant dead. converse.py did
        # exactly that: --noise-margin-db defaulted to a literal 12.0, so
        # editing NOISE_MARGIN_DB had no effect through the only entry point
        # anyone uses.
        expected = self.FLOOR * (10 ** (C.Segmenter.NOISE_MARGIN_DB / 20.0))
        self.assertAlmostEqual(C.Segmenter.threshold_for(self.FLOOR), expected)
        self.assertAlmostEqual(
            C.Segmenter.threshold_for(self.FLOOR, margin_db=None), expected)

    def test_an_explicit_margin_overrides_the_default(self):
        self.assertNotAlmostEqual(
            C.Segmenter.threshold_for(self.FLOOR, margin_db=5.0),
            C.Segmenter.threshold_for(self.FLOOR))

    def test_a_smaller_margin_hears_quieter_speech(self):
        loose = C.Segmenter.threshold_for(self.FLOOR, margin_db=5.0)
        tight = C.Segmenter.threshold_for(self.FLOOR, margin_db=20.0)
        self.assertLess(loose, tight)

    def test_zero_margin_is_the_floor_itself(self):
        self.assertAlmostEqual(
            C.Segmenter.threshold_for(self.FLOOR, margin_db=0.0), self.FLOOR)

    def test_the_live_failure_is_reproduced_and_the_override_fixes_it(self):
        # floor -59.4 dBFS, speech -48 dBFS: 11.4 dB apart.
        floor, speech = 0.00107, 0.00398          # -59.4 and -48.0 dBFS
        self.assertLess(speech, C.Segmenter.threshold_for(floor),
                        "the 12 dB default should MISS this speech")
        self.assertGreater(speech,
                           C.Segmenter.threshold_for(floor, margin_db=8.0),
                           "an 8 dB margin should catch it")

    def test_the_hard_floor_still_applies_to_digital_silence(self):
        self.assertGreater(C.Segmenter.threshold_for(0.0, margin_db=0.0), 0.0)


class TestWakeEngineSeam(unittest.TestCase):
    """The engine owns its frame size; nothing upstream may assume one."""

    def test_frame_bytes_follows_the_engine_not_the_caller(self):
        self.assertEqual(W.ScriptedEngine(frame_samples=512).frame_bytes, 1024)
        self.assertEqual(W.ScriptedEngine(frame_samples=1280).frame_bytes, 2560)
        # The two real engines genuinely disagree, which is why Chunker exists.
        self.assertNotEqual(
            W.PorcupineEngine.frame_samples, W.OpenWakeWordEngine.frame_samples
        )

    def test_a_wrong_sized_frame_is_refused_rather_than_silently_padded(self):
        eng = W.ScriptedEngine(frame_samples=512)
        with self.assertRaises(C.FormatError):
            eng.process(silence(10))

    def test_scripted_engine_fires_only_where_scripted(self):
        eng = W.ScriptedEngine(fire_on={2}, frame_samples=512)
        frames = [silence(32) for _ in range(5)]
        fired = [i for i, f in enumerate(frames) if eng.process(f) is not None]
        self.assertEqual(fired, [2])

    def test_an_untrained_model_path_fails_with_instructions_not_a_stack_trace(self):
        # The first wall the operator hits: "z2" has no stock model anywhere,
        # so the default path does not exist until the training step is run.
        # openWakeWord's own error for a missing path is unhelpful; this must
        # name the phrase, the reason, and where the instructions are.
        try:
            import openwakeword  # noqa: F401
        except ImportError:
            self.skipTest("openwakeword not installed")
        with self.assertRaises(RuntimeError) as ctx:
            W.OpenWakeWordEngine(keywords=["models/definitely-not-trained.onnx"])
        msg = str(ctx.exception)
        self.assertIn("does not exist yet", msg)
        self.assertIn("README", msg)

    def test_the_default_keyword_lives_in_the_models_dir(self):
        """Always checkable: the default must point INTO `voice/models/`.

        Anchored to the package, not the cwd, so `-m voice` works anywhere.
        """
        p = Path(W.DEFAULT_KEYWORD)
        self.assertTrue(p.is_absolute())
        self.assertEqual(p.parent, W.MODELS_DIR)

    def test_the_default_keyword_names_a_model_that_is_actually_there(self):
        """The property that matters, and the one the old test missed.

        The old assertion was `endswith("z2.onnx")`. It stayed green for weeks
        while no z2.onnx had ever been trained, because it pinned the STRING
        and not the FILE. A default naming a missing model is not a default:
        `OpenWakeWordEngine(keywords=None)` raises on it, so every caller that
        did not pass its own path was broken.

        `voice/models/*.onnx` is gitignored (.gitignore:58) -- the models are
        downloaded per machine, never committed. So this SKIPS on a bare clone
        rather than failing, and does the real check on any machine that has
        run the README's setup. That is the machine the bug bites on.
        """
        if not any(W.MODELS_DIR.glob("*.onnx")):
            self.skipTest("no models downloaded here; see voice/README.md")
        self.assertTrue(
            Path(W.DEFAULT_KEYWORD).exists(),
            f"DEFAULT_KEYWORD names {Path(W.DEFAULT_KEYWORD).name!r}, which is "
            f"not in voice/models/ (found: "
            f"{sorted(p.name for p in W.MODELS_DIR.glob('*.onnx'))}). "
            f"Point it at a model the README actually installs.")

    def test_unknown_engine_names_are_refused(self):
        with self.assertRaises(ValueError):
            W.create_engine("whisper-is-not-a-wake-word-engine")

    def test_every_registered_engine_is_a_wake_engine(self):
        for name, cls in W.ENGINES.items():
            with self.subTest(engine=name):
                self.assertTrue(issubclass(cls, W.WakeEngine))

    def test_porcupine_refuses_to_start_without_a_key(self):
        # It must fail with the licensing situation named, not with a stack
        # trace from inside the SDK. The free tier closed 2026-06-30.
        try:
            import pvporcupine  # noqa: F401
        except ImportError:
            self.skipTest("pvporcupine not installed")
        import os
        old = os.environ.pop("PICOVOICE_ACCESS_KEY", None)
        try:
            with self.assertRaises(RuntimeError) as ctx:
                W.PorcupineEngine()
            self.assertIn("PICOVOICE_ACCESS_KEY", str(ctx.exception))
        finally:
            if old is not None:
                os.environ["PICOVOICE_ACCESS_KEY"] = old


class TestTheWholePipelineWithoutHardware(unittest.TestCase):
    """mic bytes -> Chunker -> engine -> Segmenter -> Utterance, end to end."""

    def test_a_wake_word_produces_one_correctly_formatted_utterance(self):
        engine = W.ScriptedEngine(fire_on={3}, frame_samples=512)
        chunk = C.Chunker(engine.frame_bytes)
        ring = C.RingBuffer(C.FORMAT.ms_to_bytes(500))
        seg = C.Segmenter()

        stream = silence(500) + tone(800) + silence(1200)
        got = None
        # 777 bytes is deliberately not a frame multiple, and deliberately odd
        # at the sample level, so the chunker is actually exercised.
        for i in range(0, len(stream), 777):
            for frame in chunk.feed(stream[i:i + 777]):
                if seg.active:
                    got = got or seg.feed(frame)
                    continue
                ring.write(frame)
                if engine.process(frame) is not None:
                    seg.begin(preroll=ring.read())
                    ring.clear()

        self.assertIsNotNone(got, "the pipeline never produced an utterance")
        self.assertEqual(got.reason, "silence")
        self.assertEqual(len(got.pcm) % 2, 0)
        self.assertEqual(got.fmt, C.FORMAT)
        self.assertGreater(got.duration_s, 0.8)
        self.assertGreater(got.peak_level, 0.5)

    def test_importing_this_package_needs_no_microphone(self):
        # The isolation #42 is really buying: if V3 misbehaves we must be able
        # to tell the mic from the choreography, and that starts with this
        # module being importable on a machine with neither.
        self.assertNotIn("sounddevice", sys.modules)
        self.assertNotIn("pvporcupine", sys.modules)


if __name__ == "__main__":
    unittest.main(verbosity=2)
