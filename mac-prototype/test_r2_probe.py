#!/usr/bin/env python3
"""Tests for the bridge event ring, permission tiers and idle control — issue #7.

Runs WITHOUT hardware, WITHOUT a daemon, and WITHOUT bleak: r2_probe makes the
BLE import optional precisely so the packet layer and the request handler can be
exercised by the stock interpreter.

    python3 -m unittest discover -s mac-prototype -p 'test_*.py' -v
"""

import asyncio
import json
import sys
import tempfile
import time
import types
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import r2_probe as P


# ── packet helpers ───────────────────────────────────────────────────────────
# Built the long way round on purpose: if these mirrored P.build() a framing
# regression would cancel itself out and the tests would still pass.

def _frame(body: bytes) -> bytes:
    return bytes((P.SOP,)) + P.escape(body + bytes((P.packet_chk(body),))) + bytes((P.EOP,))


def response_packet(did, cid, seq, data=b"", err=0) -> bytes:
    """A reply to something we sent: is_response set, error byte present."""
    return _frame(bytes((P.FLAG_IS_RESPONSE, did, cid, seq, err)) + data)


def notify_packet(did, cid, data=b"", seq=P.NOTIFY_SEQ) -> bytes:
    """Robot-initiated. flags=0x00 and seq=0xff, per freer2/index.js:137."""
    return _frame(bytes((0x00, did, cid, seq)) + data)


def make_r2() -> P.R2:
    """An R2 with no radio behind it. `client` is None until a test installs
    a fake, which is exactly what a bleak-less host gets."""
    return P.R2("00:00:00:00:00:00", verbose=False)


class FakeClient:
    """Reassembles what R2 writes and feeds back whatever `responder` says.

    Responses go through the real `_on_notify`, so byte reassembly and packet
    framing are genuinely under test. The threading hand-off is NOT — this
    delivers on the loop thread. See TestCrossThreadCompletion for that."""

    def __init__(self, r2, responder):
        self.r2, self.responder = r2, responder
        self.buf = bytearray()
        self.sent: list[bytes] = []

    async def write_gatt_char(self, uuid, chunk, response=True):
        self.buf += chunk
        if self.buf and self.buf[-1] == P.EOP:
            packet, self.buf = bytes(self.buf), bytearray()
            self.sent.append(packet)
            reply = self.responder(P.parse(packet))
            if reply is not None:
                self.r2._on_notify(None, bytearray(reply))


def echo(request: P.Response) -> bytes:
    """Default responder: acknowledge whatever was asked, same seq."""
    return response_packet(request.did, request.cid, request.seq)


def run(coro):
    return asyncio.run(coro)


# ── AC1: unmatched notifications survive ─────────────────────────────────────

class TestEventCapture(unittest.TestCase):
    """AC1: an unmatched notification is retrievable rather than dropped."""

    def test_unsolicited_notify_is_captured_and_named(self):
        r2 = make_r2()
        r2._on_notify(None, bytearray(notify_packet(
            P.DID_ANIMATRONIC, P.CID_ANIM_COMPLETE_NOTIFY, b"\x23")))
        drained = r2.drain_events()
        self.assertEqual(drained["count"], 1)
        event = drained["events"][0]
        self.assertEqual(event["name"], "animation_complete")
        self.assertEqual(event["seq"], P.NOTIFY_SEQ)
        self.assertTrue(event["unsolicited"])
        self.assertEqual(event["data"], "23")

    def test_all_three_animatronic_notifies_decode(self):
        r2 = make_r2()
        for cid in (P.CID_ANIM_COMPLETE_NOTIFY, P.CID_ANIM_LEG_COMPLETE_NOTIFY,
                    P.CID_ANIM_HEAD_RESET_NOTIFY):
            r2._on_notify(None, bytearray(notify_packet(P.DID_ANIMATRONIC, cid)))
        names = [e["name"] for e in r2.drain_events()["events"]]
        self.assertEqual(names, ["animation_complete", "leg_action_complete",
                                 "head_reset_to_zero"])

    def test_unknown_notify_is_kept_with_name_none(self):
        """An undocumented notify is the interesting one — never discard it
        just because we cannot name it."""
        r2 = make_r2()
        r2._on_notify(None, bytearray(notify_packet(0x99, 0x77, b"\xde\xad")))
        event = r2.drain_events()["events"][0]
        self.assertIsNone(event["name"])
        self.assertEqual((event["did"], event["cid"], event["data"]),
                         (0x99, 0x77, "dead"))

    def test_notify_payload_is_not_eaten_as_an_error_byte(self):
        """flags=0x00 means not-a-response, so parse() must leave all four
        payload bytes alone. If it stripped one, data would be '02030405'."""
        r2 = make_r2()
        r2._on_notify(None, bytearray(notify_packet(
            P.DID_SENSOR, 0x02, b"\x01\x02\x03\x04\x05")))
        event = r2.drain_events()["events"][0]
        self.assertEqual(event["data"], "0102030405")
        self.assertEqual(event["err"], 0)
        self.assertEqual(event["name"], "sensor_stream")

    def test_drain_empties_the_ring(self):
        r2 = make_r2()
        r2._on_notify(None, bytearray(notify_packet(P.DID_ANIMATRONIC, 0x11)))
        self.assertEqual(r2.drain_events()["count"], 1)
        self.assertEqual(r2.drain_events(), {"events": [], "count": 0,
                                             "dropped": 0, "framing_errors": 0})

    def test_a_stream_that_never_sends_eop_does_not_grow_forever(self):
        """A SOP with no EOP behind it used to grow _rx for the life of the
        daemon. Over a 30-minute session that is an unbounded leak."""
        r2 = make_r2()
        r2._on_notify(None, bytearray([P.SOP]))
        r2._on_notify(None, bytearray(b"\x01" * (P.MAX_PACKET_BYTES * 3)))
        self.assertLessEqual(len(r2._rx), P.MAX_PACKET_BYTES)

    def test_it_resyncs_on_the_next_sop_after_an_overrun(self):
        r2 = make_r2()
        r2._on_notify(None, bytearray([P.SOP]))
        r2._on_notify(None, bytearray(b"\x01" * (P.MAX_PACKET_BYTES + 10)))
        r2._on_notify(None, bytearray(notify_packet(
            P.DID_ANIMATRONIC, P.CID_ANIM_COMPLETE_NOTIFY)))
        names = [e["name"] for e in r2.drain_events()["events"]]
        self.assertIn("animation_complete", names)

    def test_packet_split_across_notifications_still_lands(self):
        """R2 can deliver a packet byte-by-byte (r2d2_central.c:68)."""
        r2 = make_r2()
        for byte in notify_packet(P.DID_ANIMATRONIC, P.CID_ANIM_COMPLETE_NOTIFY):
            r2._on_notify(None, bytearray([byte]))
        self.assertEqual(r2.drain_events()["events"][0]["name"],
                         "animation_complete")


# ── AC2: the ring is bounded ─────────────────────────────────────────────────

class TestEventRingBounded(unittest.TestCase):
    """AC2: 300 synthetic unmatched packets leave exactly 200, oldest evicted."""

    def test_300_packets_leave_exactly_200_oldest_evicted(self):
        r2 = make_r2()
        for i in range(300):
            r2._on_notify(None, bytearray(notify_packet(
                P.DID_ANIMATRONIC, P.CID_ANIM_COMPLETE_NOTIFY,
                i.to_bytes(2, "big"))))
        drained = r2.drain_events()
        self.assertEqual(drained["count"], P.EVENT_RING_SIZE)
        self.assertEqual(drained["count"], 200)
        # Survivors are the LAST 200: 100..299, in order.
        kept = [int(e["data"], 16) for e in drained["events"]]
        self.assertEqual(kept[0], 100)
        self.assertEqual(kept[-1], 299)
        self.assertEqual(kept, list(range(100, 300)))
        self.assertEqual(drained["dropped"], 100)

    def test_dropped_counter_resets_after_being_reported(self):
        r2 = make_r2()
        for i in range(250):
            r2._on_notify(None, bytearray(notify_packet(P.DID_ANIMATRONIC, 0x11)))
        self.assertEqual(r2.drain_events()["dropped"], 50)
        self.assertEqual(r2.drain_events()["dropped"], 0)

    def test_ring_never_grows_past_the_cap_between_drains(self):
        r2 = make_r2()
        for i in range(1000):
            r2._on_notify(None, bytearray(notify_packet(P.DID_ANIMATRONIC, 0x11)))
            self.assertLessEqual(len(r2._events), P.EVENT_RING_SIZE)


# ── AC3: matched request/response still works ────────────────────────────────

