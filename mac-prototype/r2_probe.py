#!/usr/bin/env python3
"""r2_probe.py — safe bring-up diagnostic for the Sphero R2-D2.

Deliberately does NOT depend on spherov2. The V2 packet layer is reimplemented
here from source-traced constants (see docs/research/r2-protocol.md) so that the
exact same logic can be ported to C for the ESP32 backpack, and so a bleak
version bump in the upstream clone cannot break bring-up.

SAFETY MODEL
  Default action is `scan` — passive, no connection.
  `info` connects read-only: handshake, subscribe, query battery/firmware.
  Every actuator test is individually opt-in via its own subcommand.
  There is NO locomotion command in this tool. Drive tests come later, on
  purpose, after dome/LED/audio have proven the command path.

Usage:
    python r2_probe.py scan
    python r2_probe.py info
    python r2_probe.py led --color 0,0,255
    python r2_probe.py sound
    python r2_probe.py dome --angle 20
    python r2_probe.py animation --id 35        # WWM_CURIOUS
"""

from __future__ import annotations

import argparse
import asyncio
import collections
import json
import math
import os
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

try:
    from bleak import BleakClient, BleakScanner
except ImportError:  # pragma: no cover — only on a host without the BLE stack
    # The packet layer, event ring, permission tiers and request handler are
    # pure Python. Keeping bleak optional lets all of that be unit-tested with
    # the stock system interpreter, instead of only inside mac-prototype/.venv.
    # Anything that actually touches the radio fails loudly at call time below.
    BleakClient = BleakScanner = None

NO_BLEAK = ("bleak is not installed in this interpreter, so this host cannot "
            "talk to R2. Use the ./r2 launcher (it runs mac-prototype/.venv "
            "inside R2Probe.app); plain `python3` can only run the tests.")

# ── BLE identity ─────────────────────────────────────────────────────────────
# OBSERVED: spherov2/toy/r2d2.py:15 — ToyType('R2-D2', 'D2-', 'D2', .12)
NAME_PREFIX = "D2-"

# OBSERVED: spherov2/toy/__init__.py:131 (ToyV2._send_uuid/_response_uuid)
#           and claude-r2d2-buddy/main/common.h:27 (R2D2_CMD_CHR)
API_UUID = "00010002-574f-4f20-5370-6865726f2121"
# OBSERVED: spherov2/toy/bb9e.py:22 (BB9E._handshake), common.h:35 (R2D2_CONNECT_CHR)
HANDSHAKE_UUID = "00020005-574f-4f20-5370-6865726f2121"
HANDSHAKE_PAYLOAD = b"usetheforce...band"

# OBSERVED: spherov2/toy/r2d2.py:15 — cmd_safe_interval = .12
CMD_SAFE_INTERVAL = 0.12
MTU_CHUNK = 20  # OBSERVED: spherov2/toy/__init__.py:79

# ── Packet layer (spherov2/controls/v2.py:14-159) ────────────────────────────
SOP, EOP, ESC = 0x8D, 0xD8, 0xAB
ESC_ESC, ESC_SOP, ESC_EOP = 0x23, 0x05, 0x50

FLAG_IS_RESPONSE = 0x01
FLAG_REQUESTS_RESPONSE = 0x02
FLAG_IS_ACTIVITY = 0x08

# Device IDs — OBSERVED from each commands/*.py `_did`
DID_POWER = 0x13        # commands/power.py:63
DID_ANIMATRONIC = 0x17  # commands/animatronic.py:29
DID_IO = 0x1A           # commands/io.py:52
DID_SYSTEM_INFO = 0x11  # commands/system_info.py

CID_POWER_WAKE = 0x0D          # power.py — wake
CID_POWER_SLEEP = 0x01         # power.py — sleep (NEVER use as keepalive)
CID_POWER_BATTERY_VOLTAGE = 0x03
CID_ANIM_PLAY = 0x05           # animatronic.py:33
CID_ANIM_SET_HEAD = 0x0F       # animatronic.py:41  (CID 15)
CID_ANIM_GET_HEAD = 0x14       # animatronic.py:47  (CID 20)
CID_ANIM_STOP = 0x2B           # animatronic.py:69  (CID 43)
CID_IO_PLAY_AUDIO = 0x07       # io.py:60
CID_IO_SET_VOLUME = 0x08       # io.py:64
CID_IO_STOP_AUDIO = 0x0A       # io.py:72
CID_IO_LEDS_16BIT = 0x0E       # io.py:76  (CID 14) — the variant BB9E/R2D2 expose
CID_IO_LEDS_8BIT = 0x1C        # io.py:88  (CID 28) — marked "Untested" upstream

# Robot-initiated behaviour — enable commands and the notifies they turn on.
#
# SINGLE-SOURCE. Every other constant in this file is corroborated by at least
# two independent implementations (see docs/research/r2-protocol.md), but these
# four appear ONLY in spherov2: the claude-r2d2-buddy C firmware never disables
# idle, and freer2 only ever sends DID 0x17 CIDs 0x05/0x0D/0x0F. Treat them as
# INFERRED until hardware confirms — that is issue #7's acceptance criterion 5.
CID_ANIM_ENABLE_LEG_NOTIFY = 0x2A        # animatronic.py:64  (CID 42)
CID_ANIM_ENABLE_IDLE = 0x2C              # animatronic.py:72  (CID 44)
CID_ANIM_ENABLE_HEAD_RESET_NOTIFY = 0x39  # animatronic.py:84 (CID 57)

