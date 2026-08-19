"""The status layer, on synthetic time. No robot, no daemon, no waiting."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import r2_status as S
import r2_lights as LG
import r2_behavior as B
import r2_probe as P


class FakeBridge(B.Bridge):
    def __init__(self, fail_after: int | None = None):
        self.batches = []
        self.fail_after = fail_after

    def _send(self, steps, timeout):
        self.batches.append(steps)
        if self.fail_after is not None and len(self.batches) > self.fail_after:
            return [{"ok": False, "error": "synthetic"}]
        return [{"ok": True, "op": s.op} for s in steps]

    def running(self):
        return True

    @property
    def channels(self):
        """Every channels dict written, in order."""
        return [s.params["channels"] for b in self.batches for s in b
                if s.op == "leds"]


def _layer(**kw):
    d = Path(tempfile.mkdtemp())
    return S.StatusLayer(FakeBridge(), S.StatusStore(d / "status.json"), **kw)


class TestTheConstantCannotDrift(unittest.TestCase):

    def test_the_rate_ceiling_agrees_across_modules(self):
        # Same measured hardware property, two names, two files. If they ever
        # disagree there is a rate ceiling nobody is enforcing.
        self.assertEqual(LG.CMD_SAFE_INTERVAL_S, P.CMD_SAFE_INTERVAL)
        self.assertEqual(S.SAFE_INTERVAL_S, P.CMD_SAFE_INTERVAL)


class TestConnectAsserts(unittest.TestCase):

    def test_a_fresh_droid_is_asserted_to_idle(self):
        st = _layer()
        self.assertEqual(st.connect(), ("idle", None))
        self.assertEqual(st.bridge.channels[0], LG.assertion("idle"))

    def test_connect_writes_before_anything_else_can(self):
        # The invariant is that the assertion is FIRST, not that it is alone:
        # a chirp and an optional greeting follow it, each in its own batch.
        # What must never happen is something reaching the droid before the
        # status it is supposed to be in.
        st = _layer()
        st.connect()
        self.assertTrue(st.bridge.batches)
        self.assertEqual([s.op for s in st.bridge.batches[0]], ["leds"],
                         "the assertion must be the first thing sent")

    def test_a_pending_issue_survives_the_night(self):
        d = Path(tempfile.mkdtemp())
        store = S.StatusStore(d / "s.json")
        store.write("attention")
        st = S.StatusLayer(FakeBridge(), store)
        self.assertEqual(st.connect(), ("attention", None))
        self.assertEqual(st.bridge.channels[0], LG.assertion("attention"))

    def test_a_stale_danger_is_dropped_AND_reported(self):
        d = Path(tempfile.mkdtemp())
        store = S.StatusStore(d / "s.json")
        store.write("danger")
        st = S.StatusLayer(FakeBridge(), store)
        shown, dropped = st.connect()
        self.assertEqual(shown, "idle")
        self.assertEqual(dropped, "danger",
                         "silently clearing a danger claim is as wrong as "
                         "silently asserting a stale one")

    def test_connect_persists_what_it_asserted(self):
        d = Path(tempfile.mkdtemp())
        store = S.StatusStore(d / "s.json")
        store.write("danger")                 # non-restorable
        S.StatusLayer(FakeBridge(), store).connect()
        self.assertEqual(store.read(), "idle",
                         "the store must not keep re-dropping the same claim")


class TestStore(unittest.TestCase):

    def test_a_corrupt_store_reads_as_no_memory(self):
        d = Path(tempfile.mkdtemp())
        p = d / "s.json"
        for junk in ("", "{", "[]", '"idle"', '{"state": 7}', "null"):
            p.write_text(junk)
            with self.subTest(content=junk):
                self.assertIsNone(S.StatusStore(p).read())

    def test_a_missing_store_reads_as_no_memory(self):
        self.assertIsNone(S.StatusStore(Path("/nonexistent/x.json")).read())

    def test_writing_an_unknown_state_is_refused(self):
        d = Path(tempfile.mkdtemp())
        with self.assertRaises(KeyError):
            S.StatusStore(d / "s.json").write("nonsense")

    def test_the_write_is_atomic(self):
        # A half-written store must read as no memory, never as a partial
        # name. No memory is safe; a partial name might not be.
        d = Path(tempfile.mkdtemp())
        store = S.StatusStore(d / "s.json")
        store.write("attention")
        self.assertEqual(json.loads(store.path.read_text())["state"],
                         "attention")
        self.assertFalse(list(d.glob("*.tmp")), "temp file left behind")


class TestSet(unittest.TestCase):

    def test_the_store_is_written_before_the_hardware(self):
        # If the send fails or the process dies mid-render, the store must
        # already say what we intended, so the next connect asserts it.
        d = Path(tempfile.mkdtemp())
        store = S.StatusStore(d / "s.json")
        seen = []

        class Watching(FakeBridge):
            def _send(self, steps, timeout):
                seen.append(store.read())
                return super()._send(steps, timeout)

        st = S.StatusLayer(Watching(), store)
        st.connect()
        seen.clear()
        st.set("attention")
        self.assertEqual(seen, ["attention"])

    def test_a_blocked_state_cannot_be_entered(self):
        st = _layer()
        st.connect()
        with self.assertRaises(ValueError) as cm:
            st.set("sleep")
        self.assertIn("BLOCKED", str(cm.exception))

    def test_an_unknown_state_is_refused(self):
        st = _layer()
        with self.assertRaises(KeyError):
            st.set("nonsense")


class TestConnectChirp(unittest.TestCase):

    @staticmethod
    def _sounds(bridge):
        return [s for b in bridge.batches for s in b if s.op == "sound"]

    def test_connect_chirps(self):
        st = _layer()
        st.connect()
        sounds = self._sounds(st.bridge)
        self.assertEqual(len(sounds), 1)
        self.assertEqual(sounds[0].params["id"], S.CONNECT_CHIRP)
        self.assertEqual(sounds[0].params["volume"], S.CHIRP_VOLUME)

    def test_the_chirp_is_its_OWN_batch_after_the_assertion(self):
        # The chirp needs `audio`, the assertion needs only `leds`. Bundled,
        # a daemon at --allow leds could refuse the batch and cost us the
        # assertion -- the louder, less important half taking the quieter,
        # more important half down with it.
        st = _layer()
        st.connect()
        led_batch = next(i for i, b in enumerate(st.bridge.batches)
                         if any(s.op == "leds" for s in b))
        snd_batch = next(i for i, b in enumerate(st.bridge.batches)
                         if any(s.op == "sound" for s in b))
        self.assertLess(led_batch, snd_batch, "assert before you chirp")
        self.assertNotEqual(led_batch, snd_batch, "never the same batch")
        for b in st.bridge.batches:
            ops = {s.op for s in b}
            self.assertNotEqual(ops, {"leds", "sound"})

    def test_a_refused_chirp_does_not_cost_the_assertion(self):
        # fail_after=1: the assertion lands, everything after is refused.
        d = Path(tempfile.mkdtemp())
        st = S.StatusLayer(FakeBridge(fail_after=1),
                           S.StatusStore(d / "s.json"))
        shown, _ = st.connect()
        self.assertEqual(shown, "idle")
        self.assertEqual(st.bridge.channels[0], LG.assertion("idle"))

    def test_quiet_hours_suppress_the_chirp(self):
        # A chirp is the one part of a connect that carries into another room.
        st = _layer(quiet_value=0.25)
        st.connect()
        self.assertEqual(self._sounds(st.bridge), [])

    def test_chirping_in_quiet_hours_must_be_asked_for_explicitly(self):
        st = _layer(quiet_value=0.25, chirp_in_quiet_hours=True)
        st.connect()
        self.assertEqual(len(self._sounds(st.bridge)), 1)

    def test_the_chirp_can_be_turned_off(self):
        st = _layer(chirp=False)
        st.connect()
        self.assertEqual(self._sounds(st.bridge), [])

    def test_the_chirp_is_a_sound_we_have_actually_HEARD(self):
        # S1b rated CHATTY_1 "quick success". R2_STEP_* are the only sounds
        # confirmed short, but were sampled for DURATION only and never rated
        # for character -- short and unknown is worse than short and rated.
        self.assertEqual(S.CONNECT_CHIRP, B._sid("R2_CHATTY_1"))


class TestConnectGreeting(unittest.TestCase):

    @staticmethod
    def _domes(bridge):
        return [s for b in bridge.batches for s in b if s.op == "dome"]

    def test_the_dome_does_NOT_move_by_default(self):
        # Each actuator is individually opt-in and never bundled. Asserting a
        # status is not a moment anybody asked for motion.
        st = _layer()
        st.connect()
        self.assertEqual(self._domes(st.bridge), [])

    def test_the_greeting_goes_out_and_back(self):
        # A single move would displace the dome further on every connect and
        # nothing would ever put it back.
        st = _layer(greet=True)
        st.connect()
        deltas = [s.params["delta"] for s in self._domes(st.bridge)]
        self.assertEqual(deltas, [S.GREET_TRAVEL_DEG, -S.GREET_TRAVEL_DEG])
        self.assertEqual(sum(deltas), 0)

    def test_the_greeting_clears_the_silent_floor(self):
        # Under ~10.5 deg the firmware ignores the command and still reports
        # ok: true, so a smaller nod would be invisible AND look successful.
        self.assertGreaterEqual(S.GREET_TRAVEL_DEG, B.MIN_DOME_TRAVEL_DEG)

    def test_the_greeting_is_expressed_as_travel_not_destination(self):
        # The dome has no resting position, so an angle is meaningless.
        st = _layer(greet=True)
        st.connect()
        for s in self._domes(st.bridge):
            self.assertIn("delta", s.params)
            self.assertNotIn("angle", s.params)

    def test_each_dome_move_is_its_own_batch(self):
        st = _layer(greet=True)
        st.connect()
        for b in st.bridge.batches:
            self.assertLessEqual(len([s for s in b if s.op == "dome"]), 1)

    def test_the_greeting_never_shares_a_batch_with_the_assertion(self):
        # `dome` tier vs `leds` tier: bundled, a refused dome move could cost
        # us the assertion.
        st = _layer(greet=True)
        st.connect()
        for b in st.bridge.batches:
            ops = {s.op for s in b}
            self.assertFalse(
                {"dome", "leds"} <= ops,
                f"batch mixes tiers: {sorted(ops)}")


class TestQuietHours(unittest.TestCase):

    def test_quiet_hours_dim_an_ordinary_state(self):
        st = _layer(quiet_value=0.25)
        st.connect()
        self.assertEqual(st.bridge.channels[0]["2"], 64)     # blue * 0.25

    def test_safety_states_ignore_quiet_hours(self):
        st = _layer(quiet_value=0.1)
        st.connect()
        st.set("offline")
        self.assertEqual(st.bridge.channels[-1], LG.assertion("offline"))


class TestRender(unittest.TestCase):

    def _fake_clock(self):
        t = {"now": 0.0}
        return (lambda s: t.__setitem__("now", t["now"] + s),
                lambda: t["now"])

    def test_render_drives_the_current_state(self):
        st = _layer()
        st.connect()
        st.set("attention")
        sleep, now = self._fake_clock()
        sent = st.render(6.0, sleep=sleep, now=now)
        self.assertEqual(sent, [ch for _, ch in
                                LG.STATES["attention"].frames(6.0)])

    def test_render_paces_to_the_frame_schedule(self):
        st = _layer()
        st.connect()
        st.set("attention")            # 2.4 s blink -> a frame every 1.2 s
        slept, now = [], None
        t = {"now": 0.0}

        def sleep(s):
            slept.append(round(s, 6))
            t["now"] += s

        st.render(6.0, sleep=sleep, now=lambda: t["now"])
        self.assertTrue(all(abs(s - 1.2) < 1e-6 for s in slept), slept)

    def test_render_never_sleeps_a_negative_interval(self):
        # A frame already overdue must be sent immediately, not scheduled in
        # the past.
        st = _layer()
        st.connect()
        st.set("danger")
        slept = []
        st.render(2.0, sleep=slept.append, now=lambda: 1e9)
        self.assertFalse([s for s in slept if s < 0])

    def test_idle_renders_one_frame_for_an_hour(self):
        st = _layer()
        st.connect()
        sent = st.render(3600.0, sleep=lambda s: None, now=lambda: 0.0)
        self.assertEqual(len(sent), 1)


class TestRenderHonoursItsDuration(unittest.TestCase):
    """render(seconds) used to return after the LAST TRANSITION, so a steady
    state returned in 0.0000 s having sent one write. The obvious main loop --
    `while running: layer.render(10)` -- then measured at 368,780 writes/s
    against a ceiling of 8.3."""

    def _clock(self):
        t = {"now": 0.0}
        return (lambda s: t.__setitem__("now", t["now"] + s),
                lambda: t["now"])

    def test_a_steady_state_occupies_the_whole_duration(self):
        st = _layer()
        st.connect()
        sleep, now = self._clock()
        st.render(3600.0, sleep=sleep, now=now)
        self.assertAlmostEqual(now(), 3600.0, places=6)

    def test_a_steady_state_still_costs_only_one_write(self):
        # Occupying the duration must not mean spinning on it.
        st = _layer()
        st.connect()
        sleep, now = self._clock()
        sent = st.render(3600.0, sleep=sleep, now=now)
        self.assertEqual(len(sent), 1)

    def test_an_animated_state_occupies_the_whole_duration_too(self):
        st = _layer()
        st.connect()
        st.set("attention")
        sleep, now = self._clock()
        st.render(6.0, sleep=sleep, now=now)
        self.assertAlmostEqual(now(), 6.0, places=6)

    def test_a_repeated_render_loop_stays_under_the_ceiling(self):
        # The regression that motivated all of this, as an assertion.
        st = _layer()
        st.connect()
        sleep, now = self._clock()
        st.bridge.batches.clear()
        # BOUNDED. The first version looped on `while now() < 60` and, against
        # a render that does not advance the clock, spun forever -- a test
        # that HANGS instead of failing tells you nothing at 3am and blocks
        # the suite. Six calls of 10 s should reach 60 s; anything else is the
        # bug, and it now reports as one.
        for i in range(6):
            st.render(10.0, sleep=sleep, now=now)
            self.assertAlmostEqual(now(), 10.0 * (i + 1), places=6,
                                   msg="render did not occupy its duration")
        writes = len(st.bridge.channels)
        self.assertLessEqual(writes / now(), LG.MAX_WRITES_PER_S,
                             f"{writes} writes in {now()}s blows the ceiling")


class TestQuietHoursReachExpression(unittest.TestCase):

    def test_rest_colour_is_dimmed_during_quiet_hours(self):
        # It returned full brightness, so every beat ended on a bright flash
        # before the re-assert pulled it back down. At 2am.
        st = _layer(quiet_value=0.2)
        st.connect()
        self.assertEqual(st.rest_colour(), (0, 0, 51))

    def test_a_beat_may_rest_on_a_DIMMED_status_colour(self):
        st = _layer(quiet_value=0.2)
        st.connect()
        st.express(B.express_curious, ceiling="dome", sleep=lambda _: None)

    def test_no_frame_of_a_quiet_hours_beat_is_full_brightness(self):
        st = _layer(quiet_value=0.2)
        st.connect()
        st.bridge.batches.clear()
        st.express(B.express_curious, ceiling="dome", sleep=lambda _: None)
        for ch in st.bridge.channels:
            front = (ch.get("0", 0), ch.get("1", 0), ch.get("2", 0))
            self.assertNotEqual(front, B.BASE_NEUTRAL,
                                "full-brightness flash during quiet hours")

    def test_a_dimmed_corner_is_recognised_and_a_muddle_is_not(self):
        self.assertTrue(LG.is_status_colour((0, 0, 51)))       # dimmed blue
        self.assertTrue(LG.is_status_colour((0, 51, 51)))      # dimmed cyan
        self.assertTrue(LG.is_status_colour(B.BASE_DANGER))    # exact corner
        self.assertFalse(LG.is_status_colour((0, 40, 51)))     # lit unequal
        self.assertFalse(LG.is_status_colour((120, 190, 255)))  # the pale blue
        self.assertFalse(LG.is_status_colour((0, 0, 0)))       # off is not a
                                                               # status claim


class TestConnectPersistsBeforeSending(unittest.TestCase):

    def test_the_store_is_written_before_the_hardware_on_connect(self):
        # set() argued for this order and connect() did the opposite. A crash
        # between the two left a dropped claim in the store, to be dropped
        # again on the next connect.
        d = Path(tempfile.mkdtemp())
        store = S.StatusStore(d / "s.json")
        store.write("danger")                 # non-restorable
        seen = []

        class Watching(FakeBridge):
            def _send(self, steps, timeout):
                seen.append(store.read())
                return super()._send(steps, timeout)

        S.StatusLayer(Watching(), store).connect()
        self.assertEqual(seen[0], "idle",
                         "the store must already be correct by the first send")


class TestExpressRidesOnStatus(unittest.TestCase):

    def test_the_beat_rests_on_the_CURRENT_status_not_a_default(self):
        # The wiring. Before it, express_curious() defaulted to blue and every
        # call site accepted it, so a beat run during `attention` painted blue
        # over a pending issue and the issue stopped being pending.
        st = _layer()
        st.connect()
        st.set("attention")
        st.express(B.express_curious, ceiling="dome",
                   sleep=lambda _: None)
        beat = B.express_curious(rest=st.rest_colour())
        self.assertEqual(beat.rest_colour, B.BASE_PENDING)

    def test_rest_colour_follows_status(self):
        st = _layer()
        st.connect()
        self.assertEqual(st.rest_colour(), B.BASE_NEUTRAL)
        st.set("attention")
        self.assertEqual(st.rest_colour(), B.BASE_PENDING)

    def test_the_status_is_reasserted_after_the_beat(self):
        # A beat ends on ONE frame. Run during `attention` it would leave
        # yellow held steady and the blink would never resume -- the claim
        # still visible, its urgency silently gone.
        st = _layer()
        st.connect()
        st.set("attention")
        st.express(B.express_curious, ceiling="dome",
                   sleep=lambda _: None)
        self.assertEqual(st.bridge.channels[-1], LG.assertion("attention"))

    def test_the_status_is_reasserted_even_when_the_beat_FAILS(self):
        # A part-way beat is exactly when status most needs re-establishing.
        d = Path(tempfile.mkdtemp())
        st = S.StatusLayer(FakeBridge(fail_after=2),
                           S.StatusStore(d / "s.json"))
        st.connect()
        st.express(B.express_curious, ceiling="dome",
                   sleep=lambda _: None)
        self.assertEqual(st.bridge.channels[-1], LG.assertion("idle"))

    def test_a_caller_cannot_supply_a_contradicting_rest_colour(self):
        st = _layer()
        st.connect()
        with self.assertRaises(TypeError):
            st.express(B.express_curious, ceiling="dome",
                       sleep=lambda _: None, rest=B.BASE_SUCCESS)


if __name__ == "__main__":
    unittest.main(verbosity=2)