class TestMatchedResponsesStillResolve(unittest.TestCase):
    """AC3: capturing the strays must not break the normal path."""

    def test_info_still_returns_battery_and_head(self):
        import struct

        def responder(request):
            if (request.did, request.cid) == (P.DID_POWER, P.CID_POWER_BATTERY_VOLTAGE):
                return response_packet(request.did, request.cid, request.seq,
                                       b"\x01\x8b")
            if (request.did, request.cid) == (P.DID_ANIMATRONIC, P.CID_ANIM_GET_HEAD):
                return response_packet(request.did, request.cid, request.seq,
                                       struct.pack(">f", 103.08))
            return echo(request)

        async def body():
            r2 = make_r2()
            r2.client = FakeClient(r2, responder)
            r2._loop = asyncio.get_running_loop()
            battery = await r2.battery_voltage()
            head = await r2.get_head()
            return r2, battery, head

        r2, battery, head = run(body())
        self.assertEqual(battery.data.hex(), "018b")
        self.assertAlmostEqual(head, 103.08, places=2)
        # And nothing leaked into the ring — these were all matched.
        self.assertEqual(r2.drain_events()["count"], 0)

    def test_response_with_the_wrong_seq_is_captured_not_silently_lost(self):
        """The failure this whole ring exists to make visible."""
        async def body():
            r2 = make_r2()
            r2.client = FakeClient(r2, lambda req: response_packet(
                req.did, req.cid, (req.seq + 7) % 0xFF))
            r2._loop = asyncio.get_running_loop()
            result = await r2.send(P.DID_POWER, P.CID_POWER_WAKE, timeout=0.3)
            return r2, result

        r2, result = run(body())
        self.assertIsNone(result)                       # timed out, as before
        drained = r2.drain_events()
        self.assertEqual(drained["count"], 1)           # but no longer invisible
        self.assertFalse(drained["events"][0]["unsolicited"])

    def test_repeated_response_is_captured_once_its_waiter_is_gone(self):
        """The waiter is popped on the first match, so a second copy is simply
        unmatched — captured, not dropped, and not mistaken for a live reply."""
        async def body():
            r2 = make_r2()
            replies = []

            def responder(request):
                replies.append((request.did, request.cid, request.seq))
                return response_packet(request.did, request.cid, request.seq)

            r2.client = FakeClient(r2, responder)
            r2._loop = asyncio.get_running_loop()
            first = await r2.wake()
            did, cid, seq = replies[0]
            r2._on_notify(None, bytearray(response_packet(did, cid, seq)))
            return r2, first

        r2, first = run(body())
        self.assertIsNotNone(first)                     # the real one resolved
        events = r2.drain_events()["events"]
        self.assertEqual(len(events), 1)                # the copy was kept
        self.assertFalse(events[0]["unsolicited"])

    def test_reply_that_lost_the_race_with_its_timeout_is_noted(self):
        """A settled-but-still-registered waiter means the reply lost a race
        with its own send() timeout. Recording it is how you debug that."""
        async def body():
            r2 = make_r2()
            r2._loop = asyncio.get_running_loop()
            stale = asyncio.get_running_loop().create_future()
            stale.cancel()
            r2._waiters[(P.DID_POWER, P.CID_POWER_WAKE, 3)] = stale
            r2._on_notify(None, bytearray(
                response_packet(P.DID_POWER, P.CID_POWER_WAKE, 3)))
            return r2

        r2 = run(body())
        events = r2.drain_events()["events"]
        self.assertEqual(len(events), 1)
        self.assertIn("waiter already settled", events[0]["note"])

    def test_packet_without_the_response_flag_never_resolves_a_waiter(self):
        """Matching on (did, cid, seq) alone let a non-response packet resolve
        a live command AS SUCCESS — and since parse() skips the error byte for
        non-responses, every payload field was shifted by one as well."""
        def impostor(request):
            return _frame(bytes((0x00, request.did, request.cid, request.seq))
                          + b"\xde\xad")

        async def body():
            r2 = make_r2()
            r2.client = FakeClient(r2, impostor)
            r2._loop = asyncio.get_running_loop()
            return r2, await r2.send(P.DID_POWER, P.CID_POWER_WAKE, timeout=0.3)

        r2, result = run(body())
        self.assertIsNone(result, "a non-response packet resolved the waiter")
        events = r2.drain_events()["events"]
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]["data"], "dead")

    def test_a_failed_write_does_not_leak_its_waiter(self):
        """The waiter is registered before the write, so every exit path has to
        clean it up. On a flaky link the 3 s keepalive leaked one per cycle."""
        class Broken:
            async def write_gatt_char(self, *a, **k):
                raise OSError("link dropped")

        async def body():
            r2 = make_r2()
            r2.client = Broken()
            r2._loop = asyncio.get_running_loop()
            for _ in range(5):
                with self.assertRaises(OSError):
                    await r2.send(P.DID_POWER, P.CID_POWER_WAKE, timeout=0.1)
            return r2

        self.assertEqual(len(run(body())._waiters), 0)

    def test_a_timed_out_send_does_not_leak_its_waiter(self):
        async def body():
            r2 = make_r2()
            r2.client = FakeClient(r2, lambda req: None)   # never answers
            r2._loop = asyncio.get_running_loop()
            await r2.send(P.DID_POWER, P.CID_POWER_WAKE, timeout=0.1)
            return r2

        self.assertEqual(len(run(body())._waiters), 0)


class TestCrossThreadCompletion(unittest.TestCase):
    """bleak calls _on_notify on the CoreBluetooth dispatch thread, never the
    loop thread. Completing a Future directly from there wedged this daemon
    once already (c7ee81b). Every other test delivers on the loop thread, so
    without this one, reverting call_soon_threadsafe passes the whole suite."""

    def test_response_delivered_from_a_foreign_thread_resolves_the_future(self):
        import threading

        async def body():
            r2 = make_r2()
            r2._loop = asyncio.get_running_loop()
            buf = bytearray()

            class ThreadedClient:
                async def write_gatt_char(self, uuid, chunk, response=True):
                    buf.extend(chunk)
                    if buf and buf[-1] == P.EOP:
                        request = P.parse(bytes(buf))
                        buf.clear()
                        reply = response_packet(request.did, request.cid,
                                                request.seq, b"\x01\x8b")
                        threading.Thread(
                            target=r2._on_notify,
                            args=(None, bytearray(reply)),
                            daemon=True).start()

            r2.client = ThreadedClient()
            return await asyncio.wait_for(r2.battery_voltage(), 3.0)

        self.assertEqual(run(body()).data.hex(), "018b")

    def test_events_survive_appends_from_many_threads_while_draining(self):
        """The ring's whole premise is append-from-BLE-thread while the loop
        thread drains. Nothing else in the suite exercises that concurrently."""
        import threading

        r2 = make_r2()
        threads, per_thread = 4, 250
        drained, stop = [], threading.Event()
        errors = []

        def produce(tag):
            try:
                for i in range(per_thread):
                    r2._on_notify(None, bytearray(notify_packet(
                        P.DID_ANIMATRONIC, P.CID_ANIM_COMPLETE_NOTIFY,
                        bytes((tag, i & 0xFF)))))
            except Exception as e:            # noqa: BLE001 — surface to assert
                errors.append(e)

        def consume():
            try:
                while not stop.is_set():
                    drained.append(r2.drain_events())
                    if len(r2._events) > P.EVENT_RING_SIZE:
                        errors.append(AssertionError("ring exceeded its cap"))
            except Exception as e:            # noqa: BLE001
                errors.append(e)

        consumer = threading.Thread(target=consume, daemon=True)
        consumer.start()
        producers = [threading.Thread(target=produce, args=(t,))
                     for t in range(threads)]
        for p in producers:
            p.start()
        for p in producers:
            p.join()
        stop.set()
        consumer.join(5)
        drained.append(r2.drain_events())

        self.assertEqual(errors, [])
        kept = sum(d["count"] for d in drained)
        self.assertGreater(kept, 0)
        self.assertLessEqual(kept, threads * per_thread)
        self.assertLessEqual(len(r2._events), P.EVENT_RING_SIZE)

    def test_transmitted_seq_never_equals_the_notify_seq(self):
        """Read the seq off the WIRE, not off `_seq`.

        The first version of this test re-implemented `(seq + 1) % 0xFF` inside
        itself and asserted its own arithmetic — mutating send() to `% 0x100`
        left it green, which is precisely the regression it claimed to guard.
        "Unmatched seq" is the discriminator the whole event ring rests on."""
        async def body():
            r2 = make_r2()
            r2.client = FakeClient(r2, echo)
            r2._loop = asyncio.get_running_loop()
            for _ in range(300):
                await r2.send(P.DID_POWER, P.CID_POWER_WAKE, expect=False)
            return r2.client.sent

        seqs = [P.parse(pkt).seq for pkt in run(body())]
        self.assertEqual(len(seqs), 300)
        self.assertNotIn(P.NOTIFY_SEQ, seqs)
        self.assertEqual(max(seqs), P.NOTIFY_SEQ - 1)


# ── AC4/AC5: permission tiers and idle control ───────────────────────────────

