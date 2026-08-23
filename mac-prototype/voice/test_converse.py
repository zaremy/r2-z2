#!/usr/bin/env python3
"""Tests for the conversation loop (#41 integration).

No mic, no daemon, no robot, no network. Covers the two things that must not
regress: what may reach R2's body, and what counts as something he was told.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).parent.parent))

import converse as CV
import r2_behavior as B
import reason as RSN

# MEASURED in #12: these emit WADDLE and can fell him.
WADDLERS = (2, 3, 4, 5, 7, 8, 9, 10, 12, 13, 14, 15, 19, 21, 22, 24, 31, 32,
            33, 35, 36, 37, 38, 39, 40, 41, 42, 43, 45, 46, 48, 49, 50, 51,
            53, 54)


class TestNothingThatWaddlesCanBeSent(unittest.TestCase):
    """The safety boundary. Keyed on the #12 measurement, not on leg position:
    the animation retracts the stabiliser itself, so a pre-set tripod is not a
    mitigation, and `get_leg_action` cannot sense stance to verify one."""

    def test_every_measured_waddler_is_refused(self):
        for i in WADDLERS:
            with self.subTest(anim=i):
                self.assertIn("REFUSED",
                              CV.send_animation(B.FakeBridge(), i, "stance"))

    def test_the_two_lists_do_not_overlap(self):
        self.assertEqual(set(WADDLERS) & set(CV.SAFE_ANIMATIONS), set())

    def test_unclassified_ids_are_refused_by_default(self):
        unclassified = [i for i in range(56)
                        if i not in WADDLERS and i not in CV.SAFE_ANIMATIONS]
        self.assertTrue(unclassified, "expected some unmeasured ids")
        for i in unclassified:
            with self.subTest(anim=i):
                self.assertIn("REFUSED",
                              CV.send_animation(B.FakeBridge(), i, "stance"))

    def test_ids_outside_the_table_entirely_are_refused(self):
        for i in (-1, 56, 999):
            self.assertIn("REFUSED",
                          CV.send_animation(B.FakeBridge(), i, "stance"))

    def test_animations_need_the_stance_ceiling(self):
        for ceiling in ("read", "leds", "audio", "dome"):
            with self.subTest(ceiling=ceiling):
                self.assertIn("held back",
                              CV.send_animation(B.FakeBridge(), 1, ceiling))

    def test_the_configured_defaults_are_on_the_safe_list(self):
        self.assertIn(CV.WAKE_ANIM, CV.SAFE_ANIMATIONS)
        self.assertIn(CV.REPLY_ANIM, CV.SAFE_ANIMATIONS)


class TestWakePhraseIsNotSpeech(unittest.TestCase):
    """OBSERVED live: whisper renders "R2-D2" as "2D2", "to do too" and
    "To detour". Every turn of one run fed the wake phrase to the reasoning
    layer as content, and Threepio replied ABOUT the string."""

    def test_the_real_transcripts_are_cleaned(self):
        cases = [
            ("2D2. Are you up?", "Are you up?"),
            ("to do too. Meet my son, Luca.", "Meet my son, Luca."),
            ("2d2. Luca is actually a Jedi.", "Luca is actually a Jedi."),
            ("R2-D2 what time is it?", "what time is it?"),
            ("Artoo Detoo, come here.", "come here."),
        ]
        for raw, want in cases:
            with self.subTest(raw=raw):
                self.assertEqual(CV.strip_wake(raw)[0], want)

    def test_real_speech_starting_similarly_is_left_alone(self):
        # A stripper that eats real speech is worse than the bug it fixes.
        for t in ("Two dogs are barking.", "Todo list for tomorrow.",
                  "Are you up?", "Detour the truth is a strange phrase.",
                  "To do that I need help."):
            with self.subTest(text=t):
                self.assertEqual(CV.strip_wake(t), (t, ""))

    def test_it_only_strips_at_the_start(self):
        t = "Tell me about R2-D2 please."
        self.assertEqual(CV.strip_wake(t)[0], t)

    def test_a_transcript_that_is_only_the_wake_phrase_becomes_empty(self):
        # This is the "nothing intelligible" path, not a reply about nothing.
        for t in ("2D2.", "to do too", "R2-D2"):
            with self.subTest(text=t):
                self.assertEqual(CV.strip_wake(t)[0], "")

    def test_the_removed_text_is_reported_for_the_log(self):
        self.assertEqual(CV.strip_wake("2D2. Hello")[1], "2D2.")


class TestBridgeOpening(unittest.TestCase):

    def test_send_none_opens_nothing(self):
        bridge, why = CV.open_bridge("none")
        self.assertIsNone(bridge)
        self.assertIn("not sending", why)

    def test_a_missing_daemon_is_reported_not_raised(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            bridge, why = CV.open_bridge("audio", Path(d))
            self.assertIsNone(bridge)
            self.assertIn("no daemon", why)


class TestBeatDegradation(unittest.TestCase):
    """A beat above the ceiling is degraded, not dropped — `reason()` almost
    always returns a dome angle, so rejecting outright made `--send audio`
    silent in practice. Degrading only ever removes motion."""

    def beh(self, deg):
        return RSN.reason("x", provider=RSN.StubProvider(
            {"sound": "R2_HEY_1", "dome_deg": deg, "mood": "happy"}))

    def test_a_dome_beat_still_chirps_at_the_audio_ceiling(self):
        fake = B.FakeBridge()
        self.assertIn("sent", CV.send_r2(self.beh(18), fake, "audio"))
        ops = [s.op for s in fake.sent]
        self.assertIn("sound", ops)
        self.assertNotIn("dome", ops, "degrading must REMOVE motion")

    def test_the_full_beat_survives_at_the_dome_ceiling(self):
        fake = B.FakeBridge()
        CV.send_r2(self.beh(18), fake, "dome")
        self.assertIn("dome", [s.op for s in fake.sent])

    def test_it_is_held_back_when_even_the_sound_is_too_high(self):
        self.assertIn("held back", CV.send_r2(self.beh(18), B.FakeBridge(), "leds"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
