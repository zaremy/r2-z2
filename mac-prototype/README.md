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

### Native idle — REFUTED, you cannot turn it off

> **R2-D2 does not implement `enable_idle_animations`.** Verified 2026-08-16
> against `D2-6F6B`: `DID 0x17 CID 0x2C` → `bad_command_id`, reproducibly.
> It is a **BB9E** command (`bb9e.py:121`); the R2-D2 toy class
> (`r2d2.py:483-497`) never listed it. The CID was taken from the shared
> `Animatronic` *commands* module without checking the *toy* class.
>
> The `idle` op is kept as the record of a refuted claim — and because if a
> firmware revision ever adds the command, the gate will notice. Running it
> costs one rejected packet:
>
> ```bash
> ./r2 send idle --params '{"enable": false}'   # -> ok:false, bad_command_id
> ```
>
> **Open question, now the important one: does R2-D2 have a native idle loop at
> all?** Baseline measured the same session — over 30 s, connected and awake,
> the dome did not move and he emitted zero unsolicited packets. Suggestive,
> not conclusive. If there is no idle loop, the survey never needed this
> precondition; if there is one, it cannot be disabled and the survey has to
> tolerate it. That is now the question to settle, not the command.

The op's tier depends on which way you point it: turning idle *off* belongs
with `stop` — it makes R2 quieter — while turning it *on* starts spontaneous
motion and needs `--allow motion`. Gating both directions behind `motion`
permanently would have made the precondition unreachable from the LED and audio
sessions, which are the two cheapest and run at lower ceilings.

#### The unproven-CID gate — read this before the first hardware session

`idle` and `notify` are the only ops that write to the **motion device**
(`DID 0x17`) using CIDs that **only one implementation documents**
(`0x2C`, `0x2A`, `0x39` — see `../docs/research/r2-protocol.md`). The `read`
ceiling's whole promise is *nothing sent here can move him*, and that promise
cannot rest on constants no second source corroborates.

So both ops require **`--allow motion` until one session confirms them**:

```bash
./r2 daemon --allow motion --idle-timeout 1800   # first time only
./r2 send idle --params '{"enable": false}'
```

If R2 acknowledges, the daemon records it in `.bridge/verified-animatronic-cids.json`
and both ops drop to the `read` tier from then on, automatically. If R2 answers
`bad_command_id`, or does not answer, **the gate stays shut** — that is the
outcome it exists to catch, and it means the CID is wrong.

The daemon prints the gate's state in its startup banner. This is issue #7's
AC5 enforced by the ladder rather than left as a checklist item; once it lifts,
promote the three constants from INFERRED to OBSERVED in
`../docs/research/r2-protocol.md`.

`enable` must be a JSON `true` or `false`. A string like `"false"` is refused
rather than interpreted, because it is truthy in Python and would enable motion
— the opposite of what you typed.

**The op fails loudly if R2 does not acknowledge.** No observation of its own
catches a silently-failed precondition, so a timeout or an error code returns
`ok: false` and a non-zero exit rather than a cheerful success. `bad_command_id`
is a live possibility here, not a hypothetical: CID `0x2C` is single-source and
unproven on this firmware. If this op fails, **discard the session's data** —
idle state is unknown. `notify` behaves the same way, for the same reason:
absence of an event you never successfully enabled is not evidence.

The daemon does **not** re-enable idle on exit; it prints a reminder instead.
Restoring it would mean commanding motion on the way out of the path whose job
is to leave R2 stopped.

### Events — hearing R2 speak first

Not everything on the wire is a reply. R2 sends notifications unprompted, marked
by `seq = 0xFF`, and the bridge used to drop them: the response dispatcher looked
for a matching waiter and returned early when there wasn't one. Now they land in
a bounded 200-entry ring:

```bash
./r2 send notify --params '{"leg": true, "head_reset": true}'
./r2 send events
```

`events` drains the ring, oldest first, and reports two loss counters alongside
the events: `dropped` (evicted by the ring cap before you read them) and
`framing_errors` (truncated frames discarded during reassembly). Both matter
more than they look — a silently lost notification is indistinguishable from
"R2 never sent one", which is exactly the question the survey is trying to
answer. It is available at **every** tier; reading is never a hazard.

Only two of the three known notifications have an enable command upstream.
`play_animation_complete_notify` has none at all, so whether it fires unprompted
is an open question: play an animation, then read `events`. That signal matters
beyond the survey — `../docs/architecture.md` wants choreography sequenced on
completion events rather than fixed sleeps.

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