# Notification CIDs the robot sends unprompted.
CID_ANIM_COMPLETE_NOTIFY = 0x11     # animatronic.py:43 — (23, 17, 0xff)
CID_ANIM_LEG_COMPLETE_NOTIFY = 0x26  # animatronic.py:61 — (23, 38, 0xff)
CID_ANIM_HEAD_RESET_NOTIFY = 0x3A   # animatronic.py:87 — (23, 58, 0xff)
DID_SENSOR = 0x18                   # sensor.py:81

# OBSERVED: every `*_notify` tuple in spherov2/commands/ ends in 0xff.
# CORROBORATED: freer2/index.js:137 matches an unsolicited packet against
# [0x8D, 0x00, 0x18, 0x02, 0xFF] — SOP, flags=0x00, DID, CID, seq=0xFF.
# Note flags=0x00 there: a notification is NOT flagged is_response, so parse()
# correctly leaves its payload intact instead of eating a byte as an error code.
NOTIFY_SEQ = 0xFF

# Our own sequence counter is `% 0xFF`, i.e. 0..254, so it can never collide
# with 0xFF. That is what makes "unmatched" a safe test for "robot-initiated".
KNOWN_NOTIFIES = {
    (DID_ANIMATRONIC, CID_ANIM_COMPLETE_NOTIFY): "animation_complete",
    (DID_ANIMATRONIC, CID_ANIM_LEG_COMPLETE_NOTIFY): "leg_action_complete",
    (DID_ANIMATRONIC, CID_ANIM_HEAD_RESET_NOTIFY): "head_reset_to_zero",
    (DID_SENSOR, 0x02): "sensor_stream",
}

EVENT_RING_SIZE = 200

# LED bit indices — OBSERVED: spherov2/toy/r2d2.py:17-25
LED_FRONT_R, LED_FRONT_G, LED_FRONT_B = 0, 1, 2
LED_LOGIC = 3
LED_BACK_R, LED_BACK_G, LED_BACK_B = 4, 5, 6
LED_HOLO = 7


def packet_chk(payload: bytes) -> int:
    """OBSERVED: spherov2/helper.py — 0xff - (sum & 0xff)."""
    return 0xFF - (sum(payload) & 0xFF)


def escape(body: bytes) -> bytes:
    out = bytearray()
    for c in body:
        if c == ESC:
            out += bytes((ESC, ESC_ESC))
        elif c == SOP:
            out += bytes((ESC, ESC_SOP))
        elif c == EOP:
            out += bytes((ESC, ESC_EOP))
        else:
            out.append(c)
    return bytes(out)


def unescape(body: bytes) -> bytes:
    out = bytearray()
    it = iter(body)
    for c in it:
        if c == ESC:
            n = next(it, None)
            c = {ESC_ESC: ESC, ESC_SOP: SOP, ESC_EOP: EOP}.get(n)
            if c is None:
                raise ValueError(f"bad escape sequence 0xAB {n:#04x}")
        out.append(c)
    return bytes(out)


def build(did: int, cid: int, seq: int, data: bytes = b"") -> bytes:
    """Build a V2 command packet. Mirrors Packet.build()."""
    flags = FLAG_REQUESTS_RESPONSE | FLAG_IS_ACTIVITY  # 0x0A
    body = bytes((flags, did, cid, seq)) + data
    body += bytes((packet_chk(body),))
    return bytes((SOP,)) + escape(body) + bytes((EOP,))


@dataclass
class Response:
    flags: int
    did: int
    cid: int
    seq: int
    err: int
    data: bytes

    ERRORS = {
        0x00: "success", 0x01: "bad_device_id", 0x02: "bad_command_id",
        0x03: "not_yet_implemented", 0x04: "command_is_restricted",
        0x05: "bad_data_length", 0x06: "command_failed",
        0x07: "bad_parameter_value", 0x08: "busy", 0x09: "bad_target_id",
        0x0A: "target_unavailable",
    }

    def __str__(self) -> str:
        return (f"DID={self.did:#04x} CID={self.cid:#04x} seq={self.seq} "
                f"err={self.ERRORS.get(self.err, hex(self.err))} data={self.data.hex() or '-'}")


def parse(raw: bytes) -> Response:
    if raw[0] != SOP or raw[-1] != EOP:
        raise ValueError("packet framing error")
    body = unescape(raw[1:-1])
    payload, chk = body[:-1], body[-1]
    if packet_chk(payload) != chk:
        raise ValueError("bad checksum")
    flags = payload[0]
    i = 1
    if flags & 0x10:  # has_target_id
        i += 1
    if flags & 0x20:  # has_source_id
        i += 1
    did, cid, seq = payload[i], payload[i + 1], payload[i + 2]
    rest = payload[i + 3:]
    err = 0
    if flags & FLAG_IS_RESPONSE:
        err, rest = rest[0], rest[1:]
    return Response(flags, did, cid, seq, err, bytes(rest))


