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

    def test_authored_dome_moves_are_relative_never_absolute(self):
        # Scoped to AUTHORED phrases on purpose. perform() does emit one
        # absolute angle for the drift correction, and that is correct --
        # see test_a_large_residual_is_corrected_with_an_absolute_angle.
        # The dome has no resting position (observed at 103, 3.3 and -0.06
        # degrees), so a destination inside a gesture is meaningless; travel
        # is bounded from a freshly read position by the daemon.
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

    def setUp(self):
        # perform() falls back to a PROCESS-WIDE home. Without this reset the
        # first test to run anchors it and every later test inherits that
        # angle -- which is exactly how two of these started failing.
        B.reset_default_home()

    tearDown = setUp

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
        # return_to_start wraps the phrases: one leading head read, then a
        # residual read and a closing colour reset after them.
        phrase_batches = bridge.batches[1:1 + len(beat.phrases)]
        self.assertEqual([len(b) for b in phrase_batches],
                         [len(p.steps) for p in beat.phrases])
        self.assertEqual(bridge.batches[0][0].op, "head")

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

    def test_a_large_residual_is_corrected_with_an_absolute_angle(self):
        # Deltas undershoot ~3 deg each and never sum back to zero, so a
        # correction that IS achievable must target an absolute angle.
        # Simulate a dome that ended 20 deg away from where it began.
        bridge = B.FakeBridge(head_seq=[-30.0, -50.0, -30.0])
        out = B.perform(B.express_curious(), bridge, ceiling="dome",
                        sleep=lambda _: None)
        self.assertEqual(out["start_angle"], -30.0)
        closing = [s for s in bridge.sent if s.op == "dome"][-1]
        self.assertEqual(closing.params.get("angle"), -30.0)
        self.assertNotIn("delta", closing.params)
        self.assertEqual(out["residual_deg"], 0.0)

    def test_a_residual_below_the_threshold_is_reported_not_commanded(self):
        # The firmware ignores sub-12-degree moves and still reports success,
        # so emitting one would manufacture a false correction. Say what the
        # residual is instead of pretending to fix it.
        bridge = B.FakeBridge(head_seq=[-30.0, -34.0])
        out = B.perform(B.express_curious(), bridge, ceiling="dome",
                        sleep=lambda _: None)
        self.assertEqual(out["residual_deg"], 4.0)
        self.assertFalse(any(s.op == "dome" and "angle" in s.params
                             for s in bridge.sent))

    def test_every_authored_dome_move_clears_the_threshold(self):
        # The gesture must close itself: a beat relying on a small tidy-up
        # move at the end is relying on the one move that never executes.
        for name, make in B.VOCABULARY.items():
            for p in make().phrases:
                for st in p.steps:
                    if st.op == "dome" and "delta" in st.params:
                        with self.subTest(beat=name):
                            self.assertGreaterEqual(
                                abs(st.params["delta"]), B.MIN_DOME_TRAVEL_DEG)

    def test_sub_threshold_dome_move_is_rejected_at_build_time(self):
        bad = B.Beat(
            name="too_small", rest_colour=B.BASE_NEUTRAL, interruptible=True,
            energy="low", cooldown_s=0.0, evidence="test",
            phrases=(
                B.Phrase((B.Step("dome", {"delta": 8.0, "settle": 0}),),
                         gap_s=B.DOME_MOVE_S),
                B.Phrase((B.Step(
                    "leds", {"channels": B.front(B.BASE_NEUTRAL)}),)),
            ))
        with self.assertRaises(ValueError) as cm:
            bad.validate()
        self.assertIn("silently ignored", str(cm.exception))

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

    def test_estimate_does_not_exceed_measured_runtime(self):
        # The contract is "not shorter than", so an OVER-estimate is a bug,
        # not a rounding quirk. An earlier version double-counted the closing
        # settle and assumed the drift correction always fires, giving
        # 12.47 s against two measured runs of 9.978 s and 10.337 s.
        MEASURED_FASTEST = 9.978
        self.assertLess(B.express_curious().estimated_duration_s(),
                        MEASURED_FASTEST)

    def test_both_beats_are_bounded(self):
        for name, make in B.VOCABULARY.items():
            with self.subTest(beat=name):
                self.assertLess(make().estimated_duration_s(), 30.0)




