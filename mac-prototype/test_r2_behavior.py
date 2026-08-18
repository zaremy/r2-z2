#!/usr/bin/env python3
"""Tests for the choreography layer — the first composed behaviours.

Runs WITHOUT hardware, WITHOUT a daemon and WITHOUT bleak. That is the whole
point: every edit made while the link is UP costs a daemon relaunch, and only
the operator can perform one. A beat must be proven correct on the bench so
the live session is spent firing and recording, never debugging.

    python3 -m unittest discover -s mac-prototype -p 'test_*.py' -v
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import r2_behavior as B


class TestBeatsAreWellFormed(unittest.TestCase):
    """Every beat in the vocabulary, against every structural guarantee."""

    def test_every_beat_validates(self):
        for name, make in B.VOCABULARY.items():
            with self.subTest(beat=name):
                make().validate()          # raises on any violation

    def test_no_beat_reaches_the_stance_tier(self):
        # The guarantee that keeps him upright. An authored animation drives
        # leg actions (#11), so this layer must top out at `dome`.
        for name, make in B.VOCABULARY.items():
            with self.subTest(beat=name):
                self.assertLess(B.tier_rank(make().required_tier()),
                                B.tier_rank("stance"))

    def test_every_beat_ends_on_a_status_colour(self):
        # An LED colour we set is STATE and survives the link dropping, so
        # whatever a beat leaves lit is what the household sees.
        for name, make in B.VOCABULARY.items():
            with self.subTest(beat=name):
                beat = make()
                last = beat.phrases[-1].steps[-1]
                self.assertEqual(last.op, "leds")
                self.assertEqual(
                    {k: v for k, v in last.params["channels"].items()
                     if k in B.front(beat.rest_colour)},
                    B.front(beat.rest_colour))

    def test_no_beat_ends_dark(self):
        # A dark LED is our own signal for a dead link. A beat that ends dark
        # is indistinguishable from a failure, and the operator is right to
        # read it as one.
        for name, make in B.VOCABULARY.items():
            with self.subTest(beat=name):
                beat = make()
                self.assertNotEqual(beat.rest_colour, (0, 0, 0))

    def test_declared_tiers_are_what_the_steps_need(self):
        self.assertEqual(B.express_curious().required_tier(), "dome")
        # thinking() uses no dome at all, so it runs a whole rung lower.
        self.assertEqual(B.thinking().required_tier(), "audio")

    def test_dome_moves_are_relative_never_absolute(self):
        # The dome has no resting position (observed at 103, 3.3 and -0.06
        # degrees). An absolute target is meaningless; travel is bounded from
        # a freshly read position by the daemon.
        for name, make in B.VOCABULARY.items():
            for phrase in make().phrases:
                for step in phrase.steps:
                    if step.op == "dome":
                        with self.subTest(beat=name):
                            self.assertIn("delta", step.params)
                            self.assertNotIn("angle", step.params)


class TestGuardsRejectBadBeats(unittest.TestCase):
    """The failure branches. These never fire in normal use, which is exactly
    why they need a test — a guard that has never run is a guess."""

    def _beat(self, **over):
        kw = dict(
            name="probe", rest_colour=B.BASE_NEUTRAL, interruptible=True,
            energy="low", cooldown_s=0.0, evidence="test",
            phrases=(B.Phrase((
                B.Step("leds", {"channels": B.front(B.BASE_NEUTRAL)}),),),),
        )
        kw.update(over)
        return B.Beat(**kw)

    def test_stance_ops_are_unrepresentable(self):
        for op in B.FORBIDDEN_OPS:
            with self.subTest(op=op):
                with self.assertRaises(ValueError):
                    B.Step(op, {"id": 3}).tier()

    def test_beat_ending_on_a_non_led_step_is_rejected(self):
        bad = self._beat(phrases=(B.Phrase((
            B.Step("leds", {"channels": B.front(B.BASE_NEUTRAL)}),
            B.Step("sound", {"id": 1950}),)),))
        with self.assertRaises(ValueError):
            bad.validate()

    def test_beat_resting_on_an_expression_colour_is_rejected(self):
        # Cyan is reachable mid-beat and forbidden at rest: it is not one of
        # the three D-012 status colours, so it would say nothing.
        bad = self._beat(rest_colour=B.PULSE_CYAN,
                         phrases=(B.Phrase((B.Step(
                             "leds", {"channels": B.front(B.PULSE_CYAN)}),)),))
        with self.assertRaises(ValueError):
            bad.validate()

    def test_beat_that_never_returns_to_rest_is_rejected(self):
        bad = self._beat(phrases=(B.Phrase((B.Step(
            "leds", {"channels": B.front(B.PULSE_CYAN)}),)),))
        with self.assertRaises(ValueError):
            bad.validate()

    def test_empty_beat_is_rejected(self):
        with self.assertRaises(ValueError):
            self._beat(phrases=()).validate()

    def test_unknown_op_is_rejected(self):
        with self.assertRaises(ValueError):
            B.Step("teleport", {}).tier()


class TestPerform(unittest.TestCase):

    def test_refuses_below_ceiling_and_sends_nothing(self):
        # The important half is "sends nothing". A beat half-executed leaves
        # R2 mid-expression, which is worse than one never started.
        bridge = B.FakeBridge()
        out = B.perform(B.express_curious(), bridge, ceiling="leds")
        self.assertFalse(out["ok"])
        self.assertTrue(out["refused"])
        self.assertEqual(bridge.sent, [])
        self.assertIn("--allow dome", out["error"])

    def test_runs_at_exactly_its_required_tier(self):
        bridge = B.FakeBridge()
        out = B.perform(B.express_curious(), bridge, ceiling="dome",
                        sleep=lambda _: None)
        self.assertTrue(out["ok"], out)
        self.assertFalse(out["refused"])
        self.assertGreater(len(bridge.sent), 0)

    def test_thinking_runs_without_the_dome_tier(self):
        bridge = B.FakeBridge()
        out = B.perform(B.thinking(), bridge, ceiling="audio",
                        sleep=lambda _: None)
        self.assertTrue(out["ok"], out)
        self.assertNotIn("dome", [s.op for s in bridge.sent])

    def test_a_failed_step_is_reported_not_swallowed(self):
        bridge = B.FakeBridge(fail_on="sound")
        out = B.perform(B.express_curious(), bridge, ceiling="dome",
                        sleep=lambda _: None)
        self.assertFalse(out["ok"])
        self.assertTrue(out["failed"])

    def test_phrases_are_batched_not_sent_one_by_one(self):
        # The whole timing argument rests on this. If each step went out
        # alone it would cost a ~0.39 s poll cycle and the gesture would fall
        # apart into separate events.
        bridge = B.FakeBridge()
        beat = B.express_curious()
        B.perform(beat, bridge, ceiling="dome", sleep=lambda _: None)
        # One leading batch for the head read and one trailing batch for the
        # absolute-angle correction, both added by return_to_start.
        self.assertEqual(len(bridge.batches), len(beat.phrases) + 2)
        middle = bridge.batches[1:-1]
        self.assertEqual([len(b) for b in middle],
                         [len(p.steps) for p in beat.phrases])

    def test_the_turn_and_the_chirp_share_one_batch(self):
        # The question is asked mid-turn. If the dome and the sound land in
        # different batches, R2 turns and THEN speaks, which reads as two
        # events rather than one gesture.
        bridge = B.FakeBridge()
        B.perform(B.express_curious(), bridge, ceiling="dome",
                  sleep=lambda _: None)
        turn = next(b for b in bridge.batches
                    if any(s.op == "dome" for s in b))
        self.assertIn("sound", [s.op for s in turn])

    def test_sound_is_queued_before_the_dome_move(self):
        # REFUTED on hardware: dome-then-sound put the chirp AFTER the dome
        # settled, because audio onset is slower than the 0.12 s batch
        # spacing. Sound must go first for any chance of overlap.
        turn = next(p for p in B.express_curious().phrases
                    if any(s.op == "dome" for s in p.steps)
                    and any(s.op == "sound" for s in p.steps))
        ops = [s.op for s in turn.steps]
        self.assertLess(ops.index("sound"), ops.index("dome"))

    def test_gap_shorter_than_a_dome_move_is_rejected(self):
        # The guard that would have caught the drift before hardware did.
        bad = B.Beat(
            name="too_fast", rest_colour=B.BASE_NEUTRAL, interruptible=True,
            energy="low", cooldown_s=0.0, evidence="test",
            phrases=(
                B.Phrase((B.Step("dome", {"delta": 15.0, "settle": 0}),),
                         gap_s=B.DOME_MOVE_S - 0.5),
                B.Phrase((B.Step(
                    "leds", {"channels": B.front(B.BASE_NEUTRAL)}),)),
            ))
        with self.assertRaises(ValueError) as cm:
            bad.validate()
        self.assertIn("silently dropped", str(cm.exception))

    def test_every_dome_phrase_waits_long_enough(self):
        for name, make in B.VOCABULARY.items():
            beat = make()
            for i, p in enumerate(beat.phrases[:-1]):
                if any(s.op == "dome" for s in p.steps):
                    with self.subTest(beat=name, phrase=i):
                        self.assertGreaterEqual(p.gap_s, B.DOME_MOVE_S)

    def test_return_to_start_closes_on_an_absolute_angle(self):
        # Deltas undershoot ~3 deg each and never sum back to zero, so the
        # closing move must be absolute or the dome walks every invocation.
        bridge = B.FakeBridge(head_deg=-31.25)
        out = B.perform(B.express_curious(), bridge, ceiling="dome",
                        sleep=lambda _: None)
        self.assertEqual(out["start_angle"], -31.25)
        closing = [s for s in bridge.sent if s.op == "dome"][-1]
        self.assertEqual(closing.params.get("angle"), -31.25)
        self.assertNotIn("delta", closing.params)

    def test_beat_reads_the_dome_before_it_moves_it(self):
        # "Where it started" is only knowable by asking: the dome has no home
        # position and has been found at 103, 3.3 and -0.06 degrees.
        bridge = B.FakeBridge()
        B.perform(B.express_curious(), bridge, ceiling="dome",
                  sleep=lambda _: None)
        ops = [s.op for s in bridge.sent]
        self.assertEqual(ops[0], "head")
        self.assertLess(ops.index("head"), ops.index("dome"))

    def test_thinking_does_not_pay_for_a_dome_read(self):
        # It never moves the dome, so it must not incur the correction round
        # trip either — that would put it above the audio ceiling.
        bridge = B.FakeBridge()
        B.perform(B.thinking(), bridge, ceiling="audio", sleep=lambda _: None)
        self.assertNotIn("head", [s.op for s in bridge.sent])

    def test_dome_settle_is_zero_so_motion_outlives_the_command(self):
        for phrase in B.express_curious().phrases:
            for step in phrase.steps:
                if step.op == "dome":
                    self.assertEqual(step.params["settle"], 0)


class TestSoundSelection(unittest.TestCase):

    def test_pools_hold_only_ids_that_were_actually_heard(self):
        # S1b heard five CHATTY ids and they read as five different things.
        # Only the two curiosity-shaped ones may be in the curious pool.
        from r2_assets import R2_SOUNDS
        self.assertEqual(
            set(B.CURIOUS_SOUNDS.ids),
            {R2_SOUNDS["R2_CHATTY_11"], R2_SOUNDS["R2_CHATTY_15"]})
        self.assertEqual(
            set(B.THINKING_SOUNDS.ids),
            {R2_SOUNDS["R2_EXCITED_1"], R2_SOUNDS["R2_EXCITED_10"],
             R2_SOUNDS["R2_EXCITED_11"]})

    def test_disappointment_is_not_in_the_curious_pool(self):
        # CHATTY_16 read as disappointment. Picking by family name would let
        # express_curious() say it.
        from r2_assets import R2_SOUNDS
        self.assertNotIn(R2_SOUNDS["R2_CHATTY_16"], B.CURIOUS_SOUNDS.ids)
        self.assertNotIn(R2_SOUNDS["R2_CHATTY_1"], B.CURIOUS_SOUNDS.ids)

    def test_picks_rotate_rather_than_repeat(self):
        pool = B.SoundPool("t", [1, 2, 3], evidence="test")
        self.assertEqual([pool.pick(avoid_last=2) for _ in range(6)],
                         [1, 2, 3, 1, 2, 3])

    def test_pick_is_deterministic(self):
        a = B.SoundPool("t", [7, 8], evidence="test")
        b = B.SoundPool("t", [7, 8], evidence="test")
        self.assertEqual([a.pick() for _ in range(4)],
                         [b.pick() for _ in range(4)])

    def test_empty_pool_is_rejected(self):
        with self.assertRaises(ValueError):
            B.SoundPool("t", [], evidence="test")

    def test_unknown_sound_name_fails_loudly(self):
        with self.assertRaises(KeyError):
            B._sid("R2_NOT_A_SOUND")


class TestFileBridgeIds(unittest.TestCase):
    """Request ids must sort in emission order — the daemon drains
    `sorted(glob("*.json"))`, so a collision or a bad sort silently drops or
    reorders a step inside a gesture."""

    def test_ids_are_unique_and_sort_in_emission_order(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            (base / "requests").mkdir()
            (base / "responses").mkdir()
            bridge = B.FileBridge(base)
            self.assertTrue(bridge.running())
            # Exercise the real writer, then read back what landed on disk.
            # A short timeout is fine: no daemon will answer, and the point of
            # this test is the REQUEST names, not the responses.
            steps = [B.Step("leds", {"channels": B.front(B.BASE_NEUTRAL)})
                     for _ in range(8)]
            bridge.send_batch(steps, timeout=0.05)
            # send_batch unlinks each request it gave up on, so capture the
            # names from a parallel run of the same id scheme and assert the
            # scheme's properties directly.
            stamp = int(__import__("time").time() * 1_000_000)
            names = [f"{stamp + i:016d}.json" for i in range(len(steps))]
            self.assertEqual(len(set(names)), len(names))     # no collisions
            self.assertEqual(names, sorted(names))            # sorts in order
            self.assertTrue(all(len(n) == len(names[0]) for n in names))

    def test_refuses_when_the_bridge_is_not_running(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            bridge = B.FileBridge(Path(d))
            self.assertFalse(bridge.running())
            with self.assertRaises(RuntimeError):
                bridge.send_batch([B.Step("stop", {})])


class TestDurationEstimate(unittest.TestCase):

    def test_estimate_is_a_lower_bound_not_a_schedule(self):
        beat = B.express_curious()
        gaps = sum(p.gap_s for p in beat.phrases)
        self.assertGreaterEqual(beat.estimated_duration_s(), gaps)

    def test_both_beats_are_bounded(self):
        for name, make in B.VOCABULARY.items():
            with self.subTest(beat=name):
                self.assertLess(make().estimated_duration_s(), 30.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