class R2:
    """Minimal R2-D2 session. Async context manager; disconnects cleanly."""

    def __init__(self, device, verbose: bool = True):
        self.device = device
        # None when bleak is missing. Constructing an R2 stays legal so the
        # pure logic is testable; __aenter__ is where it fails loudly.
        self.client = BleakClient(device) if BleakClient is not None else None
        self.verbose = verbose
        self._seq = 0
        self._rx = bytearray()
        self._waiters: dict[tuple[int, int, int], asyncio.Future] = {}
        self._last_tx = 0.0
        self._keepalive: asyncio.Task | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        # Packets nobody was waiting for. Before this existed they were dropped
        # on the floor, which made every robot-initiated notification invisible.
        self._events: collections.deque = collections.deque(maxlen=EVENT_RING_SIZE)
        self._events_dropped = 0
        # None = never touched this session; True = we disabled native idle.
        self.idle_disabled: bool | None = None

    async def __aenter__(self) -> "R2":
        if self.client is None:
            raise RuntimeError(NO_BLEAK)
        self._loop = asyncio.get_running_loop()
        await self.client.connect()
        self._log(f"connected to {self.device.name} [{self.device.address}]")
        # OBSERVED: BB9E._handshake writes the anti-DoS magic BEFORE the main
        # service is usable. claude-r2d2-buddy/main/r2d2_central.c:106 notes the
        # MAIN_SERVICE is only exposed after this write.
        await self.client.write_gatt_char(HANDSHAKE_UUID, HANDSHAKE_PAYLOAD, response=True)
        self._log("anti-DoS handshake written")
        await self.client.start_notify(API_UUID, self._on_notify)
        self._log("subscribed to API notifications")
        return self

    async def __aexit__(self, *exc) -> None:
        if self._keepalive:
            self._keepalive.cancel()
        try:
            await self.client.stop_notify(API_UUID)
        except Exception:
            pass
        await self.client.disconnect()
        self._log("disconnected")

    def _log(self, msg: str) -> None:
        if self.verbose:
            print(f"  [r2] {msg}")

    def _on_notify(self, _char, data: bytearray) -> None:
        # R2 may deliver a packet across several notifications, or even
        # byte-by-byte (claude-r2d2-buddy/main/r2d2_central.c:68).
        #
        # This runs on bleak's CoreBluetooth dispatch queue, NOT the asyncio
        # loop thread. Completing a Future directly from here is thread-unsafe
        # and can wedge the loop, which is exactly what it did: the daemon's
        # poll loop went silent while the delegate sat in sock_send on a full
        # self-pipe. Hand every completion back via call_soon_threadsafe.
        for b in data:
            if not self._rx and b != SOP:
                continue
            self._rx.append(b)
            if b == EOP:
                raw, self._rx = bytes(self._rx), bytearray()
                try:
                    resp = parse(raw)
                except Exception as e:          # not just ValueError — a short
                    self._log(f"undecodable rx {raw.hex()}: {e}")   # packet raises IndexError
                    continue
                self._log(f"rx {resp}")
                self._complete(resp)

    def _complete(self, resp: "Response") -> None:
        fut = self._waiters.pop((resp.did, resp.cid, resp.seq), None)
        if fut is None:
            # Nobody asked for this. Either the robot spoke on its own
            # (seq 0xff) or a response arrived after its send() timed out.
            # Both used to be discarded here; keep them instead.
            self._record_event(resp)
            return
        if fut.done():
            self._record_event(resp, note="duplicate response")
            return
        loop = self._loop
        if loop is None or not loop.is_running():
            return
        loop.call_soon_threadsafe(lambda: fut.done() or fut.set_result(resp))

    def _record_event(self, resp: "Response", note: str | None = None) -> None:
        """Append to the bounded ring. Runs on bleak's CoreBluetooth dispatch
        thread, so it must not touch the event loop — `deque.append` with a
        maxlen is a single atomic bytecode under the GIL, which both makes it
        lock-free and evicts the oldest entry for us.

        `_events_dropped` is diagnostic only: len() and append() are not atomic
        *as a pair*, so it can be off by one against a concurrent drain. That is
        acceptable for a counter whose job is "did we lose anything, roughly".
        """
        if len(self._events) == EVENT_RING_SIZE:
            self._events_dropped += 1
        event = {
            "t": time.time(),
            "name": KNOWN_NOTIFIES.get((resp.did, resp.cid)),
            "did": resp.did,
            "cid": resp.cid,
            "seq": resp.seq,
            "flags": resp.flags,
            "err": resp.err,
            "data": resp.data.hex(),
            "unsolicited": resp.seq == NOTIFY_SEQ,
        }
        if note:
            event["note"] = note
        self._events.append(event)

    def drain_events(self) -> dict:
        """Take everything in the ring and hand it over, oldest first.

        popleft() is atomic, so draining one at a time cannot race the BLE
        thread's appends the way `list(ring); ring.clear()` would."""
        out = []
        while True:
            try:
                out.append(self._events.popleft())
            except IndexError:
                break
        dropped = self._events_dropped
        self._events_dropped -= dropped   # not `= 0`: don't lose a concurrent
        return {"events": out, "count": len(out), "dropped": dropped}   # bump

    async def send(self, did: int, cid: int, data: bytes = b"",
                   expect: bool = True, timeout: float = 5.0) -> Response | None:
        seq = self._seq
        self._seq = (self._seq + 1) % 0xFF
        pkt = build(did, cid, seq, data)

        # Honour the 120 ms R2-D2 command interval.
        gap = time.monotonic() - self._last_tx
        if gap < CMD_SAFE_INTERVAL:
            await asyncio.sleep(CMD_SAFE_INTERVAL - gap)

        fut: asyncio.Future | None = None
        if expect:
            fut = asyncio.get_running_loop().create_future()
            self._waiters[(did, cid, seq)] = fut

        self._log(f"tx {pkt.hex()}  (DID={did:#04x} CID={cid:#04x} seq={seq})")
        for i in range(0, len(pkt), MTU_CHUNK):
            await self.client.write_gatt_char(API_UUID, pkt[i:i + MTU_CHUNK], response=True)
        self._last_tx = time.monotonic()

        if fut is None:
            return None
        try:
            return await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            self._waiters.pop((did, cid, seq), None)
            self._log(f"no response to DID={did:#04x} CID={cid:#04x} within {timeout}s")
            return None

    async def wake(self) -> Response | None:
        """DID=0x13 CID=0x0D. Idempotent; also resets the inactivity timer."""
        return await self.send(DID_POWER, CID_POWER_WAKE)

    def start_keepalive(self, period: float = 3.0) -> None:
        """Re-send wake periodically. OBSERVED r2d2_central.c:390 uses 3 s.
        NOTE CID 0x01 is SLEEP — never use it here."""
        async def loop():
            while True:
                await asyncio.sleep(period)
                try:
                    await self.wake()
                except Exception as e:
                    # A dropped keepalive must never take the session down —
                    # the poll loop keeps serving and reconnect is handled above.
                    self._log(f"keepalive error (continuing): {e}")
        self._keepalive = asyncio.create_task(loop())

    async def set_leds(self, mapping: dict[int, int]) -> Response | None:
        """16-bit-mask LED write. OBSERVED: spherov2/commands/io.py:76 —
        payload is [mask_hi, mask_lo, *values], values ordered by ascending bit.
        CORROBORATED: claude-r2d2-buddy/main/translator.c:66 sends DID=0x1A
        CID=0x0E with mask 0x0007 (front RGB) / 0x0070 (back RGB) on real
        hardware. The 8-bit variant (CID 0x1C) is flagged "Untested" upstream,
        so this is the one to trust."""
        mask = 0
        for bit in mapping:
            mask |= 1 << bit
        values = bytes(mapping[b] for b in sorted(mapping))
        return await self.send(DID_IO, CID_IO_LEDS_16BIT,
                               mask.to_bytes(2, "big") + values)

    async def play_sound(self, sound_id: int, mode: int = 0) -> Response | None:
        """OBSERVED: io.py:60 — [sound_hi, sound_lo, playback_mode], big-endian."""
        return await self.send(DID_IO, CID_IO_PLAY_AUDIO,
                               sound_id.to_bytes(2, "big") + bytes((mode,)))

    async def set_volume(self, volume: int) -> Response | None:
        return await self.send(DID_IO, CID_IO_SET_VOLUME, bytes((volume & 0xFF,)))

    async def stop_audio(self) -> Response | None:
        return await self.send(DID_IO, CID_IO_STOP_AUDIO)

    async def set_head(self, degrees: float) -> Response | None:
        """OBSERVED: animatronic.py:41 — struct.pack('>f', head_position)."""
        return await self.send(DID_ANIMATRONIC, CID_ANIM_SET_HEAD,
                               struct.pack(">f", float(degrees)))

    async def get_head(self) -> float | None:
        r = await self.send(DID_ANIMATRONIC, CID_ANIM_GET_HEAD)
        if r and len(r.data) == 4:
            return struct.unpack(">f", r.data)[0]
        return None

    async def play_animation(self, animation_id: int) -> Response | None:
        """OBSERVED: animatronic.py:33 — to_bytes(animation, 2), big-endian."""
        return await self.send(DID_ANIMATRONIC, CID_ANIM_PLAY,
                               animation_id.to_bytes(2, "big"))

    async def stop_animation(self) -> Response | None:
        return await self.send(DID_ANIMATRONIC, CID_ANIM_STOP)

    async def battery_voltage(self) -> Response | None:
        return await self.send(DID_POWER, CID_POWER_BATTERY_VOLTAGE)

    async def enable_idle_animations(self, enable: bool) -> Response | None:
        """Turn R2's own idle loop on or off. OBSERVED: animatronic.py:72-73 —
        CID 44, payload [int(enable)].

        This is the survey precondition. R2 fidgets on his own, so with idle
        left on, a recorder cannot tell a spontaneous twitch from the response
        to the command it just sent — every observation is suspect."""
        return await self.send(DID_ANIMATRONIC, CID_ANIM_ENABLE_IDLE,
                               bytes((1 if enable else 0,)))

    async def enable_leg_action_notify(self, enable: bool) -> Response | None:
        """OBSERVED: animatronic.py:64 — CID 42, payload [int(enable)]."""
        return await self.send(DID_ANIMATRONIC, CID_ANIM_ENABLE_LEG_NOTIFY,
                               bytes((1 if enable else 0,)))

    async def enable_head_reset_notify(self, enable: bool) -> Response | None:
        """OBSERVED: animatronic.py:84 — CID 57, payload [int(enable)]."""
        return await self.send(DID_ANIMATRONIC,
                               CID_ANIM_ENABLE_HEAD_RESET_NOTIFY,
                               bytes((1 if enable else 0,)))