class FakeR2:
    """Records commands instead of sending them.

    Implements the MOTION methods too, even though a correct daemon never
    reaches them at a low ceiling. Without them, deleting the tier check made
    the handler raise AttributeError, which is indistinguishable from a
    refusal — the permission ladder could be removed entirely and the suite
    stayed green. A fake that is too small hides the bug it should expose."""

    device = "D2-TEST"   # gate evidence is bound to the robot that produced it

    def __init__(self, err: int = 0):
        self.calls: list[tuple] = []
        self.idle_disabled = None
        self.err = err
        self.events = {"events": [], "count": 0, "dropped": 0}

    def drain_events(self):
        self.calls.append(("drain_events",))
        return self.events

    async def _cmd(self, name, *args):
        self.calls.append((name, *args))
        return P.Response(P.FLAG_IS_RESPONSE, 0x17, 0x00, 1, self.err, b"")

    async def get_head(self):
        await self._cmd("get_head")
        return 103.0

    leg_action = P.LEG_STATE_THREE_LEGS

    async def get_leg_action(self):
        await self._cmd("get_leg_action")
        return self.leg_action

    async def perform_leg_action(self, action):
        return await self._cmd("perform_leg_action", action)

    leg_position = 0.0

    async def get_leg_position(self):
        await self._cmd("get_leg_position")
        return self.leg_position

    async def set_head(self, degrees):
        return await self._cmd("set_head", degrees)

    async def play_animation(self, animation_id):
        return await self._cmd("play_animation", animation_id)

    async def set_leds(self, mapping):
        return await self._cmd("set_leds", tuple(sorted(mapping.items())))

    async def play_sound(self, sound_id, mode=0):
        return await self._cmd("play_sound", sound_id, mode)

    async def set_volume(self, volume):
        return await self._cmd("set_volume", volume)

    async def enable_idle_animations(self, enable):
        return await self._cmd("enable_idle_animations", enable)

    async def enable_leg_action_notify(self, enable):
        return await self._cmd("enable_leg_action_notify", enable)

    async def enable_head_reset_notify(self, enable):
        return await self._cmd("enable_head_reset_notify", enable)

    async def stop_animation(self):
        return await self._cmd("stop_animation")

    async def stop_audio(self):
        return await self._cmd("stop_audio")


def handle(payload, ceiling="read", r2=None):
    r2 = r2 or FakeR2()
    resp = run(P.handle_request(r2, payload, ceiling, log=lambda *_: None))
    return resp, r2


class GateControl(unittest.TestCase):
    """Kept as a base class only so the tests below keep their names and
    setUp contract. The unproven-CID marker it used to redirect is gone: both
    animatronic ops are pinned to `motion` unconditionally, so there is no
    stored state a test could contaminate or depend on."""


class VerifiedGate(GateControl):
    pass


class TestEventsOpTier(GateControl):
    """AC4: `events` is available at every ceiling — 'read' is the floor."""

    def test_events_allowed_at_every_ceiling(self):
        for ceiling in P.TIERS:
            resp, _ = handle({"op": "events"}, ceiling)
            self.assertTrue(resp["ok"], f"events refused at ceiling {ceiling}")

    def test_events_is_tier_read(self):
        self.assertEqual(P.op_tier("events"), "read")

    def test_events_op_drains(self):
        r2 = FakeR2()
        r2.events = {"events": [{"name": "animation_complete"}], "count": 1,
                     "dropped": 0}
        resp, r2 = handle({"op": "events"}, "read", r2)
        self.assertEqual(resp["data"]["count"], 1)
        self.assertIn(("drain_events",), r2.calls)


class TestIdleControl(VerifiedGate):
    """AC5 (the half that needs no robot): the op sends the right bytes at the
    right tier. That it actually quiets him is a hardware check."""

    def test_idle_needs_the_dome_ceiling_in_BOTH_directions(self):
        """No longer direction-dependent, and no longer relaxable. It writes to
        the motion device, so it sits above the read ceiling permanently —
        and nothing needs it lower, because the command does not work at all."""
        for ceiling in ("read", "leds", "audio"):
            for enable in (True, False):
                resp, r2 = handle({"op": "idle", "params": {"enable": enable}},
                                  ceiling)
                self.assertFalse(resp["ok"], f"idle ran at {ceiling}")
                self.assertIn("needs tier 'dome'", resp["error"])
                self.assertEqual(r2.calls, [])

    def test_idle_defaults_to_disabling(self):
        resp, r2 = handle({"op": "idle"}, "motion")
        self.assertTrue(resp["ok"])
        self.assertIn(("enable_idle_animations", False), r2.calls)
        self.assertFalse(resp["data"]["idle_enabled"])

    def test_enabling_idle_needs_the_dome_ceiling(self):
        for ceiling in ("read", "leds", "audio"):
            resp, r2 = handle({"op": "idle", "params": {"enable": True}}, ceiling)
            self.assertFalse(resp["ok"], f"idle-on wrongly allowed at {ceiling}")
            self.assertIn("needs tier 'dome'", resp["error"])
            self.assertEqual(r2.calls, [])       # refused BEFORE any command
        resp, r2 = handle({"op": "idle", "params": {"enable": True}}, "motion")
        self.assertTrue(resp["ok"])
        self.assertIn(("enable_idle_animations", True), r2.calls)

    def test_idle_records_the_state_it_left_r2_in(self):
        _, r2 = handle({"op": "idle", "params": {"enable": False}}, "motion")
        self.assertTrue(r2.idle_disabled)
        _, r2 = handle({"op": "idle", "params": {"enable": True}}, "motion")
        self.assertFalse(r2.idle_disabled)

    def test_unacknowledged_disable_fails_loudly(self):
        """No observation catches a silently-failed precondition. Reporting ok
        here hands the operator clean-looking data from a fidgeting robot."""
        class Silent(FakeR2):
            async def enable_idle_animations(self, enable):
                self.calls.append(("enable_idle_animations", enable))
                return None

        resp, r2 = handle({"op": "idle", "params": {"enable": False}}, "motion",
                          Silent())
        self.assertFalse(resp["ok"])
        self.assertIn("no response", resp["error"])
        self.assertIn("UNKNOWN", resp["error"])
        self.assertIsNone(r2.idle_disabled, "claimed a state R2 never confirmed")

    def test_rejected_disable_fails_loudly(self):
        """CID 0x2C is single-source and unproven — bad_command_id is a live
        possibility on this firmware, not a hypothetical."""
        resp, r2 = handle({"op": "idle", "params": {"enable": False}}, "motion",
                          FakeR2(err=0x02))
        self.assertFalse(resp["ok"])
        self.assertIn("bad_command_id", resp["error"])
        self.assertIsNone(r2.idle_disabled)

    def test_idle_defaults_to_the_quiet_direction(self):
        """An op's ceiling no longer depends on its arguments, but the default
        direction still matters."""
        self.assertIs(P._idle_enable({}), False)
        _, r2 = handle({"op": "idle"}, "motion")
        self.assertIn(("enable_idle_animations", False), r2.calls)

    def test_non_boolean_enable_is_refused_not_guessed(self):
        """"false" is a truthy Python string. Guessing would ENABLE motion."""
        for bad in ["false", "true", 0, 1, None, "yes"]:
            resp, r2 = handle({"op": "idle", "params": {"enable": bad}}, "motion")
            self.assertFalse(resp["ok"], f"accepted {bad!r}")
            self.assertIn("must be JSON true or false", resp["error"])
            self.assertEqual(r2.calls, [])


class TestIdlePayloadBytes(unittest.TestCase):
    """The wire bytes, against animatronic.py:72-73 — CID 44, [int(enable)]."""

    def _sent(self, coro_factory):
        async def body():
            r2 = make_r2()
            r2.client = FakeClient(r2, echo)
            r2._loop = asyncio.get_running_loop()
            await coro_factory(r2)
            return r2.client.sent
        return run(body())

    def test_disable_idle_is_did_17_cid_2c_data_00(self):
        packet = self._sent(lambda r2: r2.enable_idle_animations(False))[0]
        parsed = P.parse(packet)
        self.assertEqual(parsed.did, 0x17)
        self.assertEqual(parsed.cid, 0x2C)
        self.assertEqual(parsed.data, b"\x00")

    def test_enable_idle_is_data_01(self):
        parsed = P.parse(self._sent(lambda r2: r2.enable_idle_animations(True))[0])
        self.assertEqual((parsed.did, parsed.cid, parsed.data), (0x17, 0x2C, b"\x01"))

    def test_leg_action_notify_is_cid_2a(self):
        parsed = P.parse(self._sent(lambda r2: r2.enable_leg_action_notify(True))[0])
        self.assertEqual((parsed.did, parsed.cid, parsed.data), (0x17, 0x2A, b"\x01"))

    def test_head_reset_notify_is_cid_39(self):
        parsed = P.parse(self._sent(lambda r2: r2.enable_head_reset_notify(True))[0])
        self.assertEqual((parsed.did, parsed.cid, parsed.data), (0x17, 0x39, b"\x01"))

    def test_constants_match_the_traced_command_ids(self):
        """Decimal CIDs as written in animatronic.py, so a typo in the hex
        constant above cannot pass unnoticed."""
        self.assertEqual(P.CID_ANIM_ENABLE_LEG_NOTIFY, 42)
        self.assertEqual(P.CID_ANIM_ENABLE_IDLE, 44)
        self.assertEqual(P.CID_ANIM_ENABLE_HEAD_RESET_NOTIFY, 57)
        self.assertEqual(P.CID_ANIM_COMPLETE_NOTIFY, 17)
        self.assertEqual(P.CID_ANIM_LEG_COMPLETE_NOTIFY, 38)
        self.assertEqual(P.CID_ANIM_HEAD_RESET_NOTIFY, 58)


