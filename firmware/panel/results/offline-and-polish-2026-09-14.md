# OFFLINE, the wake frame, and four fixes — on glass, 2026-09-14

Two runs after `tour-2026-09-14.md`, and between them the last states nobody
had seen are now photographed.

## Reaching `offline` without touching R2

`-DPANEL_P4_IDLE=ON` now sets scan-only **before the BLE host syncs**. It used
to set it from its own task 500 ms in, by which time `on_sync` had already
started a scan that could find him and connect — which is why an earlier
session recorded this build as unable to produce an offline frame. Set early,
the panel never connects, `waking` runs out at 4.1 s (#161) and `offline`
follows, deterministically, with R2 sitting right there switched on.

## What the two frames show

**`wake-frame-offline`** — the wake frame (#162), captured 700 ms after it
rose by `-DPANEL_SHOT_WAKE=ON`:

- 26x26 amber swatch at (26,48) — decoded, not eyeballed: exactly 26x26 and
  `F2B23C` to within RGB565 rounding;
- **OFFLINE** in amber Michroma 32, `R2 LINK DOWN` beneath it;
- the boxed **R2** in amber with `BLE` / **NOT SEEN** beside it. NOT SEEN
  because this boot never saw him, which is the case the count was taught to
  refuse: the attempt began at boot, so its age is how long *we* have been
  looking, not how long he has been gone;
- the fault chain with R2's node a hollow amber ring and its label amber,
  MIC/NET/LLM dim — three of four unknown, as this board's single sensor
  demands;
- no status chrome, as the reference has none there.

**`face-offline`** — the resting face the frame decays into:

- amber swatch, **OFFLINE** in white (the word carries colour only on the wake
  frame), `R2 LINK DOWN`;
- **R2 PWR** an empty grey track and `----`; **DOME** a dial with no needle,
  a grey hub and `----`. Nothing R2 said survives his link (AC7), and this is
  what that looks like rather than what it sounds like;
- the fault chain, and the page dots below it.

## The four fixes, verified

Everything below was found by looking at the previous run's frames.

| Was | Now |
|---|---|
| `PAIRS WITH  FIRST D2-` — read like a cut-off string | `ANY D2-*` |
| `R2 FW ----`, permanently: nothing ever asked | **`7.0.101`** — probed once per connection, read tier |
| `FREE HEAP 7203 KB` — counted 8 MB of PSRAM | **`183 KB`** internal, the heap that actually runs out |
| VOICE/CAMERA notes 12 px above their body's centre | centred: measured midpoint **262**, body centre **262** |

The version probe **latches on a successful send**, not on the attempt: it is
the only one-shot request in the link loop, so a single dropped GATT write
would otherwise blank `R2 FW` for the whole connection and leave `ANSWERED`
one short with nothing able to close it. Found in review; re-photographed
after the fix, still `7.0.101`.

Only the `ANY D2-*` string has a host test, and that one asserts shipped data
rather than a guard. The latch, the heap call and the note geometry are
device-only and rest on the frames in this directory.

`ANSWERED` reads **4/4** on the polished build — the version probe is the
fourth, and it is counted as asked, so the ratio stays honest.

## Still not proven

**Touch hardware.** Every frame here was produced without a finger: the tour
enters below `panel_touch.c`. The controller, the press and release edges, the
tap-versus-scroll thresholds and the moving-list guard remain unobserved, and
only a hand on the glass can settle them.

**`danger` and the other two offline views** (NET, LLM) have no source on this
board and were not reached.