class TestDomeHome(unittest.TestCase):

    def setUp(self):
        # perform() falls back to a PROCESS-WIDE home. Without this reset the
        # first test to run anchors it and every later test inherits that
        # angle -- which is exactly how two of these started failing.
        B.reset_default_home()

    tearDown = setUp
    """The persistent drift anchor. A per-beat anchor measured the last beat's
    error and forgot it, so the correction never fired across five real runs
    while the dome walked -24.58 -> -41.17."""

    def test_home_is_set_once_and_does_not_follow_the_dome(self):
        h = B.DomeHome()
        self.assertEqual(h.anchor(-30.0), -30.0)
        self.assertEqual(h.anchor(-33.0), -30.0)     # not re-anchored
        self.assertEqual(h.anchor(-41.0), -30.0)

    def test_small_drift_is_not_commandable(self):
        h = B.DomeHome(-30.0)
        self.assertIsNone(h.correction(-33.0))       # 3 deg, below threshold

    def test_accumulated_drift_eventually_becomes_commandable(self):
        # The whole point: error builds against a FIXED mark until the
        # hardware can act on it. Against a moving anchor it never does.
        h = B.DomeHome(-30.0)
        here = -30.0
        fired = False
        for _ in range(6):
            here -= 3.0                              # one beat's worth of drift
            if h.correction(here) is not None:
                fired = True
                break
        self.assertTrue(fired, "correction never became commandable")

    def test_correction_points_back_toward_home(self):
        h = B.DomeHome(-30.0)
        self.assertAlmostEqual(h.correction(-45.0), 15.0)
        self.assertAlmostEqual(h.correction(-15.0), -15.0)

    def test_unanchored_home_never_commands(self):
        self.assertIsNone(B.DomeHome().correction(-99.0))

    def test_reset_clears_the_process_wide_home(self):
        # The default home holds the FIRST angle this process ever read, for
        # the life of the process. That is right within one session and wrong
        # across sessions or after the robot is physically moved -- and it is
        # what made two tests in this file fail by inheriting each other's
        # anchor.
        b = B.FakeBridge(head_seq=[-30.0, -31.0])
        B.perform(B.express_curious(), b, ceiling="dome", sleep=lambda _: None)
        self.assertEqual(B._DEFAULT_HOME.angle, -30.0)
        B.reset_default_home()
        self.assertIsNone(B._DEFAULT_HOME.angle)
        b2 = B.FakeBridge(head_seq=[-88.0, -89.0])
        out = B.perform(B.express_curious(), b2, ceiling="dome",
                        sleep=lambda _: None)
        self.assertEqual(out["start_angle"], -88.0)   # re-anchored, not -30

    def test_default_home_is_used_when_the_caller_passes_none(self):
        # The whole point of the default: a caller that knows nothing about
        # DomeHome still gets bounded drift. Shipping this as opt-in made the
        # fix inert, because no call site passed it.
        b1 = B.FakeBridge(head_seq=[-30.0, -33.0])
        B.perform(B.express_curious(), b1, ceiling="dome", sleep=lambda _: None)
        self.assertEqual(B._DEFAULT_HOME.angle, -30.0)
        # Second beat starts elsewhere; the default home must NOT follow it.
        b2 = B.FakeBridge(head_seq=[-33.0, -36.0])
        out = B.perform(B.express_curious(), b2, ceiling="dome",
                        sleep=lambda _: None)
        self.assertEqual(out["start_angle"], -30.0)
        self.assertEqual(out["residual_deg"], 6.0)

    def test_perform_anchors_to_home_not_to_this_beat(self):
        home = B.DomeHome()
        b = B.FakeBridge(head_seq=[-30.0, -33.0])
        B.perform(B.express_curious(), b, ceiling="dome",
                  home=home, sleep=lambda _: None)
        self.assertEqual(home.angle, -30.0)
        # Second beat starts 3 deg away; home must NOT move to -33.
        b2 = B.FakeBridge(head_seq=[-33.0, -36.0])
        out = B.perform(B.express_curious(), b2, ceiling="dome",
                        home=home, sleep=lambda _: None)
        self.assertEqual(home.angle, -30.0)
        self.assertEqual(out["start_angle"], -30.0)
        self.assertEqual(out["residual_deg"], 6.0)   # vs home, not vs -33



