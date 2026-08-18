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

NO_BLEAK_MSG = ("bleak is not installed in this interpreter, so this host cannot "
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
CID_ANIM_GET_LEG_ACTION = 0x25  # animatronic.py:58  (CID 37) — READ, cannot move him
CID_ANIM_PERFORM_LEG_ACTION = 0x0D  # animatronic.py:36 (CID 13) — WRITE, can fell him
CID_ANIM_GET_LEG_POSITION = 0x16    # animatronic.py:54 (CID 22) — READ, a float
# NOTE set_leg_position is CID 21 (0x15) and is deliberately NOT defined here.
# #22 AC6: understand what the float MEANS from reads before anything writes
# it. It is finer-grained than the stance enum and documented nowhere, so a
# write is a guess at an actuator that can fell him.
CID_ANIM_STOP = 0x2B           # animatronic.py:69  (CID 43)
CID_IO_PLAY_AUDIO = 0x07       # io.py:60
CID_IO_SET_VOLUME = 0x08       # io.py:64
CID_IO_STOP_AUDIO = 0x0A       # io.py:72
CID_IO_LEDS_16BIT = 0x0E       # io.py:76  (CID 14) — the variant BB9E/R2D2 expose
CID_IO_LEDS_8BIT = 0x1C        # io.py:88  (CID 28) — marked "Untested" upstream

# Robot-initiated behaviour — enable commands and the notifies they turn on.
#
# These were single-source in spherov2. Hardware has now ruled on all three
# against D2-6F6B, 2026-08-16:
CID_ANIM_ENABLE_LEG_NOTIFY = 0x2A        # animatronic.py:64 — OBSERVED, success
CID_ANIM_ENABLE_HEAD_RESET_NOTIFY = 0x39  # animatronic.py:84 — OBSERVED, success
CID_ANIM_ENABLE_IDLE = 0x2C              # animatronic.py:72 — REFUTED,
# bad_command_id. spherov2 DOES expose enable_idle_animations on R2D2 (class
# R2D2(BB9E), bb9e.py:121, resolved via the MRO) — the library and the firmware
# genuinely disagree, and no source reading predicts that. Kept only so the
# gate can re-probe it if a firmware revision ever adds the command.

# Notification CIDs the robot sends unprompted.
CID_ANIM_COMPLETE_NOTIFY = 0x11     # animatronic.py:43 — (23, 17, 0xff)
CID_ANIM_LEG_COMPLETE_NOTIFY = 0x26  # animatronic.py:61 — (23, 38, 0xff)
CID_ANIM_HEAD_RESET_NOTIFY = 0x3A   # animatronic.py:87 — (23, 58, 0xff)
DID_SENSOR = 0x18                   # sensor.py:81
CID_SENSOR_MASK = 0x00              # sensor.py:84  set_sensor_streaming_mask
CID_SENSOR_MASK_GET = 0x01          # sensor.py:88  get_sensor_streaming_mask
CID_SENSOR_STREAM_NOTIFY = 0x02     # sensor.py:92 — (24, 2, 0xff)
CID_SENSOR_EXT_MASK = 0x0C          # sensor.py:95  set_extended_..._mask

# The sensor tables, transcribed from the class bodies rather than guessed.
# ORDER IS LOAD-BEARING: the stream is a flat array of big-endian float32s and
# the only way to know which value is which is to walk these OrderedDicts in
# declaration order, taking one float per ENABLED component. Get the order
# wrong and every field is silently mis-assigned -- gyro read as attitude,
# with no error anywhere.
#
# `class R2D2(BB9E)`, so `sensors` is inherited from bb9e.py:80-112 while
# `extended_sensors` is R2D2's OWN (r2d2.py:470-477) and REPLACES BB9E's.
# Grepping r2d2.py alone would miss the whole normal-sensor table.
SENSORS = (                          # bb9e.py:80-112, inherited by R2D2
    ("quaternion",    (("x", 0x2000000), ("y", 0x1000000),
                       ("z", 0x800000),  ("w", 0x400000))),
    ("attitude",      (("pitch", 0x40000), ("roll", 0x20000),
                       ("yaw", 0x10000))),
    ("accelerometer", (("x", 0x8000), ("y", 0x4000), ("z", 0x2000))),
    ("accel_one",     (("accel_one", 0x200),)),
    ("locator",       (("x", 0x40), ("y", 0x20))),      # modifier: x100
    ("velocity",      (("x", 0x10), ("y", 0x8))),       # modifier: x100
    ("speed",         (("speed", 0x4),)),
    ("core_time",     (("core_time", 0x2),)),
)
EXT_SENSORS = (                      # r2d2.py:470-477 — R2D2's own
    ("r2_head_angle", (("r2_head_angle", 0x4000000),)),
    ("gyroscope",     (("x", 0x2000000), ("y", 0x1000000), ("z", 0x800000))),
)
# locator and velocity arrive scaled down by 100 (bb9e.py:99-105).
SENSOR_MODIFIERS = {("locator", "x"), ("locator", "y"),
                    ("velocity", "x"), ("velocity", "y")}


def _mask_of(table, groups) -> int:
    """OR together every component bit of the named groups.

    `|=`, not `sum()`. Every bit in both tables is currently distinct, so a sum
    happens to give the same answer -- but a group added later with an
    overlapping bit would make sum CARRY into a neighbouring bit and quietly
    request the wrong sensors. The bug would surface as a length mismatch in
    decode_sensor_stream, one layer away from the cause."""
    want = set(groups)
    mask = 0
    for name, comps in table:
        if name in want:
            for _, bit in comps:
                mask |= bit
    return mask


def sensor_mask(groups) -> int:
    return _mask_of(SENSORS, groups)


def ext_sensor_mask(groups) -> int:
    return _mask_of(EXT_SENSORS, groups)


def decode_sensor_stream(payload: bytes, mask: int, ext_mask: int) -> dict:
    """Decode one sensor_stream packet into {group: {component: value}}.

    The wire format is `struct.unpack('>%df' % (len(data)//4), data)`
    (sensor.py:92-93) -- already floats, so there is NO int scaling to apply.
    Only locator and velocity carry a modifier, and it is a plain x100.

    Raises on a length mismatch rather than returning a partial decode: a
    short packet means our idea of the enabled mask disagrees with the
    robot's, and silently decoding the first N fields would produce
    plausible, wrongly-labelled numbers -- the worst possible outcome for a
    survey whose entire job is to characterise a signal.
    """
    n = len(payload) // 4
    if len(payload) % 4:
        raise ValueError(f"sensor payload {len(payload)} bytes is not a "
                         f"whole number of float32s")
    values = list(struct.unpack(f">{n}f", payload))
    enabled = [(g, c, bit) for g, comps in SENSORS for c, bit in comps
               if mask & bit]
    enabled += [(g, c, bit) for g, comps in EXT_SENSORS for c, bit in comps
                if ext_mask & bit]
    if len(enabled) != n:
        raise ValueError(
            f"sensor stream carried {n} floats but the masks "
            f"(0x{mask:X}/0x{ext_mask:X}) describe {len(enabled)} components; "
            f"refusing to guess which is which")
    out: dict = {}
    for (group, comp, _), value in zip(enabled, values):
        if (group, comp) in SENSOR_MODIFIERS:
            value *= 100.0
        out.setdefault(group, {})[comp] = value
    return out

# Longest V2 frame we will reassemble before giving up and resyncing on the
# next SOP. Real frames are header + payload + checksum, well under 100 bytes
# even with worst-case escaping; this is a generous ceiling whose only job is
# to stop a truncated stream growing the buffer for the life of the daemon.
MAX_PACKET_BYTES = 512

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
    (DID_SENSOR, CID_SENSOR_STREAM_NOTIFY): "sensor_stream",
}

