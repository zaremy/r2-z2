#!/usr/bin/env python3
"""Tests for sensor streaming — issue #29 (S1e).

No hardware, no daemon, no bleak. The decode is the dangerous part: the stream
is a flat array of float32s whose meaning comes ENTIRELY from the mask and the
declaration order of two OrderedDicts. A mis-ordered table produces plausible
numbers under the wrong names, with no error anywhere, and those numbers go
straight into docs/ as measurements.

    python3 -m unittest discover -s mac-prototype -p 'test_*.py' -v
"""

import asyncio
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import r2_probe as P


def pack(*vals) -> bytes:
    return struct.pack(f">{len(vals)}f", *vals)


class TestMasks(unittest.TestCase):

    def test_bits_match_the_source_tables(self):
        # Transcribed from bb9e.py:80-112 and r2d2.py:470-477. If someone
        # "tidies" the tables, this catches it.
        self.assertEqual(P.sensor_mask(["accelerometer"]), 0x8000 | 0x4000 | 0x2000)
        self.assertEqual(P.sensor_mask(["attitude"]), 0x40000 | 0x20000 | 0x10000)
        self.assertEqual(P.ext_sensor_mask(["r2_head_angle"]), 0x4000000)
        self.assertEqual(P.ext_sensor_mask(["gyroscope"]),
                         0x2000000 | 0x1000000 | 0x800000)

    def test_unknown_group_contributes_nothing(self):
        self.assertEqual(P.sensor_mask(["not_a_sensor"]), 0)

    def test_normal_and_extended_bits_may_collide_safely(self):
        # quaternion.x and gyroscope.x are BOTH 0x2000000. They live in
        # different masks, so this is fine — but conflating the two tables
        # would silently swap them.
        self.assertEqual(P.sensor_mask(["quaternion"]) & 0x2000000, 0x2000000)
        self.assertEqual(P.ext_sensor_mask(["gyroscope"]) & 0x2000000, 0x2000000)


class TestDecode(unittest.TestCase):

    def test_accelerometer_only(self):
        m = P.sensor_mask(["accelerometer"])
        out = P.decode_sensor_stream(pack(0.1, -0.2, 9.8), m, 0)
        self.assertAlmostEqual(out["accelerometer"]["x"], 0.1, places=5)
        self.assertAlmostEqual(out["accelerometer"]["z"], 9.8, places=5)

    def test_declaration_order_decides_assignment(self):
        # attitude is declared BEFORE accelerometer in bb9e.py, so its three
        # floats come first regardless of the order the caller named them.
        m = P.sensor_mask(["accelerometer", "attitude"])
        out = P.decode_sensor_stream(pack(1., 2., 3., 4., 5., 6.), m, 0)
        self.assertEqual([out["attitude"]["pitch"], out["attitude"]["roll"],
                          out["attitude"]["yaw"]], [1., 2., 3.])
        self.assertEqual([out["accelerometer"]["x"], out["accelerometer"]["y"],
                          out["accelerometer"]["z"]], [4., 5., 6.])

    def test_normal_sensors_come_before_extended(self):
        m, e = P.sensor_mask(["accelerometer"]), P.ext_sensor_mask(["r2_head_angle"])
        out = P.decode_sensor_stream(pack(1., 2., 3., 42.5), m, e)
        self.assertEqual(out["accelerometer"]["x"], 1.)
        self.assertEqual(out["r2_head_angle"]["r2_head_angle"], 42.5)

    def test_head_angle_precedes_gyro_in_the_extended_table(self):
        e = P.ext_sensor_mask(["r2_head_angle", "gyroscope"])
        out = P.decode_sensor_stream(pack(10., 1., 2., 3.), 0, e)
        self.assertEqual(out["r2_head_angle"]["r2_head_angle"], 10.)
        self.assertEqual(out["gyroscope"]["z"], 3.)

    def test_locator_and_velocity_carry_the_x100_modifier(self):
        m = P.sensor_mask(["locator"])
        out = P.decode_sensor_stream(pack(1.5, -2.0), m, 0)
        self.assertAlmostEqual(out["locator"]["x"], 150.0, places=4)
        self.assertAlmostEqual(out["locator"]["y"], -200.0, places=4)

    def test_length_mismatch_refuses_rather_than_guessing(self):
        # The whole point. A short packet means our mask and the robot's
        # disagree; decoding the first N fields would yield plausible numbers
        # under the wrong names and they would reach docs/ as measurements.
        m = P.sensor_mask(["accelerometer"])
        with self.assertRaises(ValueError) as cm:
            P.decode_sensor_stream(pack(1., 2.), m, 0)      # 2 floats, want 3
        self.assertIn("refusing to guess", str(cm.exception))

    def test_extra_floats_also_refuse(self):
        m = P.sensor_mask(["accelerometer"])
        with self.assertRaises(ValueError):
            P.decode_sensor_stream(pack(1., 2., 3., 4.), m, 0)

    def test_ragged_payload_is_rejected(self):
        with self.assertRaises(ValueError):
            P.decode_sensor_stream(b"\x00\x01\x02", P.sensor_mask(["speed"]), 0)

    def test_empty_masks_decode_an_empty_packet(self):
        self.assertEqual(P.decode_sensor_stream(b"", 0, 0), {})


