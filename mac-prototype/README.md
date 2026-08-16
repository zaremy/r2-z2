# Mac prototype

Fast-iteration host for proving the interaction model before any embedded work.
Python here is disposable — it exists to learn R2's real behavior vocabulary,
not to be ported. See `../docs/research/embedded-path.md`.

## Setup (already done)

```bash
python3.14 -m venv .venv && .venv/bin/pip install bleak
```

Installed: **bleak 3.0.2** on **Python 3.14.6**.

## ⚠️ One-time Bluetooth permission — needed before the first scan

Running `r2_probe.py` under plain `.venv/bin/python` **crashes with SIGABRT**
before it reaches the radio:

> This app has crashed because it attempted to access privacy-sensitive data
> without a usage description. The app's Info.plist must contain an
> `NSBluetoothAlwaysUsageDescription` key.

Homebrew's `Python.framework` re-execs into its own `Python.app`, and that
bundle has no Bluetooth usage key. **Use `./r2` instead of `.venv/bin/python`** —
it launches `tools/R2Probe.app`, a project-local ad-hoc-signed copy of that
bundle with the key added.

macOS still needs a one-time grant, attributed to whichever app launches it, and
that prompt only appears in an interactive session:

```bash
./r2 scan
```

Run that **once from Terminal.app** and approve the Bluetooth prompt. If no
prompt appears, add the terminal under
System Settings → Privacy & Security → Bluetooth. After that, scans work from
anywhere.

Rebuild the bundle after `brew upgrade python@3.14`:

```bash
./r2 --rebuild
```

## Safe bring-up sequence

Each step is a separate command **on purpose**. Do not skip ahead, and do not
build a "run all tests" wrapper — an unattended actuator sweep is exactly the
failure mode `../CLAUDE.md` forbids.

```bash
./r2 scan                        # 1. passive — no connection
./r2 info                        # 2. connect, handshake, read-only queries
./r2 led --color 0,0,255         # 3. LEDs only, no motion
./r2 sound --id 2813             # 4. one sound (R2_HEY_1), then stop
./r2 dome --angle 20             # 5. FIRST MOVEMENT — small dome, returns to 0
./r2 animation --id 35           # 6. one authored animation (WWM_CURIOUS)
```

Stance and locomotion are **not implemented in this tool**, deliberately.
They come after the above all pass.

Before step 1: R2 powered on, not connected to a phone (close the Sphero app),
within ~2 m. If he does not appear, briefly place him on the charger to wake him.

## What `r2_probe.py` is

A clean-room implementation of the Sphero V2 packet layer plus a small command
set, with **no dependency on `spherov2`** — so a bleak version bump in the
upstream clone cannot break bring-up, and so the same logic can be ported
directly to C for the ESP32.

It is verified, not assumed: the encoder produces byte-identical packets to
`spherov2.Packet.build()` **and** to the hand-rolled C encoder in
`claude-r2d2-buddy`, across wake, battery, LED, audio, head float (positive and
negative), animation play/stop, and a payload deliberately containing
`0x8D 0xD8 0xAB` to exercise escaping.

Safety properties built in:

- Default subcommand does nothing but listen.
- Dome angle is clamped to ±45° (the robot allows -162…+182).
- 120 ms minimum inter-command interval, per `toy/r2d2.py:15`.
- Clean disconnect on exit and on Ctrl-C.
- Every TX and RX packet is logged as hex with the decoded error name.

## Known gaps

- **Never run against the physical robot** — blocked on the permission grant
  above. Nothing in `../docs/research/r2-protocol.md` marked OBSERVED has been
  confirmed on hardware.
- No sensor streaming yet.
- No reconnect logic; the probe is one-shot by design.
- `research/external/sphero-r2d2` pins `bleak>=1.1.1` and we run 3.0.2. Not an
  issue for the probe (it does not import spherov2), but relevant if that
  library is ever used directly.