EVENT_RING_SIZE = 200

# Stance states. OBSERVED animatronic.py:8-13 — this is `R2DoLegActions`, what
# get_leg_action RETURNS, and it is NOT `R2LegActions` (:16-20), what
# perform_leg_action TAKES. They agree on 1/2/3 and diverge at 0 and 4, so
# decoding a state against the command enum is silently plausible and wrong:
# it renders TRANSITIONING as an undocumented value and UNKNOWN as STOP.
# Commands. OBSERVED animatronic.py:16-20 — `R2LegActions`, what
# perform_leg_action TAKES. Keyed by name, never by raw int: `{"action": 3}` is
# WADDLE, and a caller who meant "the third option" would get locomotion.
LEG_ACTION_STOP, LEG_ACTION_THREE_LEGS = 0, 1
LEG_ACTION_TWO_LEGS, LEG_ACTION_WADDLE = 2, 3
LEG_ACTIONS = {
    "stop": LEG_ACTION_STOP,
    "three_legs": LEG_ACTION_THREE_LEGS,
    "two_legs": LEG_ACTION_TWO_LEGS,
}
# WADDLE is deliberately ABSENT from the table above, not merely rejected by a
# branch: it is translation on two tracks with the stabiliser up, it is what
# caused the only fall this project has had (#11), and it is locomotion —
# which belongs to #23, behind a stop-distance criterion that does not exist
# yet. Keeping it out of the mapping means no typo, no off-by-one and no
# future refactor of the guard can reach it.

