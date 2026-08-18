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
        c = Clock()
        b = ScriptedBridge(noisy=False)
        feed = quiet_feed(b, 20)
        loop = R.Reactive(b, feed, baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        loop.react()
        self.assertEqual(rec_recover(loop), "quiet")

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


def rec_recover(loop):
    return loop.reactions[-1]["recover"] if loop.reactions else "quiet"


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

    def test_arming_lights_him_blue_so_the_state_is_glanceable(self):
        # Nothing that needs a glance may live only on my console -- the
        # operator's hand is the input device and they are across the room.
        c = Clock()
        b = ScriptedBridge(noisy=False)
        loop = R.Reactive(b, quiet_feed(b, 20), baseline_thresholds(), 6,
                          now=c.now, sleep=c.sleep)
        mark = len(b.batches)          # quiet_feed already sent events
        loop.run(max_s=1.0, max_reactions=1)
        first = b.batches[mark][0]
        self.assertEqual(first.op, "leds")
        self.assertEqual(first.params["channels"], B.front(B.BASE_NEUTRAL))

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
        b = SessionBridge(noisy=False, go_noisy_after=self.NOISY_ONCE_ARMED)
        self.assertEqual(self._session(b), 0)
        self.assertIn("dome", [s.op for s in b.sent])

    def test_the_daemons_ceiling_overrides_the_flag(self):
        """The flag is the operator's memory of how they launched it; the lock
        is what they actually launched. This test only means anything if a
        reaction actually FIRES -- the first version asserted 'no dome op' on
        a session where nothing ever triggered, so it passed whatever the
        ceiling logic did."""
        b = SessionBridge(noisy=False, go_noisy_after=self.NOISY_ONCE_ARMED)
        b.lock = {"pid": 1, "ceiling": "read", "started": 0}
        self._session(b, ceiling="dome")
        self.assertNotIn("dome", [s.op for s in b.sent])

    def test_a_stuck_detector_refuses_the_whole_session(self):
        b = SessionBridge(noisy=False)
        b.go_noisy_after = 125         # quiet through calibration, hot after
        self.assertEqual(self._session(b), 5)
        self.assertNotIn("dome", [s.op for s in b.sent])
        # It must still tidy up after refusing.
        self.assertIn(False, [s.params.get("enable")
                              for s in b.sent if s.op == "sensors"])


class SessionBridge(ScriptedBridge):
    """A ScriptedBridge that also answers the daemon-liveness question."""

    def __init__(self, go_noisy_after: int | None = None, **kw):
        super().__init__(**kw)
        self.lock = {"pid": 1, "ceiling": "dome", "started": 0}
        self.go_noisy_after = go_noisy_after
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
        return super()._reply(step)


def run() -> int:
    loaded = unittest.TestLoader().loadTestsFromModule(sys.modules[__name__])
    res = unittest.TextTestRunner(verbosity=2).run(loaded)
    return 0 if res.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(run())
