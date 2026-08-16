# R2-D2 BLE + Sphero V2 Protocol — traced from source

Evidence labels: **OBSERVED** (read in current source), **INFERRED**,
**UNKNOWN**, **SPECULATION**. Citations are `repo/path:line` against the SHAs
in [source-map.md](source-map.md).

Everything marked OBSERVED below has additionally been **executed**: our
independent implementation in `mac-prototype/r2_probe.py` produces byte-identical
packets to `spherov2`'s `Packet.build()` and to the hand-rolled C encoder in
`claude-r2d2-buddy`. Nothing here has yet been confirmed against the physical
robot — see [Open questions](#open-questions).

---

## 1. Discovery

**OBSERVED** — R2-D2 advertises a local name with prefix `D2-`.
`sphero-r2d2/spherov2/toy/r2d2.py:15` declares
`ToyType('R2-D2', 'D2-', 'D2', .12)` — (display name, filter prefix, prefix,
`cmd_safe_interval`). Matching is `name.startswith(filter_prefix)` at
`spherov2/scanner.py:62`. `claude-r2d2-buddy/main/common.h:22` uses the same
prefix, matched with `strncmp` at `r2d2_central.c:299`.

**OBSERVED** — the suffix is 4 hex characters (e.g. `D2-55E3`), per
`sphero-r2d2/docs/BLE_CHARACTERISTICS.md`. INFERRED from a doc, not code.

## 2. GATT layout

Base UUID pattern `XXXXXXXX-574f-4f20-5370-6865726f2121`; the tail is ASCII
`WO O Sphero!!`.

| Role | UUID | Source |
|---|---|---|
| CONNECT service | `00020001-…` | `freer2/index.js:3`, `claude-r2d2-buddy/main/common.h:31` |
| HANDLE char (notify) | `00020002-…` | `freer2/index.js:6`, `common.h:33` |
| CONNECT char (anti-DoS write) | `00020005-…` | `sphero-r2d2/spherov2/toy/bb9e.py:22`, `freer2/index.js:4`, `common.h:35` |
| MAIN service | `00010001-…` | `freer2/index.js:8`, `common.h:25` |
| MAIN char (command + response) | `00010002-…` | `spherov2/toy/__init__.py:131`, `freer2/index.js:9`, `common.h:27` |

**OBSERVED** — the command channel and the response channel are the *same*
characteristic. `spherov2/toy/__init__.py:131` literally assigns
`_response_uuid = _send_uuid = '00010002-…'`.

**OBSERVED** — legacy V1 UUIDs (`22bb746f-…`) are present in the codebase for
older Sphero toys and in commented-out blocks at
`spherov2/toy/__init__.py:132-160`. **They are not the R2-D2 path.** Do not
port them.

## 3. Connection handshake

**OBSERVED** sequence, corroborated by three independent implementations:

1. Connect (GAP).
2. Discover the CONNECT service; subscribe to HANDLE char notifications.
   (`claude-r2d2-buddy/main/r2d2_central.c:194-202`;
   `freer2/index.js:87-93`.)
3. **Write the anti-DoS magic** `usetheforce...band` (18 bytes ASCII) to
   CONNECT char `00020005-…`.
   `spherov2/toy/bb9e.py:22`; `common.h:39`; `freer2/index.js:11`.
4. Discover the MAIN service; subscribe to MAIN char notifications.
5. Send wake.

**OBSERVED, and architecturally important** —
`claude-r2d2-buddy/main/r2d2_central.c:96-110,252-256` states and implements
that **R2-D2 only exposes MAIN_SERVICE after the magic write succeeds**, so
service discovery must run *twice*: once pre-handshake (find CONNECT), once
post-handshake (find MAIN). `freer2/index.js:87-96` does the same nested
re-discovery. spherov2 sidesteps this by addressing characteristics by UUID
through Bleak, which caches the full GATT table — so the Python path does not
reveal the constraint. **A naive ESP32 port that discovers once will fail.**

**INFERRED** — the two-phase discovery is a property of the robot's GATT
server, not of NimBLE. Two unrelated stacks (NimBLE, noble) both needed it.

**UNKNOWN** — whether the HANDLE char (`00020002-…`) subscription is required
or merely observed-and-copied. spherov2 never touches it and reportedly works;
both C and JS implementations subscribe to it. Cheap to keep; test later.

## 4. Packet format (Sphero API V2)

**OBSERVED** — `spherov2/controls/v2.py:14-159`.

```
[SOP] [FLAGS] [TID?] [SID?] [DID] [CID] [SEQ] [ERR?] [DATA…] [CHK] [EOP]
```

| Field | Value / rule |
|---|---|
| SOP | `0x8D` |
| EOP | `0xD8` |
| FLAGS | bitfield, below |
| TID/SID | present only if the corresponding flag is set; R2-D2 does not use them (`_require_target = False`) |
| SEQ | `0…0xFE`, incremented per packet (`Packet.Manager`, `v2.py:147-159`) |
| ERR | present only on responses (`is_response` set) |
| CHK | `0xFF - (sum(FLAGS…last DATA byte) & 0xFF)` — `spherov2/helper.py` |

Flags (`v2.py:27-35`):

| Bit | Value | Name |
|---|---|---|
| 0 | `0x01` | is_response |
| 1 | `0x02` | requests_response |
| 2 | `0x04` | requests_only_error_response |
| 3 | `0x08` | is_activity |
| 4 | `0x10` | has_target_id |
| 5 | `0x20` | has_source_id |
| 7 | `0x80` | extended_flags |

**OBSERVED** — outbound commands use `FLAGS = 0x0A`
(`requests_response | is_activity`), set at `v2.py:152`. The C code hardcodes
the same `0x0A` at `r2d2_central.c:278` and `translator.c:28`, where the
comment glosses bit 3 as "resetInactivityTimeout" — a **naming conflict**
with spherov2's "is_activity". Same bit, same value; the C reading is the more
useful one operationally (it explains why re-sending any command keeps the
robot awake).

### Escaping

**OBSERVED** — `v2.py:37-43, 87-141`. Applied to the body only, *after* the
checksum is computed, never to SOP/EOP themselves.

| Raw | Encoded |
|---|---|
| `0xAB` | `0xAB 0x23` |
| `0x8D` | `0xAB 0x05` |
| `0xD8` | `0xAB 0x50` |

Endianness is **big-endian** throughout: `spherov2/helper.py` `to_bytes`/`to_int`
use `byteorder='big'`; floats are `struct.pack('>f', …)`
(`commands/animatronic.py:41`).

### Verification

`mac-prototype/r2_probe.py` reimplements the above independently. Byte-exact
agreement with `spherov2.Packet.build()` across wake, battery, LED (16-bit
mask), audio, head-set (positive and negative float), animation play/stop, and
a payload deliberately containing `0x8D 0xD8 0xAB` to exercise escaping;
response parsing round-trips a float. The wake and LED packets also match the
hand-rolled C encoders at `r2d2_central.c:277-278` and `translator.c:61-67`
byte for byte.

## 5. Transport rules

- **OBSERVED** — writes are chunked to 20 bytes (`spherov2/toy/__init__.py:79`).
- **OBSERVED** — a **120 ms** minimum interval between commands for R2-D2
  (`toy/__init__.py:81` sleeps `cmd_safe_interval`; value `.12` from
  `toy/r2d2.py:15`). BB-8 is 60 ms; BB-9E is 120 ms.
- **OBSERVED, conflicting** — spherov2 writes **with** response
  (`adapter/bleak_adapter.py`: `write_gatt_char(uuid, data, True)`); the ESP32
  firmware writes **without** response
  (`r2d2_central.c:162 ble_gattc_write_no_rsp_flat`). Both are reported
  working. INFERRED: write-without-response is fine and cheaper, but it removes
  ATT-level backpressure, which likely makes the 120 ms interval *more*
  important on the embedded side, not less.
- **OBSERVED** — responses may arrive fragmented, in the worst case one byte
  per ATT notification. `r2d2_central.c:68-93` documents exactly this and
  resynchronises on SOP, discarding bytes while idle. `spherov2`'s
  `Packet.Collector` (`v2.py:161-174`) accumulates until EOP but does *not*
  resync on SOP — the C version is the more robust design. **Port the C
  behaviour.** Our probe already does (`r2_probe.py`, `_on_notify`).

## 6. Wake / keepalive / sleep

| Action | DID | CID | Payload |
|---|---|---|---|
| Wake | `0x13` (19, Power) | `0x0D` | — |
| Sleep | `0x13` | `0x01` | — |
| Battery voltage | `0x13` | `0x03` | — |

Encoded wake with `seq=0`: `8D 0A 13 0D 00 D5 D8` (verified identical across
all three implementations).

**OBSERVED** — `r2d2_central.c:389-405` re-sends **wake** every 3 s as a
keepalive, with an explicit comment that it is idempotent and resets the
inactivity timer, and a warning that **CID `0x01` is SLEEP and must not be used
as a keepalive**. `freer2/index.js:12-13` names the same two triples
`MSG_INIT` / `MSG_OFF`. Carry that warning into our code.

**UNKNOWN** — the actual inactivity timeout. 3 s is one implementer's choice,
not a measured value. Worth measuring; a slower keepalive saves radio time and
battery.

## 7. Command surface traced

Device IDs are the `_did` class attribute on each `spherov2/commands/*.py`.

| Subsystem | DID | File |
|---|---|---|
| SystemInfo | 17 (`0x11`) | `commands/system_info.py` |
| Power | 19 (`0x13`) | `commands/power.py:63` |
| Drive | 22 (`0x16`) | `commands/drive.py` (corroborated `freer2/index.js:18`) |
| Animatronic | 23 (`0x17`) | `commands/animatronic.py:29` |
| Sensor | 24 (`0x18`) | `commands/sensor.py` (corroborated `freer2/index.js:27`) |
| IO | 26 (`0x1A`) | `commands/io.py:52` |

Traced command paths, public API → wire:

| Behaviour | DID | CID | Payload | Source |
|---|---|---|---|---|
| Play animation | `0x17` | `0x05` | `u16 BE` animation id | `animatronic.py:33` |
| Stop animation | `0x17` | `0x2B` | — | `animatronic.py:69` |
| Set head position | `0x17` | `0x0F` | `float32 BE` degrees | `animatronic.py:41` |
| Get head position | `0x17` | `0x14` | — → `float32 BE` | `animatronic.py:47` |
| Perform leg action | `0x17` | `0x0D` | `u8` `R2LegActions` | `animatronic.py:37` |
| Set leg position | `0x17` | `0x15` | `float32 BE` | `animatronic.py:51` |
| Enable idle animations | `0x17` | `0x2C` | `u8` bool | `animatronic.py:73` |
| Play audio file | `0x1A` | `0x07` | `u16 BE` id + `u8` mode | `io.py:60` |
| Set volume | `0x1A` | `0x08` | `u8` | `io.py:64` |
| Stop all audio | `0x1A` | `0x0A` | — | `io.py:72` |
| **LEDs (16-bit mask)** | `0x1A` | `0x0E` | `u16 BE` mask + `u8[]` values | `io.py:76` |
| LEDs (8-bit mask) | `0x1A` | `0x1C` | `u8` mask + `u8[]` values | `io.py:88` — *marked untested upstream* |
| Wake | `0x13` | `0x0D` | — | `power.py` |
| Drive with heading | `0x16` | `0x07` | speed/heading/flags | `commands/drive.py`, `freer2/index.js:18` |

**Use the 16-bit LED mask (`CID 0x0E`).** `spherov2/toy/bb9e.py:153` exposes
`set_all_leds_with_16_bit_mask` — and R2D2 subclasses BB9E — while `io.py:87`
tags the 8-bit variant `# Untested / Unknown Param Names`.
`claude-r2d2-buddy/main/translator.c:61-77` uses `CID 0x0E` with masks `0x0007`
(front RGB) and `0x0070` (back RGB) on real hardware. Values follow the mask in
ascending-bit order.

`R2LegActions` (`animatronic.py:16-20`): `STOP=0, THREE_LEGS=1, TWO_LEGS=2,
WADDLE=3`. Read-back enum `R2DoLegActions` adds `TRANSITIONING=4`.

### Notifications available

`animatronic.py:43` `play_animation_complete_notify = (23, 17, 0xff)`;
`:61` `leg_action_complete_notify = (23, 38, 0xff)`;
`:87` `head_reset_to_zero_notify = (23, 58, 0xff)`.
**INFERRED** — animation-complete is the right signal for a non-blocking
choreography scheduler; polling or fixed sleeps would fight the robot.

## 8. Error codes

`v2.py:45-56`: `0x00` success, `0x01` bad_device_id, `0x02` bad_command_id,
`0x03` not_yet_implemented, `0x04` command_is_restricted, `0x05`
bad_data_length, `0x06` command_failed, `0x07` bad_parameter_value, `0x08`
busy, `0x09` bad_target_id, `0x0A` target_unavailable.

**INFERRED** — `0x02 bad_command_id` and `0x03 not_yet_implemented` are how a
firmware-version difference will surface. Our probe prints the decoded error
name for exactly this reason.

## 9. Disconnect and recovery

**OBSERVED** — `r2d2_central.c:333-349`: on `BLE_GAP_EVENT_DISCONNECT`, clear
every characteristic handle, clear `ready`, clear `handshake_sent`, reset the
RX reassembly buffer, and restart scanning. The handshake must be redone from
scratch — handles are not durable across a reconnect.

**OBSERVED** — spherov2 has no reconnect logic at all; `Toy.__exit__` just
closes. Reconnect is ours to build.

**UNKNOWN** — behaviour when the robot sleeps under us versus a radio drop; and
whether a stale bonded pairing on a phone blocks connection.

## Open questions

1. **Nothing verified against the physical robot yet** — no BLE grant on this
   host. See `mac-prototype/README.md`.
2. Real inactivity timeout (keepalive period is a guess).
3. Whether the HANDLE char subscription is functionally required.
4. Firmware version reporting — `commands/system_info.py` is untraced; we do
   not yet know what our unit reports.
5. Whether write-without-response is safe at the 120 ms cadence.