class TestProbeStepsAreConstructible(unittest.TestCase):
    """The probe drives the bridge through r2_behavior.Step, whose OP_TIER is
    a deliberate WHITELIST. An op missing from it raises only when the step is
    actually sent -- which meant AC1 blew up on live hardware with the
    operator waiting, and the baseline collect loop would have too."""

    def test_every_op_the_probe_sends_is_known_to_step(self):
        import r2_behavior as B
        for op in ("events", "sensors", "head", "dome"):
            with self.subTest(op=op):
                self.assertEqual(B.Step(op, {}).tier(), 
                                 "read" if op != "dome" else "dome")

    def test_sensor_ops_sit_at_read_in_both_modules(self):
        import r2_behavior as B
        for op in ("events", "sensors"):
            self.assertEqual(B.OP_TIER[op], P.op_tier(op))


class TestStreamTeardown(unittest.TestCase):
    """The exit path must leave the stream in a known state.

    Nothing else in the stack does this: spherov2 provides `disable_all`
    (controls/v2.py:326) and never calls it, and there is no destructor or
    disconnect hook anywhere. If the daemon does not turn it off, nobody does.
    """

    def _fake_r2(self):
        class FakeR2:
            def __init__(self):
                self.sensor_streaming = False
                self.sent = []
            async def set_sensor_mask(self, interval, count, mask):
                self.sent.append(("mask", interval, count, mask)); return None
            async def set_extended_sensor_mask(self, mask):
                self.sent.append(("ext", mask)); return None
            async def get_sensor_mask(self):
                return (250, 0, P.sensor_mask(["accelerometer"]))
        return FakeR2()

    def test_enabling_marks_the_session_as_streaming(self):
        r2 = self._fake_r2()
        asyncio.run(P._op_sensors(r2, {"enable": True,
                                       "groups": ["accelerometer"],
                                       "ext_groups": []}))
        self.assertTrue(r2.sensor_streaming)

    def test_disable_sends_zero_masks(self):
        r2 = self._fake_r2()
        asyncio.run(P._op_sensors(r2, {"enable": False}))
        self.assertIn(("ext", 0), r2.sent)
        self.assertTrue(any(op == "mask" and mask == 0
                            for op, _i, _c, mask in
                            [s for s in r2.sent if s[0] == "mask"]))

    def test_unconfirmed_enable_still_marks_streaming(self):
        # THE REASON THIS RULE EXISTS. `send` returning None means the reply
        # was lost, not that the packet missed. If that cleared the flag, the
        # exit path would skip the disable and leave him streaming unattended.
        r2 = self._fake_r2()
        asyncio.run(P._op_sensors(r2, {"enable": True,
                                       "groups": ["accelerometer"],
                                       "ext_groups": []}))
        self.assertTrue(r2.sensor_streaming)

    def test_unconfirmed_disable_keeps_the_flag_set_for_a_retry(self):
        # Errors must fall towards "send a disable we did not need".
        r2 = self._fake_r2()
        r2.sensor_streaming = True
        asyncio.run(P._op_sensors(r2, {"enable": False}))
        self.assertTrue(r2.sensor_streaming)

    def test_flag_starts_false_so_a_read_only_session_sends_nothing(self):
        # The exit path keys off this flag. A session that never enabled the
        # stream must not fire a disable at a robot it does not own.
        r2 = self._fake_r2()
        self.assertFalse(r2.sensor_streaming)