LEG_STATE_UNKNOWN, LEG_STATE_THREE_LEGS = 0, 1
LEG_STATE_TWO_LEGS, LEG_STATE_WADDLE, LEG_STATE_TRANSITIONING = 2, 3, 4
LEG_STATES = {
    LEG_STATE_UNKNOWN: "unknown",
    LEG_STATE_THREE_LEGS: "three_legs",
    LEG_STATE_TWO_LEGS: "two_legs",
    LEG_STATE_WADDLE: "waddle",
    LEG_STATE_TRANSITIONING: "transitioning",
}

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
        # Serialises seq allocation, the 120 ms pacing, and the chunked write.
        self._tx_lock = asyncio.Lock()
        self._keepalive: asyncio.Task | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        # Packets nobody was waiting for. Before this existed they were dropped
        # on the floor, which made every robot-initiated notification invisible.
        self._events: collections.deque = collections.deque(maxlen=EVENT_RING_SIZE)
        self._events_evicted = 0    # written only by the BLE thread
        self._events_reported = 0   # written only by the draining thread
        # Truncated/undecodable frames. Surfaced by `events` so a lost
        # notification is visible as a loss rather than as an absence: the
        # daemon's log is --verbose-gated and off by default.
        self._framing_errors = 0
        self._framing_reported = 0
        # None = never touched this session; True = we disabled native idle.
        self.idle_disabled: bool | None = None
        # Set by `_op_sensors`. Tracked for the SAME reason idle_disabled is:
        # the exit path has to know what THIS session turned on, so it can put
        # it back without clobbering state some other process owns.
        self.sensor_streaming: bool = False

    async def __aenter__(self) -> "R2":
        if self.client is None:
            raise RuntimeError(NO_BLEAK_MSG)
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
            if b == SOP and self._rx:
                # A SOP inside a frame is impossible — it is escaped to AB 05
                # on the wire — so this is unambiguous evidence the previous
                # frame was truncated. Resync here rather than appending, which
                # used to merge the runt with the NEXT good packet and kill
                # them both on the checksum. A silently lost notification is
                # indistinguishable from "R2 never sent one", which is exactly
                # the UNKNOWN this harness exists to settle.
                self._framing_errors += 1
                self._log(f"truncated frame {bytes(self._rx).hex()}; resyncing")
                self._rx = bytearray()
            if len(self._rx) >= MAX_PACKET_BYTES:
                # A SOP with no EOP behind it grows this buffer forever. Over a
                # 30-minute session that is a slow leak with no upper bound, so
                # resynchronise instead: drop the runt and wait for a fresh SOP.
                self._framing_errors += 1
                self._log(f"rx overrun >{MAX_PACKET_BYTES}B without EOP; resyncing")
                self._rx = bytearray()
                if b != SOP:
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
        if not resp.flags & FLAG_IS_RESPONSE:
            # Only a packet flagged is_response can answer a command. Matching
            # on (did, cid, seq) alone let a NOTIFICATION that happened to carry
            # a non-0xff seq resolve a live waiter — and because parse() skips
            # the error byte for non-responses, it resolved as `err=success`
            # with every payload field shifted by one. Spec says notifications
            # use seq 0xff, but "the firmware always follows spec" is exactly
            # the assumption this tool exists to test.
            self._record_event(resp)
            return
        fut = self._waiters.pop((resp.did, resp.cid, resp.seq), None)
        if fut is None:
            # Nobody asked for this: a response that arrived after its send()
            # timed out, or a second copy of one already handed over.
            self._record_event(resp)
            return
        if fut.done():
            # The waiter is present but already resolved or cancelled — i.e.
            # this reply lost a race with its own send() timeout.
            self._record_event(resp, note="late reply; waiter already settled")
            return
        loop = self._loop
        if loop is None or not loop.is_running():
            self._record_event(resp, note="loop not running; nobody to hand to")
            return

        def deliver() -> None:
            # By the time this runs on the loop thread, wait_for may already
            # have timed out and cancelled the future. `fut.done() or
            # set_result(...)` quietly discarded the reply in that window — the
            # command was answered, but send() reported "no response" and the
            # packet was recorded nowhere at all.
            if fut.done():
                self._record_event(resp, note="late reply; waiter already settled")
            else:
                fut.set_result(resp)

        loop.call_soon_threadsafe(deliver)

    def _record_event(self, resp: "Response", note: str | None = None) -> None:
        """Append to the bounded ring. Runs on bleak's CoreBluetooth dispatch
        thread, so it must not touch the event loop.

        No lock is needed because `collections.deque` is implemented in C and
        documented thread-safe for append and popleft, so this thread can append
        while the loop thread drains. `maxlen` gives us eviction for free. (The
        guarantee is deque's own, NOT the GIL's — this file targets Python 3.14,
        where a free-threaded build has no GIL to reason from.)

        `_events_evicted` is a monotonic total that ONLY this thread writes;
        the drain side never mutates it, it just remembers the last value it
        reported. That makes the count exact rather than merely "narrowed" — an
        under-reported eviction count is worse than no count, because it tells
        the operator nothing was lost when something was.
        """
        if len(self._events) == EVENT_RING_SIZE:
            self._events_evicted += 1
        event = {
            "t": time.time(),
            "name": KNOWN_NOTIFIES.get((resp.did, resp.cid)),
            "did": resp.did,
            "cid": resp.cid,
            "seq": resp.seq,
            "flags": resp.flags,
            "err": resp.err,
            "data": resp.data.hex(),
            # Not just `seq == 0xff`: a notification that deviates from spec on
            # seq is exactly the case the is_response gate was added to catch,
            # and labelling it "solicited" would bury it.
            "unsolicited": not resp.flags & FLAG_IS_RESPONSE
                           or resp.seq == NOTIFY_SEQ,
        }
        if note:
            event["note"] = note
        self._events.append(event)

    def drain_events(self) -> dict:
        """Take everything in the ring and hand it over, oldest first.

        popleft() is individually thread-safe, so draining one at a time cannot
        race the BLE thread's appends the way `list(ring); ring.clear()` would
        — that pair has a window where an append lands between the copy and the
        clear and is dropped without ever being reported."""
        out = []
        while True:
            try:
                out.append(self._events.popleft())
            except IndexError:
                break
        # Single-writer counter, read-only here: take a snapshot and diff it
        # against what we last reported. No read-modify-write on the shared
        # value, so no bump can be lost regardless of interleaving.
        evicted = self._events_evicted
        dropped, self._events_reported = evicted - self._events_reported, evicted
        framing = self._framing_errors
        bad, self._framing_reported = framing - self._framing_reported, framing
        return {"events": out, "count": len(out), "dropped": dropped,
                "framing_errors": bad}

    async def send(self, did: int, cid: int, data: bytes = b"",
                   expect: bool = True, timeout: float = 5.0) -> Response | None:
        # Serialise the whole transmit path. `_last_tx` was read at the top and
        # written after the write, with awaits in between and no mutual
        # exclusion — so the 3 s keepalive task and a daemon op could both see
        # the same stale timestamp, both decide they were clear to send, and
        # transmit back to back. Measured 0.0 ms apart against a 120 ms
        # CMD_SAFE_INTERVAL, with seqs reaching the wire out of order. R2 drops
        # commands sent faster than that interval, which shows up as a survey
        # item that "didn't work" rather than as an error.
        async with self._tx_lock:
            seq = self._seq
            self._seq = (self._seq + 1) % 0xFF
            pkt = build(did, cid, seq, data)

            # Honour the 120 ms R2-D2 command interval.
            gap = time.monotonic() - self._last_tx
            if gap < CMD_SAFE_INTERVAL:
                await asyncio.sleep(CMD_SAFE_INTERVAL - gap)

            fut: asyncio.Future | None = None
            if expect:
                # Registered before the write so a fast reply cannot beat it.
                # That makes every exit path responsible for unregistering it —
                # see the finally below.
                fut = asyncio.get_running_loop().create_future()
                self._waiters[(did, cid, seq)] = fut

            self._log(f"tx {pkt.hex()}  (DID={did:#04x} CID={cid:#04x} seq={seq})")
            try:
                # Inside the lock: a packet larger than MTU_CHUNK goes out in
                # several writes, and another sender interleaving between them
                # would splice two packets together on the wire.
                for i in range(0, len(pkt), MTU_CHUNK):
                    await self.client.write_gatt_char(
                        API_UUID, pkt[i:i + MTU_CHUNK], response=True)
                self._last_tx = time.monotonic()
            except BaseException:
                self._waiters.pop((did, cid, seq), None)
                raise

        # Wait OUTSIDE the lock: holding it for the 5 s response timeout would
        # stall the keepalive and every other op behind a single slow reply.
        if fut is None:
            return None
        try:
            return await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            self._log(f"no response to DID={did:#04x} CID={cid:#04x} "
                      f"within {timeout}s")
            return None
        finally:
            self._waiters.pop((did, cid, seq), None)

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

    async def perform_leg_action(self, action: int) -> Response | None:
        """OBSERVED animatronic.py:36 — CID 13, one byte payload.

        Takes the raw int so the packet layer stays a packet layer; the name
        allowlist and the WADDLE exclusion live in the op, where the refusal
        can be explained to whoever sent it."""
        return await self.send(DID_ANIMATRONIC, CID_ANIM_PERFORM_LEG_ACTION,
                               bytes((action,)))

    async def get_leg_position(self) -> float | None:
        """OBSERVED animatronic.py:54 — CID 22, big-endian float. Semantics
        UNKNOWN; this exists to find out what the number does, not because we
        know."""
        r = await self.send(DID_ANIMATRONIC, CID_ANIM_GET_LEG_POSITION)
        if r and len(r.data) == 4:
            return struct.unpack(">f", r.data)[0]
        return None

    async def get_leg_action(self) -> int | None:
        """Current stance, as a raw byte. OBSERVED animatronic.py:58 — CID 37,
        returns `data[0]`.

        Returns the RAW value, not a decoded name, because an undocumented
        state is exactly the kind of thing this project exists to find, and a
        lookup that silently maps it to None would hide it."""
        r = await self.send(DID_ANIMATRONIC, CID_ANIM_GET_LEG_ACTION)
        if r and len(r.data) >= 1:
            return r.data[0]
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
        """**REFUTED — R2-D2 answers bad_command_id to this.**

        Verified 2026-08-16 on D2-6F6B. spherov2 exposes it on R2D2 by
        inheritance (bb9e.py:121 via class R2D2(BB9E)), so the library claims
        the capability and the firmware refuses it. There is NO way to turn
        R2's idle behaviour off; whether he idles at all is now an open
        question. See docs/research/r2-capabilities.md."""
        return await self.send(DID_ANIMATRONIC, CID_ANIM_ENABLE_IDLE,
                               bytes((1 if enable else 0,)))

    async def set_sensor_mask(self, interval: int, count: int,
                              mask: int) -> Response | None:
        """OBSERVED sensor.py:84-85 — `>HBI`: interval(2), count(1), mask(4)."""
        return await self.send(DID_SENSOR, CID_SENSOR_MASK,
                               struct.pack(">HBI", int(interval), int(count),
                                           int(mask)))

    async def set_extended_sensor_mask(self, mask: int) -> Response | None:
        """OBSERVED sensor.py:95-96 — CID 12, a bare 4-byte mask."""
        return await self.send(DID_SENSOR, CID_SENSOR_EXT_MASK,
                               struct.pack(">I", int(mask)))

    async def get_sensor_mask(self) -> tuple[int, int, int] | None:
        """Read back what the robot thinks is enabled (sensor.py:88-89).

        This is the instrument check: it is the only way to tell "the stream
        is off" from "the stream is on and nothing is moving", and those two
        produce identical silence.
        """
        r = await self.send(DID_SENSOR, CID_SENSOR_MASK_GET)
        if r and len(r.data) == 7:
            return struct.unpack(">HBI", r.data)
        return None

    async def enable_leg_action_notify(self, enable: bool) -> Response | None:
        """OBSERVED in spherov2 source: animatronic.py:64 — CID 42, payload
        [int(enable)]. SINGLE-SOURCE, so INFERRED on the wire until hardware
        confirms (#7 AC5)."""
        return await self.send(DID_ANIMATRONIC, CID_ANIM_ENABLE_LEG_NOTIFY,
                               bytes((1 if enable else 0,)))

    async def enable_head_reset_notify(self, enable: bool) -> Response | None:
        """OBSERVED in spherov2 source: animatronic.py:84 — CID 57, payload
        [int(enable)]. SINGLE-SOURCE, so INFERRED on the wire until hardware
        confirms (#7 AC5)."""
        return await self.send(DID_ANIMATRONIC,
                               CID_ANIM_ENABLE_HEAD_RESET_NOTIFY,
                               bytes((1 if enable else 0,)))