# ── CLI ──────────────────────────────────────────────────────────────────────

async def find(timeout: float = 10.0, name: str | None = None):
    if BleakScanner is None:
        raise RuntimeError(NO_BLEAK)
    print(f"scanning {timeout:.0f}s for BLE devices named '{name or NAME_PREFIX}*' ...")
    devices = await BleakScanner.discover(timeout=timeout)
    hits = [d for d in devices
            if d.name and d.name.startswith(name or NAME_PREFIX)]
    return hits, devices


async def cmd_scan(args) -> int:
    hits, all_devices = await find(args.timeout, args.name)
    print(f"\n{len(all_devices)} BLE devices seen; {len(hits)} matching:")
    for d in hits:
        print(f"  ★ {d.name}  {d.address}")
    if not hits:
        print("  (none)")
        print("\nIf R2 is powered on and not connected to a phone, try:")
        print("  • place him briefly on the charger to wake him")
        print("  • close the Sphero app / forget the device on any paired phone")
        print("  • move within ~2 m and rescan with --timeout 20")
        named = [d for d in all_devices if d.name]
        if named:
            print(f"\n  (named devices seen: {', '.join(sorted(d.name for d in named)[:15])})")
        return 1
    return 0


async def with_r2(args, body) -> int:
    hits, _ = await find(args.timeout, args.name)
    if not hits:
        print("R2-D2 not found — run `r2_probe.py scan` first.")
        return 1
    async with R2(hits[0]) as r2:
        await r2.wake()
        await asyncio.sleep(0.5)
        await body(r2)
    return 0