class TestNotifyOp(VerifiedGate):

    def test_enables_both_notifies(self):
        resp, r2 = handle({"op": "notify",
                           "params": {"leg": True, "head_reset": True}}, "motion")
        self.assertTrue(resp["ok"])
        self.assertIn(("enable_leg_action_notify", True), r2.calls)
        self.assertIn(("enable_head_reset_notify", True), r2.calls)

    def test_empty_params_is_an_error_not_a_silent_noop(self):
        resp, r2 = handle({"op": "notify"}, "motion")
        self.assertFalse(resp["ok"])
        self.assertIn("nothing to enable", resp["error"])
        self.assertEqual(r2.calls, [])

    def test_reports_only_the_channels_it_actually_enabled(self):
        """Upstream has no setter for animation-complete (animatronic.py:43),
        so the op must not imply it turned anything else on. The previous
        assertion looked for a key named 'animation' that no regression would
        ever produce — it could not fail."""
        resp, _ = handle({"op": "notify", "params": {"leg": True}}, "motion")
        self.assertEqual(set(resp["data"]) - {"note"}, {"leg"})
        self.assertIn("animation_complete has no enable", resp["data"]["note"])
        resp, _ = handle({"op": "notify",
                          "params": {"leg": True, "head_reset": True}}, "motion")
        self.assertEqual(set(resp["data"]) - {"note"}, {"leg", "head_reset"})

    def test_a_bad_param_does_not_half_execute(self):
        """Validating as it went put the leg notify on the wire and THEN
        returned ok:false — a refusal that had already changed robot state."""
        resp, r2 = handle({"op": "notify",
                           "params": {"leg": True, "head_reset": "yes"}}, "motion")
        self.assertFalse(resp["ok"])
        self.assertIn("must be JSON true or false", resp["error"])
        self.assertEqual(r2.calls, [], "leg notify was sent before validation")

    def test_non_boolean_leg_is_refused(self):
        for bad in ["true", "false", 1, 0, None]:
            resp, r2 = handle({"op": "notify", "params": {"leg": bad}}, "motion")
            self.assertFalse(resp["ok"], f"accepted {bad!r}")
            self.assertEqual(r2.calls, [])


class TestRequestEnvelope(GateControl):
    """A stray queue file must never end the session — that session is the
    only channel through which `stop` can be sent."""

    def test_non_object_payloads_are_refused_not_raised(self):
        for payload in ([1, 2], "hello", 42, None, True):
            resp, r2 = handle(payload, "read")
            self.assertFalse(resp["ok"])
            self.assertIn("must be a JSON object", resp["error"])
            self.assertEqual(r2.calls, [])

    def test_unhashable_op_is_refused_not_raised(self):
        """`{"op": []}` raised TypeError on the `op not in OPS` lookup."""
        for bad_op in ([], {}, 7, None):
            resp, r2 = handle({"op": bad_op}, "read")
            self.assertFalse(resp["ok"])
            self.assertIn("unknown op", resp["error"])
            self.assertEqual(r2.calls, [])

    def test_non_object_params_are_refused(self):
        resp, r2 = handle({"op": "idle", "params": [1, 2]}, "motion")
        self.assertFalse(resp["ok"])
        self.assertIn("'params' must be a JSON object", resp["error"])
        self.assertEqual(r2.calls, [])


class TestNothingCanWedgeTheQueueLoop(GateControl):
    """The loop is serial: whatever blocks inside an op also blocks `stop`."""

    def test_dome_settle_is_clamped(self):
        resp, _ = handle({"op": "dome", "params": {"delta": 5, "settle": 1e9}},
                         "motion")
        self.assertTrue(resp["ok"])
        self.assertLessEqual(P.MAX_SETTLE_S, 30.0)

    def test_dome_settle_clamp_is_actually_applied(self):
        import time as _time
        started = _time.monotonic()
        resp, _ = handle({"op": "dome", "params": {"delta": 5, "settle": -5}},
                         "motion")
        self.assertTrue(resp["ok"])
        self.assertLess(_time.monotonic() - started, 5.0)

    def test_absurd_led_channel_is_refused_before_the_shift(self):
        """`1 << int(k)` with a huge key hangs or MemoryErrors inside the op."""
        resp, r2 = handle({"op": "leds",
                           "params": {"channels": {"1000000000000": 1}}}, "leds")
        self.assertFalse(resp["ok"])
        self.assertIn("out of range", resp["error"])
        self.assertEqual(r2.calls, [])

    def test_led_level_out_of_range_is_refused(self):
        resp, r2 = handle({"op": "leds", "params": {"channels": {"0": 999}}},
                          "leds")
        self.assertFalse(resp["ok"])
        self.assertIn("out of range", resp["error"])
        self.assertEqual(r2.calls, [])

    def test_valid_led_write_still_works(self):
        resp, r2 = handle({"op": "leds",
                           "params": {"channels": {"0": 0, "1": 0, "2": 255}}},
                          "leds")
        self.assertTrue(resp["ok"])
        self.assertIn(("set_leds", ((0, 0), (1, 0), (2, 255))), r2.calls)


class TestHandlerBasics(VerifiedGate):

    def test_unknown_op_lists_what_is_allowed(self):
        resp, _ = handle({"op": "nope"}, "read")
        self.assertFalse(resp["ok"])
        self.assertIn("events", resp["allowed"])

    def test_stop_halts_both_animation_and_audio(self):
        """Asserting only stop_animation let stop_audio be deleted from the op
        with the suite still green. `stop` is where 'default to STOP' lives."""
        resp, r2 = handle({"op": "stop"}, "read")
        self.assertTrue(resp["ok"])
        self.assertTrue(resp["data"]["stopped"])
        self.assertEqual(r2.calls, [("stop_animation",), ("stop_audio",),
                                    ("perform_leg_action", P.LEG_ACTION_STOP)])
        self.assertEqual(resp["data"]["results"],
                         {"animation": "success", "audio": "success",
                          "legs": "success"})

    def test_a_rejected_stop_exits_non_zero(self):
        """`./r2 send stop || panic` must see the failure. Returning ok:True
        with `stopped: false` buried in the payload meant the shell saw
        success while the body read 'R2 may still be moving'."""
        resp, _ = handle({"op": "stop"}, "read", FakeR2(err=0x02))
        self.assertFalse(resp["ok"], "a failed stop reported ok -> exit 0")
        self.assertIn("bad_command_id", resp["error"])
        self.assertIn("power him down by hand", resp["error"])

    def test_stop_still_tries_audio_when_animation_raises_at_await(self):
        class RaisesInBody(FakeR2):
            async def stop_animation(self):
                self.calls.append(("stop_animation",))
                raise RuntimeError("link dropped")

        _, r2 = handle({"op": "stop"}, "read", RaisesInBody())
        self.assertIn(("stop_audio",), r2.calls)

    def test_stop_still_tries_audio_when_animation_raises_at_CALL_time(self):
        """The coroutines used to be built eagerly in the loop's iterable, so
        a failure at call time skipped the other half and leaked an un-awaited
        coroutine — the guarantee was false in the case it existed for."""
        class RaisesOnCall(FakeR2):
            def stop_animation(self):
                self.calls.append(("stop_animation",))
                raise AttributeError("no client")

        _, r2 = handle({"op": "stop"}, "read", RaisesOnCall())
        self.assertIn(("stop_audio",), r2.calls,
                      "audio stop was skipped by a call-time failure")

    def test_stop_still_tries_audio_when_animation_is_CANCELLED(self):
        """CancelledError is not an Exception. A second Ctrl-C landing in the
        animation stop must not leave audio playing on an unattended robot."""
        class Cancelled(FakeR2):
            async def stop_animation(self):
                self.calls.append(("stop_animation",))
                raise asyncio.CancelledError()

        _, r2 = handle({"op": "stop"}, "read", Cancelled())
        self.assertIn(("stop_audio",), r2.calls)

    def test_stop_audio_op_reports_what_r2_said(self):
        """Its sibling was hardened while it still returned a hardcoded True
        under the same key name."""
        resp, _ = handle({"op": "stop_audio"}, "audio", FakeR2(err=0x02))
        self.assertFalse(resp["data"]["stopped"])
        self.assertEqual(resp["data"]["err"], "bad_command_id")

    def test_motion_ops_refused_at_read_FOR_THE_RIGHT_REASON(self):
        """Assert the refusal REASON. `assertFalse(resp["ok"])` alone passed
        even with `_tier_ok` stubbed to return True, because FakeR2 lacked the
        motion methods and the resulting AttributeError also produced ok:False.
        The permission ladder could be deleted wholesale and this stayed green."""
        # The two are named with their OWN tiers, not a shared one: since #11
        # they sit on different rungs, and a test that let them share would go
        # green again if `animation` were ever quietly moved back down.
        for op, params, tier in (("dome", {"delta": 5}, "dome"),
                                 ("animation", {"id": 1}, "stance")):
            resp, r2 = handle({"op": op, "params": params}, "read")
            self.assertFalse(resp["ok"])
            self.assertIn(f"needs tier '{tier}'", resp["error"])
            self.assertEqual(r2.calls, [], "a command went out before refusal")

    def test_every_op_above_its_ceiling_is_refused_with_a_tier_reason(self):
        """Sweep the whole ladder rather than spot-checking two ops."""
        for ceiling in P.TIERS:
            for op in sorted(set(P.OPS) - set(P.allowed_ops(ceiling))):
                resp, r2 = handle({"op": op, "params": {"id": 1, "delta": 5,
                                                        "channels": {"0": 1}}},
                                  ceiling)
                self.assertFalse(resp["ok"], f"{op} allowed at {ceiling}")
                self.assertIn("needs tier", resp["error"],
                              f"{op} at {ceiling} failed for the wrong reason")
                self.assertEqual(r2.calls, [], f"{op} ran at {ceiling}")

    def test_op_failure_is_reported_not_raised(self):
        class Boom(FakeR2):
            async def enable_idle_animations(self, enable):
                raise RuntimeError("radio gone")
        resp, _ = handle({"op": "idle"}, "motion", Boom())
        self.assertFalse(resp["ok"])
        self.assertIn("RuntimeError: radio gone", resp["error"])

    def test_err_of_names_an_unknown_code_instead_of_null(self):
        self.assertEqual(P._err_of(None), "no response")
        ok = P.Response(1, 0x17, 0x05, 1, 0x00, b"")
        self.assertEqual(P._err_of(ok), "success")
        weird = P.Response(1, 0x17, 0x05, 1, 0x7F, b"")
        self.assertEqual(P._err_of(weird), "unknown_error_0x7f")


