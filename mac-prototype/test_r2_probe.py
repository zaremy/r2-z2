#!/usr/bin/env python3
"""Tests for the bridge event ring, permission tiers and idle control — issue #7.

Runs WITHOUT hardware, WITHOUT a daemon, and WITHOUT bleak: r2_probe makes the
BLE import optional precisely so the packet layer and the request handler can be
exercised by the stock interpreter.

    python3 -m unittest discover -s mac-prototype -p 'test_*.py' -v
"""

import asyncio
import sys
import time
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


class TestEventsOpTier(unittest.TestCase):
    """AC4: `events` is available at every ceiling — 'read' is the floor."""

    def test_events_allowed_at_every_ceiling(self):
        for ceiling in P.TIERS:
            resp, _ = handle({"op": "events"}, ceiling)
            self.assertTrue(resp["ok"], f"events refused at ceiling {ceiling}")

    def test_events_is_tier_read(self):
        self.assertEqual(P.op_tier("events"), "read")
        self.assertEqual(P.min_tier("events"), "read")

    def test_events_op_drains(self):
        r2 = FakeR2()
        r2.events = {"events": [{"name": "animation_complete"}], "count": 1,
                     "dropped": 0}
        resp, r2 = handle({"op": "events"}, "read", r2)
        self.assertEqual(resp["data"]["count"], 1)
        self.assertIn(("drain_events",), r2.calls)


class TestIdleControl(unittest.TestCase):
    """AC5 (the half that needs no robot): the op sends the right bytes at the
    right tier. That it actually quiets him is a hardware check."""

    def test_disabling_idle_is_allowed_at_read(self):
        """#8 runs `--allow leds` and #9 runs `--allow audio`; both need idle
        off. Gating the disable behind 'motion' would strand them."""
        for ceiling in P.TIERS:
            resp, r2 = handle({"op": "idle", "params": {"enable": False}}, ceiling)
            self.assertTrue(resp["ok"], f"idle-off refused at ceiling {ceiling}")
            self.assertIn(("enable_idle_animations", False), r2.calls)

    def test_idle_defaults_to_disabling(self):
        resp, r2 = handle({"op": "idle"}, "read")
        self.assertTrue(resp["ok"])
        self.assertIn(("enable_idle_animations", False), r2.calls)
        self.assertFalse(resp["data"]["idle_enabled"])

    def test_enabling_idle_needs_the_motion_ceiling(self):
        for ceiling in ("read", "leds", "audio"):
            resp, r2 = handle({"op": "idle", "params": {"enable": True}}, ceiling)
            self.assertFalse(resp["ok"], f"idle-on wrongly allowed at {ceiling}")
            self.assertIn("needs tier 'motion'", resp["error"])
            self.assertEqual(r2.calls, [])       # refused BEFORE any command
        resp, r2 = handle({"op": "idle", "params": {"enable": True}}, "motion")
        self.assertTrue(resp["ok"])
        self.assertIn(("enable_idle_animations", True), r2.calls)

    def test_idle_records_the_state_it_left_r2_in(self):
        _, r2 = handle({"op": "idle", "params": {"enable": False}}, "read")
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

        resp, r2 = handle({"op": "idle", "params": {"enable": False}}, "read",
                          Silent())
        self.assertFalse(resp["ok"])
        self.assertIn("no response", resp["error"])
        self.assertIn("UNKNOWN", resp["error"])
        self.assertIsNone(r2.idle_disabled, "claimed a state R2 never confirmed")

    def test_rejected_disable_fails_loudly(self):
        """CID 0x2C is single-source and unproven — bad_command_id is a live
        possibility on this firmware, not a hypothetical."""
        resp, r2 = handle({"op": "idle", "params": {"enable": False}}, "read",
                          FakeR2(err=0x02))
        self.assertFalse(resp["ok"])
        self.assertIn("bad_command_id", resp["error"])
        self.assertIsNone(r2.idle_disabled)

    def test_tier_gate_and_handler_read_the_same_default(self):
        """The gate authorises on one derivation and the handler acts on
        another. If those defaults ever diverge, a read-tier request commands
        motion — so pin them to the single shared helper."""
        self.assertIs(P._idle_enable({}), False)
        self.assertEqual(P._idle_tier({}), "read")
        _, r2 = handle({"op": "idle"}, "read")
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


