#!/usr/bin/env python3
"""Dry tests for the touch -> behaviour loop. No robot, no daemon.

The settle and recovery paths NEVER fire on a robot that is behaving, so if
they are not exercised here they are not exercised at all -- and both are
safety-relevant: recovery is the only thing standing between one pet and an
infinite self-triggering loop.
"""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import r2_probe as P
import r2_behavior as B
import r2_reactive as R
from sensor_probe import GROUPS, EXT_GROUPS, masks, channels, empirical_thresholds

MASK, EXT = masks()

# Component order, derived from the SAME tables the decoder reads, so a test
# payload cannot drift away from what the decoder expects.
ORDER = ([(g, c) for g, comps in P.SENSORS for c, bit in comps if MASK & bit] +
         [(g, c) for g, comps in P.EXT_SENSORS for c, bit in comps if EXT & bit])


def payload(values: dict[tuple[str, str], float]) -> str:
    return struct.pack(f">{len(ORDER)}f",
                       *[values.get(k, 0.0) for k in ORDER]).hex()


def rest_sample(i: int) -> str:
    """Rest is not silence -- it is a tiny stable jitter."""
    return payload({k: 0.001 * (i % 2) for k in ORDER})


def touch_sample(i: int) -> str:
    """A hand disturbs several channels at once. Two is the corroboration
    floor, so disturb exactly three: enough to fire, few enough that a rule
    demanding four would fail here rather than pass by accident."""
    v = {k: 0.001 * (i % 2) for k in ORDER}
    swing = 5.0 if i % 2 else -5.0
    v[("attitude", "pitch")] = swing
    v[("accelerometer", "x")] = swing
    v[("gyroscope", "x")] = swing
    return payload(v)


class Clock:
    def __init__(self):
        self.t = 0.0

    def now(self) -> float:
        return self.t

    def sleep(self, s: float) -> None:
        self.t += s


class ScriptedBridge(B.FakeBridge):
    """A FakeBridge that also serves synthetic sensor events.

    `noisy` is flipped by the test to simulate a hand. `per_poll` matches the
    4 Hz stream against a 0.25 s poll: one sample per drain.
    """

    def __init__(self, noisy: bool = False, per_poll: int = 1, **kw):
        super().__init__(**kw)
        self.noisy = noisy
        self.per_poll = per_poll
        self.i = 0
        self.flushes = 0
        self.dropped = 0
        self.extra_events: list[dict] = []
        self.noise_log: list[bool] = []

    def _reply(self, step):
        if step.op != "events":
            return super()._reply(step)
        self.noise_log.append(self.noisy)
        evs = []
        for _ in range(self.per_poll):
            self.i += 1
            evs.append({"name": "sensor_stream", "t": float(self.i),
                        "data": touch_sample(self.i) if self.noisy
                                else rest_sample(self.i)})
        evs.extend(self.extra_events)
        self.extra_events = []
        return {"ok": True, "op": "events",
                "data": {"events": evs, "count": len(evs),
                         "dropped": self.dropped}}


def quiet_feed(bridge, n: int = 60) -> R.Feed:
    feed = R.Feed(bridge, MASK, EXT)
    for _ in range(n):
        feed.drain()
    return feed


def baseline_thresholds(window_n: int = 6):
    """Thresholds derived from pure rest, exactly as a session would."""
    b = ScriptedBridge(noisy=False)
    feed = quiet_feed(b, 80)
    return empirical_thresholds(channels(feed.samples), window_n)


class TestWindowing(unittest.TestCase):
    def test_window_n_never_drops_below_four(self):
        # A two-sample peak-to-peak has no null distribution worth the name.
        self.assertEqual(R.window_n_for(4.0, 0.1), 4)
        self.assertEqual(R.window_n_for(4.0, 1.5), 6)

    def test_short_window_is_no_window(self):
        feed = quiet_feed(ScriptedBridge(), 3)
        self.assertEqual(feed.window(6), [])
        feed.drain(); feed.drain(); feed.drain()
        self.assertEqual(len(feed.window(6)), 6)

    def test_empty_window_never_fires(self):
        self.assertEqual(R.fires([], baseline_thresholds()), (False, None))