class TestDomeTravelCapCannotBeDefeated(GateControl):
    """The cap is the one safety property bounded_head_move exists to enforce.

    NaN slipped past every guard in the dangerous direction: `abs(nan) > 45` is
    False so the travel cap was skipped, and `min(182, nan)` returns 182 so the
    clamp handed back its own bound. From a resting -100 degrees that is a 282
    degree swing. `json.dumps` emits a bare `NaN` by default and `json.loads`
    accepts it, so an ordinary survey script could put one in the queue."""

    class Head:
        def __init__(self, start):
            self.start, self.commanded = start, None

        async def get_head(self):
            return self.start

        async def set_head(self, degrees):
            self.commanded = degrees

    def _move(self, start, **kw):
        head = self.Head(start)
        run(P.bounded_head_move(head, **kw))
        return head.commanded

    def test_nan_delta_is_refused(self):
        for start in (103.0, -100.0, 0.0):
            with self.assertRaises(ValueError):
                self._move(start, delta=float("nan"))

    def test_nan_and_inf_angle_are_refused(self):
        for bad in (float("nan"), float("inf"), float("-inf")):
            with self.assertRaises(ValueError):
                self._move(103.0, angle=bad)
            with self.assertRaises(ValueError):
                self._move(103.0, delta=bad)

    def test_non_finite_reading_from_the_robot_is_refused(self):
        """A garbled float32 off the wire must not become a move command."""
        with self.assertRaises(ValueError):
            self._move(float("nan"), delta=5.0)

    def test_travel_is_still_capped_for_ordinary_values(self):
        self.assertAlmostEqual(self._move(103.0, delta=200.0),
                               103.0 + P.MAX_HEAD_MOVE)
        self.assertAlmostEqual(self._move(103.0, delta=-200.0),
                               103.0 - P.MAX_HEAD_MOVE)
        self.assertAlmostEqual(self._move(103.0, delta=10.0), 113.0)

    def test_no_input_can_exceed_the_cap(self):
        """Sweep instead of spot-checking: whatever comes in, travel is bounded."""
        for start in (-162.0, -100.0, 0.0, 103.0, 182.0):
            for delta in (-1e9, -300.0, -46.0, -1.0, 0.0, 1.0, 46.0, 300.0, 1e9):
                commanded = self._move(start, delta=delta)
                self.assertLessEqual(abs(commanded - start), P.MAX_HEAD_MOVE + 1e-6,
                                     f"start={start} delta={delta}")
                self.assertGreaterEqual(commanded, P.HEAD_MIN)
                self.assertLessEqual(commanded, P.HEAD_MAX)

    def test_dome_op_refuses_a_nan_and_sends_nothing(self):
        resp, r2 = handle({"op": "dome", "params": {"delta": float("nan")}},
                          "motion")
        self.assertFalse(resp["ok"])
        self.assertNotIn(("set_head", None), r2.calls)
        self.assertFalse([c for c in r2.calls if c[0] == "set_head"],
                         "commanded a move with a non-finite target")


class TestStopOnExitSurvivesCancellation(unittest.TestCase):
    def test_cancelled_error_is_not_an_exception_subclass(self):
        """Pins the language fact the exit handler depends on. `except
        Exception` there meant a second Ctrl-C skipped the stop entirely and
        left an animation running on an unattended robot."""
        self.assertFalse(issubclass(asyncio.CancelledError, Exception))
        self.assertTrue(issubclass(asyncio.CancelledError, BaseException))

    def test_exit_path_uses_the_same_stop_implementation(self):
        """The epilogue used to inline its own stops and print 'stop sent'
        without looking at either Response — so a rejected stop and a real one
        were indistinguishable on the path that runs when nobody is watching."""
        import inspect
        # Both halves: the daemon body was split out of cmd_daemon so the
        # single-instance lock could wrap it in a finally. Reading both keeps
        # this pinned to the epilogue itself rather than to which function
        # happens to hold it today.
        source = inspect.getsource(P.cmd_daemon) + inspect.getsource(P._run_daemon)
        self.assertIn("Default to STOP", source,
                      "the stop-on-exit epilogue is gone entirely")
        epilogue = source[source.index("Default to STOP"):]
        self.assertIn("stop_everything(r2, shield=True)", epilogue)
        self.assertNotIn("stop sent (animation + audio)", epilogue)

    def test_stop_everything_shields_and_reports(self):
        class Rejects(FakeR2):
            pass
        result = run(P.stop_everything(Rejects(err=0x02), shield=True))
        self.assertFalse(result["stopped"])
        self.assertIsNotNone(result["warning"])
        self.assertEqual(set(result["results"]), {"animation", "audio", "legs"})

    def test_stop_everything_shape_is_fixed_on_success(self):
        """`warning` is always present (None on success). A present-or-absent
        key forces every consumer into .get() and cannot be relied on."""
        result = run(P.stop_everything(FakeR2()))
        self.assertEqual(set(result), {"stopped", "results", "warning"})
        self.assertIsNone(result["warning"])
        self.assertTrue(result["stopped"])


class TestNoReplyIsSilentlyDestroyed(unittest.TestCase):
    def test_reply_cancelled_between_schedule_and_delivery_is_recorded(self):
        """`fut.done() or fut.set_result(resp)` quietly dropped a reply that
        raced its own timeout: the command was answered, send() reported no
        response, and the packet was recorded nowhere at all."""
        async def body():
            r2 = make_r2()
            r2._loop = asyncio.get_running_loop()
            fut = asyncio.get_running_loop().create_future()
            r2._waiters[(P.DID_POWER, P.CID_POWER_WAKE, 1)] = fut
            r2._on_notify(None, bytearray(
                response_packet(P.DID_POWER, P.CID_POWER_WAKE, 1)))
            fut.cancel()                       # timeout wins the race
            await asyncio.sleep(0.05)
            return r2

        drained = run(body()).drain_events()
        self.assertEqual(drained["count"], 1, "an answered command vanished")
        self.assertIn("waiter already settled", drained["events"][0]["note"])


class TestFramingErrorsAreVisible(unittest.TestCase):
    """A silently lost notification is indistinguishable from 'R2 never sent
    one' — precisely the UNKNOWN this harness exists to settle."""

    def test_a_truncated_frame_does_not_eat_the_next_good_packet(self):
        r2 = make_r2()
        good = notify_packet(P.DID_ANIMATRONIC, P.CID_ANIM_COMPLETE_NOTIFY)
        r2._on_notify(None, bytearray(good[:6]))      # truncated, no EOP
        for _ in range(5):
            r2._on_notify(None, bytearray(good))
        drained = r2.drain_events()
        self.assertEqual(drained["count"], 5, "a good packet was consumed by the runt")
        self.assertEqual(drained["framing_errors"], 1)

    def test_framing_error_count_is_reported_once(self):
        r2 = make_r2()
        good = notify_packet(P.DID_ANIMATRONIC, P.CID_ANIM_COMPLETE_NOTIFY)
        r2._on_notify(None, bytearray(good[:6]))
        r2._on_notify(None, bytearray(good))
        self.assertEqual(r2.drain_events()["framing_errors"], 1)
        self.assertEqual(r2.drain_events()["framing_errors"], 0)


