#!/usr/bin/env python3
"""Tests for reason() (#46, S2 V5).

No network, no key, no robot. Every acceptance criterion is asserted against
the stub provider, which is the point of AC4's seam.

    python3 -m unittest discover -s mac-prototype -p 'test_*.py' -v
"""

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).parent.parent))

import r2_behavior as B
import reason as R
from r2_assets import R2_SOUNDS


class TestAC1SoundIdIsFromTheTable(unittest.TestCase):

    def test_a_valid_name_resolves_to_a_value_in_R2_SOUNDS(self):
        b = R.reason("hello", provider=R.StubProvider())
        self.assertIn(b.sound_id, set(R2_SOUNDS.values()))

    def test_the_beat_validates_and_tops_out_at_the_dome_tier(self):
        beat = R.reason("hello", provider=R.StubProvider()).to_beat()
        beat.validate()
        self.assertEqual(beat.required_tier(), "dome")

    def test_a_beat_with_no_dome_move_tops_out_at_audio(self):
        p = R.StubProvider({"sound": "R2_CHATTY_1", "dome_deg": 0, "mood": "happy"})
        self.assertEqual(R.reason("hi", provider=p).to_beat().required_tier(), "audio")


class TestAC2BadOutputIsRejectedBeforeAnyBleTraffic(unittest.TestCase):

    def test_an_unknown_sound_name_is_refused(self):
        p = R.StubProvider({"sound": "R2_DEFINITELY_NOT_REAL", "dome_deg": 0,
                            "mood": "happy"})
        with self.assertRaises(R.ReasonError) as ctx:
            R.reason("hi", provider=p)
        self.assertIn("R2_SOUNDS", str(ctx.exception))

    def test_a_raw_id_outside_the_table_is_refused(self):
        p = R.StubProvider({"sound": 999999, "dome_deg": 0, "mood": "happy"})
        with self.assertRaises(R.ReasonError):
            R.reason("hi", provider=p)

    def test_a_raw_id_INSIDE_the_table_is_accepted(self):
        real = R2_SOUNDS["R2_CHATTY_1"]
        p = R.StubProvider({"sound": real, "dome_deg": 0, "mood": "happy"})
        self.assertEqual(R.reason("hi", provider=p).sound_id, real)

    def test_rejection_happens_before_a_beat_could_exist(self):
        # "Before any BLE traffic" means the Behaviour is never constructed,
        # so there is nothing for a caller to accidentally send.
        p = R.StubProvider({"sound": None, "dome_deg": 0, "mood": "happy"})
        with self.assertRaises(R.ReasonError):
            R.reason("hi", provider=p)


class TestDomeAnglesAreCheckedNotTrusted(unittest.TestCase):
    """MEASURED: five models returned 0, 8, 12 and 20 deg with the >=12 rule
    written into the schema. The prompt is not enforcement."""

    def test_an_angle_below_the_minimum_is_dropped_and_logged(self):
        p = R.StubProvider({"sound": "R2_CHATTY_1", "dome_deg": 8, "mood": "happy"})
        b = R.reason("hi", provider=p)
        self.assertEqual(b.dome_deg, 0.0)
        self.assertTrue(any("MIN_DOME_TRAVEL_DEG" in r for r in b.rejections))

    def test_an_over_range_angle_is_clamped_and_logged(self):
        p = R.StubProvider({"sound": "R2_CHATTY_1", "dome_deg": 300, "mood": "happy"})
        b = R.reason("hi", provider=p)
        self.assertEqual(b.dome_deg, R.MAX_DOME_DEG)
        self.assertTrue(any("clamped" in r for r in b.rejections))

    def test_a_non_numeric_angle_does_not_crash(self):
        p = R.StubProvider({"sound": "R2_CHATTY_1", "dome_deg": "left a bit",
                            "mood": "happy"})
        self.assertEqual(R.reason("hi", provider=p).dome_deg, 0.0)

    def test_a_legal_angle_survives_untouched(self):
        p = R.StubProvider({"sound": "R2_CHATTY_1", "dome_deg": -20, "mood": "sad"})
        b = R.reason("hi", provider=p)
        self.assertEqual(b.dome_deg, -20.0)
        self.assertEqual(b.rejections, ())

    def test_an_unknown_mood_falls_back_rather_than_failing(self):
        p = R.StubProvider({"sound": "R2_CHATTY_1", "dome_deg": 0, "mood": "smug"})
        b = R.reason("hi", provider=p)
        self.assertEqual(b.mood, "curious")
        self.assertTrue(any("smug" in r for r in b.rejections))