class TestFeed(unittest.TestCase):
    def test_non_sensor_events_are_ignored(self):
        b = ScriptedBridge()
        b.extra_events = [{"name": "animation_complete", "t": 1.0, "data": ""}]
        feed = R.Feed(b, MASK, EXT)
        self.assertEqual(feed.drain(), 1)

    def test_a_bad_length_payload_is_counted_not_guessed(self):
        # A short packet means our mask and the robot's disagree. Decoding the
        # first N fields would produce plausible, wrongly-labelled numbers.
        b = ScriptedBridge()
        b.extra_events = [{"name": "sensor_stream", "t": 1.0, "data": "0011"}]
        feed = R.Feed(b, MASK, EXT)
        feed.drain()
        self.assertEqual(feed.decode_errors, 1)
        self.assertEqual(len(feed.samples), 1)   # the good one survived

    def test_dropped_events_are_accumulated(self):
        b = ScriptedBridge()
        b.dropped = 2
        feed = R.Feed(b, MASK, EXT)
        feed.drain(); feed.drain()
        self.assertEqual(feed.dropped, 4)

    def test_flush_forgets_what_we_already_held(self):
        b = ScriptedBridge()
        feed = quiet_feed(b, 10)
        self.assertTrue(feed.samples)
        feed.flush()
        self.assertEqual(feed.samples, [])

    def test_ring_is_bounded(self):
        b = ScriptedBridge()
        feed = R.Feed(b, MASK, EXT, keep=10)
        for _ in range(40):
            feed.drain()
        self.assertEqual(len(feed.samples), 10)


class TestDetection(unittest.TestCase):
    def test_rest_does_not_fire(self):
        th = baseline_thresholds()
        feed = quiet_feed(ScriptedBridge(noisy=False), 20)
        self.assertFalse(R.fires(feed.window(6), th)[0])

    def test_touch_fires_and_names_its_channel(self):
        th = baseline_thresholds()
        feed = quiet_feed(ScriptedBridge(noisy=True), 20)
        hot, chan = R.fires(feed.window(6), th)
        self.assertTrue(hot)
        self.assertIsNotNone(chan)


class TestRatios(unittest.TestCase):
    """The statistic behind the boolean. `fires` collapsing to True/False is
    what made two live runs undiagnosable."""

    def test_rest_sits_well_under_every_limit(self):
        th = baseline_thresholds()
        feed = quiet_feed(ScriptedBridge(noisy=False), 20)
        rs = R.ratios(feed.window(6), th)
        self.assertTrue(rs)
        self.assertLess(max(rs.values()), 1.0)

    def test_touch_pushes_at_least_two_channels_over(self):
        th = baseline_thresholds()
        feed = quiet_feed(ScriptedBridge(noisy=True), 20)
        rs = R.ratios(feed.window(6), th)
        self.assertGreaterEqual(sum(1 for v in rs.values() if v >= 1.0), 2)

    def test_a_dead_channel_is_omitted_not_divided_by(self):
        # gyroscope.z came back with a rest limit of exactly 0.0 on the floor.
        # A channel that reported a constant for 30 s must drop out, not
        # produce an infinity that dominates every ranking.
        th = dict(baseline_thresholds())
        th["gyroscope.z"] = (0.0, 0.0)
        feed = quiet_feed(ScriptedBridge(noisy=True), 20)
        rs = R.ratios(feed.window(6), th)
        self.assertNotIn("gyroscope.z", rs)
        self.assertTrue(all(v == v and v != float("inf") for v in rs.values()))

    def test_an_empty_window_scores_nothing(self):
        self.assertEqual(R.ratios([], baseline_thresholds()), {})