class TestCommandPacing(unittest.TestCase):
    def test_concurrent_senders_respect_the_command_interval(self):
        """The keepalive runs on its own task every 3 s while the daemon is
        inside an op. Both used to read the same stale _last_tx and transmit
        back to back — measured 0.0 ms apart against a 120 ms floor. R2 drops
        commands sent faster than that, which reads as 'the item didn't work'."""
        async def body():
            r2 = make_r2()
            stamps = []

            class Timed:
                async def write_gatt_char(self, uuid, chunk, response=True):
                    if chunk and chunk[-1] == P.EOP:
                        stamps.append(time.monotonic())

            r2.client = Timed()
            r2._loop = asyncio.get_running_loop()
            await asyncio.gather(*[
                r2.send(P.DID_POWER, P.CID_POWER_WAKE, expect=False)
                for _ in range(4)])
            return stamps

        stamps = run(body())
        self.assertEqual(len(stamps), 4)
        gaps = [b - a for a, b in zip(stamps, stamps[1:])]
        for gap in gaps:
            self.assertGreaterEqual(gap, P.CMD_SAFE_INTERVAL * 0.9,
                                    f"sent {gap*1000:.1f} ms apart, floor is "
                                    f"{P.CMD_SAFE_INTERVAL*1000:.0f} ms")

    def test_packets_are_not_spliced_by_a_concurrent_sender(self):
        """A packet over MTU_CHUNK goes out in several writes; another sender
        interleaving between them would splice two packets on the wire."""
        async def body():
            r2 = make_r2()
            chunks = []

            class Chunky:
                async def write_gatt_char(self, uuid, chunk, response=True):
                    chunks.append(bytes(chunk))
                    await asyncio.sleep(0)      # yield mid-packet

            r2.client = Chunky()
            r2._loop = asyncio.get_running_loop()
            await asyncio.gather(*[
                r2.send(P.DID_IO, P.CID_IO_LEDS_16BIT, b"\x00\x07" + bytes(24),
                        expect=False) for _ in range(3)])
            return chunks

        stream = b"".join(run(body()))
        # Every SOP must be followed by exactly one EOP before the next SOP.
        depth = 0
        for byte in stream:
            if byte == P.SOP:
                self.assertEqual(depth, 0, "a second SOP opened mid-packet")
                depth = 1
            elif byte == P.EOP and depth:
                depth = 0
        self.assertEqual(depth, 0)


class TestStatusTellsTheTruth(GateControl):
    def test_connected_reflects_the_link_not_the_object(self):
        """`r2 is not None` was structurally always True, so the op could never
        report a dropped link — the one thing a status check is for."""
        class Link:
            def __init__(self, up): self.client = type("C", (), {"is_connected": up})()
            idle_disabled = None
            async def get_head(self): return 103.0

        resp, _ = handle({"op": "status"}, "read", Link(True))
        self.assertTrue(resp["data"]["connected"])
        resp, _ = handle({"op": "status"}, "read", Link(False))
        self.assertFalse(resp["data"]["connected"])
        self.assertIsNone(resp["data"]["head"])


class TestNotifyFailsLoudly(VerifiedGate):
    def test_rejected_enable_is_not_reported_as_success(self):
        """A survey that silently failed to enable leg notifications would
        conclude 'R2 never emits leg_action_complete' — a wrong INFERRED
        finding entering docs/."""
        resp, _ = handle({"op": "notify", "params": {"leg": True}}, "motion",
                         FakeR2(err=0x02))
        self.assertFalse(resp["ok"])
        self.assertIn("bad_command_id", resp["error"])
        self.assertIn("NOT evidence", resp["error"])


class TestOnlyOneDaemonCanHoldTheBridge(unittest.TestCase):
    """#17 finding 3. Two daemons wipe each other's queue at startup and then
    race to consume requests, so which `--allow` ceiling applies to a given
    command stops being predictable — a `motion` daemon behind a `read` one
    silently re-arms every op the operator believes is refused."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        # All four, not just the two under test: cmd_daemon reaches for
        # REQ_DIR/RESP_DIR, and a test that wrote to the REAL bridge queue
        # would be indistinguishable from a stray request during a session.
        self.saved = (P.BRIDGE, P.LOCK, P.REQ_DIR, P.RESP_DIR)
        P.BRIDGE = Path(self.tmp.name) / ".bridge"
        P.LOCK = P.BRIDGE / "daemon.lock"
        P.REQ_DIR, P.RESP_DIR = P.BRIDGE / "requests", P.BRIDGE / "responses"

    def tearDown(self):
        P.BRIDGE, P.LOCK, P.REQ_DIR, P.RESP_DIR = self.saved
        self.tmp.cleanup()

    def test_first_acquire_succeeds_and_records_the_ceiling(self):
        self.assertIsNone(P.acquire_daemon_lock("read"))
        held = json.loads(P.LOCK.read_text())
        self.assertEqual(held["pid"], P.os.getpid())
        self.assertEqual(held["ceiling"], "read")

    def test_second_daemon_is_refused_while_the_first_lives(self):
        P.acquire_daemon_lock("read")
        # A different, live pid: pid 1 (launchd) always exists on macOS and is
        # never us. Using a fabricated pid would test _pid_alive, not the lock.
        P.LOCK.write_text(json.dumps({"pid": 1, "ceiling": "read",
                                      "started": time.time()}))
        holder = P.acquire_daemon_lock("motion")
        self.assertIsNotNone(holder, "a second daemon was allowed to start")
        self.assertEqual(holder["pid"], 1)
        self.assertEqual(holder["ceiling"], "read")

    def test_the_refusal_happens_before_the_queue_is_wiped(self):
        """The wipe is what makes a second daemon destructive rather than
        merely redundant, so the guard has to run first. Asserted against the
        real cmd_daemon, not a reimplementation of its order."""
        P.REQ_DIR.mkdir(parents=True, exist_ok=True)
        inflight = P.REQ_DIR / "0001.json"
        inflight.write_text(json.dumps({"op": "status"}))
        P.LOCK.parent.mkdir(parents=True, exist_ok=True)
        P.LOCK.write_text(json.dumps({"pid": 1, "ceiling": "read",
                                      "started": time.time()}))

        args = types.SimpleNamespace(allow="motion", idle_timeout=1.0,
                                     timeout=1.0, name=None, verbose=False)
        rc = run(P.cmd_daemon(args))
        self.assertEqual(rc, 1)
        self.assertTrue(inflight.exists(),
                        "the running daemon's in-flight request was wiped")

    def test_a_stale_lock_does_not_wedge_the_bridge(self):
        """A daemon killed with SIGKILL leaves its lock behind. If that were
        permanent, every later start would refuse until someone deleted a file
        they have no reason to know exists."""
        dead = 999_999            # above /proc pid_max and macOS's default
        self.assertFalse(P._pid_alive(dead))
        P.LOCK.parent.mkdir(parents=True, exist_ok=True)
        P.LOCK.write_text(json.dumps({"pid": dead, "ceiling": "motion",
                                      "started": 0}))
        self.assertIsNone(P.acquire_daemon_lock("read"))
        self.assertEqual(json.loads(P.LOCK.read_text())["pid"], P.os.getpid())

    def test_a_truncated_lock_is_treated_as_stale(self):
        """A lock written by a process that died mid-write must not be more
        durable than one written by a process that died after it."""
        P.LOCK.parent.mkdir(parents=True, exist_ok=True)
        P.LOCK.write_text('{"pid": 12')
        self.assertIsNone(P.acquire_daemon_lock("read"))

    def test_release_only_drops_our_own_lock(self):
        """A daemon exiting slowly must not delete a lock a newer daemon
        legitimately holds — that re-opens the double-daemon window at the one
        moment it looks safest."""
        P.LOCK.parent.mkdir(parents=True, exist_ok=True)
        P.LOCK.write_text(json.dumps({"pid": 1, "ceiling": "motion",
                                      "started": time.time()}))
        P.release_daemon_lock()
        self.assertTrue(P.LOCK.exists(), "deleted a lock belonging to pid 1")

    def test_release_drops_our_lock_so_the_next_start_is_clean(self):
        P.acquire_daemon_lock("read")
        P.release_daemon_lock()
        self.assertFalse(P.LOCK.exists())
        self.assertIsNone(P.acquire_daemon_lock("motion"))

    def test_the_lock_survives_the_startup_wipe(self):
        """The wipe globs *.json. A lock named *.json would delete itself."""
        self.assertFalse(P.LOCK.name.endswith(".json"))

    def test_permission_error_counts_as_alive(self):
        """EPERM means the process exists and belongs to someone else. Reading
        that as 'not there' would hand the bridge to a second daemon."""
        real = P.os.kill
        P.os.kill = lambda pid, sig: (_ for _ in ()).throw(PermissionError())
        try:
            self.assertTrue(P._pid_alive(4242))
        finally:
            P.os.kill = real
class TestStanceIsReadableAtEveryCeiling(GateControl):
    """#11 found an authored animation (EMOTE_YES, a *nod*) ending in TWO_LEGS
    with the stabiliser retracted and nothing restoring it — R2 fell. Knowing
    the stance is what makes the animation survey safe to run, and it must not
    itself require a ceiling that can move him."""

    def test_stance_is_tier_read(self):
        """It is `get_leg_action`, CID 0x25 — a read. `head` already reads the
        same device (DID 0x17) at tier read, so this is the same class."""
        self.assertEqual(P.op_tier("stance"), "read")

    def test_stance_allowed_at_every_ceiling(self):
        for ceiling in P.TIERS:
            resp, _ = handle({"op": "stance"}, ceiling)
            self.assertTrue(resp["ok"], f"stance refused at ceiling {ceiling}")

    def test_stance_reads_and_does_not_write(self):
        resp, r2 = handle({"op": "stance"}, "read")
        self.assertIn(("get_leg_action",), r2.calls)
        for call in r2.calls:
            self.assertNotIn(call[0], {"set_head", "play_animation",
                                       "perform_leg_action", "set_leg_position"})

    def test_every_documented_state_decodes(self):
        expected = {0: "unknown", 1: "three_legs", 2: "two_legs",
                    3: "waddle", 4: "transitioning"}
        for raw, name in expected.items():
            r2 = FakeR2()
            r2.leg_action = raw
            resp, _ = handle({"op": "stance"}, "read", r2)
            self.assertEqual(resp["data"]["state"], name)
            self.assertEqual(resp["data"]["raw"], raw)

    def test_only_three_legs_counts_as_stable(self):
        """`stable` is the field a survey branches on. TRANSITIONING and
        WADDLE are mid-motion, not a stance to trust."""
        for raw in (0, 2, 3, 4):
            r2 = FakeR2()
            r2.leg_action = raw
            resp, _ = handle({"op": "stance"}, "read", r2)
            self.assertFalse(resp["data"]["stable"],
                             f"raw {raw} was reported stable")
        r2 = FakeR2()
        r2.leg_action = P.LEG_STATE_THREE_LEGS
        resp, _ = handle({"op": "stance"}, "read", r2)
        self.assertTrue(resp["data"]["stable"])

    def test_an_undocumented_state_is_surfaced_not_swallowed(self):
        """A state outside the enum is exactly what this project exists to
        find. Reporting it as None would hide it; reporting it as stable would
        be dangerous."""
        r2 = FakeR2()
        r2.leg_action = 9
        resp, _ = handle({"op": "stance"}, "read", r2)
        self.assertEqual(resp["data"]["state"], "undocumented_9")
        self.assertEqual(resp["data"]["raw"], 9)
        self.assertFalse(resp["data"]["stable"])

    def test_no_response_is_not_reported_as_stable(self):
        """An unanswered read means the stance is unknown. Unknown must never
        read as safe — the one direction this must not fail in."""
        r2 = FakeR2()
        r2.leg_action = None
        resp, _ = handle({"op": "stance"}, "read", r2)
        self.assertIsNone(resp["data"]["stable"])
        self.assertIsNone(resp["data"]["state"])
        self.assertIn("UNKNOWN", resp["data"]["note"])

    def test_the_state_enum_is_not_the_command_enum(self):
        """Pins the distinction that produced a wrong published claim: the
        payload decodes against R2DoLegActions (what get_leg_action returns),
        NOT R2LegActions (what perform_leg_action takes). They agree on 1/2/3
        and diverge at 0 and 4."""
        self.assertEqual(P.LEG_STATES[0], "unknown")        # R2LegActions[0] is STOP
        self.assertEqual(P.LEG_STATES[4], "transitioning")  # R2LegActions has no 4


class TestDomeCeilingCannotKnockHimOver(GateControl):
    """The ladder used to top out at `motion`, described as "dome and
    animations". #11 proved that description false in the direction that
    matters: `play_animation` drives leg actions — EMOTE_YES, a *nod*, emitted
    WADDLE x3 and put R2 on the floor, with `perform_leg_action` never called.

    So a session opened to survey the dome could topple the robot while the
    ceiling's own name promised it could not. These tests pin the split."""

    def test_animation_is_above_dome(self):
        self.assertEqual(P.op_tier("dome"), "dome")
        self.assertEqual(P.op_tier("animation"), "stance")
        self.assertGreater(P.TIERS.index("stance"), P.TIERS.index("dome"))

    def test_a_dome_session_cannot_play_an_animation(self):
        """The whole point. #10 ran at this ceiling; had `animation` been
        reachable there, one stray request could have ended it on the floor."""
        resp, r2 = handle({"op": "animation", "params": {"id": 21}}, "dome")
        self.assertFalse(resp["ok"])
        self.assertIn("needs tier 'stance'", resp["error"])
        self.assertEqual(r2.calls, [], "the animation went out anyway")

    def test_a_dome_session_can_still_move_the_dome(self):
        resp, _ = handle({"op": "dome", "params": {"delta": 5}}, "dome")
        self.assertTrue(resp["ok"])

    def test_motion_is_an_alias_that_resolves_DOWN_not_up(self):
        """Someone who typed the old name gets refusals on animation — a
        message on a terminal. Resolving it UP would silently re-grant the
        ability to knock him over. Ambiguity resolves toward less capability."""
        self.assertEqual(P.resolve_tier("motion"), "dome")
        resp, r2 = handle({"op": "animation", "params": {"id": 21}}, "motion")
        self.assertFalse(resp["ok"])
        self.assertEqual(r2.calls, [])

    def test_the_old_name_is_not_a_tier_any_more(self):
        self.assertNotIn("motion", P.TIERS)

    def test_stance_read_is_still_available_at_every_rung(self):
        """Reading the leg state must never require a ceiling that can change
        it — otherwise the only way to find out whether he is stable is to
        open a session that can destabilise him."""
        for ceiling in P.TIERS:
            resp, _ = handle({"op": "stance"}, ceiling)
            self.assertTrue(resp["ok"], f"stance refused at {ceiling}")

    def test_stop_is_still_reachable_from_every_rung(self):
        for ceiling in P.TIERS:
            self.assertIn("stop", P.allowed_ops(ceiling))