# ── CLI ──────────────────────────────────────────────────────────────────────

async def find(timeout: float = 10.0, name: str | None = None):
    if BleakScanner is None:
        raise RuntimeError(NO_BLEAK_MSG)
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
    # NaN defeats every guard below, silently and in the dangerous direction:
    # `abs(nan) > 45` is False so the travel cap is skipped, and `min(182, nan)`
    # returns 182, so the clamp hands back its own BOUND instead of the value.
    # Measured from a resting -100 degrees that is a 282 degree swing against a
    # +/-45 cap. This is not exotic input — `json.dumps` emits a bare `NaN` by
    # default, so any survey script that computes a delta from a failed reading
    # writes one into the queue and `json.loads` accepts it without complaint.
    for name, value in (("start", start), ("delta", delta), ("angle", angle)):
        if value is not None and not math.isfinite(value):
            raise ValueError(f"{name}={value!r} is not a finite number; "
                             f"refusing to move the dome")
    target = start + delta if delta is not None else angle
    travel = target - start
    if not math.isfinite(target) or not math.isfinite(travel):
        raise ValueError("computed a non-finite dome target; refusing to move")
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
# Deliberately NOT `*.json`: startup wipes every .json under requests/ and
# responses/, and a lock that the wipe could delete is not a lock.
LOCK = BRIDGE / "daemon.lock"


def _pid_alive(pid: int) -> bool:
    """Does a process with this pid exist?

    Signal 0 runs the permission and existence checks without delivering
    anything. PermissionError means it exists and belongs to someone else,
    which still counts as alive — treating "not mine" as "not there" would
    hand the bridge to a second daemon, the exact outcome being prevented.

    PID reuse can make this wrong on a long-lived stale lock. Accepted: on a
    single-user Mac the consequence is one refused start with a pid printed,
    and the operator can see for themselves that nothing is running."""
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except OSError:
        return True
    return True


def _read_lock() -> dict | None:
    try:
        held = json.loads(LOCK.read_text())
    except (OSError, ValueError):
        return None
    return held if isinstance(held, dict) and isinstance(held.get("pid"), int) else None


def acquire_daemon_lock(ceiling: str) -> dict | None:
    """Claim the bridge for this daemon. Returns None on success, else the
    record of the daemon that already holds it.

    This is a safety fix, not tidiness. Daemon startup unlinks EVERY file in
    requests/ and responses/, so a second daemon started at a different
    `--allow` erases the first's in-flight queue, and the two then race to
    consume each request. Which permission ceiling applies to a given command
    becomes non-deterministic — a `motion` daemon left running behind a `read`
    one silently re-arms every op the operator believes is refused.

    O_CREAT|O_EXCL rather than exists()-then-write: the check-then-act version
    has a window where two daemons both see no lock and both take it, which is
    the same double-daemon state this exists to prevent."""
    BRIDGE.mkdir(parents=True, exist_ok=True)
    record = json.dumps({"pid": os.getpid(), "ceiling": ceiling,
                         "started": time.time()})
    for _ in range(2):
        try:
            fd = os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        except FileExistsError:
            held = _read_lock()
            if held is not None and _pid_alive(held["pid"]):
                return held
            # No readable owner, or the owner is gone. Clear it and retry once.
            # An unreadable lock is treated as stale on purpose: a truncated
            # write must not wedge the bridge until someone deletes it by hand.
            try:
                LOCK.unlink()
            except FileNotFoundError:
                pass
            continue
        with os.fdopen(fd, "w") as f:
            f.write(record)
        return None
    # Lost the retry to another starter. Report whoever won.
    return _read_lock() or {"pid": -1, "ceiling": "unknown", "started": 0}


def release_daemon_lock() -> None:
    """Drop the lock, but only if it is still ours.

    An unconditional unlink would let a daemon shutting down slowly delete a
    lock that a newer daemon legitimately holds, re-opening the window this
    closes at the one moment it looks safest."""
    held = _read_lock()
    if held is not None and held.get("pid") == os.getpid():
        LOCK.unlink(missing_ok=True)

# Permission ladder — mirrors the fixed bring-up order in CLAUDE.md:
# read-only → LEDs → audio → small dome → stance → locomotion.
# The daemon is started with a ceiling; ops above it are refused, not executed.
#
# `motion` used to be the top rung and covered dome AND animations, described as
# "dome and animations". #11 showed that description was false in the direction
# that matters: `play_animation` drives LEG ACTIONS. EMOTE_YES — a nod — emitted
# WADDLE x3 and put R2 on the floor, without `perform_leg_action` ever being
# called. So a session opened for a dome survey could change his stance and
# topple him, while the ceiling's own name promised otherwise.
#
# Split so the rungs mean what the ladder says:
#   dome   — set_head and the DID 0x17 writes that cannot change stance.
#            Bounded by bounded_head_move; the worst case is a 45 degree turn.
#   stance — anything that can put him on the floor. `animation` lives here
#            BECAUSE OF THE OBSERVATION, not by category: an authored animation
#            is a stance command we do not get to inspect first.
TIERS = ["read", "leds", "audio", "dome", "stance"]

# `motion` is accepted and mapped DOWN to `dome`, never up. Someone who typed it
# expecting the old dome+animation grant now gets refusals on animation, which
# is a message on a terminal; mapping it up would silently re-grant the ability
# to knock the robot over. When a rename is ambiguous, resolve toward less
# capability.
TIER_ALIASES = {"motion": "dome"}