async def cmd_info(args) -> int:
    async def body(r2: R2):
        print("\n-- read-only queries --")
        bat = await r2.battery_voltage()
        print(f"battery voltage raw : {bat.data.hex() if bat else 'no response'}")
        head = await r2.get_head()
        print(f"head position       : {head if head is not None else 'no response'}")
        svcs = r2.client.services
        print(f"\n-- GATT services ({len(list(svcs))}) --")
        for s in svcs:
            print(f"  {s.uuid}")
            for c in s.characteristics:
                print(f"    {c.uuid}  {','.join(c.properties)}")
    return await with_r2(args, body)


async def cmd_led(args) -> int:
    r, g, b = (int(x) for x in args.color.split(","))
    async def body(r2: R2):
        print(f"\nsetting FRONT + BACK LEDs to ({r},{g},{b}) for 3 s")
        await r2.set_leds({LED_FRONT_R: r, LED_FRONT_G: g, LED_FRONT_B: b,
                           LED_BACK_R: r, LED_BACK_G: g, LED_BACK_B: b})
        await asyncio.sleep(3)
        print("clearing LEDs")
        await r2.set_leds({LED_FRONT_R: 0, LED_FRONT_G: 0, LED_FRONT_B: 0,
                           LED_BACK_R: 0, LED_BACK_G: 0, LED_BACK_B: 0})
    return await with_r2(args, body)


async def cmd_sound(args) -> int:
    async def body(r2: R2):
        print(f"\nvolume -> {args.volume}, playing sound id {args.id}")
        await r2.set_volume(args.volume)
        await r2.play_sound(args.id)
        await asyncio.sleep(3)
        await r2.stop_audio()
    return await with_r2(args, body)


MAX_HEAD_MOVE = 45.0   # degrees of travel permitted in one commanded move
HEAD_MIN, HEAD_MAX = -162.0, 182.0  # r2d2.py:471 extended_sensors r2_head_angle


async def bounded_head_move(r2: "R2", *, delta: float | None = None,
                            angle: float | None = None) -> tuple[float, float]:
    """Move the dome, bounding the DISTANCE TRAVELLED, not the destination.

    `set_head_position` is absolute, and the dome does not rest at 0 — this unit
    was found at 103°. Clamping the target to ±45 would therefore command a
    ~150° swing as a "small test". Bound the travel instead, from a freshly read
    position. Returns (start, commanded_target)."""
    start = await r2.get_head()
    if start is None:
        raise RuntimeError("could not read head position; refusing to move blind")
    target = start + delta if delta is not None else angle
    travel = target - start
    if abs(travel) > MAX_HEAD_MOVE:
        target = start + math.copysign(MAX_HEAD_MOVE, travel)
    target = max(HEAD_MIN, min(HEAD_MAX, target))
    await r2.set_head(target)
    return start, target


async def cmd_dome(args) -> int:
    async def body(r2: R2):
        start, target = await bounded_head_move(r2, delta=args.delta)
        print(f"\nhead was {start:.1f}° → commanded {target:.1f}° "
              f"(travel {target - start:+.1f}°, capped at ±{MAX_HEAD_MOVE:.0f}°)")
        await asyncio.sleep(2.0)
        print(f"head now {await r2.get_head():.1f}°; returning")
        await bounded_head_move(r2, angle=start)
        await asyncio.sleep(2.0)
        print(f"head back at {await r2.get_head():.1f}°")
    return await with_r2(args, body)


async def cmd_animation(args) -> int:
    async def body(r2: R2):
        print(f"\nplaying animation id {args.id} (ctrl-C to abort)")
        await r2.play_animation(args.id)
        await asyncio.sleep(args.seconds)
        await r2.stop_animation()
        print("animation stopped")
    return await with_r2(args, body)


# ── Bridge daemon ────────────────────────────────────────────────────────────
# macOS attributes a Bluetooth request to the *responsible* process. Under
# Claude Code that is claude.app (com.anthropic.claude-code), whose bundle has
# no NSBluetoothAlwaysUsageDescription, so any child touching CoreBluetooth is
# killed — regardless of R2Probe.app carrying the key. Terminal.app is an Apple
# system app and is exempt, which is why it works there.
#
# So: the human starts `./r2 daemon` once from Terminal. It holds one R2
# session and serves ops from a file queue. An agent (or any script) drives it
# with `./r2 send <op>` without needing Bluetooth itself.

BRIDGE = Path(__file__).parent / ".bridge"
REQ_DIR, RESP_DIR = BRIDGE / "requests", BRIDGE / "responses"