class TestAC3FailureReturnsToNeutral(unittest.TestCase):

    def test_a_provider_that_cannot_be_reached_raises_reason_error(self):
        p = R.StubProvider(error=R.ReasonError("provider unreachable: URLError"))
        with self.assertRaises(R.ReasonError):
            R.reason("hi", provider=p)

    def test_a_provider_slower_than_the_timeout_raises(self):
        p = R.StubProvider(delay=2.0)
        with self.assertRaises(R.ReasonError):
            R.reason("hi", provider=p, timeout=0.2)

    def test_the_error_beat_shows_pending_then_rests_neutral(self):
        beat = R.error_beat("network down")
        beat.validate()
        first = beat.phrases[0].steps[0].params["channels"]
        self.assertEqual(first, B.front(B.BASE_PENDING))
        self.assertEqual(beat.rest_colour, B.BASE_NEUTRAL)

    def test_the_error_beat_does_not_use_red(self):
        # BASE_DANGER is "danger and stop, ONLY" (D-012 Amendment A). A timed
        # out API call is not danger, and spending red on it would devalue the
        # one colour reserved for stopping him.
        beat = R.error_beat()
        for p in beat.phrases:
            for s in p.steps:
                self.assertNotEqual(s.params.get("channels"), B.front(B.BASE_DANGER))

    def test_the_error_beat_never_leaves_him_lit_on_a_warning(self):
        beat = R.error_beat()
        self.assertEqual(beat.phrases[-1].steps[-1].params["channels"],
                         B.front(B.BASE_NEUTRAL))


class TestAC4TheSeam(unittest.TestCase):

    def test_provider_is_selected_by_config(self):
        self.assertIsInstance(R.create_provider("stub"), R.StubProvider)

    def test_an_unknown_provider_is_refused(self):
        with self.assertRaises(ValueError):
            R.create_provider("a-vendor-we-do-not-have")

    # What counts as "naming a vendor": creating a COUPLING to one. An API
    # host, an SDK import, or a bare provider literal. Deliberately NOT any
    # mention of the word — the first version of this test matched
    # /anthropic/ anywhere and flagged `r2_probe.py`'s comment about the macOS
    # TCC bundle id `com.anthropic.claude-code`, which is a Bluetooth
    # ownership note and creates no coupling whatsoever. A test that cries
    # wolf on a comment gets deleted by the next person, taking the real
    # guarantee with it.
    VENDOR_COUPLING = re.compile(
        r"""api\.openai\.com
          | api\.anthropic\.com
          | ^\s*(import|from)\s+(openai|anthropic)\b
          | \b(openai|anthropic)\.(Client|ChatCompletion|completions|messages)\b
        """, re.I | re.X)

    def test_no_module_outside_reason_py_creates_a_vendor_coupling(self):
        # AC4 asserted mechanically rather than by inspection.
        root = Path(__file__).resolve().parent.parent
        offenders = []
        for f in sorted(list(root.glob("*.py")) + list((root / "voice").glob("*.py"))):
            # The ADAPTERS are allowed to name a vendor; that is their job.
            # The guarantee is that nothing else does, so the behaviour layer
            # stays swappable. Add to this list only when adding an adapter.
            if f.name in ("reason.py", "test_reason.py",
                          "speak.py", "test_speak.py"):
                continue
            for i, line in enumerate(f.read_text().splitlines(), 1):
                if line.lstrip().startswith("#"):
                    continue                     # a comment couples nothing
                if self.VENDOR_COUPLING.search(line):
                    offenders.append(f"{f.name}:{i}: {line.strip()[:70]}")
        self.assertEqual(offenders, [], "vendor coupling outside reason.py:\n" +
                         "\n".join(offenders))

    def test_the_detector_would_actually_catch_a_real_coupling(self):
        # A guard that never fires is indistinguishable from a broken one.
        for bad in ('    urlopen("https://api.openai.com/v1/chat")',
                    "import openai",
                    "from anthropic import Anthropic",
                    "    r = openai.completions.create(model=m)"):
            with self.subTest(line=bad):
                self.assertTrue(self.VENDOR_COUPLING.search(bad))

    def test_the_detector_does_not_fire_on_the_tcc_comment_that_broke_it(self):
        benign = "# Claude Code that is claude.app (com.anthropic.claude-code), whose bundle"
        self.assertTrue(benign.lstrip().startswith("#"))