class TestStanceWriteCannotReachWaddle(GateControl):
    """#22's op. WADDLE is translation on two tracks with the stabiliser up —
    the thing that put R2 on the floor in #11 — and it is locomotion, which
    belongs to #23 behind a stop-distance criterion that does not exist yet."""

    def test_waddle_is_absent_from_the_mapping_not_merely_branched_around(self):
        """Structural, not behavioural. A guard can be refactored away; a name
        that was never in the table cannot be reached by a typo or an
        off-by-one in the first place."""
        self.assertNotIn("waddle", P.LEG_ACTIONS)
        self.assertNotIn(P.LEG_ACTION_WADDLE, P.LEG_ACTIONS.values())

    def test_waddle_by_name_is_refused_with_a_reason(self):
        resp, r2 = handle({"op": "set_stance", "params": {"action": "waddle"}},
                          "stance")
        self.assertFalse(resp["ok"])
        self.assertIn("locomotion", resp["error"])
        self.assertEqual([c for c in r2.calls if c[0] == "perform_leg_action"],
                         [], "a waddle went out anyway")

    def test_raw_numbers_are_refused(self):
        """`{"action": 3}` is WADDLE. A caller who meant 'the third option'
        would get locomotion, so numbers are not accepted at all."""
        for bad in (0, 1, 2, 3, True, None, [1]):
            resp, r2 = handle({"op": "set_stance", "params": {"action": bad}},
                              "stance")
            self.assertFalse(resp["ok"], f"action={bad!r} was accepted")
            self.assertEqual([c for c in r2.calls if c[0] == "perform_leg_action"], [])

    def test_the_two_real_stances_go_out_with_the_right_byte(self):
        for name, value in (("three_legs", 1), ("two_legs", 2)):
            resp, r2 = handle({"op": "set_stance", "params": {"action": name}},
                              "stance")
            self.assertTrue(resp["ok"], resp.get("error"))
            self.assertIn(("perform_leg_action", value), r2.calls)

    def test_it_is_tier_stance_not_dome(self):
        self.assertEqual(P.op_tier("set_stance"), "stance")
        resp, r2 = handle({"op": "set_stance", "params": {"action": "three_legs"}},
                          "dome")
        self.assertFalse(resp["ok"])
        self.assertIn("needs tier 'stance'", resp["error"])
        self.assertEqual([c for c in r2.calls if c[0] == "perform_leg_action"], [])

    def test_a_rejected_stance_command_fails_loudly(self):
        """A silently-refused stance command during #22 would be recorded as
        'R2 cannot deploy the tripod' — a wrong finding entering docs/."""
        resp, _ = handle({"op": "set_stance", "params": {"action": "three_legs"}},
                         "stance", FakeR2(err=0x02))
        self.assertFalse(resp["ok"])
        self.assertIn("NOT evidence", resp["error"])

    def test_it_reports_measured_before_and_after_not_the_command(self):
        r2 = FakeR2()
        r2.leg_action = P.LEG_STATE_TWO_LEGS
        resp, _ = handle({"op": "set_stance", "params": {"action": "three_legs"}},
                         "stance", r2)
        self.assertEqual(resp["data"]["commanded"], "three_legs")
        self.assertEqual(resp["data"]["before"], "two_legs")
        # The fake does not change state, so `after` must NOT echo the command.
        self.assertEqual(resp["data"]["after_reported"], "two_legs")

    def test_the_after_reading_is_labelled_as_belief_not_observation(self):
        """#11: get_leg_action reports tracked state, not a sensed position.
        A survey that read `after_reported` as proof of a physical tripod
        would record a stance R2 may not be in."""
        resp, _ = handle({"op": "set_stance", "params": {"action": "two_legs"}},
                         "stance")
        self.assertIn("BELIEVES", resp["data"]["note"])


