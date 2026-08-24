#!/usr/bin/env python3
"""Tests for the happiness scalar (#85 AC2, AC3, AC4).

No hardware, no sleeping, no wall clock. Every test injects the clock, which
is the point of AC4: a decay that secretly measured process uptime would pass
a test that used the real clock and fail the first restart.

    python3 -m unittest discover -s mac-prototype -p 'test_*.py' -v
"""

import json
import tempfile
import unittest
from pathlib import Path

import r2_mood as M

T0 = 1_700_000_000.0        # an arbitrary but fixed wall-clock epoch


class TestAC2NoSingleTouchSaturates(unittest.TestCase):
    """The load-bearing property. If one touch pinned happiness to 100 the
    scalar would be a constant with a decay, and every behaviour reading it
    would read nothing about how R2 has been treated."""

    def test_from_every_starting_value_one_touch_stays_below_the_ceiling(self):
        for start in range(0, 101):
            with self.subTest(start=start):
                h = M.Happiness(value=float(start), updated_at=T0)
                new, _ = h.touched(T0 + 1.0)
                self.assertLess(new.value, M.CEILING,
                                f"a single touch saturated from {start}")

    def test_it_holds_for_a_freshly_touched_state_too(self):
        # The harder case: last_touch_at set, so recency is at its floor and
        # the deficit is whatever the previous touch left.
        h = M.Happiness(value=0.0, updated_at=T0)
        for i in range(40):
            h, _ = h.touched(T0 + i * 0.5)
            self.assertLess(h.value, M.CEILING)

    def test_forty_touches_approach_the_ceiling_without_reaching_it(self):
        h = M.Happiness(value=0.0, updated_at=T0)
        for i in range(40):
            h, _ = h.touched(T0 + i * 60.0)
        self.assertGreater(h.value, 80.0, "it should get close")
        self.assertLess(h.value, M.CEILING, "but never arrive")

    def test_the_gain_is_structurally_below_one(self):
        # The invariant is a property of the formula, not of a clamp. A clamp
        # would produce the same numbers and none of the meaning.
        self.assertLess(M.GAIN, 1.0)
        self.assertLess(M.RECENCY_FLOOR, 1.0)
        self.assertGreater(M.RECENCY_FLOOR, 0.0)

    def test_the_degenerate_zero_elapsed_case_is_documented_not_hidden(self):
        # At literally zero elapsed time from exactly the ceiling the deficit
        # is zero, so the value is unchanged. This cannot arise from a real
        # touch (a touch happens after the stored instant) and is asserted
        # here so it is a known property rather than a surprise.
        h = M.Happiness(value=M.CEILING, updated_at=T0)
        new, delta = h.touched(T0)
        self.assertEqual(delta, 0.0)
        self.assertEqual(new.value, M.CEILING)


class TestAC3RepeatsDiminish(unittest.TestCase):

    def test_four_touches_in_one_window_give_decreasing_deltas(self):
        h = M.Happiness(value=0.0, updated_at=T0)
        deltas = []
        for i in range(4):
            h, d = h.touched(T0 + i * 20.0)      # inside one cooldown window
            deltas.append(d)
        for a, b in zip(deltas, deltas[1:]):
            self.assertLess(b, a, f"deltas not decreasing: {deltas}")

    def test_the_first_touch_is_worth_far_more_than_the_fourth(self):
        h = M.Happiness(value=0.0, updated_at=T0)
        first = h.touched(T0)[1]
        for i in range(3):
            h, last = h.touched(T0 + (i + 1) * 20.0)
        self.assertGreater(first, last * 4,
                           "a repeat should be worth a fraction of the first")

    def test_a_touch_on_a_lonely_r2_beats_the_same_touch_on_a_recent_one(self):
        # The sentence the issue says must not be simplified away, as a test.
        lonely = M.Happiness(value=0.0, updated_at=T0)
        _, lonely_delta = lonely.touched(T0 + 1.0)

        recent = M.Happiness(value=0.0, updated_at=T0)
        recent, _ = recent.touched(T0)
        _, recent_delta = recent.touched(T0 + 30.0)

        self.assertGreater(lonely_delta, recent_delta * 3)

    def test_waiting_restores_the_welcome(self):
        h = M.Happiness(value=0.0, updated_at=T0)
        h, _ = h.touched(T0)
        soon = h.recency(T0 + 1.0)
        later = h.recency(T0 + 600.0)
        self.assertLess(soon, 0.2)
        self.assertGreater(later, 0.95)