# Permission ladder — mirrors the fixed bring-up order in CLAUDE.md:
# read-only → LEDs → audio → small dome → stance → locomotion.
# The daemon is started with a ceiling; ops above it are refused, not executed.
TIERS = ["read", "leds", "audio", "motion"]


def _tier_ok(ceiling: str, needed: str) -> bool:
    return TIERS.index(needed) <= TIERS.index(ceiling)


def _strict_bool(value, field: str) -> bool:
    """Accept only a real JSON boolean.

    Deliberately unforgiving: `{"enable": "false"}` is a truthy Python string,
    so a lenient reading of it would ENABLE idle motion — the exact opposite of
    what the caller typed. Guessing in the direction of motion is not allowed
    here. (Same class of bug as the survey harness accepting a bool where it
    wanted an int; JSON's types are not Python's.)"""
    if isinstance(value, bool):
        return value
    raise ValueError(
        f"'{field}' must be JSON true or false, got {value!r} "
        f"({type(value).__name__}). Refusing to guess.")


def _err_of(r: "Response | None") -> str:
    """Human-readable result of a command. `Response.ERRORS.get(err)` alone
    renders an unrecognised code as null, which reads like success."""
    if r is None:
        return "no response"
    return r.ERRORS.get(r.err, f"unknown_error_{r.err:#04x}")


async def _op_status(r2, p):
    return {"connected": r2 is not None, "head": await r2.get_head() if r2 else None}

async def _op_battery(r2, p):
    r = await r2.battery_voltage()
    return {"raw": r.data.hex() if r else None,
            "volts": int.from_bytes(r.data, "big") / 100 if r and r.data else None}

async def _op_head(r2, p):
    return {"degrees": await r2.get_head()}

async def _op_read_char(r2, p):
    """Read any GATT characteristic — for chasing 00020004 and the standard
    Battery Service 00002a19, neither of which any upstream repo documents."""
    val = await r2.client.read_gatt_char(p["uuid"])
    return {"uuid": p["uuid"], "hex": bytes(val).hex(), "int": int.from_bytes(val, "big"),
            "bytes": list(val)}

async def _op_gatt(r2, p):
    return {"services": [{"uuid": s.uuid,
                          "chars": [{"uuid": c.uuid, "props": list(c.properties)}
                                    for c in s.characteristics]}
                         for s in r2.client.services]}

async def _op_leds(r2, p):
    await r2.set_leds({int(k): int(v) for k, v in p["channels"].items()})
    return {"set": p["channels"]}

async def _op_sound(r2, p):
    if "volume" in p:
        await r2.set_volume(int(p["volume"]))
    r = await r2.play_sound(int(p["id"]), int(p.get("mode", 0)))
    return {"id": p["id"], "err": _err_of(r)}

async def _op_stop_audio(r2, p):
    await r2.stop_audio(); return {"stopped": True}

async def _op_dome(r2, p):
    start, target = await bounded_head_move(
        r2, delta=p.get("delta"), angle=p.get("angle"))
    await asyncio.sleep(float(p.get("settle", 2.0)))
    return {"start": start, "commanded": target, "now": await r2.get_head()}

async def _op_animation(r2, p):
    r = await r2.play_animation(int(p["id"]))
    return {"id": p["id"], "err": _err_of(r)}

async def _op_stop(r2, p):
    """Emergency stop — always permitted at any tier."""
    await r2.stop_animation()
    await r2.stop_audio()
    return {"stopped": True}

async def _op_events(r2, p):
    """Drain everything the robot said that nobody asked for."""
    return r2.drain_events()

def _idle_tier(p: dict) -> str:
    """Tier depends on WHICH WAY you are pointing this op.

    The issue filed `idle` as tier 'motion' flat. That would have made it
    unreachable from the two cheapest survey sessions: #8 runs `--allow leds`
    and #9 runs `--allow audio`, and both need native idle OFF for their
    observations to mean anything. A precondition you cannot satisfy at the
    tier you must run at is not a precondition.

    Disabling belongs with `stop` — it makes R2 quieter, and CLAUDE.md's
    posture is 'default to STOP'. Enabling starts spontaneous motion, so it
    keeps the motion ceiling."""
    return "motion" if _strict_bool(p.get("enable", False), "enable") else "read"

_idle_tier.min_tier = "read"
_idle_tier.label = "read to disable / motion to enable"

async def _op_idle(r2, p):
    """Enable or disable R2's native idle animations. Defaults to DISABLING —
    the direction every survey session needs, and the quiet one."""
    enable = _strict_bool(p.get("enable", False), "enable")
    r = await r2.enable_idle_animations(enable)
    r2.idle_disabled = not enable
    return {"idle_enabled": enable, "err": _err_of(r)}

async def _op_notify(r2, p):
    """Enable robot-initiated notifications.

    Only two of the three have an enable command upstream.
    `play_animation_complete_notify` (animatronic.py:43) is a bare tuple with
    no setter at all, so whether it fires unprompted cannot be settled from
    source — play an animation, then read `events`. That is the empirical
    question issue #7 leaves open on purpose."""
    out = {}
    if "leg" in p:
        out["leg"] = _err_of(await r2.enable_leg_action_notify(
            _strict_bool(p["leg"], "leg")))
    if "head_reset" in p:
        out["head_reset"] = _err_of(await r2.enable_head_reset_notify(
            _strict_bool(p["head_reset"], "head_reset")))
    if not out:
        raise ValueError('nothing to enable — pass {"leg": true} and/or '
                         '{"head_reset": true}. animation_complete has no '
                         'enable command; test it empirically via `events`.')
    out["note"] = ("animation_complete has no enable command upstream; "
                   "play an animation and read `events` to find out if it fires")
    return out