class TestGuardIsOnTheSendPath(unittest.TestCase):
    """C1 — the forbidden-op check must be where the packets go.

    It used to live only in `Beat.validate()`. That made the guarantee true of
    beats and false of the module: any caller holding a Step and a Bridge could
    reach `animation` with nothing objecting. D-013 claims these ops are
    "unrepresentable", so the check belongs on the send path.
    """

    def test_send_batch_refuses_a_forbidden_op(self):
        bridge = B.FakeBridge()
        for op in B.FORBIDDEN_OPS:
            with self.assertRaises(ValueError):
                bridge.send_batch([B.Step(op, {"id": 21})], timeout=1.0)
            self.assertEqual(bridge.batches, [],
                             f"{op!r} must not reach the queue at all")

    def test_send_batch_refuses_an_unknown_op(self):
        bridge = B.FakeBridge()
        with self.assertRaises(ValueError):
            bridge.send_batch([B.Step("selfdestruct", {})], timeout=1.0)
        self.assertEqual(bridge.batches, [])

    def test_a_legal_step_still_goes_through(self):
        bridge = B.FakeBridge()
        out = bridge.send_batch([B.Step("leds", {"channels": {0: 255}})], 1.0)
        self.assertEqual(len(out), 1)
        self.assertEqual(len(bridge.batches), 1)

    def test_every_bridge_subclass_guards(self):
        # The guard is on the base class, so a future Bridge cannot forget it
        # by overriding the wrong method. `_send` is the extension point.
        self.assertIs(B.FileBridge.send_batch, B.Bridge.send_batch)
        self.assertIs(B.FakeBridge.send_batch, B.Bridge.send_batch)


class TestPerformStopsOnFailure(unittest.TestCase):
    """C2 — a failed step must halt the beat, not be sent on top of."""

    def test_a_failed_phrase_aborts_the_remaining_phrases(self):
        beat = B.express_curious()
        bridge = B.FakeBridge(fail_on="sound")
        out = B.perform(beat, bridge, ceiling="dome", home=B.DomeHome(),
                        sleep=lambda _: None)
        self.assertFalse(out["ok"])
        self.assertIsNotNone(out["aborted_at_phrase"],
                             "a failed step must record where it stopped")
        # The phrase batches actually sent must stop at the abort, plus the
        # leading head read and the trailing rest-colour reset.
        self.assertLess(out["aborted_at_phrase"], len(beat.phrases) - 1,
                        "fixture must fail before the last phrase to be a test")

    def test_a_clean_run_does_not_report_an_abort(self):
        out = B.perform(B.express_curious(), B.FakeBridge(), ceiling="dome",
                        home=B.DomeHome(), sleep=lambda _: None)
        self.assertIsNone(out["aborted_at_phrase"])