def resolve_tier(name: str) -> str:
    return TIER_ALIASES.get(name, name)


def _tier_ok(ceiling: str, needed: str) -> bool:
    return TIERS.index(needed) <= TIERS.index(resolve_tier(ceiling))


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
    # `r2 is not None` was structurally always True — the op could never report
    # a dropped link, which is the one thing a status check is for.
    client = getattr(r2, "client", None)
    connected = bool(getattr(client, "is_connected", False))
    return {"connected": connected,
            "head": await r2.get_head() if connected else None,
            # None means "never established", NOT "idle is on". On current
            # firmware it is always None: `_op_idle` raises on the
            # bad_command_id before it can set the flag.
            "idle_disabled": r2.idle_disabled}

async def _op_battery(r2, p):
    r = await r2.battery_voltage()
    return {"raw": r.data.hex() if r else None,
            "volts": int.from_bytes(r.data, "big") / 100 if r and r.data else None}

async def _op_head(r2, p):
    return {"degrees": await r2.get_head()}

def _stance_name(raw: int | None) -> str | None:
    """Raw byte to a name. An out-of-enum value is surfaced, never dropped."""
    if raw is None:
        return None
    return LEG_STATES.get(raw, f"undocumented_{raw}")


async def _op_stance(r2, p):
    """Read the stance. A READ — it cannot move him, so it sits at tier `read`
    alongside `head`, which already reads this same device (DID 0x17).

    Exists because of what #11 found: an authored animation (EMOTE_YES, a
    *nod*) ended in TWO_LEGS with the stabiliser retracted and nothing put it
    back, and R2 fell. `stable` answers the only question that matters before
    or after playing anything — is the third leg down?

    An undocumented raw value is reported as `undocumented_<n>` rather than
    dropped. `stable` is then False: unknown stance is not a safe stance."""
    raw = await r2.get_leg_action()
    if raw is None:
        return {"raw": None, "state": None, "stable": None,
                "note": "no response to get_leg_action; stance is UNKNOWN"}
    return {"raw": raw,
            "state": LEG_STATES.get(raw, f"undocumented_{raw}"),
            "stable": raw == LEG_STATE_THREE_LEGS}


async def _op_set_stance(r2, p):
    """Change the stance. Tier `stance` — this is the op that can fell him.

    Takes a NAME, not a number. `{"action": 3}` is WADDLE, and a caller who
    meant "the third option" would get locomotion out of an off-by-one.

    Returns the stance BEFORE and AFTER as measured, not as commanded. #11
    established that `get_leg_action` reports tracked state rather than a
    sensed one, so `after` is what the firmware now believes — which is
    exactly the thing #22 needs to find out, and is NOT proof he is physically
    on three legs. Only a human can confirm that."""
    action = p.get("action")
    if not isinstance(action, str):
        raise ValueError(
            f"'action' must be one of {sorted(LEG_ACTIONS)} as a string, got "
            f"{action!r}. Numbers are refused on purpose: 3 is WADDLE.")
    if action == "waddle":
        raise ValueError(
            "WADDLE is refused here. It is translation on two tracks with the "
            "stabiliser up — the thing that put R2 on the floor in #11 — and it "
            "is locomotion, which belongs to issue #23 behind a stop-distance "
            "criterion that does not exist yet.")
    if action not in LEG_ACTIONS:
        raise ValueError(f"unknown action {action!r}; expected one of "
                         f"{sorted(LEG_ACTIONS)}")
    before = await r2.get_leg_action()
    err = _err_of(await r2.perform_leg_action(LEG_ACTIONS[action]))
    if err != "success":
        # Loud, like `notify`. A silently-refused stance command during a
        # survey would be recorded as "R2 cannot deploy the tripod", which is
        # a wrong finding entering docs/.
        raise RuntimeError(
            f"perform_leg_action({action}) returned {err!r}. This is NOT "
            f"evidence that the stance is unreachable — it is one rejected "
            f"command. Re-probe before recording anything.")
    after = await r2.get_leg_action()
    return {
        "commanded": action,
        "before": _stance_name(before),
        "after_reported": _stance_name(after),
        "note": "`after_reported` is what the firmware BELIEVES, not a sensed "
                "position. Confirm the physical stance by eye (#11).",
    }


async def _op_leg_position(r2, p):
    """Read the raw leg position float. A READ, so tier `read`.

    Reported alongside the stance NAME because the float alone means nothing
    yet — #22 AC6 is to learn what it corresponds to at each known stance
    before `set_leg_position` is ever written."""
    return {"degrees": await r2.get_leg_position(),
            "stance": _stance_name(await r2.get_leg_action()),
            "note": "semantics UNKNOWN — see #22 AC6. Do not write until read."}


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

MAX_SETTLE_S = 10.0   # ceiling on any in-op sleep; see _op_dome

async def _op_leds(r2, p):
    # Bound the channel indices. `mask |= 1 << int(k)` with an arbitrary key
    # means {"channels": {"1000000000000": 1}} hangs or MemoryErrors *inside*
    # the op, and the queue loop is serial, so `stop` cannot be dequeued while
    # it does. Only bits 0-7 exist (r2d2.py:17-25) — reject the rest.
    channels = {}
    for key, value in p["channels"].items():
        bit, level = int(key), int(value)
        if not 0 <= bit <= LED_HOLO:
            raise ValueError(f"LED channel {bit} out of range 0-{LED_HOLO}")
        if not 0 <= level <= 255:
            raise ValueError(f"LED level {level} out of range 0-255")
        channels[bit] = level
    await r2.set_leds(channels)
    return {"set": p["channels"]}

async def _op_sound(r2, p):
    if "volume" in p:
        await r2.set_volume(int(p["volume"]))
    r = await r2.play_sound(int(p["id"]), int(p.get("mode", 0)))
    return {"id": p["id"], "err": _err_of(r)}

async def _op_stop_audio(r2, p):
    # Was `{"stopped": True}` unconditionally — the same hardcoded claim
    # `stop` was hardened against, under the same key name, so one op gave a
    # measured answer and its sibling gave a lie.
    err = _err_of(await r2.stop_audio())
    return {"stopped": err == "success", "err": err}

async def _op_dome(r2, p):
    start, target = await bounded_head_move(
        r2, delta=p.get("delta"), angle=p.get("angle"))
    # Clamp the settle. This sleep happens AFTER the dome is commanded to move,
    # and the queue loop is serial: an unbounded value would leave R2 mid-move
    # with `stop` undeliverable and the idle-timeout check unreachable, with
    # Ctrl-C at the operator's terminal as the only way out.
    settle = max(0.0, min(float(p.get("settle", 2.0)), MAX_SETTLE_S))
    await asyncio.sleep(settle)
    return {"start": start, "commanded": target, "now": await r2.get_head()}

