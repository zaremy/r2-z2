# Mac prototype

Fast-iteration host for proving the interaction model before any embedded work.
Python here is disposable — it exists to learn R2's real behavior vocabulary,
not to be ported. See `../docs/research/embedded-path.md`.

## Setup (already done)

```bash
python3.14 -m venv .venv && .venv/bin/pip install bleak
```

Installed: **bleak 3.0.2** on **Python 3.14.6**.

## ⚠️ One-time Bluetooth permission (done — kept for rebuilds and new machines)

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

## Bridge daemon — driving R2 from an agent

macOS attributes a Bluetooth request to the **responsible process**. Under
Claude Code that is `claude.app` (`com.anthropic.claude-code`), whose bundle has
no `NSBluetoothAlwaysUsageDescription`, so any child of it touching
CoreBluetooth is killed — even though `R2Probe.app` carries the key. Terminal is
an Apple system app and is exempt, which is why it works there. An MCP server
would not help: same host, same responsible process.

So: **you** start the daemon once from Terminal; anything else drives it through
a file queue.

```bash
./r2 daemon --allow read          # read-only (default)
./r2 daemon --allow leds          # + LED writes
./r2 daemon --allow audio         # + sounds
./r2 daemon --allow motion        # + dome and animations
```

The `--allow` ceiling mirrors the fixed bring-up ladder in `../CLAUDE.md`:
**read-only → LEDs → audio → small dome → stance → locomotion.** Ops above the
ceiling are **refused and logged, never executed** — so the class of physical
behavior R2 can produce is set by you, at launch, not by whatever sends a
command. Raising it means restarting the daemon, deliberately.

Other safety properties:

- Every request is printed with a timestamp before it runs — the Terminal window
  is a live audit log, and Ctrl-C is always available
- `stop` (stop animation + stop audio) is permitted at **every** tier
- On exit — Ctrl-C, idle timeout, or error — it sends stop before disconnecting.
  Default to STOP, never last-command
- Idle timeout disconnects after 15 minutes so R2 is never left awake unattended

Then, from anywhere:

```bash
./r2 send status
./r2 send battery
./r2 send read_char --params '{"uuid":"00002a19-0000-1000-8000-00805f9b34fb"}'
./r2 send leds --params '{"channels":{"0":0,"1":0,"2":255}}'
./r2 send sound --params '{"id":2813,"volume":80}'
./r2 send dome --params '{"delta":20}'
./r2 send stop
```

Responses print as JSON. The queue lives in `.bridge/` (gitignored).

## Survey harness — `r2_survey.py`

Tracks the S1 capability survey ([#5](https://github.com/zaremy/r2-z2/issues/5)).
**It does not fire anything** — it emits the command for the next item and you
run it, deliberately, one at a time. That is enforced by a test that sabotages
`subprocess`, `os.system`, `os.popen` and `socket` and requires every command to
still work, so the constraint survives future edits.

```bash
./r2s manifest --tier leds       # 104 items total; filter per session
./r2s next --count 5             # prints commands — does NOT run them
./r2s record led:0 --json '{"outcome":"played","energy_cost_class":"low",
                            "wear_class":"none","recommended_cooldown_s":0}'
./r2s status                     # progress per tier
./r2s export                     # markdown table (blocked while attempts are unresolved)
```

State lives in `.survey/` (gitignored). `attempts.jsonl` is append-only and
fsync'd per write and is the source of truth; `state.json` is a cache you can
delete and rebuild with `./r2s rebuild`.

If a session aborts — battery, dropped link, daemon timeout — mark what was
in flight so it cannot masquerade as observed:

```bash
./r2s resolve anim:8 --state aborted_unknown --reason "battery died"
```

An `aborted_unknown` on an animation that may drive the body should be moved to
`unsafe_replay_review`; `next` then refuses to re-emit it until you decide,
because a blind retry of a driving id while you are not braced for it is the
hazard the whole safety model exists to prevent.

## Safe bring-up sequence

Each step is a separate command **on purpose**. Do not skip ahead, and do not
build a "run all tests" wrapper — an unattended actuator sweep is exactly the
failure mode `../CLAUDE.md` forbids.

```bash
./r2 scan                        # 1. passive — no connection
./r2 info                        # 2. connect, handshake, read-only queries
./r2 led --color 0,0,255         # 3. LEDs only, no motion
./r2 sound --id 2813             # 4. one sound (R2_HEY_1), then stop
./r2 dome --delta 20             # 5. FIRST MOVEMENT — small dome, returns
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
- Dome moves are bounded by **travel, not destination** — capped at ±45° from a
  freshly read position. This matters: `set_head_position` is absolute and the
  dome does not rest at 0 (this unit was found at **103°**), so clamping the
  target would have turned a "small test" into a ~150° swing.
- 120 ms minimum inter-command interval, per `toy/r2d2.py:15`.
- Clean disconnect on exit and on Ctrl-C.
- Every TX and RX packet is logged as hex with the decoded error name.

## Known gaps

- **Connect, handshake and read-only queries are confirmed on hardware**
  (2026-08-15): wake ACKed, battery 3.95 V, head 103.08°. **No actuator has been
  driven** — no LED, sound, dome or animation command has reached the robot, so
  those paths in `../docs/research/r2-protocol.md` remain source-verified only.
- No sensor streaming yet.
- No reconnect logic; the probe is one-shot by design.
- `reference/sphero-r2d2` pins `bleak>=1.1.1` and we run 3.0.2. Not an
  issue for the probe (it does not import spherov2), but relevant if that
  library is ever used directly.