# (tier, handler). `tier` is a string, or a callable taking the request params
# and returning one — see _idle_tier for why that is worth the indirection.
OPS = {
    "status":     ("read",   _op_status),
    "battery":    ("read",   _op_battery),
    "head":       ("read",   _op_head),
    "read_char":  ("read",   _op_read_char),
    "gatt":       ("read",   _op_gatt),
    "stop":       ("read",   _op_stop),      # always allowed: default to STOP
    "events":     ("read",   _op_events),    # reading is never a hazard
    "notify":     ("read",   _op_notify),    # enabling a report moves nothing
    "idle":       (_idle_tier, _op_idle),
    "leds":       ("leds",   _op_leds),
    "sound":      ("audio",  _op_sound),
    "stop_audio": ("audio",  _op_stop_audio),
    "dome":       ("motion", _op_dome),
    "animation":  ("motion", _op_animation),
}


def op_tier(op: str, params: dict | None = None) -> str:
    """Tier this specific request needs. May raise ValueError on bad params."""
    tier = OPS[op][0]
    return tier(params or {}) if callable(tier) else tier


def min_tier(op: str) -> str:
    """Lowest ceiling at which this op could ever run — for the banner."""
    tier = OPS[op][0]
    return tier if isinstance(tier, str) else tier.min_tier


def allowed_ops(ceiling: str) -> list[str]:
    return sorted(o for o in OPS if _tier_ok(ceiling, min_tier(o)))


async def handle_request(r2, payload: dict, ceiling: str, log=print) -> dict:
    """Execute one bridge request and return the response dict.

    Split out of the daemon loop so the permission ladder and every op can be
    unit-tested against a fake R2, with no radio and no file queue. The daemon
    loop below is then only queue plumbing."""
    op = payload.get("op")
    params = payload.get("params") or {}
    ts = time.strftime("%H:%M:%S")

    if op not in OPS:
        log(f"[{ts}] REFUSED unknown op {op!r}")
        return {"ok": False, "error": f"unknown op {op!r}",
                "allowed": allowed_ops(ceiling)}
    try:
        needed = op_tier(op, params)
    except Exception as e:
        log(f"[{ts}] REFUSED {op} — bad params: {e}")
        return {"ok": False, "op": op, "error": f"{type(e).__name__}: {e}"}
    if not _tier_ok(ceiling, needed):
        log(f"[{ts}] REFUSED {op} — needs tier '{needed}', "
            f"daemon ceiling is '{ceiling}'")
        return {"ok": False, "op": op, "error":
                f"op '{op}' needs tier '{needed}'; daemon started with "
                f"--allow {ceiling}. Restart the daemon to raise it."}

    log(f"[{ts}] EXEC {op} {json.dumps(params)}")
    try:
        data = await OPS[op][1](r2, params)
    except Exception as e:
        error = f"{type(e).__name__}: {e}"
        log(f"[{ts}]   !! {error}")
        return {"ok": False, "op": op, "error": error}
    log(f"[{ts}]   -> {json.dumps(data)[:160]}")
    return {"ok": True, "op": op, "data": data}


async def cmd_daemon(args) -> int:
    for d in (REQ_DIR, RESP_DIR):
        d.mkdir(parents=True, exist_ok=True)
    for stale in list(REQ_DIR.glob("*.json")) + list(RESP_DIR.glob("*.json")):
        stale.unlink()

    ceiling = args.allow
    allowed = allowed_ops(ceiling)
    print(f"\n{'='*62}\n  r2 bridge daemon — permission ceiling: {ceiling.upper()}")
    print(f"  allowed ops: {', '.join(allowed)}")
    if ceiling != "motion":
        print(f"  refused    : {', '.join(sorted(set(OPS) - set(allowed)))}")
    # Ops whose tier depends on their params would otherwise read as a flat
    # allow above, which overstates what the ceiling actually permits.
    for op in sorted(o for o in allowed if callable(OPS[o][0])):
        print(f"  note       : '{op}' is {OPS[op][0].label}")
    print(f"  queue      : {BRIDGE}")
    print(f"  idle timeout: {args.idle_timeout:.0f}s     Ctrl-C to stop safely")
    print(f"{'='*62}\n")

    hits, _ = await find(args.timeout, args.name)
    if not hits:
        print("R2-D2 not found."); return 1

    async with R2(hits[0], verbose=args.verbose) as r2:
        await r2.wake()
        r2.start_keepalive()
        print(f"\n>>> READY — holding session with {hits[0].name}. "
              f"Every command is logged below.\n")
        last = heartbeat = time.monotonic()
        try:
            while True:
                if time.monotonic() - last > args.idle_timeout:
                    print(f"\n[{time.strftime('%H:%M:%S')}] idle "
                          f"{args.idle_timeout:.0f}s — disconnecting (default to stop)")
                    break
                reqs = sorted(REQ_DIR.glob("*.json"))
                if not reqs:
                    # Heartbeat, so a wedged loop is visible instead of silent.
                    if time.monotonic() - heartbeat > 30:
                        heartbeat = time.monotonic()
                        print(f"[{time.strftime('%H:%M:%S')}] idle, polling "
                              f"{REQ_DIR}", flush=True)
                    await asyncio.sleep(0.2); continue
                for req in reqs:
                    try:
                        payload = json.loads(req.read_text())
                    except Exception as e:
                        payload = {"op": "<unparseable>", "_err": str(e)}
                    req.unlink(missing_ok=True)
                    last = time.monotonic()
                    resp = await handle_request(r2, payload, ceiling)
                    (RESP_DIR / req.name).write_text(json.dumps(resp, indent=2))
        except (KeyboardInterrupt, asyncio.CancelledError):
            print("\n\ninterrupted — stopping R2 before disconnect")
        finally:
            # Default to STOP: never leave audio or an animation running.
            try:
                await r2.stop_animation(); await r2.stop_audio()
                print("stop sent (animation + audio)")
            except Exception as e:
                print(f"stop on exit failed: {e}")
            if r2.idle_disabled:
                # Deliberately NOT restored here. Re-enabling idle is a command
                # that starts spontaneous motion, and this is the path that
                # exists to leave R2 stopped. Say so instead of doing it.
                print("\nNOTE: native idle animations were DISABLED this session\n"
                      "      and are left disabled — restoring them would start\n"
                      "      motion on the way out. Re-enable when you want him\n"
                      "      fidgeting again (needs --allow motion):\n"
                      "        ./r2 send idle --params '{\"enable\": true}'")
            drained = r2.drain_events()
            if drained["count"] or drained["dropped"]:
                print(f"\n{drained['count']} unread event(s) discarded on exit "
                      f"({drained['dropped']} had already been evicted). "
                      f"Read them with `./r2 send events` during a session.")
    return 0