class TestAC4DecayIsWallClock(unittest.TestCase):

    def test_a_stored_value_reloaded_later_reflects_elapsed_time(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "mood.json"
            M.save(M.Happiness(value=80.0, updated_at=T0), p)
            # A new process, a later wall clock, no shared state whatsoever.
            reloaded = M.load(p, now=T0 + M.DECAY_HALF_LIFE_S)
            self.assertAlmostEqual(reloaded.level(T0 + M.DECAY_HALF_LIFE_S),
                                   40.0, places=4)

    def test_two_half_lives_quarter_it(self):
        h = M.Happiness(value=100.0, updated_at=T0)
        self.assertAlmostEqual(h.level(T0 + 2 * M.DECAY_HALF_LIFE_S), 25.0,
                               places=4)

    def test_an_overnight_gap_leaves_him_lonely(self):
        h = M.Happiness(value=95.0, updated_at=T0)
        self.assertLess(h.level(T0 + 12 * 3600), 1.0)

    def test_decay_is_computed_on_read_not_by_a_timer(self):
        # Reading twice at the same instant must give the same answer, and
        # reading must not mutate. A ticking timer would fail both.
        h = M.Happiness(value=50.0, updated_at=T0)
        self.assertEqual(h.level(T0 + 60), h.level(T0 + 60))
        self.assertEqual(h.value, 50.0, "read mutated the state")

    def test_a_backwards_clock_does_not_raise_happiness(self):
        # An NTP correction must not look like a touch. Inventing decay from a
        # negative interval would multiply by 2**n and silently inflate him.
        h = M.Happiness(value=40.0, updated_at=T0)
        self.assertEqual(h.level(T0 - 3600), 40.0)

    def test_the_clock_is_injectable_end_to_end(self):
        with tempfile.TemporaryDirectory() as d:
            now = [T0]
            mood = M.Mood(Path(d) / "m.json", clock=lambda: now[0])
            mood.touch()
            hot = mood.level()
            now[0] += M.DECAY_HALF_LIFE_S
            self.assertAlmostEqual(mood.level(), hot / 2, places=4)

    def test_it_uses_wall_clock_semantics_not_monotonic(self):
        # r2_reactive runs on time.monotonic because it measures durations.
        # Persisting a monotonic timestamp and subtracting after a restart
        # yields a meaningless — often negative — number. The default clock
        # here must be the wall clock.
        import inspect
        import time as _t
        clock = inspect.signature(M.Mood.__init__).parameters["clock"].default
        self.assertIs(clock, _t.time, "the mood clock must be the wall clock")
        self.assertIsNot(clock, _t.monotonic)


class TestPersistence(unittest.TestCase):

    def test_a_missing_file_is_a_fresh_r2_not_an_error(self):
        h = M.load(Path("/nonexistent/nowhere/mood.json"), now=T0)
        self.assertEqual(h.value, 0.0)

    def test_a_corrupt_file_is_a_fresh_r2_not_an_outage(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.json"
            p.write_text("{ this is not json")
            self.assertEqual(M.load(p, now=T0).value, 0.0)

    def test_a_round_trip_preserves_the_state(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.json"
            h = M.Happiness(value=12.5, updated_at=T0, last_touch_at=T0 - 5)
            M.save(h, p)
            back = M.load(p, now=T0)
            self.assertAlmostEqual(back.value, 12.5)
            self.assertEqual(back.last_touch_at, T0 - 5)

    def test_the_write_is_atomic(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.json"
            M.save(M.Happiness(value=1.0, updated_at=T0), p)
            self.assertEqual([f.name for f in Path(d).iterdir()], ["m.json"],
                             "a .tmp file survived the write")
            json.loads(p.read_text())


class TestIntensityIsTheDeficit(unittest.TestCase):

    def test_a_starved_r2_yields_full_intensity(self):
        self.assertAlmostEqual(
            M.Happiness(value=0.0, updated_at=T0).intensity(T0), 1.0)

    def test_a_contented_r2_yields_almost_none(self):
        self.assertLess(
            M.Happiness(value=95.0, updated_at=T0).intensity(T0), 0.06)

    def test_reading_intensity_has_no_side_effect(self):
        h = M.Happiness(value=30.0, updated_at=T0)
        h.intensity(T0 + 100)
        self.assertEqual((h.value, h.updated_at), (30.0, T0))

    def test_intensity_falls_as_happiness_rises(self):
        h = M.Happiness(value=0.0, updated_at=T0)
        seen = []
        for i in range(5):
            seen.append(h.intensity(T0 + i * 30.0))
            h, _ = h.touched(T0 + i * 30.0)
        for a, b in zip(seen, seen[1:]):
            self.assertLess(b, a)


if __name__ == "__main__":
    unittest.main(verbosity=2)
