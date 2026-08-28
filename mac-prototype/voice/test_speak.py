#!/usr/bin/env python3
"""Tests for Threepio's voice (#47, S2 V6).

No key, no network, no sound. Nothing here plays audio — a test suite that
makes noise cannot be run at night, and this is the one module whose output
reaches a sleeping household through a closed door.
"""

import sys
import unittest
from datetime import datetime, time as dtime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).parent.parent))

import r2_behavior as B
import speak as S

# `speak()` refuses unless told R2 is reachable, so every test that needs to
# get PAST that gate says so explicitly. Tests asserting a different failure
# must pass this, or they pass for the wrong reason -- which is exactly what
# happened to the two AC2 tests below when the gate was added.
PRESENT = S.Embodiment(present=True)


class TestQuietHours(unittest.TestCase):
    """#47 AC3. V7 owns the policy; this is the floor beneath it."""

    def at(self, h, m=0):
        return datetime(2026, 8, 18, h, m)

    def test_it_is_active_late_at_night(self):
        self.assertTrue(S.QuietHours().active(self.at(23, 30)))

    def test_it_is_active_in_the_small_hours(self):
        self.assertTrue(S.QuietHours().active(self.at(3)))

    def test_it_is_not_active_in_the_afternoon(self):
        self.assertFalse(S.QuietHours().active(self.at(15)))

    def test_the_window_crosses_midnight_correctly(self):
        q = S.QuietHours(start=dtime(22, 0), end=dtime(8, 0))
        for h, want in ((21, False), (22, True), (2, True), (7, True), (8, False)):
            with self.subTest(hour=h):
                self.assertEqual(q.active(self.at(h)), want)

    def test_a_daytime_window_does_not_wrap(self):
        q = S.QuietHours(start=dtime(9, 0), end=dtime(17, 0))
        self.assertTrue(q.active(self.at(12)))
        self.assertFalse(q.active(self.at(23)))

    def test_speak_refuses_during_quiet_hours(self):
        spk = S.StubSpeaker()
        with self.assertRaises(S.QuietHoursError):
            S.speak("Oh dear.", speaker=spk, now=self.at(2))
        self.assertEqual(spk.calls, [], "it synthesised anyway")

    def test_refusing_is_not_a_fault_but_IS_a_SpeakError_subclass(self):
        # Callers that catch SpeakError must not be surprised by it, but code
        # that cares can tell "asleep" from "broken".
        self.assertTrue(issubclass(S.QuietHoursError, S.SpeakError))

    def test_an_explicit_audition_can_override_it(self):
        # Deliberate, operator-initiated listening. The override is a
        # parameter at the call site, NOT a weaker default.
        utt = S.speak("Testing.", speaker=S.StubSpeaker(),
                      quiet_hours=S.QuietHours(enabled=False),
                      embodiment=PRESENT, now=self.at(2))
        self.assertEqual(utt.text, "Testing.")

    def test_the_gate_is_on_by_default(self):
        self.assertTrue(S.QuietHours().enabled)


