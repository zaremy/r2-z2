"""The player, dry-tested against synthetic time. No robot, no daemon.

Every test here is a frame-maths question, which is the whole point: the
arithmetic that decides what R2 shows is provable at a desk, and the hardware
window is then spent on the three things that genuinely need him.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import r2_lights as L
import r2_behavior as B


class TestPatternMaths(unittest.TestCase):

    def test_steady_never_changes(self):
        p = L.Pattern("steady", (B.BASE_NEUTRAL,))
        for t in (0.0, 0.5, 17.3, 9999.0):
            self.assertEqual(p.sample(t), B.BASE_NEUTRAL)
        self.assertEqual(p.writes_per_s, 0.0)

    def test_blink_is_dark_for_the_second_half(self):
        p = L.Pattern("blink", (B.BASE_DANGER,), 1.0)
        self.assertEqual(p.sample(0.0), B.BASE_DANGER)
        self.assertEqual(p.sample(0.49), B.BASE_DANGER)
        self.assertIs(p.sample(0.5), L.DARK)
        self.assertIs(p.sample(0.99), L.DARK)
        self.assertEqual(p.sample(1.0), B.BASE_DANGER)      # wraps

    def test_alternate_swaps_between_two_colours_and_is_never_dark(self):
        p = L.Pattern("alternate", (B.BASE_ENGAGED, B.BASE_NEUTRAL), 1.0)
        self.assertEqual(p.sample(0.25), B.BASE_ENGAGED)
        self.assertEqual(p.sample(0.75), B.BASE_NEUTRAL)
        for t in (0.0, 0.3, 0.6, 0.9):
            self.assertIsNot(p.sample(t), L.DARK)

    def test_a_sweep_holds_its_last_member_forever(self):
        # A transition ARRIVES somewhere. A looping sweep would be a state
        # that never settles, which is not what wake means.
        p = L.Pattern("sweep", (B.BASE_NEUTRAL, B.BASE_ENGAGED,
                                B.BASE_SUCCESS), 0.45)
        self.assertEqual(p.sample(0.0), B.BASE_NEUTRAL)
        self.assertEqual(p.sample(0.5), B.BASE_ENGAGED)
        self.assertEqual(p.sample(1.0), B.BASE_SUCCESS)
        self.assertEqual(p.sample(60.0), B.BASE_SUCCESS)

    def test_sweep_period_is_the_dwell_not_the_whole_cycle(self):
        # The bug this pins: `sample` read period_s as the per-member dwell
        # and `transitions` read it as the full cycle, so a wake sweep emitted
        # three instants INSIDE one dwell. All three sampled to the same
        # colour, the dedup collapsed them, and the state rendered as "jump to
        # blue and stay" while reporting success.
        p = L.Pattern("sweep", (B.BASE_NEUTRAL, B.BASE_ENGAGED,
                                B.BASE_SUCCESS), 0.45)
        self.assertEqual(p.transitions(10.0), [0.0, 0.45, 0.9])
        seen = [p.sample(t) for t in p.transitions(10.0)]
        self.assertEqual(len(set(seen)), 3, "each dwell shows a new colour")

    def test_a_two_member_sweep_is_rejected(self):
        with self.assertRaises(ValueError):
            L.Pattern("sweep", (B.BASE_NEUTRAL, B.BASE_ENGAGED), 0.45)

    def test_scale_of_zero_is_rejected_because_that_is_off_not_dim(self):
        with self.assertRaises(ValueError):
            L.Pattern("steady", (B.BASE_NEUTRAL,), scale=0.0)


class TestWriteBudget(unittest.TestCase):

    def test_no_state_exceeds_the_ceiling(self):
        # R2 DROPS commands sent faster than cmd_safe_interval, so a state
        # over budget renders as something other than what it says.
        for name, state in L.STATES.items():
            with self.subTest(state=name):
                self.assertLessEqual(state.writes_per_s, L.MAX_WRITES_PER_S)

    def test_simultaneous_fixtures_cost_one_write_not_two(self):
        # danger blinks front AND back at 0.25 s. Merged that is 8 writes/s
        # and fits; summed per-fixture it is 16 and does not. An over-counting
        # guard would force the blink slower -- making the most urgent signal
        # on the droid less urgent to satisfy an arithmetic error.
        danger = L.STATES["danger"]
        self.assertAlmostEqual(danger.writes_per_s, 8.0, places=2)
        self.assertEqual(danger.front.writes_per_s, 8.0)
        self.assertEqual(danger.back.writes_per_s, 8.0)
        frames = danger.frames(1.0)
        self.assertEqual(len(frames), 8)
        for _, ch in frames:                       # one dict carries both PSIs
            self.assertTrue({"0", "1", "2", "4", "5", "6"} <= set(ch))

    def test_an_over_budget_state_is_rejected_at_construction(self):
        with self.assertRaises(ValueError) as cm:
            L.StateLights(
                name="too_fast",
                front=L.Pattern("blink", (B.BASE_DANGER,), 0.1),
                back=L.Pattern("blink", (B.BASE_SUCCESS,), 0.11),
            ).validate()
        self.assertIn("ceiling", str(cm.exception))

    def test_idle_costs_nothing_because_it_runs_for_hours(self):
        self.assertEqual(L.STATES["idle"].writes_per_s, 0.0)
        self.assertEqual(len(L.STATES["idle"].frames(3600.0)), 1)


class TestStatesMatchTheSpec(unittest.TestCase):

    def test_every_state_validates(self):
        for name, state in L.STATES.items():
            with self.subTest(state=name):
                state.validate()

    def test_no_state_paints_a_non_status_colour(self):
        for name, state in L.STATES.items():
            for label, psi in (("front", state.front), ("back", state.back)):
                for v in psi.values:
                    with self.subTest(state=name, psi=label):
                        self.assertIn(v, B.STATUS_COLOURS)

    def test_a_non_status_colour_is_rejected_at_construction(self):
        # Tests the GUARD, not the table. Checking only that every shipped
        # state uses a legal colour passes just as happily with the guard
        # deleted -- which is exactly what a mutation run showed. The table
        # being correct today is not the property worth pinning; the next
        # state somebody adds is.
        pale = (120, 190, 255)              # the corner-less pale blue
        for label, kwargs in (
                ("front", dict(front=L.Pattern("steady", (pale,)),
                               back=L.Pattern("steady", (B.BASE_NEUTRAL,)))),
                ("back", dict(front=L.Pattern("steady", (B.BASE_NEUTRAL,)),
                              back=L.Pattern("steady", (pale,))))):
            with self.subTest(psi=label):
                with self.assertRaises(ValueError) as cm:
                    L.StateLights(name="bad", **kwargs).validate()
                self.assertIn("status meaning", str(cm.exception))

    def test_a_non_status_colour_is_caught_mid_pattern_too(self):
        # Not just the first member. A sweep or an alternate that passes
        # THROUGH an illegal colour is illegal.
        with self.assertRaises(ValueError):
            L.StateLights(
                name="bad_sweep",
                front=L.Pattern("sweep", (B.BASE_NEUTRAL, (120, 190, 255),
                                          B.BASE_SUCCESS), 0.45),
                back=L.Pattern("steady", (B.BASE_NEUTRAL,)),
            ).validate()

    def test_alternating_on_the_front_needs_a_stated_reason(self):
        # R2 alternates red/blue on the front uncommanded, so a status layer
        # that alternates cannot be told apart from him being himself.
        with self.assertRaises(ValueError):
            L.StateLights(
                name="unjustified",
                front=L.Pattern("alternate",
                                (B.BASE_ENGAGED, B.BASE_NEUTRAL), 0.9),
                back=L.Pattern("steady", (B.BASE_NEUTRAL,)),
            ).validate()
        self.assertTrue(L.STATES["thinking"].note)    # the knowing exception

    def test_wake_sweeps_blue_to_cyan_to_green(self):
        frames = L.STATES["wake"].frames(3.0)
        self.assertEqual(len(frames), 3)
        got = [(ch["0"], ch["1"], ch["2"]) for _, ch in frames]
        self.assertEqual(got, [B.BASE_NEUTRAL, B.BASE_ENGAGED, B.BASE_SUCCESS])
        self.assertEqual([round(t, 2) for t, _ in frames], [0.0, 0.45, 0.9])

    def test_wake_keeps_headroom_under_the_ceiling(self):
        # 0.45 s per step is 6.7 writes/s at peak. 0.36 s would sit exactly ON
        # the 8.3 ceiling, with nothing left for the holo riding alongside.
        peak = 1.0 / L.STATES["wake"].front.period_s * 2
        self.assertLess(peak, L.MAX_WRITES_PER_S)

    def test_danger_is_the_fastest_thing_on_the_droid(self):
        # It cannot be the brightest: red at full is 0.213 relative luminance
        # against yellow's 0.928. Urgency rides on rate or not at all.
        rates = {n: s.writes_per_s for n, s in L.STATES.items()}
        self.assertEqual(max(rates, key=rates.get), "danger")

    def test_red_appears_only_on_danger_and_offline(self):
        red = {n for n, s in L.STATES.items()
               if B.BASE_DANGER in s.front.values + s.back.values}
        self.assertEqual(red, {"danger", "offline"})

    def test_the_two_red_states_are_told_apart_by_rate_alone(self):
        # Both are faults; only one means go and pick him up.
        self.assertEqual(L.STATES["danger"].front.values,
                         L.STATES["offline"].front.values)
        self.assertLess(L.STATES["danger"].front.period_s,
                        L.STATES["offline"].front.period_s)

    def test_sleep_is_declared_blocked(self):
        # The keepalive IS the wake command, so any sleep is undone within
        # three seconds (#38). The row exists so the language is complete and
        # must not read as available.
        self.assertIn("sleep", L.BLOCKED)
        self.assertIn("BLOCKED", L.STATES["sleep"].note)


class TestWaitingIsNotWithholding(unittest.TestCase):
    """OPERATOR RULING: R2 should never decide not to answer. That retired
    `withholding` -- deliberate silence -- and freed frames already the right
    shape for a wait on the backpack."""

    def test_no_state_means_deliberate_refusal(self):
        self.assertNotIn("withholding", L.STATES)
        self.assertIn("waiting", L.STATES)

    def test_waiting_looks_patient_and_thinking_looks_busy(self):
        # Both are waits. The difference has to be visible or the pair is one
        # state with two names: thinking is working on something (holo
        # breathing, logic blinking), waiting is blocked on something (both
        # dark, because he is not working at all).
        waiting, thinking = L.STATES["waiting"], L.STATES["thinking"]
        self.assertIsNone(waiting.holo)
        self.assertIsNone(waiting.logic)
        self.assertIsNotNone(thinking.holo)
        self.assertIsNotNone(thinking.logic)

    def test_waiting_can_hold_for_hours(self):
        # A backpack that never answers is the case this exists for, so the
        # rate has to be sustainable rather than merely legal.
        self.assertLess(L.STATES["waiting"].writes_per_s, 1.0)

    def test_waiting_is_not_restored_on_connect(self):
        # The link is re-derived live on every connect; a remembered wait
        # would assert a block that may have cleared overnight.
        self.assertFalse(L.STATES["waiting"].restorable)
        self.assertEqual(L.resume("waiting"), (L.DEFAULT_STATE, "waiting"))


class TestQuietHours(unittest.TestCase):

    def test_scaling_keeps_the_hue(self):
        # Scaling a saturated corner keeps it saturated: (0,255,0) -> (0,64,0)
        # is still unambiguously green. This is why quiet hours scale VALUE
        # and never shift hue.
        green = L.Pattern("steady", (B.BASE_SUCCESS,))
        state = L.StateLights(name="q", front=green, back=green)
        _, ch = state.frames(1.0, value=0.25)[0]
        self.assertEqual((ch["0"], ch["1"], ch["2"]), (0, 64, 0))

    def test_safety_states_ignore_quiet_hours(self):
        for name in ("danger", "offline"):
            with self.subTest(state=name):
                dim = L.STATES[name].frames(1.0, value=0.1)[0][1]
                full = L.STATES[name].frames(1.0, value=1.0)[0][1]
                self.assertEqual(dim, full)

    def test_per_fixture_scale_is_independent_of_quiet_hours(self):
        # waiting dims only its BACK: a dim steady claim under a front that
        # blinks once every three seconds.
        w = L.STATES["waiting"]
        _, ch = w.frames(1.0)[0]
        self.assertEqual((ch["4"], ch["5"], ch["6"]), (0, 0, 64))
        self.assertEqual((ch["0"], ch["1"], ch["2"]), (0, 0, 255))


class TestFrames(unittest.TestCase):

    def test_no_two_consecutive_frames_are_identical(self):
        # A write that changes nothing still spends budget.
        for name, state in L.STATES.items():
            frames = state.frames(12.0)
            for i in range(1, len(frames)):
                with self.subTest(state=name, frame=i):
                    self.assertNotEqual(frames[i][1], frames[i - 1][1])

    def test_every_frame_writes_whole_psis(self):
        # A partial PSI write leaves one channel at its previous value, which
        # is a colour nobody chose.
        for name, state in L.STATES.items():
            for i, (_, ch) in enumerate(state.frames(6.0)):
                with self.subTest(state=name, frame=i):
                    for bits in (("0", "1", "2"), ("4", "5", "6")):
                        self.assertEqual(len([b for b in bits if b in ch]), 3)

    def test_frames_start_at_zero(self):
        for name, state in L.STATES.items():
            with self.subTest(state=name):
                self.assertEqual(state.frames(5.0)[0][0], 0.0)

    def test_every_channel_value_is_a_byte(self):
        for name, state in L.STATES.items():
            for _, ch in state.frames(6.0, value=0.37):
                for bit, v in ch.items():
                    with self.subTest(state=name, bit=bit):
                        self.assertIsInstance(v, int)
                        self.assertTrue(0 <= v <= 255)


class TestWakeAssertion(unittest.TestCase):
    """MEASURED 2026-08-18: a colour survives a link drop and a fresh connect,
    but NOT a sleep cycle. Magenta written at 23:52:47, confirmed on the droid,
    gone 22.1 h later after he had slept -- back to the firmware's own red/blue
    alternation. Nothing re-established it, so the household would have seen
    R2's own idiom every morning whatever the language said."""

    def test_a_fresh_store_shows_idle(self):
        self.assertEqual(L.resume(None), (L.DEFAULT_STATE, None))

    def test_a_pending_issue_is_still_pending_in_the_morning(self):
        # The whole point. attention is a claim about the SYSTEM, and it
        # outlives the session that made it.
        self.assertEqual(L.resume("attention"), ("attention", None))

    def test_interaction_states_are_not_restored(self):
        # No exchange survives a disconnect, so restoring one asserts a
        # conversation that is not happening.
        for name in ("listen", "thinking", "misheard", "waiting", "wake"):
            with self.subTest(state=name):
                shown, dropped = L.resume(name)
                self.assertEqual(shown, L.DEFAULT_STATE)
                self.assertEqual(dropped, name)

    def test_live_conditions_are_not_restored_but_ARE_reported(self):
        # Silently asserting a stale danger and silently clearing one are both
        # wrong. Handing it back lets the caller re-derive it.
        for name in ("danger", "offline"):
            with self.subTest(state=name):
                shown, dropped = L.resume(name)
                self.assertEqual(shown, L.DEFAULT_STATE)
                self.assertEqual(dropped, name,
                                 "a dropped claim must be reported, not lost")

    def test_a_corrupt_store_does_not_raise(self):
        # A wake path that crashes on a bad name leaves R2 showing the
        # firmware default -- the exact outcome this code exists to prevent.
        for junk in ("", "nonsense", "IDLE", "../../etc/passwd"):
            with self.subTest(value=junk):
                shown, _ = L.resume(junk)
                self.assertIn(shown, L.STATES)

    def test_restoring_is_the_exception_not_the_rule(self):
        restorable = {n for n, s in L.STATES.items() if s.restorable}
        self.assertEqual(restorable, {"idle", "attention"})

    def test_the_assertion_writes_every_bit(self):
        # Emitting only the channels a state mentions leaves the rest holding
        # whatever was there before, which is what an assertion exists to
        # stop. Asserting idle would otherwise leave yesterday's holo lit.
        want = {"0", "1", "2", "3", "4", "5", "6", "7"}
        for name in L.STATES:
            with self.subTest(state=name):
                self.assertEqual(set(L.assertion(name)), want)

    def test_asserting_idle_turns_the_holo_off(self):
        self.assertEqual(L.assertion("idle")["7"], 0)
        self.assertEqual(L.assertion("idle")["3"], 0)
        self.assertGreater(L.assertion("listen")["7"], 0)   # control

    def test_a_blinking_state_asserts_on_its_LIT_half(self):
        # Frame zero, not an arbitrary phase: a state that asserted itself on
        # its dark half would read as "off" until the first tick.
        for name in ("attention",):
            with self.subTest(state=name):
                ch = L.assertion(name)
                self.assertNotEqual((ch["0"], ch["1"], ch["2"]), (0, 0, 0))

    def test_the_assertion_is_a_status_colour(self):
        for name in L.STATES:
            with self.subTest(state=name):
                ch = L.assertion(name)
                rgb = (ch["0"], ch["1"], ch["2"])
                scaled = any(s.front.scale != 1.0
                             for s in [L.STATES[name]])
                if not scaled:
                    self.assertIn(rgb, B.STATUS_COLOURS)


if __name__ == "__main__":
    unittest.main(verbosity=2)