class TestNegativeControl(unittest.TestCase):
    """The gate that S1e learned the hard way. A detector stuck ON produces
    the loud answer, and loud reads as success."""

    def test_quiet_detector_passes_the_control(self):
        c = Clock()
        b = ScriptedBridge(noisy=False)
        loop = R.Reactive(b, quiet_feed(b, 20), baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        out = loop.control(5.0)
        self.assertTrue(out["ok"])
        self.assertEqual(out["outcome"], "timeout")

    def test_a_stuck_detector_refuses_to_arm(self):
        c = Clock()
        b = ScriptedBridge(noisy=True)
        loop = R.Reactive(b, quiet_feed(b, 20), baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        out = loop.control(5.0)
        self.assertFalse(out["ok"])
        self.assertEqual(out["outcome"], "fired")
        self.assertIn("stuck on", out["why"])

    def test_control_flushes_before_it_judges(self):
        # Otherwise it scores history from calibration, not the control.
        c = Clock()
        b = ScriptedBridge(noisy=False)
        feed = quiet_feed(b, 20)
        loop = R.Reactive(b, feed, baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        loop.control(1.0)
        self.assertLess(len(feed.samples), 20)


class TestReaction(unittest.TestCase):
    def test_settle_waits_for_quiet_before_performing(self):
        c = Clock()
        b = ScriptedBridge(noisy=False)
        loop = R.Reactive(b, quiet_feed(b, 20), baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        rec = loop.react()
        self.assertEqual(rec["settle"], "quiet")
        self.assertGreaterEqual(rec["settle_s"], R.SETTLE_QUIET_S)
        self.assertTrue(rec["beat_ok"])

    def test_a_hand_that_never_leaves_still_gets_an_answer(self):
        # Settle must be capped. A sustained hold would otherwise mean R2
        # simply never responds to being held, which reads as broken.
        c = Clock()
        b = ScriptedBridge(noisy=True)
        loop = R.Reactive(b, quiet_feed(b, 20), baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        rec = loop.react()
        self.assertEqual(rec["settle"], "timeout")
        self.assertLessEqual(rec["settle_s"], R.SETTLE_MAX_S + R.POLL_S)
        self.assertTrue(rec["beat_ok"])

    def test_the_beat_is_express_curious(self):
        c = Clock()
        b = ScriptedBridge(noisy=False)
        loop = R.Reactive(b, quiet_feed(b, 20), baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        rec = loop.react()
        self.assertEqual(rec["beat"], "express_curious")
        self.assertIn("dome", [s.op for s in b.sent])

    def test_the_beats_own_noise_is_flushed_before_recovery(self):
        """Hazard #1 in the module docstring: the beat turns the dome, the
        dome disturbs every channel the detector watches, and a loop that
        re-arms on that aftermath answers itself forever.

        The previous version of this test asserted on a helper that read
        `loop.reactions[-1]` -- but `react()` never appends to `reactions`,
        only `run()` does, so the helper returned its own literal default and
        the assertion was `"quiet" == "quiet"` with the code disconnected.
        Deleting `self.feed.flush()` survived the entire suite."""
        c = Clock()
        b = ScriptedBridge(noisy=False)
        feed = quiet_feed(b, 20)
        loop = R.Reactive(b, feed, baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        before = feed.flushes
        rec = loop.react()                      # assert on react()'s OWN result
        self.assertEqual(feed.flushes - before, 1)
        self.assertEqual(rec["recover"], "quiet")
        # Behavioural corollary: recovery can only have scored samples that
        # arrived AFTER the beat. 20 pre-beat samples went in; if any survive,
        # the flush did not happen.
        self.assertLess(len(feed.samples), 20)

    def test_a_low_ceiling_refuses_the_beat_without_crashing(self):
        c = Clock()
        b = ScriptedBridge(noisy=False)
        loop = R.Reactive(b, quiet_feed(b, 20), baseline_thresholds(), 6,
                          ceiling="read", now=c.now, sleep=c.sleep)
        rec = loop.react()
        self.assertTrue(rec["refused"])
        self.assertFalse(rec["beat_ok"])
        self.assertNotIn("dome", [s.op for s in b.sent])


class TestCooldown(unittest.TestCase):
    """`cooldown_s` is one of the four fields the Behavior Library requires so
    R2 can be scheduled without becoming twitchy. A loop that ignores it
    re-fires as fast as the sensor allows."""

    def _loop(self, clock, bridge, **kw):
        return R.Reactive(bridge, quiet_feed(bridge, 20), baseline_thresholds(),
                          6, now=clock.now, sleep=clock.sleep, **kw)

    def test_the_declared_cooldown_is_held_after_the_beat(self):
        c = Clock()
        b = ScriptedBridge(noisy=False)
        rec = self._loop(c, b).react()
        self.assertGreater(rec["cooldown_held_s"], 0.0)

    def test_recovery_time_counts_toward_the_cooldown(self):
        # Not additive. Recovery already kept him silent; charging the full
        # cooldown on top would stack two unrelated waits and make him feel
        # dead for twice as long as either gate intends.
        c = Clock()
        b = ScriptedBridge(noisy=False)
        loop = self._loop(c, b)
        t0 = c.now()
        rec = loop.react()
        spent = c.now() - t0
        beat = B.express_curious()
        self.assertLess(rec["cooldown_held_s"], beat.cooldown_s)
        self.assertGreaterEqual(spent, beat.cooldown_s)

    def test_a_beat_with_no_cooldown_holds_nothing(self):
        c = Clock()
        b = ScriptedBridge(noisy=False)

        def eager():
            beat = B.express_curious()
            object.__setattr__(beat, "cooldown_s", 0.0)
            return beat

        rec = self._loop(c, b, beat_factory=eager).react()
        self.assertEqual(rec["cooldown_held_s"], 0.0)


class TestStalledStream(unittest.TestCase):
    """A dead stream is the one failure the design cannot see by itself: the
    buffer keeps its last full window, every poll re-scores those same
    samples, and "quiet" comes back forever."""

    class DeadBridge(ScriptedBridge):
        def _reply(self, step):
            if step.op == "events":
                return {"ok": True, "op": "events",
                        "data": {"events": [], "count": 0, "dropped": 0}}
            return super()._reply(step)

    def test_a_dead_stream_is_detected_not_read_as_calm(self):
        b = self.DeadBridge()
        feed = R.Feed(b, MASK, EXT)
        self.assertFalse(feed.stalled(12))
        for _ in range(12):
            feed.drain()
        self.assertTrue(feed.stalled(12))

    def test_it_refuses_to_perform_on_stale_data(self):
        """settle returning "quiet" on a stalled feed is not a measurement.
        Moving the robot on it is acting on data that stopped arriving."""
        c = Clock()
        b = ScriptedBridge(noisy=False)
        feed = quiet_feed(b, 20)
        loop = R.Reactive(b, feed, baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        b.__class__ = self.DeadBridge          # the stream dies right here
        rec = loop.react()
        self.assertTrue(rec["stalled"])
        self.assertTrue(rec["refused"])
        self.assertNotIn("dome", [s.op for s in b.sent])

    def test_settle_never_declares_quiet_on_a_dead_stream(self):
        """The pre-perform refusal is the second line of defence. This is the
        first: a full window that has STOPPED being refreshed is not evidence
        of calm, and re-scoring the same six samples every 250 ms produces
        "quiet" forever."""
        c = Clock()
        b = ScriptedBridge(noisy=False)
        feed = quiet_feed(b, 20)          # a full, quiet window already held
        loop = R.Reactive(b, feed, baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        b.__class__ = self.DeadBridge     # ...and now nothing more arrives
        outcome, _ = loop._watch_until(c.now() + 10.0,
                                       want_quiet_s=R.SETTLE_QUIET_S)
        self.assertEqual(outcome, "timeout")

    def test_a_flush_clears_the_stall_counter(self):
        b = self.DeadBridge()
        feed = R.Feed(b, MASK, EXT)
        for _ in range(12):
            feed.drain()
        feed.flush()
        self.assertFalse(feed.stalled(12))


class TestCorroboration(unittest.TestCase):
    """CLAUDE.md: single-channel firing meant one encoder blip could trip it,
    and did -- r2_head_angle produced a lone false positive in 1 of 4 no-touch
    controls. Nothing in any unittest suite covered the rule; changing
    CHANNELS_TO_CORROBORATE from 2 to 1 survived all four suites."""

    def _one_channel_window(self, n=6):
        import struct
        out = []
        for i in range(n):
            v = {k: 0.001 * (i % 2) for k in ORDER}
            v[("gyroscope", "x")] = 5.0 if i % 2 else -5.0   # exactly ONE
            out.append({"t": float(i), "raw": "",
                        "decoded": P.decode_sensor_stream(
                            bytes.fromhex(payload(v)), MASK, EXT)})
        return out

    def test_one_channel_alone_does_not_fire(self):
        hot, _ = R.fires(self._one_channel_window(), baseline_thresholds())
        self.assertFalse(hot)

    def test_but_it_does_show_up_in_the_ratios(self):
        # It must be visible as evidence even though it does not fire --
        # otherwise `monitor` could not tell a near-miss from silence.
        rs = R.ratios(self._one_channel_window(), baseline_thresholds())
        self.assertGreaterEqual(max(rs.values()), 1.0)


class TestSelfTrigger(unittest.TestCase):
    """The failure this loop exists to avoid: R2 reacting to himself, forever."""

    def test_recovery_waits_out_the_dome_it_just_turned(self):
        c = Clock()
        b = ScriptedBridge(noisy=True)      # everything reads as disturbed
        loop = R.Reactive(b, quiet_feed(b, 20), baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        rec = loop.react()
        # Noise never stops, so recovery must hit its cap rather than declare
        # quiet -- and must NOT loop forever waiting.
        self.assertEqual(rec["recover"], "timeout")

    def test_one_pet_produces_exactly_one_reaction(self):
        c = Clock()
        b = ScriptedBridge(noisy=True)
        feed = quiet_feed(b, 20)
        loop = R.Reactive(b, feed, baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)

        # The hand lifts the moment the first reaction begins; everything
        # after that is R2's own aftermath, which must not re-arm him.
        original = loop.react

        def react_then_calm():
            out = original()
            b.noisy = False
            return out

        loop.react = react_then_calm
        out = loop.run(max_s=90.0, max_reactions=4)
        self.assertEqual(out["count"], 1)
        self.assertEqual(out["stopped"], "time")


class TestRunBounds(unittest.TestCase):
    def test_it_stops_at_max_reactions(self):
        c = Clock()
        b = ScriptedBridge(noisy=True)
        loop = R.Reactive(b, quiet_feed(b, 20), baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        out = loop.run(max_s=10_000.0, max_reactions=2)
        self.assertEqual(out["count"], 2)
        self.assertEqual(out["stopped"], "max_reactions")

    def test_it_stops_at_the_time_bound_with_nothing_happening(self):
        c = Clock()
        b = ScriptedBridge(noisy=False)
        loop = R.Reactive(b, quiet_feed(b, 20), baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        out = loop.run(max_s=20.0, max_reactions=5)
        self.assertEqual(out["count"], 0)
        self.assertEqual(out["stopped"], "time")
        self.assertLessEqual(c.now(), 20.0 + R.POLL_S)

    def test_arming_shows_the_listen_state_front_and_back(self):
        """Nothing that needs a glance may live only on my console -- the
        operator's hand is the input device and they are across the room.

        Cyan, not blue: docs/behaviour-states.md gives blue steady to *idle,
        nothing engaged*, which is the opposite of what an armed loop is
        doing. Both PSIs, because that is what the `listen` row specifies."""
        c = Clock()
        b = ScriptedBridge(noisy=False)
        loop = R.Reactive(b, quiet_feed(b, 20), baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        mark = len(b.batches)          # quiet_feed already sent events
        loop.run(max_s=1.0, max_reactions=1)
        first = b.batches[mark][0]
        self.assertEqual(first.op, "leds")
        self.assertEqual(first.params["channels"],
                         {**B.front(B.BASE_ENGAGED), **B.back(B.BASE_ENGAGED)})

    def test_arming_is_not_the_colour_the_session_ends_on(self):
        """The whole point of D-014: a signal defined only by its destination
        is a no-op whenever the destination is already current. `_tidy` ends
        on BASE_NEUTRAL, so arming must not BE BASE_NEUTRAL."""
        self.assertNotEqual(B.BASE_ENGAGED, B.BASE_NEUTRAL)
        c = Clock()
        b = ScriptedBridge(noisy=False)
        loop = R.Reactive(b, quiet_feed(b, 20), baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        mark = len(b.batches)
        loop.run(max_s=1.0, max_reactions=1)
        armed = b.batches[mark][0].params["channels"]
        self.assertNotEqual(armed, B.front(B.BASE_NEUTRAL))

    def test_dropped_events_and_decode_errors_are_reported(self):
        c = Clock()
        b = ScriptedBridge(noisy=False)
        b.dropped = 1
        loop = R.Reactive(b, quiet_feed(b, 5), baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        out = loop.run(max_s=2.0, max_reactions=1)
        self.assertGreater(out["dropped_events"], 0)


class TestCalibration(unittest.TestCase):
    def test_rest_yields_a_limit_for_every_channel(self):
        c = Clock()
        b = ScriptedBridge(noisy=False)
        cal = R.calibrate(b, R.Feed(b, MASK, EXT), 30.0, 6,
                          now=c.now, sleep=c.sleep)
        self.assertEqual(cal["thresholds"], len(ORDER))
        self.assertGreater(cal["samples"], 12)

    def test_too_short_a_baseline_yields_no_thresholds(self):
        # empirical_thresholds needs 2*window_n samples per channel. Fewer
        # means no limit at all, which run_session refuses to arm on rather
        # than arming with a detector that can never fire.
        c = Clock()
        b = ScriptedBridge(noisy=False)
        cal = R.calibrate(b, R.Feed(b, MASK, EXT), 1.0, 6,
                          now=c.now, sleep=c.sleep)
        self.assertEqual(cal["thresholds"], 0)


class TestSessionWiring(unittest.TestCase):
    """Whole-session integration. Every bug that reached live hardware in this
    feature lived HERE -- in how the phases were wired together -- and not one
    of them was visible to a unit test of the pieces."""

    def _session(self, bridge, **over):
        import argparse
        args = argparse.Namespace(baseline=30.0, control=20.0, max_s=5.0,
                                  max_reactions=1, ceiling="dome", window=1.5)
        for k, v in over.items():
            setattr(args, k, v)
        import tempfile
        c = Clock()
        real, R.FileBridge = R.FileBridge, lambda *a, **k: bridge
        # Redirect the run log too. Without this the suite writes real files
        # into the project's live .bridge/ directory -- six of them per run,
        # interleaved with genuine session logs and indistinguishable from
        # them by name. Tests must not litter the runtime directory they are
        # testing.
        real_dir = R.LOG_DIR
        with tempfile.TemporaryDirectory() as d:
            R.LOG_DIR = Path(d)
            try:
                return R.run_session(args, now=c.now, sleep=c.sleep)
            finally:
                R.FileBridge, R.LOG_DIR = real, real_dir

    def test_the_arm_cue_is_a_visible_edge_not_a_colour(self):
        """The bug that cost a live run. He was armed blue while ALREADY blue
        -- the previous session's own teardown had left him there -- so the
        one signal saying 'now, pet him' was identical to the state before
        it. The operator waited for a cue that had already happened."""
        b = SessionBridge(noisy=False)
        self.assertEqual(self._session(b), 0)
        leds = [s.params["channels"] for s in b.sent if s.op == "leds"]
        dark, blue = B.front((0, 0, 0)), B.front(B.BASE_NEUTRAL)
        self.assertEqual(leds[0], dark)
        self.assertIn(blue, leds)
        self.assertLess(leds.index(dark), leds.index(blue))

    def test_it_turns_off_the_stream_it_turned_on(self):
        b = SessionBridge(noisy=False)
        self._session(b)
        flags = [s.params.get("enable") for s in b.sent if s.op == "sensors"]
        self.assertEqual(flags, [True, False])

    def test_it_ends_on_a_defined_colour(self):
        # Whatever a session leaves lit is what the household sees until
        # something changes it.
        b = SessionBridge(noisy=False)
        self._session(b)
        self.assertEqual(b.sent[-1].op, "leds")
        self.assertEqual(b.sent[-1].params["channels"],
                         B.front(B.BASE_NEUTRAL))

    def test_no_daemon_is_reported_before_anything_else_happens(self):
        b = SessionBridge(noisy=False)
        b.lock = None
        self.assertEqual(self._session(b), 2)
        self.assertEqual(b.sent, [])

    # Calibration (30s) and the control (20s) are ~200 polls at 0.25s. Going
    # noisy after that lands the disturbance in the ARMED phase, which is the
    # only way a session-level test reaches the beat at all.
    NOISY_ONCE_ARMED = 210

    def test_a_disturbance_while_armed_performs_the_beat(self):
        b = SessionBridge(noisy=False, go_noisy_after=self.NOISY_ONCE_ARMED,
                          quiet_again_after=self.NOISY_ONCE_ARMED + 8)
        self.assertEqual(self._session(b), 0)
        self.assertIn("dome", [s.op for s in b.sent])

    def test_a_detector_that_drifts_loud_fails_the_closing_control(self):
        """The opening control proves it was honest when it armed. Only the
        closing one proves it still was when it stopped -- and thresholds are
        frozen from a baseline taken in a pose the dome has since walked away
        from, so drift is the expected failure."""
        b = SessionBridge(noisy=False, go_noisy_after=self.NOISY_ONCE_ARMED)
        self.assertEqual(self._session(b), 7)

    def test_a_ceiling_too_low_is_refused_before_the_operator_waits(self):
        """`perform` refuses on tier, but only after calibration and the
        control have both run -- 50 s of the operator standing still to learn
        the daemon was started wrong. And at `read` the LED writes are
        refused too, so the arming cue silently never happens."""
        b = SessionBridge(noisy=False)
        b.lock = {"pid": 1, "ceiling": "leds", "started": 0}
        self.assertEqual(self._session(b), 6)
        self.assertEqual(b.sent, [])          # nothing spent, nothing moved

    def test_the_daemons_ceiling_overrides_the_flag_before_arming(self):
        """The flag is the operator's memory of how they launched it; the lock
        is what they actually launched. This test only means anything if a
        reaction actually FIRES -- the first version asserted 'no dome op' on
        a session where nothing ever triggered, so it passed whatever the
        ceiling logic did."""
        b = SessionBridge(noisy=False, go_noisy_after=self.NOISY_ONCE_ARMED)
        b.lock = {"pid": 1, "ceiling": "read", "started": 0}
        # The flag says dome; the lock says read. The lock wins, and the
        # pre-flight now refuses on it instead of discovering it mid-beat.
        self.assertEqual(self._session(b, ceiling="dome"), 6)
        self.assertNotIn("dome", [s.op for s in b.sent])

    def test_a_stuck_detector_refuses_the_whole_session(self):
        b = SessionBridge(noisy=False)
        b.go_noisy_after = 125         # quiet through calibration, hot after
        self.assertEqual(self._session(b), 5)
        self.assertNotIn("dome", [s.op for s in b.sent])
        # It must still tidy up after refusing.
        self.assertIn(False, [s.params.get("enable")
                              for s in b.sent if s.op == "sensors"])


class TestTeardownAlwaysRuns(unittest.TestCase):
    """Every exit path must put R2 back. The setup used to sit OUTSIDE the
    try, so the one path that returned early skipped the teardown entirely."""

    def _session(self, bridge, log_dir=None):
        import argparse
        import tempfile
        args = argparse.Namespace(baseline=30.0, control=20.0, max_s=5.0,
                                  max_reactions=1, ceiling="dome", window=1.5)
        c = Clock()
        real, R.FileBridge = R.FileBridge, lambda *a, **k: bridge
        real_dir = R.LOG_DIR
        with tempfile.TemporaryDirectory() as d:
            R.LOG_DIR = Path(log_dir) if log_dir else Path(d)
            try:
                return R.run_session(args, now=c.now, sleep=c.sleep)
            finally:
                R.FileBridge, R.LOG_DIR = real, real_dir

    def test_a_failed_stream_enable_still_tidies_up(self):
        """`ok: False` includes "no response", and a lost reply is not
        evidence the packet missed -- the stream may be ON. Returning without
        attempting the disable leaks it, and leaves R2 parked in the DARK
        that the arm cue uses as a transient."""
        b = SessionBridge(noisy=False, fail_stream_enable=True)
        self.assertEqual(self._session(b), 3)
        flags = [s.params.get("enable") for s in b.sent if s.op == "sensors"]
        self.assertIn(False, flags)              # disable was attempted
        self.assertEqual(b.sent[-1].op, "leds")
        self.assertEqual(b.sent[-1].params["channels"],
                         B.front(B.BASE_NEUTRAL))   # NOT left dark

    def test_a_refusal_to_arm_still_tidies_up(self):
        b = SessionBridge(noisy=False, go_noisy_after=125)
        self.assertEqual(self._session(b), 5)
        self.assertIn(False, [s.params.get("enable")
                              for s in b.sent if s.op == "sensors"])
        self.assertEqual(b.sent[-1].params["channels"],
                         B.front(B.BASE_NEUTRAL))

    def test_the_teardown_stops_him_first(self):
        """"Default to STOP. Disconnect or failure must result in stop, not
        last-command." Turning the sensor stream off and setting a colour are
        not stopping: if a session dies between a dome step and its settle,
        nothing else in the teardown halts him. `stop` is permitted at every
        tier precisely so it is always available here."""
        b = SessionBridge(noisy=False)
        self._session(b)
        ops = [s.op for s in b.sent]
        self.assertIn("stop", ops)
        # First in the teardown batch, before the housekeeping.
        tail = ops[ops.index("stop"):]
        self.assertLess(tail.index("stop"), tail.index("sensors"))
        self.assertLess(tail.index("stop"), tail.index("leds"))

    def test_an_unwritable_log_does_not_cost_the_teardown(self):
        """The log write lives in a `finally`. Unguarded, a full disk raises
        THERE -- masking whatever was already propagating and skipping the
        robot teardown, trading R2's physical state for a logging
        convenience."""
        import tempfile
        b = SessionBridge(noisy=False)
        with tempfile.TemporaryDirectory() as d:
            blocked = Path(d) / "not-a-dir"
            blocked.write_text("this is a file, so mkdir() must fail")
            rc = self._session(b, log_dir=blocked)
        self.assertEqual(rc, 0)                  # the run's own result stands
        self.assertIn(False, [s.params.get("enable")
                              for s in b.sent if s.op == "sensors"])
        self.assertEqual(b.sent[-1].params["channels"],
                         B.front(B.BASE_NEUTRAL))


class TestRemainingGuards(unittest.TestCase):
    """Three guards that survived mutation until an independent review pointed
    at them. Each one is a refusal, and a refusal that no test exercises is
    indistinguishable from a refusal that was deleted."""

    def _session(self, bridge, fn=None, **over):
        import argparse
        import tempfile
        args = argparse.Namespace(baseline=30.0, control=20.0, max_s=5.0,
                                  max_reactions=1, ceiling="dome", window=1.5)
        for k, v in over.items():
            setattr(args, k, v)
        c = Clock()
        real, R.FileBridge = R.FileBridge, lambda *a, **k: bridge
        real_dir = R.LOG_DIR
        with tempfile.TemporaryDirectory() as d:
            R.LOG_DIR = Path(d)
            try:
                return (fn or R.run_session)(args, now=c.now, sleep=c.sleep)
            finally:
                R.FileBridge, R.LOG_DIR = real, real_dir

    def test_too_few_channels_refuses_to_arm(self):
        """Fewer than two channels with a usable limit means the two-channel
        corroboration rule can never be met, so nothing would EVER fire and
        the session would look calm for its whole duration."""
        b = SessionBridge(noisy=False)
        self.assertEqual(self._session(b, baseline=1.0), 4)
        self.assertNotIn("dome", [s.op for s in b.sent])

    def test_the_process_wide_dome_home_is_reset_per_session(self):
        """A fresh session inherits a dome parked wherever yesterday left it.
        Carrying the old anchor over means the drift correction aims at a mark
        that no longer exists."""
        B._DEFAULT_HOME.angle = 999.0
        try:
            self._session(SessionBridge(noisy=False))
            self.assertIsNone(B._DEFAULT_HOME.angle)
        finally:
            B.reset_default_home()

    def test_monitor_moves_nothing_and_tidies_up(self):
        """monitor is what #59 tells the operator to run, and it duplicated
        the whole enable/calibrate/teardown wiring with zero coverage."""
        b = SessionBridge(noisy=False)
        self.assertEqual(self._session(b, fn=R.monitor_session, max_s=20.0), 0)
        self.assertNotIn("dome", [s.op for s in b.sent])
        self.assertNotIn("sound", [s.op for s in b.sent])
        self.assertIn(False, [s.params.get("enable")
                              for s in b.sent if s.op == "sensors"])
        self.assertEqual(b.sent[-1].params["channels"],
                         B.front(B.BASE_NEUTRAL))


class SessionBridge(ScriptedBridge):
    """A ScriptedBridge that also answers the daemon-liveness question."""

    def __init__(self, go_noisy_after: int | None = None,
                 quiet_again_after: int | None = None,
                 fail_stream_enable: bool = False, **kw):
        super().__init__(**kw)
        self.lock = {"pid": 1, "ceiling": "dome", "started": 0}
        self.go_noisy_after = go_noisy_after
        self.quiet_again_after = quiet_again_after
        self.fail_stream_enable = fail_stream_enable
        self.polls = 0

    def daemon(self):
        return self.lock

    def running(self):
        return self.lock is not None

    def _reply(self, step):
        if step.op == "events" and self.go_noisy_after is not None:
            self.polls += 1
            if self.polls > self.go_noisy_after:
                self.noisy = True
            # The hand lifts. Without this the closing control correctly
            # reports a detector that fires on nothing, and every session
            # test would exit 7 instead of reaching its own assertion.
            if (self.quiet_again_after is not None
                    and self.polls > self.quiet_again_after):
                self.noisy = False
        # Fail only the ENABLE. The disable must still be attempted, and must
        # still succeed, or the test cannot tell "teardown ran" from
        # "teardown was skipped".
        if (step.op == "sensors" and self.fail_stream_enable
                and step.params.get("enable")):
            return {"ok": False, "op": "sensors",
                    "error": "no response for 1787000000000000.json"}
        return super()._reply(step)


def run() -> int:
    loaded = unittest.TestLoader().loadTestsFromModule(sys.modules[__name__])
    res = unittest.TextTestRunner(verbosity=2).run(loaded)
    return 0 if res.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(run())