class TestHeDoesNotSpeakWithoutABody(unittest.TestCase):
    """D-019: the voice is R2's own, in the first person. OBSERVED
    2026-08-27 -- it spoke three times unprompted with him powered down.

    The illegal case is asserted FIRST and the legal ones after it, because
    a table of legal inputs passes with the guard deleted."""

    def day(self, h=15):
        return datetime(2026, 8, 18, h)

    # -- the case that must be refused -----------------------------------
    def test_speak_refuses_when_R2_is_absent(self):
        spk = S.StubSpeaker()
        with self.assertRaises(S.NotEmbodiedError):
            S.speak("Oh dear.", speaker=spk, embodiment=S.Embodiment(present=False),
                    now=self.day())
        self.assertEqual(spk.calls, [], "it synthesised anyway")

    def test_the_bare_default_refuses_too(self):
        # The gate has to hold for a caller that passes nothing, because that
        # caller is the one that has not thought about it.
        spk = S.StubSpeaker()
        with self.assertRaises(S.NotEmbodiedError):
            S.speak("Oh dear.", speaker=spk, now=self.day())
        self.assertEqual(spk.calls, [])

    def test_absent_is_the_default_not_present(self):
        self.assertFalse(S.Embodiment().present)
        self.assertTrue(S.Embodiment().enabled)
        self.assertFalse(S.Embodiment().ready())

    # -- and the ones that must not be -----------------------------------
    def test_it_speaks_when_he_is_there(self):
        utt = S.speak("Testing.", speaker=S.StubSpeaker(),
                      embodiment=PRESENT, now=self.day())
        self.assertEqual(utt.text, "Testing.")

    def test_an_explicit_audition_can_override_it(self):
        utt = S.speak("Testing.", speaker=S.StubSpeaker(),
                      embodiment=S.Embodiment(enabled=False), now=self.day())
        self.assertEqual(utt.text, "Testing.")

    def test_refusing_is_not_a_fault_but_IS_a_SpeakError_subclass(self):
        self.assertTrue(issubclass(S.NotEmbodiedError, S.SpeakError))

    def test_quiet_hours_still_wins_when_both_would_refuse(self):
        # Both gates are non-faults, but they are not interchangeable: a
        # caller that special-cases one must not silently get the other.
        with self.assertRaises(S.QuietHoursError):
            S.speak("Oh dear.", speaker=S.StubSpeaker(),
                    embodiment=S.Embodiment(present=False),
                    now=datetime(2026, 8, 18, 2))


class TestAC2FailureNeverLeavesHimWaiting(unittest.TestCase):

    def test_an_unreachable_tts_raises(self):
        # assertRaises(SpeakError) alone is not enough: NotEmbodiedError and
        # QuietHoursError are both subclasses, so this passed with the TTS
        # never reached. Assert the REASON.
        with self.assertRaises(S.SpeakError) as cm:
            S.speak("hi", speaker=S.StubSpeaker(error=S.SpeakError("TTS unreachable")),
                    embodiment=PRESENT, now=datetime(2026, 8, 18, 15))
        self.assertIn("TTS unreachable", str(cm.exception))
        self.assertNotIsInstance(cm.exception, S.NotEmbodiedError)

    def test_a_speaker_slower_than_the_timeout_raises(self):
        with self.assertRaises(S.SpeakError) as cm:
            S.speak("hi", speaker=S.StubSpeaker(delay=30.0), timeout=1.0,
                    embodiment=PRESENT, now=datetime(2026, 8, 18, 15))
        self.assertIn("timeout", str(cm.exception))
        self.assertNotIsInstance(cm.exception, S.NotEmbodiedError)

    def test_the_shared_error_beat_returns_him_to_neutral(self):
        # Same beat V5 uses — one definition of "we failed, go neutral".
        beat = S.error_beat("tts down")
        beat.validate()
        self.assertEqual(beat.rest_colour, B.BASE_NEUTRAL)
        self.assertEqual(beat.phrases[-1].steps[-1].params["channels"],
                         B.front(B.BASE_NEUTRAL))


class TestPcmIsWrappedLocally(unittest.TestCase):
    """MEASURED 2026-08-27: wav complete 2.54 s, pcm complete 1.10 s, same
    model / voice / instructions. The server spends over a second building a
    container we re-create in microseconds."""

    def test_the_wrapper_produces_a_wav_the_stdlib_can_read(self):
        import io
        import wave
        pcm = b"\x00\x01" * 2400                       # 0.1 s at 24 kHz
        blob = S._wav_container(pcm, 24_000)
        w = wave.open(io.BytesIO(blob))
        self.assertEqual(w.getframerate(), 24_000)
        self.assertEqual(w.getsampwidth(), 2)
        self.assertEqual(w.getnchannels(), 1)
        self.assertEqual(w.readframes(w.getnframes()), pcm)

    def test_the_declared_rate_matches_what_the_api_emits(self):
        # VERIFIED against the API's own wav header (24000 Hz / 16-bit / 1ch)
        # on 2026-08-27. A wrong rate here does not error — it plays the whole
        # character at the wrong pitch, which reads as a voice choice rather
        # than a bug, so it is pinned rather than trusted.
        self.assertEqual(S.OpenAITtsSpeaker.PCM_RATE_HZ, 24_000)

    def test_pcm_is_requested_by_default(self):
        import inspect
        fmt = inspect.signature(S.OpenAITtsSpeaker.__init__).parameters["fmt"]
        self.assertEqual(fmt.default, "pcm")

    def test_a_wrapped_utterance_reports_wav_not_pcm(self):
        # Downstream (afplay, .write(), the tests) only ever sees a container,
        # so the utterance must not claim a format nothing can play.
        u = S.Utterance("x", S._wav_container(b"\x00\x00" * 100, 24_000),
                        "wav", "openai", "ballad", 0.0)
        self.assertEqual(u.fmt, "wav")

    def test_an_empty_pcm_payload_still_yields_a_valid_container(self):
        import io
        import wave
        w = wave.open(io.BytesIO(S._wav_container(b"", 24_000)))
        self.assertEqual(w.getnframes(), 0)