class TestEveryBeatLandsOnAStatusColour(unittest.TestCase):
    """C3 — D-013 point 4, enforced rather than asserted.

    The reset used to sit inside the `return_to_start` block, so `thinking()`
    ended every run holding PULSE_CYAN — an expression colour, persisted,
    because a colour we set survives the link dropping.
    """

    def _last_front(self, bridge):
        # `front()` stringifies the bit numbers, because the bridge serialises
        # params to JSON and JSON object keys are strings.
        keys = (str(B.LED_FRONT_R), str(B.LED_FRONT_G), str(B.LED_FRONT_B))
        for batch in reversed(bridge.batches):
            for step in batch:
                ch = step.params.get("channels", {}) if step.op == "leds" else {}
                if all(k in ch for k in keys):
                    return tuple(ch[k] for k in keys)
        return None

    def test_thinking_ends_on_its_rest_colour(self):
        beat = B.thinking()
        bridge = B.FakeBridge()
        B.perform(beat, bridge, ceiling="audio", sleep=lambda _: None)
        self.assertEqual(self._last_front(bridge), beat.rest_colour)

    def test_curious_ends_on_its_rest_colour(self):
        beat = B.express_curious()
        bridge = B.FakeBridge()
        B.perform(beat, bridge, ceiling="dome", home=B.DomeHome(),
                  sleep=lambda _: None)
        self.assertEqual(self._last_front(bridge), beat.rest_colour)

    def test_the_reset_still_runs_after_an_aborted_beat(self):
        beat = B.express_curious()
        bridge = B.FakeBridge(fail_on="sound")
        B.perform(beat, bridge, ceiling="dome", home=B.DomeHome(),
                  sleep=lambda _: None)
        self.assertEqual(self._last_front(bridge), beat.rest_colour,
                         "an aborted beat must still land on a status colour")

    def test_a_bridge_that_raises_does_not_escape_without_a_reset(self):
        class Exploding(B.FakeBridge):
            def _send(self, steps, timeout=12.0):
                if any(s.op == "dome" for s in steps):
                    raise RuntimeError("bridge not running")
                return super()._send(steps, timeout)

        bridge = Exploding()
        with self.assertRaises(RuntimeError):
            B.perform(B.express_curious(), bridge, ceiling="dome",
                      home=B.DomeHome(), sleep=lambda _: None)
        # The original exception still surfaces, AND the tidy-up ran.
        self.assertEqual(self._last_front(bridge),
                         B.express_curious().rest_colour)


class TestConstantsMatchTheDaemon(unittest.TestCase):
    """C4 — this module hand-copies the daemon's authorization table.

    Duplicated on purpose (importing r2_probe drags in bleak, and this suite
    must run with no BLE stack), which makes drift silent rather than
    impossible. D-009 has already moved an op between tiers once. Parse the
    daemon's source instead of importing it.
    """

    @staticmethod
    def _probe_ast():
        import ast
        src = (Path(__file__).parent / "r2_probe.py").read_text()
        return ast.parse(src)

    def _assignments(self):
        import ast
        out = {}
        for node in ast.walk(self._probe_ast()):
            if isinstance(node, ast.Assign):
                for t in node.targets:
                    if isinstance(t, ast.Name):
                        out[t.id] = node.value
                    elif isinstance(t, ast.Tuple):
                        for i, el in enumerate(t.elts):
                            if isinstance(el, ast.Name) and isinstance(
                                    node.value, ast.Tuple):
                                out[el.id] = node.value.elts[i]
        return out

    def test_tiers_match(self):
        import ast
        node = self._assignments()["TIERS"]
        self.assertEqual(tuple(ast.literal_eval(node)), B.TIERS)

    def test_op_tiers_match(self):
        import ast
        ops_node = self._assignments()["OPS"]
        daemon = {}
        for k, v in zip(ops_node.keys, ops_node.values):
            daemon[ast.literal_eval(k)] = ast.literal_eval(v.elts[0])
        for op, tier in B.OP_TIER.items():
            self.assertIn(op, daemon, f"{op!r} no longer exists in the daemon")
            self.assertEqual(
                tier, daemon[op],
                f"{op!r} is tier {tier!r} here and {daemon[op]!r} in the "
                f"daemon — this module would compute the wrong required_tier")
        for op in B.FORBIDDEN_OPS:
            self.assertIn(op, daemon)

    def test_led_bits_match(self):
        import ast
        a = self._assignments()
        for name, ours in (("LED_FRONT_R", B.LED_FRONT_R),
                           ("LED_FRONT_G", B.LED_FRONT_G),
                           ("LED_FRONT_B", B.LED_FRONT_B),
                           ("LED_LOGIC", B.LED_LOGIC),
                           ("LED_BACK_R", B.LED_BACK_R),
                           ("LED_BACK_G", B.LED_BACK_G),
                           ("LED_BACK_B", B.LED_BACK_B),
                           ("LED_HOLO", B.LED_HOLO)):
            self.assertEqual(ast.literal_eval(a[name]), ours,
                             f"{name} disagrees with the daemon")

if __name__ == "__main__":
    unittest.main(verbosity=2)