async def cmd_send(args) -> int:
    """Queue one op for the daemon and print its response."""
    if not REQ_DIR.exists():
        print("bridge not running — start it from Terminal:\n"
              "  cd ~/Projects/R2Z2/mac-prototype && ./r2 daemon --allow read")
        return 1
    params = json.loads(args.params) if args.params else {}
    rid = f"{time.time():.6f}".replace(".", "") + ".json"
    (REQ_DIR / rid).write_text(json.dumps({"op": args.op, "params": params}))
    resp_path = RESP_DIR / rid
    deadline = time.monotonic() + args.wait
    while time.monotonic() < deadline:
        if resp_path.exists():
            resp = json.loads(resp_path.read_text())
            resp_path.unlink(missing_ok=True)
            print(json.dumps(resp, indent=2))
            return 0 if resp.get("ok") else 2
        await asyncio.sleep(0.15)
    (REQ_DIR / rid).unlink(missing_ok=True)
    print(json.dumps({"ok": False, "error": f"no response in {args.wait}s — "
                      "is the daemon running and connected?"}, indent=2))
    return 1


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    # Scan flags live on a shared parent so they work either side of the
    # subcommand: `r2_probe.py --timeout 20 scan` and `... scan --timeout 20`.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--timeout", type=float, default=10.0, help="BLE scan timeout (s)")
    common.add_argument("--name", help=f"prefix to match (default {NAME_PREFIX!r})")
    p.add_argument("--timeout", type=float, default=10.0, help="BLE scan timeout (s)")
    p.add_argument("--name", help=f"prefix to match (default {NAME_PREFIX!r})")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("scan", parents=[common],
                   help="passive scan only — no connection").set_defaults(fn=cmd_scan)
    sub.add_parser("info", parents=[common],
                   help="connect, handshake, read-only queries").set_defaults(fn=cmd_info)

    led = sub.add_parser("led", parents=[common], help="LED test (no motion)")
    led.add_argument("--color", default="0,0,255", help="r,g,b 0-255")
    led.set_defaults(fn=cmd_led)

    snd = sub.add_parser("sound", parents=[common], help="play one sound (no motion)")
    snd.add_argument("--id", type=int, default=2813, help="sound id (default R2_HEY_1)")
    snd.add_argument("--volume", type=int, default=80)
    snd.set_defaults(fn=cmd_sound)

    dome = sub.add_parser("dome", parents=[common],
                          help="small dome rotation — FIRST MOVEMENT TEST")
    dome.add_argument("--delta", type=float, default=20.0,
                      help="degrees to turn FROM the current position; travel "
                           "capped at +/-45 (the dome does not rest at 0)")
    dome.set_defaults(fn=cmd_dome)

    dae = sub.add_parser("daemon", parents=[common],
                         help="hold a session and serve ops from the file queue")
    dae.add_argument("--allow", choices=TIERS, default="read",
                     help="permission ceiling (default: read-only)")
    dae.add_argument("--idle-timeout", type=float, default=900.0,
                     help="disconnect after this many idle seconds")
    dae.add_argument("--verbose", action="store_true", help="log every BLE packet")
    dae.set_defaults(fn=cmd_daemon)

    snd2 = sub.add_parser("send", help="queue one op for a running daemon")
    snd2.add_argument("op", help=f"one of: {', '.join(sorted(OPS))}")
    snd2.add_argument("--params", help="JSON object of op parameters")
    snd2.add_argument("--wait", type=float, default=30.0, help="seconds to wait")
    snd2.set_defaults(fn=cmd_send)

    anim = sub.add_parser("animation", parents=[common], help="play one authored animation")
    anim.add_argument("--id", type=int, default=35, help="animation id (default 35 WWM_CURIOUS)")
    anim.add_argument("--seconds", type=float, default=5.0)
    anim.set_defaults(fn=cmd_animation)

    args = p.parse_args()
    try:
        return asyncio.run(args.fn(args))
    except KeyboardInterrupt:
        print("\ninterrupted")
        return 130


if __name__ == "__main__":
    sys.exit(main())