class TestAC4TheSeam(unittest.TestCase):

    def test_speaker_is_selected_by_config(self):
        self.assertIsInstance(S.create_speaker("stub"), S.StubSpeaker)

    def test_an_unknown_provider_is_refused(self):
        with self.assertRaises(ValueError):
            S.create_speaker("a-tts-vendor-we-do-not-have")

    def test_the_stub_tolerates_the_real_speakers_kwargs(self):
        self.assertIsInstance(
            S.create_speaker("stub", voice="ash", model="x", fmt="wav"),
            S.StubSpeaker)


class TestKeyReuse(unittest.TestCase):
    """One key, two slots, without hardwiring which vendor it is."""

    def setUp(self):
        import os
        self.saved = {k: os.environ.get(k) for k in
                      ("TTS_API_KEY", "TTS_PROVIDER", "LLM_PROVIDER", "LLM_API_KEY")}
        for k in self.saved:
            os.environ.pop(k, None)

    def tearDown(self):
        import os
        for k, v in self.saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v

    def test_an_explicit_tts_key_wins(self):
        import os
        os.environ["TTS_API_KEY"] = "explicit"
        self.assertEqual(S._tts_key(), "explicit")

    def test_the_llm_key_is_reused_when_both_slots_name_the_same_provider(self):
        import os
        os.environ.update({"TTS_PROVIDER": "openai", "LLM_PROVIDER": "openai",
                           "LLM_API_KEY": "shared"})
        self.assertEqual(S._tts_key(), "shared")

    def test_the_llm_key_is_NOT_reused_across_different_providers(self):
        # Sending one vendor's key to another vendor's endpoint would leak it.
        import os
        os.environ.update({"TTS_PROVIDER": "elevenlabs", "LLM_PROVIDER": "openai",
                           "LLM_API_KEY": "secret"})
        self.assertEqual(S._tts_key(), "")

    def test_no_key_anywhere_yields_empty_not_a_crash(self):
        self.assertEqual(S._tts_key(), "")


class TestTheCharacterLivesInOnePlace(unittest.TestCase):

    def test_the_line_writer_shares_the_steering_prompt(self):
        # Two descriptions of who he is would drift apart, and the voice would
        # stop matching the words.
        self.assertIn(S.STEERING, S.LINE_SYSTEM)

    def test_the_steering_prompt_names_the_register_47_asked_for(self):
        for word in ("fussy", "formal", "over-precise", "anx"):
            self.assertIn(word, S.STEERING.lower())

    def test_it_does_not_name_a_performer_or_a_character(self):
        # #47's note: design *a* protocol droid, not a clone. Naming one
        # invites a likeness question this project has no business raising.
        low = (S.STEERING + S.LINE_SYSTEM).lower()
        for name in ("c-3po", "c3po", "threepio", "anthony daniels"):
            self.assertNotIn(name, low)


class TestSpeakDoesNotMakeNoiseUnlessAsked(unittest.TestCase):

    def test_play_audio_defaults_to_false(self):
        import inspect
        self.assertIs(inspect.signature(S.speak).parameters["play_audio"].default,
                      False)

    def test_it_can_write_a_file_without_playing_it(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            S.speak("hello", speaker=S.StubSpeaker(), out_dir=d,
                    embodiment=PRESENT, now=datetime(2026, 8, 18, 15))
            self.assertTrue(list(Path(d).glob("line.*")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