class TestNotifyOp(unittest.TestCase):

    def test_enables_both_notifies(self):
        resp, r2 = handle({"op": "notify",
                           "params": {"leg": True, "head_reset": True}}, "read")
        self.assertTrue(resp["ok"])
        self.assertIn(("enable_leg_action_notify", True), r2.calls)
        self.assertIn(("enable_head_reset_notify", True), r2.calls)

    def test_empty_params_is_an_error_not_a_silent_noop(self):
        resp, r2 = handle({"op": "notify"}, "read")
        self.assertFalse(resp["ok"])
        self.assertIn("nothing to enable", resp["error"])
        self.assertEqual(r2.calls, [])

    def test_reports_only_the_channels_it_actually_enabled(self):
        """Upstream has no setter for animation-complete (animatronic.py:43),
        so the op must not imply it turned anything else on. The previous
        assertion looked for a key named 'animation' that no regression would
        ever produce — it could not fail."""
        resp, _ = handle({"op": "notify", "params": {"leg": True}}, "read")
        self.assertEqual(set(resp["data"]) - {"note"}, {"leg"})
        self.assertIn("animation_complete has no enable", resp["data"]["note"])
        resp, _ = handle({"op": "notify",
                          "params": {"leg": True, "head_reset": True}}, "read")
        self.assertEqual(set(resp["data"]) - {"note"}, {"leg", "head_reset"})

    def test_a_bad_param_does_not_half_execute(self):
        """Validating as it went put the leg notify on the wire and THEN
        returned ok:false — a refusal that had already changed robot state."""
        resp, r2 = handle({"op": "notify",
                           "params": {"leg": True, "head_reset": "yes"}}, "read")
        self.assertFalse(resp["ok"])
        self.assertIn("must be JSON true or false", resp["error"])
        self.assertEqual(r2.calls, [], "leg notify was sent before validation")

    def test_non_boolean_leg_is_refused(self):
        for bad in ["true", "false", 1, 0, None]:
            resp, r2 = handle({"op": "notify", "params": {"leg": bad}}, "read")
            self.assertFalse(resp["ok"], f"accepted {bad!r}")
            self.assertEqual(r2.calls, [])


class TestRequestEnvelope(unittest.TestCase):
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
        resp, r2 = handle({"op": "idle", "params": [1, 2]}, "read")
        self.assertFalse(resp["ok"])
        self.assertIn("'params' must be a JSON object", resp["error"])
        self.assertEqual(r2.calls, [])


class TestNothingCanWedgeTheQueueLoop(unittest.TestCase):
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


class TestHandlerBasics(unittest.TestCase):

    def test_unknown_op_lists_what_is_allowed(self):
        resp, _ = handle({"op": "nope"}, "read")
        self.assertFalse(resp["ok"])
        self.assertIn("events", resp["allowed"])

    def test_stop_halts_both_animation_and_audio(self):
        """Asserting only stop_animation let stop_audio be deleted from the op
        with the suite still green. `stop` is where 'default to STOP' lives."""
        resp, r2 = handle({"op": "stop"}, "read")
        self.assertTrue(resp["ok"])
        self.assertEqual(r2.calls, [("stop_animation",), ("stop_audio",)])

    def test_motion_ops_refused_at_read_FOR_THE_RIGHT_REASON(self):
        """Assert the refusal REASON. `assertFalse(resp["ok"])` alone passed
        even with `_tier_ok` stubbed to return True, because FakeR2 lacked the
        motion methods and the resulting AttributeError also produced ok:False.
        The permission ladder could be deleted wholesale and this stayed green."""
        for op, params in (("dome", {"delta": 5}), ("animation", {"id": 1})):
            resp, r2 = handle({"op": op, "params": params}, "read")
            self.assertFalse(resp["ok"])
            self.assertIn("needs tier 'motion'", resp["error"])
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
        resp, _ = handle({"op": "idle"}, "read", Boom())
        self.assertFalse(resp["ok"])
        self.assertIn("RuntimeError: radio gone", resp["error"])

    def test_err_of_names_an_unknown_code_instead_of_null(self):
        self.assertEqual(P._err_of(None), "no response")
        ok = P.Response(1, 0x17, 0x05, 1, 0x00, b"")
        self.assertEqual(P._err_of(ok), "success")
        weird = P.Response(1, 0x17, 0x05, 1, 0x7F, b"")
        self.assertEqual(P._err_of(weird), "unknown_error_0x7f")


class TestDomeTravelCapCannotBeDefeated(unittest.TestCase):
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

    def test_exit_path_catches_basexception(self):
        import inspect
        source = inspect.getsource(P.cmd_daemon)
        epilogue = source[source.index("Default to STOP"):]
        self.assertIn("except BaseException", epilogue)
        self.assertIn("asyncio.shield", epilogue)


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


class TestStatusTellsTheTruth(unittest.TestCase):
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


class TestNotifyFailsLoudly(unittest.TestCase):
    def test_rejected_enable_is_not_reported_as_success(self):
        """A survey that silently failed to enable leg notifications would
        conclude 'R2 never emits leg_action_complete' — a wrong INFERRED
        finding entering docs/."""
        resp, _ = handle({"op": "notify", "params": {"leg": True}}, "read",
                         FakeR2(err=0x02))
        self.assertFalse(resp["ok"])
        self.assertIn("bad_command_id", resp["error"])
        self.assertIn("NOT evidence", resp["error"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