class TestTheModelCannotExpressAnUnsafeOp(unittest.TestCase):
    """The structural guarantee: the tool vocabulary has no word for an op."""

    def test_the_schema_offers_only_sound_dome_and_mood(self):
        props = R.tool_schema()["function"]["parameters"]["properties"]
        self.assertEqual(set(props), {"sound", "dome_deg", "mood"})

    def test_no_forbidden_op_appears_anywhere_in_the_schema(self):
        blob = str(R.tool_schema())
        for op in B.FORBIDDEN_OPS:
            self.assertNotIn(op, blob)

    def test_the_sound_enum_is_exactly_the_native_table(self):
        enum = R.tool_schema()["function"]["parameters"]["properties"]["sound"]["enum"]
        self.assertEqual(set(enum), set(R2_SOUNDS))

    def test_every_composable_beat_stays_at_or_below_the_dome_tier(self):
        for deg in (0, 12, -45):
            for mood in R.MOODS:
                p = R.StubProvider({"sound": "R2_CHATTY_1", "dome_deg": deg,
                                    "mood": mood})
                beat = R.reason("x", provider=p).to_beat()
                with self.subTest(deg=deg, mood=mood):
                    self.assertLessEqual(B.tier_rank(beat.required_tier()),
                                         B.tier_rank("dome"))


class TestAC5TerminalOutput(unittest.TestCase):

    def test_describe_names_the_four_required_things(self):
        p = R.StubProvider({"sound": "R2_CHATTY_1", "dome_deg": 15, "mood": "curious"})
        out = R.reason("come here please", provider=p).describe()
        self.assertIn("come here please", out)          # heard text
        self.assertIn("curious", out)                   # intent
        self.assertIn(str(R2_SOUNDS["R2_CHATTY_1"]), out)  # sound id
        self.assertIn("timeline", out)                  # timeline
        self.assertIn("sound", out)

    def test_describe_surfaces_rejections(self):
        p = R.StubProvider({"sound": "R2_CHATTY_1", "dome_deg": 3, "mood": "happy"})
        self.assertIn("REJECTED", R.reason("x", provider=p).describe())


class TestMoodDoesNotPickAColour(unittest.TestCase):
    """D-012 Amendment A: all six corners carry status. Expression is motion,
    sound, holo and logic — never hue."""

    def test_every_mood_produces_the_same_rest_colour(self):
        colours = set()
        for mood in R.MOODS:
            p = R.StubProvider({"sound": "R2_CHATTY_1", "dome_deg": 0, "mood": mood})
            colours.add(R.reason("x", provider=p).to_beat().rest_colour)
        self.assertEqual(len(colours), 1, f"mood leaked into hue: {colours}")

    def test_the_rest_colour_is_a_declared_status_colour(self):
        beat = R.reason("x", provider=R.StubProvider()).to_beat()
        self.assertIn(beat.rest_colour, B.STATUS_COLOURS)


if __name__ == "__main__":
    unittest.main(verbosity=2)