class TestStopAlsoHaltsTheLegs(unittest.TestCase):
    """An animation drives leg actions (#11), and a leg action in flight is the
    state that put R2 on the floor. A stop that leaves the legs moving is not
    a stop."""

    def test_stop_everything_halts_legs_too(self):
        r2 = FakeR2()
        result = run(P.stop_everything(r2))
        self.assertIn(("perform_leg_action", P.LEG_ACTION_STOP), r2.calls)
        self.assertEqual(set(result["results"]), {"animation", "audio", "legs"})
        self.assertTrue(result["stopped"])

    def test_a_failed_leg_stop_makes_the_whole_stop_report_failure(self):
        result = run(P.stop_everything(FakeR2(err=0x02)))
        self.assertFalse(result["stopped"])
        self.assertIn("legs", result["warning"])

    def test_stop_is_still_reachable_from_the_read_ceiling(self):
        """It sends DID 0x17 traffic, but every part of it HALTS motion —
        which is what the read ceiling's promise permits, and is already true
        of stop_animation."""
        resp, r2 = handle({"op": "stop"}, "read")
        self.assertTrue(resp["ok"])
        self.assertIn(("perform_leg_action", P.LEG_ACTION_STOP), r2.calls)


class TestLegPositionIsReadOnlyForNow(GateControl):
    """#22 AC6: learn what the float MEANS from reads before anything writes
    it. It is finer-grained than the stance enum, documented nowhere, and
    drives an actuator that can fell him."""

    def test_leg_pos_is_tier_read(self):
        self.assertEqual(P.op_tier("leg_pos"), "read")

    def test_there_is_no_write_op_for_leg_position(self):
        """Structural. `set_leg_position` must not be reachable at ANY ceiling
        until its semantics are known — so it should not exist as an op at
        all, rather than existing behind a tier."""
        self.assertNotIn("set_leg_position", P.OPS)
        self.assertFalse(hasattr(P, "CID_ANIM_SET_LEG_POSITION"))

    def test_it_reports_the_stance_alongside_the_float(self):
        """The number alone means nothing yet; it is only interpretable next
        to a known stance."""
        r2 = FakeR2()
        r2.leg_position = 12.5
        r2.leg_action = P.LEG_STATE_THREE_LEGS
        resp, _ = handle({"op": "leg_pos"}, "read", r2)
        self.assertEqual(resp["data"]["degrees"], 12.5)
        self.assertEqual(resp["data"]["stance"], "three_legs")
        self.assertIn("UNKNOWN", resp["data"]["note"])

    def test_a_no_response_read_is_none_not_zero(self):
        """0.0 is a plausible leg position. Reporting a failed read as 0.0
        would put a fabricated measurement into #22's table."""
        r2 = FakeR2()
        r2.leg_position = None
        resp, _ = handle({"op": "leg_pos"}, "read", r2)
        self.assertIsNone(resp["data"]["degrees"])


class TestLinkLoss(unittest.TestCase):
    """#17 findings 1, 2 and 4. Before these, a dropped link was invisible:
    nothing noticed, nothing recovered, and the daemon answered every op with
    a failure string forever while the comment in the keepalive's error handler
    claimed "reconnect is handled above". Nothing reconnected."""

    def test_a_fresh_session_has_not_lost_the_link(self):
        r2 = make_r2()
        self.assertFalse(r2.link_lost)
        self.assertIsNone(r2.link_lost_at)

    def test_the_callback_records_when_not_just_that(self):
        """"Did it drop before or after my command?" is the first question of
        any post-mortem, and the daemon log is --verbose-gated and off."""
        r2 = make_r2()
        r2._on_disconnect(None)
        self.assertTrue(r2.link_lost)
        self.assertIsInstance(r2.link_lost_at, float)

    def test_a_second_disconnect_does_not_overwrite_the_first(self):
        # Bleak may call back more than once; the FIRST moment is the useful
        # one. Overwriting it would move the drop later than it happened.
        r2 = make_r2()
        r2._on_disconnect(None)
        first = r2.link_lost_at
        r2._on_disconnect(None)
        self.assertEqual(r2.link_lost_at, first)

    def test_the_callback_never_raises(self):
        # It runs on bleak's delivery thread. An exception there is not
        # something any of our code is positioned to catch.
        r2 = make_r2()
        r2.client = None
        r2._on_disconnect(None)          # must not raise
        self.assertTrue(r2.link_lost)

    def test_send_refuses_fast_instead_of_paying_a_timeout(self):
        """A batched phrase of six steps used to spend a full BLE failure per
        step rediscovering the same dead link."""
        r2 = make_r2()
        r2.client = FakeClient(r2, echo)
        r2._on_disconnect(None)
        with self.assertRaises(P.LinkLost):
            run(r2.send(P.DID_POWER, P.CID_POWER_WAKE))
        self.assertEqual(r2.client.sent, [])     # nothing reached the wire

    def test_link_loss_is_distinguishable_from_a_failed_op(self):
        # Both used to surface as a BleakError string in a response body,
        # which reads as "that command didn't work" rather than "there is no
        # robot on the other end".
        self.assertTrue(issubclass(P.LinkLost, RuntimeError))
        r2 = make_r2()
        r2.client = FakeClient(r2, echo)
        r2._on_disconnect(None)
        try:
            run(r2.send(P.DID_POWER, P.CID_POWER_WAKE))
        except P.LinkLost as e:
            self.assertIn("link lost", str(e).lower())

    def test_the_keepalive_stops_instead_of_spinning_on_a_dead_link(self):
        """Without this it wakes a disconnected robot every 3 s forever."""
        async def scenario():
            r2 = make_r2()
            r2.client = FakeClient(r2, echo)
            r2.start_keepalive(period=0.01)
            r2._on_disconnect(None)
            await asyncio.wait_for(r2._keepalive, timeout=2.0)
            return r2._keepalive.done()
        self.assertTrue(run(scenario()))

    def test_a_transient_keepalive_error_does_NOT_take_the_session_down(self):
        """The other half of the same branch. A single dropped keepalive must
        not end a live session -- that session is the only way to send stop."""
        async def scenario():
            r2 = make_r2()
            calls = []

            async def flaky():
                calls.append(1)
                raise RuntimeError("transient")

            r2.wake = flaky
            r2.start_keepalive(period=0.01)
            await asyncio.sleep(0.08)
            still_running = not r2._keepalive.done()
            r2._keepalive.cancel()
            return still_running, len(calls)
        running, n = run(scenario())
        self.assertTrue(running)
        self.assertGreater(n, 1)          # kept trying

    def test_link_loss_beats_idle_and_traffic_cannot_mask_it(self):
        """Finding 2. `since_last` is reset by every request drained, so a
        polling client holds it near zero forever -- a disconnected daemon
        under that traffic never reaches its idle timeout and never exits."""
        # Busy AND disconnected: the case that used to run forever.
        self.assertEqual(P.daemon_exit_reason(True, 0.0, 900.0), "link_lost")
        # Even a request that arrived this instant does not buy it time.
        self.assertEqual(P.daemon_exit_reason(True, 0.001, 900.0), "link_lost")

    def test_a_link_that_dropped_then_went_idle_reports_the_real_cause(self):
        """Both conditions true is the case that fixes the ORDER, and it is
        the likely one: the link drops, the client gives up, and the session
        then sits idle. Reporting that as a timeout tells the operator their
        session expired when in fact R2 vanished -- the exact misleading
        diagnosis this change exists to remove. It also matters because the
        link-lost branch drains queued requests with an honest error and the
        idle branch does not."""
        self.assertEqual(P.daemon_exit_reason(True, 9999.0, 900.0), "link_lost")

    def test_idle_still_exits_a_healthy_but_unused_session(self):
        self.assertEqual(P.daemon_exit_reason(False, 901.0, 900.0), "idle")
        self.assertIsNone(P.daemon_exit_reason(False, 899.0, 900.0))

    def test_a_healthy_busy_session_keeps_serving(self):
        self.assertIsNone(P.daemon_exit_reason(False, 0.0, 900.0))

    def test_exit_awaits_the_keepalive_it_cancelled(self):
        """Cancel-without-await leaves a wake() writing against a closing
        client and surfaces later as "Task exception was never retrieved"."""
        class ClosingClient(FakeClient):
            def __init__(self, r2, responder):
                super().__init__(r2, responder)
                self.disconnected = False

            async def stop_notify(self, uuid):
                pass

            async def disconnect(self):
                self.disconnected = True

        async def scenario():
            r2 = make_r2()
            r2.client = ClosingClient(r2, echo)
            r2.start_keepalive(period=0.01)
            task = r2._keepalive
            await r2.__aexit__(None, None, None)
            return task.done(), r2.client.disconnected
        done, disconnected = run(scenario())
        self.assertTrue(done)             # awaited, not orphaned
        self.assertTrue(disconnected)


if __name__ == "__main__":
    unittest.main(verbosity=2)
