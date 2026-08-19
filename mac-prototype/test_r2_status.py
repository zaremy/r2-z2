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
        st = _layer()
        st.connect()
        self.assertEqual(len(st.bridge.batches), 1,
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