async def _op_animation(r2, p):
    r = await r2.play_animation(int(p["id"]))
    return {"id": p["id"], "err": _err_of(r)}

async def stop_everything(r2, shield: bool = False) -> dict:
    """Halt animation and audio. THE single stop implementation.

    Shared by the `stop` op and the daemon's shutdown epilogue. The epilogue is
    the most important stop in the program and it used to print "stop sent"
    while discarding both Response objects — a rejected stop and a successful
    one were indistinguishable, on the path where nobody is watching.

    Each coroutine is created INSIDE its own try. Building them both up front
    (as this did) meant a failure at call time skipped the other half entirely
    and leaked an un-awaited coroutine — so the "always attempts both"
    guarantee was exactly false in the case it existed for.

    Catches BaseException because CancelledError is not an Exception: a Ctrl-C
    landing in the first stop must not skip the second. `shield=True` keeps the
    commands alive through that cancellation, for the shutdown path."""
    results = {}
    for name, make in (("animation", lambda: r2.stop_animation()),
                       ("audio", lambda: r2.stop_audio()),
                       # Added after #11: an animation drives LEG actions, and
                       # stop_animation is not documented to halt one already
                       # in flight. A stop that leaves the legs moving is not
                       # a stop — and a leg action in flight is the state that
                       # put R2 on the floor. Sent from every tier, like the
                       # other two: this is DID 0x17 traffic that HALTS motion,
                       # which is what the `read` ceiling's promise allows.
                       ("legs", lambda: r2.perform_leg_action(LEG_ACTION_STOP))):
        try:
            call = make()
            results[name] = _err_of(await (asyncio.shield(call) if shield else call))
        except BaseException as e:
            results[name] = f"{type(e).__name__}: {e}"
    failed = {k: v for k, v in results.items() if v != "success"}
    return {
        "stopped": not failed,
        "results": results,
        # Fixed shape: always present, None on success. A present-or-absent key
        # forces every consumer into .get() and cannot be relied on.
        "warning": (f"STOP DID NOT FULLY SUCCEED: {failed}. R2 may still be "
                    f"moving or playing audio — power him down by hand."
                    if failed else None),
    }


async def _op_stop(r2, p):
    """Emergency stop — always permitted at any tier.

    RAISES when the robot did not confirm both halts, so `./r2 send stop`
    exits non-zero. Returning ok:True with `stopped: false` buried in the
    payload meant `./r2 send stop || panic` saw success while the body read
    "R2 may still be moving". The commands are already on the wire by then —
    raising reports the outcome, it does not skip the attempt."""
    result = await stop_everything(r2)
    if not result["stopped"]:
        raise RuntimeError(f"{result['warning']} (results: {result['results']})")
    return result

async def _op_sensors(r2, p):
    """Turn the sensor stream on or off, and read back what actually took.

    Sits at `read`: DID_SENSOR cannot move him. It configures a notification
    stream, nothing else -- the same reasoning that puts `stop` at `read`.

    The three-call enable sequence is copied from spherov2's SensorControl
    (`controls/v2.py`), NOT invented: interval is set to 0 FIRST, then the
    extended mask, then the real interval. Setting the extended mask while a
    stream is already running at a live interval does not reliably take. This
    is exactly the kind of ordering nobody would guess from the packet spec,
    which is why it is copied verbatim and cited.
    """
    enable = _strict_bool(p.get("enable", True), "enable")
    if enable:
        groups = p.get("groups", ["accelerometer", "attitude"])
        ext_groups = p.get("ext_groups", ["r2_head_angle", "gyroscope"])
        mask, ext = sensor_mask(groups), ext_sensor_mask(ext_groups)
        if not mask and not ext:
            raise ValueError(
                f"no sensors selected. groups={groups!r} ext_groups="
                f"{ext_groups!r} matched nothing; valid names are "
                f"{[g for g, _ in SENSORS]} and {[g for g, _ in EXT_SENSORS]}")
        interval = max(10, min(int(p.get("interval", 250)), 10_000))
        count = max(0, min(int(p.get("count", 0)), 255))
    else:
        mask = ext = interval = count = 0
    errs = {
        "quiesce": _err_of(await r2.set_sensor_mask(0, count, mask)),
        "ext":     _err_of(await r2.set_extended_sensor_mask(ext)),
        "start":   _err_of(await r2.set_sensor_mask(interval, count, mask)),
    }
    # Read back rather than trust. A capability the library exposes is a claim
    # (`enable_idle_animations` is the standing counterexample), and silence on
    # a stream that was never enabled is indistinguishable from a robot that
    # feels nothing.
    readback = await r2.get_sensor_mask()
    ok = all(e == "success" for e in errs.values())
    # Assume a command we SENT may have taken effect, even unconfirmed.
    #
    # Keying this on `ok` was wrong in the dangerous direction: an enable
    # whose reply is lost (`send` returns None, so `_err_of` is not
    # "success") would leave the robot streaming while we recorded that it
    # was not -- and the exit path, which keys off this flag, would then skip
    # the disable. Silence is not evidence the packet missed.
    #
    # So: an ATTEMPTED enable sets the flag; only a CONFIRMED disable clears
    # it. A failed disable keeps it set, so the exit path tries again. Both
    # errors now fall towards "send a disable we did not need" rather than
    # "leave him streaming unattended".
    if enable:
        r2.sensor_streaming = True
    elif ok:
        r2.sensor_streaming = False
    return {"requested": {"interval": interval, "count": count,
                          "mask": mask, "ext_mask": ext},
            "errs": errs,
            "readback": ({"interval": readback[0], "count": readback[1],
                          "mask": readback[2]} if readback else None),
            "accepted": ok,
            "mask_confirmed": bool(readback) and readback[2] == mask,
            "note": ("extended mask has no documented read-back, so ext_mask "
                     "is UNCONFIRMED even when this says accepted")}


async def _op_events(r2, p):
    """Drain everything the robot said that nobody asked for."""
    return r2.drain_events()

# ── Why `idle` and `notify` are pinned to `motion` ───────────────────────────
# Both write to DID_ANIMATRONIC, the motion device. The `read` ceiling's
# contract is "nothing sent here can move him", so they sit at `motion`. Flat,
# permanent, no way to relax it.
#
# There WAS a mechanism to relax it: a marker file recording which CIDs a real
# robot had acknowledged, after which these ops dropped to `read`. It is gone,
# for two reasons.
#
# 1. It bought nothing. Its whole justification was that idle-disable had to be
#    reachable from the cheap sessions — but `enable_idle_animations` does not
#    exist on this firmware (REFUTED, see docs/research/r2-capabilities.md), and
#    no planned session needs `notify` below `motion` either: #8 runs at `leds`
#    and #9 at `audio` without it, and #11 is inherently a motion session
#    because the events it verifies are produced by animations and leg actions.
#    Passive observation needs no enable at all — `events` captures whatever
#    arrives regardless.
#
# 2. It kept getting the same bug. Three review rounds produced three distinct
#    evidence-borrowing defects at three granularities: global (notify vouched
#    for idle), per-op (a partial `notify` vouched for the CID it never sent),
#    per-CID-per-device (still an unauthenticated file in the requester's own
#    write domain, and a local process cannot start its own `--allow motion`
#    daemon, so it was a real bypass rather than a moot one). When a mechanism
#    reproduces its bug class at every level of refinement, the mechanism is
#    the problem.
#
# The evidence itself is not lost — it belongs in docs, not in runtime
# authorization state. CIDs 0x2A and 0x39 are recorded OBSERVED, and 0x2C
# REFUTED, in docs/research/r2-protocol.md.