class TestPhasesRun(unittest.TestCase):
    """Smoke-test every phase against a fake bridge.

    These exist because the phase functions had NO coverage at all: a review
    edit to `ac3_baseline` could have left a dangling name and the failure
    would have surfaced only on live hardware, costing a daemon relaunch that
    only the operator can perform. A phase that raises is the single most
    expensive bug class in this file.
    """

    def setUp(self):
        import sensor_probe as S
        import tempfile, pathlib
        self.S = S
        self._real_state = S.STATE
        self._tmp = tempfile.TemporaryDirectory()
        S.STATE = pathlib.Path(self._tmp.name) / "results.json"

    def tearDown(self):
        self.S.STATE = self._real_state
        self._tmp.cleanup()

    def _args(self, **kw):
        import argparse
        d = dict(seconds=0.05, trials=1, site="dome", interval=250, window=0.05)
        d.update(kw)
        return argparse.Namespace(**d)

    def _bridge(self):
        import r2_behavior as B
        return B.FakeBridge()

    def test_ac2_and_ac3_run_and_persist(self):
        st = {}
        self.S.ac2_characterise(self._bridge(), st, self._args())
        self.assertIn("ac2", st)
        self.S.ac3_baseline(self._bridge(), st, self._args())
        self.assertIn("ac3", st)
        # The series is what scoring consumes; it must always be written.
        self.assertIn("series", st["ac3"])

    def test_ac4_refuses_without_a_baseline(self):
        # A touch threshold with no rest baseline is meaningless, and must
        # say so rather than scoring against nothing.
        st = {}
        self.S.ac4_touch(self._bridge(), st, self._args())
        self.assertNotIn("ac4", st)

    def test_verdict_runs_on_an_empty_state(self):
        st = {}
        self.S.verdict(self._bridge(), st, self._args())
        # No stream, no trials -> must be instrument_limited, never "neither".
        self.assertEqual(st["ac6"]["verdict"], "instrument_limited")


class TestSensorsOpSafety(unittest.TestCase):

    def test_sensors_is_a_read_tier_op(self):
        # It configures a notification stream; nothing it sends can move him.
        self.assertEqual(P.op_tier("sensors"), "read")
        self.assertIn("sensors", P.allowed_ops("read"))

    def test_notify_cid_matches_upstream(self):
        # sensor.py:92 — (24, 2, 0xff)
        self.assertEqual((P.DID_SENSOR, P.CID_SENSOR_STREAM_NOTIFY), (24, 2))
        self.assertEqual(P.KNOWN_NOTIFIES[(24, 2)], "sensor_stream")

    def test_mask_command_cids_match_upstream(self):
        self.assertEqual(P.CID_SENSOR_MASK, 0)        # sensor.py:84
        self.assertEqual(P.CID_SENSOR_MASK_GET, 1)    # sensor.py:88
        self.assertEqual(P.CID_SENSOR_EXT_MASK, 12)   # sensor.py:95


if __name__ == "__main__":
    unittest.main(verbosity=2)