def _idle_enable(p: dict) -> bool:
    """The single place the `idle` direction is derived.

    The tier gate authorises based on this value and the handler acts on it. If
    those were two separate expressions, changing the default in one of them
    would grant a request at `read` and then command motion — and no existing
    test would catch it, because they all pass `enable` explicitly."""
    return _strict_bool(p.get("enable", False), "enable")


async def _op_idle(r2, p):
    """Enable or disable R2's native idle animations.

    **REFUTED ON HARDWARE 2026-08-16 — this command does not exist on R2-D2.**
    `D2-6F6B` answers `bad_command_id` to DID 0x17 CID 0x2C, reproducibly.
    `enable_idle_animations` is a BB9E command (`bb9e.py:121`); the R2-D2 toy
    class (`r2d2.py:483-497`) never exposed it. See docs/research/
    r2-capabilities.md.

    The op is KEPT rather than deleted for two reasons: it is the record of a
    refuted claim, and if a firmware revision ever adds the command the gate
    will notice and promote it. It costs exactly one rejected packet to find
    out, which is the right price for not having to trust a memory.

    It fails LOUDLY, which is how the refutation was caught at all. An earlier
    version returned ok on a timeout or an error code — that would have handed
    over clean-looking data from a robot whose idle state was never changed."""
    enable = _idle_enable(p)
    r = await r2.enable_idle_animations(enable)
    err = _err_of(r)
    if r is None or r.err != 0:
        hint = ("  EXPECTED: this command does not exist on R2-D2 — it is a "
                "BB9E command (bb9e.py:121, absent from r2d2.py). Refuted on "
                "hardware 2026-08-16. There is no way to disable R2's idle "
                "behaviour; see docs/research/r2-capabilities.md."
                if err == "bad_command_id" else "")
        raise RuntimeError(
            f"R2 did not confirm idle {'enable' if enable else 'disable'} "
            f"({err}). Idle state is UNKNOWN — do not trust survey "
            f"observations from this session until it succeeds.{hint}")
    r2.idle_disabled = not enable
    return {"idle_enabled": enable, "err": err}

async def _op_notify(r2, p):
    """Enable robot-initiated notifications.

    Only two of the three have an enable command upstream.
    `play_animation_complete_notify` (animatronic.py:43) is a bare tuple with
    no setter at all, so whether it fires unprompted cannot be settled from
    source — play an animation, then read `events`. That is the empirical
    question issue #7 leaves open on purpose."""
    # Validate EVERY parameter before sending ANY command. Validating as we go
    # meant {"leg": true, "head_reset": "yes"} put the leg notify on the wire
    # and then returned ok:false — a refusal that had already half-executed.
    wanted = {name: _strict_bool(p[name], name)
              for name in ("leg", "head_reset") if name in p}
    if not wanted:
        raise ValueError('nothing to enable — pass {"leg": true} and/or '
                         '{"head_reset": true}. animation_complete has no '
                         'enable command; test it empirically via `events`.')
    senders = {"leg": r2.enable_leg_action_notify,
               "head_reset": r2.enable_head_reset_notify}
    out = {name: _err_of(await senders[name](value))
           for name, value in wanted.items()}
    # Fail loudly. 0x2A/0x39 are OBSERVED working on D2-6F6B, so a rejection
    # now means something is genuinely wrong — and a survey that silently
    # failed to enable leg notifications would conclude "R2 never emits
    # leg_action_complete", a wrong finding entering docs/.
    failed = {n: e for n, e in out.items() if e != "success"}
    if failed:
        raise RuntimeError(f"R2 did not confirm notify enable: {failed}. "
                           f"Absence of these events is NOT evidence they "
                           f"do not fire.")
    out["note"] = ("animation_complete has no enable command upstream; "
                   "play an animation and read `events` to find out if it fires")
    return out

# (tier, handler). Every tier is a plain string: an op's ceiling never depends
# on its arguments, and never on stored state.
OPS = {
    "status":     ("read",   _op_status),
    "battery":    ("read",   _op_battery),
    "head":       ("read",   _op_head),
    "stance":     ("read",   _op_stance),    # get_leg_action — a READ, like head
    "leg_pos":    ("read",   _op_leg_position),  # get_leg_position — also a READ
    "read_char":  ("read",   _op_read_char),
    "gatt":       ("read",   _op_gatt),
    "stop":       ("read",   _op_stop),      # always allowed: default to STOP
    "events":     ("read",   _op_events),    # reading is never a hazard
    # `read`: DID_SENSOR configures a notification stream and cannot move him.
    "sensors":    ("read",   _op_sensors),
    "notify":     ("dome",   _op_notify),   # writes DID_ANIMATRONIC
    "idle":       ("dome",   _op_idle),     # writes DID_ANIMATRONIC (refuted)
    "leds":       ("leds",   _op_leds),
    "sound":      ("audio",  _op_sound),
    "stop_audio": ("audio",  _op_stop_audio),
    "dome":       ("dome",   _op_dome),
    # `stance`, not `dome`: OBSERVED #11 — an authored animation drives leg
    # actions and knocked R2 over. An animation is a stance command whose
    # contents we cannot inspect before sending it.
    "animation":  ("stance", _op_animation),
    # The op that deploys or retracts the third leg. Same rung as `animation`
    # for the same reason: both can end with him on the floor.
    "set_stance": ("stance", _op_set_stance),
}


def op_tier(op: str, params: dict | None = None) -> str:
    """The tier this op needs. Total: it cannot raise, and it cannot depend on
    the request or on anything stored on disk.

    It used to do both, and both were mistakes. Params-dependent tiers meant
    the authorisation check and the handler each derived the answer separately;
    a marker file meant authorisation state lived where the requester could
    write it. `params` is kept only so call sites read symmetrically."""
    return OPS[op][0]


def allowed_ops(ceiling: str) -> list[str]:
    return sorted(o for o in OPS if _tier_ok(ceiling, OPS[o][0]))


async def handle_request(r2, payload: dict, ceiling: str, log=print) -> dict:
    """Execute one bridge request and return the response dict.

    Split out of the daemon loop so the permission ladder and every op can be
    unit-tested against a fake R2, with no radio and no file queue. The daemon
    loop below is then only queue plumbing."""
    ts = time.strftime("%H:%M:%S")
    # Validate the envelope before touching it. A queue file holding valid JSON
    # that is not an object (`[1,2]`, `"hi"`, `42`) used to raise AttributeError
    # here, and `{"op": []}` raised TypeError on the unhashable dict lookup —
    # either one unwound the poll loop and ended the session, taking the `stop`
    # channel with it. Any local process could do that with one stray file.
    if not isinstance(payload, dict):
        log(f"[{ts}] REFUSED malformed request (expected a JSON object, "
            f"got {type(payload).__name__})")
        return {"ok": False, "error":
                f"request must be a JSON object, got {type(payload).__name__}"}
    op = payload.get("op")
    params = payload.get("params") or {}
    if not isinstance(params, dict):
        log(f"[{ts}] REFUSED {op!r} — 'params' must be a JSON object")
        return {"ok": False, "op": op, "error":
                f"'params' must be a JSON object, got {type(params).__name__}"}

    if not isinstance(op, str) or op not in OPS:
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
    # BEFORE the wipe below, which is the whole point: that wipe is what makes
    # a second daemon destructive rather than merely redundant.
    holder = acquire_daemon_lock(resolve_tier(args.allow))
    if holder is not None:
        started = holder.get("started") or 0
        print(f"\nREFUSING TO START — another r2 daemon already holds the bridge.\n"
              f"  pid     : {holder.get('pid')}\n"
              f"  ceiling : {holder.get('ceiling')}\n"
              f"  started : {time.strftime('%H:%M:%S', time.localtime(started)) if started else 'unknown'}\n"
              f"  queue   : {BRIDGE}\n\n"
              f"Two daemons wipe each other's queue and race to consume "
              f"requests,\nso which --allow ceiling applies stops being "
              f"predictable. Stop the\nrunning one first (Ctrl-C in its "
              f"terminal, or `kill {holder.get('pid')}`).\n"
              f"If nothing is actually running, delete {LOCK}.\n")
        return 1

    try:
        return await _run_daemon(args)
    finally:
        # BaseException-proof by construction: a bare finally also covers the
        # Ctrl-C that ends most sessions. A lock surviving its daemon would
        # refuse every future start until deleted by hand.
        release_daemon_lock()


async def _run_daemon(args) -> int:
    for d in (REQ_DIR, RESP_DIR):
        d.mkdir(parents=True, exist_ok=True)
    for stale in list(REQ_DIR.glob("*.json")) + list(RESP_DIR.glob("*.json")):
        stale.unlink()

    # Resolve the alias HERE, once, so everything downstream — the banner, the
    # refusal messages, the lock file — names the tier that is actually in
    # force. Printing "MOTION" while enforcing `dome` is the kind of mismatch
    # this whole split exists to remove.
    ceiling = resolve_tier(args.allow)
    if ceiling != args.allow:
        print(f"\nNOTE: --allow {args.allow} is deprecated and has been read as "
              f"'{ceiling}'.\n      It no longer grants `animation`: #11 observed an "
              f"authored\n      animation drive leg actions and put R2 on the floor. "
              f"Use\n      --allow stance if you actually want animations.\n")
    allowed = allowed_ops(ceiling)
    print(f"\n{'='*62}\n  r2 bridge daemon — permission ceiling: {ceiling.upper()}")
    print(f"  allowed ops: {', '.join(allowed)}")
    if ceiling != TIERS[-1]:
        print(f"  refused    : {', '.join(sorted(set(OPS) - set(allowed)))}")
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
                    # Belt and braces: handle_request already validates its
                    # input, but nothing a queue file contains may end a live
                    # session — that session is the only way to send `stop`.
                    try:
                        resp = await handle_request(r2, payload, ceiling)
                    except Exception as e:
                        resp = {"ok": False, "error":
                                f"request handler crashed: {type(e).__name__}: {e}"}
                        print(f"[{time.strftime('%H:%M:%S')}] !! {resp['error']}")
                    try:
                        (RESP_DIR / req.name).write_text(json.dumps(resp, indent=2))
                    except Exception as e:
                        print(f"[{time.strftime('%H:%M:%S')}] !! could not write "
                              f"response for {req.name}: {e}")
        except (KeyboardInterrupt, asyncio.CancelledError):
            print("\n\ninterrupted — stopping R2 before disconnect")
        finally:
            # Default to STOP: never leave audio or an animation running.
            #
            # `except BaseException`, not `except Exception`: CancelledError is
            # NOT an Exception subclass, so a SECOND Ctrl-C — exactly what an
            # operator does when R2 doesn't stop on the first — used to raise
            # straight through this block and skip the stop entirely, leaving
            # an animation and audio running on an unattended robot. shield()
            # keeps the stops themselves alive through that cancellation.
            # Same implementation as the `stop` op, so this path cannot drift
            # away from it — and it now reports what R2 ACTUALLY said. R2 is
            # known to reject at least one animatronic CID (0x2C), so "the
            # command returned" and "the robot stopped" are not the same claim.
            halt = await stop_everything(r2, shield=True)
            print(f"stop on exit: {json.dumps(halt['results'])}")
            if halt["warning"]:
                print(f"!! {halt['warning']}")
            # Housekeeping, deliberately AFTER the halt and deliberately NOT
            # inside stop_everything. The stop is the safety action and must
            # stay minimal -- giving it a sensor-config write would hand the
            # one path that must never fail a new way to be slow or throw.
            #
            # Only what THIS session switched on: another process may own the
            # stream, and clobbering it is the same bug pointing the other way.
            #
            # Whether the mask survives a disconnect is UNKNOWN and untested.
            # If it does not, this is a harmless no-op; if it does, it stops an
            # unattended robot streaming for hours on a droid with no off
            # switch (#38). Cheap either way, so it is not worth measuring
            # first. Upstream provides `disable_all` (spherov2
            # controls/v2.py:326) but never calls it -- nothing in the stack
            # turns this off but us.
            #
            # NOT A FAILSAFE. A SIGKILL or a dead host skips this block
            # entirely. A real failsafe would be device-side on link loss, and
            # that firmware is not ours to change.
            if r2.sensor_streaming:
                try:
                    off = _err_of(await asyncio.shield(
                        r2.set_sensor_mask(0, 0, 0)))
                    off_ext = _err_of(await asyncio.shield(
                        r2.set_extended_sensor_mask(0)))
                    print(f"sensor stream off: {off}/{off_ext}")
                except BaseException as e:
                    # Never let housekeeping mask the stop that just ran, and
                    # never let it turn a clean exit into a traceback.
                    print(f"!! could not disable sensor stream: "
                          f"{type(e).__name__}: {e}")
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
    dae.add_argument("--allow", choices=TIERS + list(TIER_ALIASES), default="read",
                     help="permission ceiling (default: read-only). 'motion' is "
                          "a deprecated alias for 'dome' and does NOT grant "
                          "animations any more — see #11")
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
