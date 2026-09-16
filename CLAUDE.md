# r2-z2 — persistent AI mind for a Sphero R2-D2

## Goal

Replace the obsolete Sphero app brain with a persistent, autonomous AI mind.
Success is *not* a robot that obeys commands; it is the sense that the same
little entity is continuously present in the household.

## Character boundary — non-negotiable

- **R2's physical body is the character interface.** Dome, locomotion, stance,
  chirps, authored animations, front/rear LEDs, logic/holo lights, idle motion.
- **The backpack touchscreen is a service/debug/settings panel.** Status,
  connectivity, diagnostics, hardware tests, provisioning. It is *not* a face.
  Do not add character rendering to the screen without an explicit decision
  recorded in `docs/decisions.md`.
- **The model is a utility panel: indicator outside, diagnostics behind the
  door.** The **LED is an affordance** — glanceable from across the room, says
  *that* something is up, never spells anything out. The **screen is the detail
  view** — read up close, deliberately, and it is where you find out *what*.
  Service signals on the body are therefore fine and expected; it is *rendering*
  that is forbidden there. Two consequences, and both bind:
  - **Nothing that needs a glance may live only on the screen.** Nobody reads a
    diagnostic display to find out whether anything is wrong. If a state is
    worth noticing without going to look, the LED must carry it.
  - **Nothing with a face or text goes on the body.**

  Colour carries meaning, steady-vs-blink carries mode (status between
  interactions, expression during one). Full scheme and its evidence: **D-012**.

## Architecture rules

- Persistent state (identity, mood, energy, boredom, relationship, memories,
  goals) lives in **our** store and survives swapping model providers.
- **LLMs are a reasoning service, not the identity.** Never hardwire the
  character to Claude or any vendor.
- **Routine life runs locally.** Idle, sleep/wake, mood drift, boredom,
  quiet hours, simple sensor reactions — no cloud round-trip.
- **Semantic behaviors above raw primitives.** The reasoning layer calls
  `express_curious()`, not `set_dome_position(-27)`. Each semantic behavior
  choreographs dome + sound + light + stance over time.
- Keep the brain decoupled from R2 BLE packets. Layer boundary:
  `brain → behavior → choreography → r2/commands → r2/protocol → r2/ble`.

## Hardware

- Sphero R2-D2 app-enabled droid (BLE name prefix `D2-`).
- Waveshare ESP32-S3-Touch-AMOLED-1.8. **Revision must be verified at runtime,
  not trusted from the retailer listing** — see `docs/research/board-revision.md`.
- Mac is the development/prototyping host.
- **No Raspberry Pi** unless a concrete, documented limitation forces one.

## Safety

- Never commit API keys or Wi-Fi credentials. Use `.env.example` and NVS.
- Never issue uncontrolled movement commands during development.
- Bring-up order is fixed: **read-only → LEDs → audio → small dome → stance →
  locomotion.** Each actuator test is individually opt-in, never bundled.
- Default to STOP. Disconnect or failure must result in stop, not last-command.
- Do not leave animation or sound loops running unattended.
- **Do not flash firmware built for another board revision.** Confirm the
  display/touch controller pair before flashing anything third-party.

## Hardware session facts that cost time to learn

- **The agent cannot start the BLE daemon.** macOS attributes Bluetooth to the
  *responsible* process; under Claude Code that is `claude.app`, which has no
  usage description, so anything it spawns that touches CoreBluetooth is
  SIGABRTed (exit 134, verified). Workaround that works: write a `.command`
  file and `open -a Terminal` it — launchd spawns it and Terminal is
  responsible. Always `ps aux | grep r2_probe` first; a stale daemon holds the
  link and the new one fails with a misleading "R2-D2 not found".
- **The dome has no resting position.** Observed at 103°, 3.3° and −0.06° in
  three sessions. `set_head_position` is absolute, so bound moves by *travel*
  from a freshly read position — never by destination, never from a remembered
  angle.
- **`spherov2` capability lookups must go through the MRO, and are still only
  claims.** `class R2D2(BB9E)`, so grepping `r2d2.py`'s class body misses
  everything inherited. And a correctly-resolved capability can still be
  refused by the firmware — `enable_idle_animations` is the standing
  counterexample. Presence means "worth probing", not "supported".
- **R2 parks himself in bipod about a minute after the link drops.** OBSERVED,
  by elimination: `stop` does not retract the third leg (tested with the link
  up), and the disconnect does not either (tested with Ctrl-C) — it raises on
  its own a minute later. So **"default to STOP" does not mean he is left
  stable**; his own safe default is two legs. He has no resting posture, the
  same way the dome has no home position. Any behaviour that assumes a tripod
  at wake-up is assuming something false.
- **Authored animations drive leg actions and can fell him.** `EMOTE_YES` — a
  nod — emitted WADDLE three times and put him on the floor, with
  `perform_leg_action` never called by us. An animation is a stance command
  whose contents cannot be inspected first, which is why it sits at the
  `stance` tier and not `dome` (D-009). Bipod is a stable *standing* stance;
  WADDLE is what topples him. **The tripod is needed to MOVE, not to STAND.**
- **Batch the hardware work, then write it up.** Interleaving measurement with
  docs and PRs lets the daemon idle-timeout expire mid-writeup, and only the
  operator can relaunch it — one session cost four manual relaunches. Their
  attention is the scarce resource, not yours: take every reading you need
  while the link is live, then let it drop and write.
- **Build and dry-test the driver while the link is DOWN.** The bullet above is
  about writing *docs* mid-link; this is about writing *code* mid-link, and it
  cost a relaunch the same way. When the link is up, only fire and record —
  every edit, extension and bugfix belongs either before the daemon starts or
  after it drops. Dry-test verdict logic against synthetic data; it needs no
  robot and catches the branch that never fires.
- **Enable the notifications you intend to read, and PROVE the channel live
  before trusting silence.** `animation_complete` fires unprompted;
  `leg_action_complete` does **not** — it needs `notify --params '{"leg":true}'`.
  A session that forgets still gets completions and still measures durations,
  and silently sees **zero** leg events. That is not a missing feature, it is a
  wrong answer in the safe direction: it classified `EMOTE_YES`, the known
  waddler, as `no_leg_activity`. Force a leg movement, see the event, *then*
  believe an empty sequence.
- **Deploy is stable; retract is not.** Any forced leg movement must be a
  **deploy**. A liveness check that toggled *away from the current state* picked
  a retraction on an already-tripod droid and knocked him over backwards. If the
  safe direction is unavailable from the current state, refuse and say so rather
  than falling back to the unsafe one — a diagnostic must never be the most
  dangerous command in the session.
- **An LED colour we set is STATE, not a command — it survives the link
  dropping, but NOT a sleep cycle.** Green set in the afternoon was still lit
  an hour later, across animations played over it, a daemon kill and a fresh
  connect. The firmware's own resting alternation does not resume and
  overwrite it *within a session*; an animation only masks it and it returns
  unprompted.

  **The exception, MEASURED 2026-08-18:** magenta written at 23:52:47 and
  confirmed by eye was gone 22.1 h later, back to R2's own red/blue
  alternation, on the charger at full battery. The difference from the green
  trial is **sleep** — every reconnect in that hour restarted the keepalive,
  and the keepalive IS the wake command, so that droid was never allowed to
  sleep. The original claim was not wrong, it was unscoped. (Sleep is the
  likely destroyer, not the proven one: the 22 h also contains duration. Set a
  colour, disconnect, wait well under the sleep timeout, reconnect and look —
  nobody has run that.)

  Two corollaries, and both bite:
  - **Whatever colour a session leaves him in is what the household sees —
    until he sleeps.** Leave him in a defined state anyway.
  - **Assert the status on connect; never inherit it.** Without that the
    household sees his own idiom every morning whatever the light language
    says, and worse, a droid left in `attention` comes back showing nothing
    about it — a pending issue silently stops being pending. Implemented in
    `r2_status.StatusLayer.connect()`; the LED equivalent of *default to
    STOP*.
- **He never sleeps while we are connected, because our keepalive IS the wake
  command.** `DID 0x13 / CID 0x0D` every ~3 s (`r2_probe.py:523`). Any `sleep`
  we send is undone within three seconds, which is why he has no idle timeout in
  practice and why there is no working "off" — unplugging does nothing on a
  charged battery. A soft power control is a **session-lifecycle** change, not a
  new op (#38). Do not read "he stayed awake" as a firmware property; it is us.

  **He DOES have an idle sleep — OBSERVED 2026-08-18, the first time we ever
  stopped the keepalive and left him alone long enough to watch.** He reverted
  to his resting alternation and then faded out, on the charger at full
  battery. So "no idle timeout in practice" is exactly right — *in practice*,
  and the practice is ours. The upper bound is a useless 22.1 h because nobody
  was watching in between; the real timeout is cheap to measure and unmeasured:
  disconnect, then look at 5, 15, 30 and 60 minutes.

  That reframes #38. "No working off" is not a firmware limitation to engineer
  around — it is us suppressing his own, every three seconds.
- **Running a survey where a human is the instrument?** Use `/survey-session`.
  Brief before firing, fire within ~3 s of "go", and never suppress stderr on a
  send loop — a silent crash reads exactly like a dead device.
  **Invoke it — do not wait to be asked.** Survey work arrives as "go", "ok",
  "keep going", or a bare checkpoint reference, never as "run the survey", and
  two consecutive sessions ran three surveys each without invoking it. Widening
  the skill's own triggers did not fix that; this line is here because
  `CLAUDE.md` is loaded every session and skill descriptions evidently are not
  matched reliably. If the next instruction will move the robot, the skill
  applies.
- **Mutate the GUARD, not the table.** Three mutations survived in one
  session, all the same shape: deleting a check left the suite green because
  the tests asserted the SHIPPED DATA was valid rather than that the check
  rejects bad input. Removing the status-colour guard passed, because every
  state in the table used a legal colour. Reverting a teardown to an unscaled
  frame passed, because the test drove the *helper* that scales instead of the
  *caller* that was meant to use it. A wiring test asserted "yellow appears
  somewhere", which an unrelated assertion satisfied on its own.

  The table being correct today is not the property worth pinning; the next
  row somebody adds is. **For every guard, write the ILLEGAL case first and
  assert it raises** — then walk the legal ones. Where a helper enforces
  something, one test must prove the caller ROUTES THROUGH it. If deleting a
  check leaves the suite green, that check has no test, whatever the coverage
  number says.
- **Run the case that must NOT fire, BEFORE any real trial — and again at the
  end.** A detector that is stuck ON is invisible to a positive control: it
  produces the *loud* answer, and loud reads as success. The S1e touch survey
  returned a unanimous **10/10** and every one of those trials was void. Four
  separate bugs each produced that same confident result — a 6σ threshold
  applied to the max across a window (the max of 85-220 draws clears 6σ by
  chance), a statistic that fired when a channel merely sat somewhere new, an
  event ring never flushed so a 5 s window swept in minutes of history, and
  single-channel firing that one encoder blip could trip. One no-touch control
  exposed all four — run after ten trials instead of before, which cost the
  operator ten wasted pets before they stopped it themselves. **A clean sweep
  is a prompt to check the null case, not a result.** Interleave controls with
  trials so specificity drift cannot hide inside a run, and store raw samples
  with every trial so a rubric that turns out wrong can be re-scored offline
  instead of re-run on the operator's patience.
- **The dome cannot make small movements, and it lies about it.** Commanded
  travel below ~10.5° is SILENTLY IGNORED and still returns `ok: true`
  (4°/6°/8°/10° all moved ≤0.11°; 10.5° moved). **The smallest legible dome
  gesture is 12°**, not the ~5° S1c inferred from a single failing 4.2°
  command. A move also takes **~2.0-2.2 s regardless of distance** — 8.35° took
  2.19 s and 22.61° took 2.07 s — so it is a fixed-duration move, not a slew
  rate, and anything that must feel quick cannot move the dome. This has now
  produced two wrong designs: a drift correction that never executed, and
  issue #29's own AC5 text telling us to probe resistance with "a much smaller
  delta", which would have moved nothing and read as total resistance. Full
  evidence in **D-013**.

- **You cannot read this board's serial output by opening the port, and the
  failure looks exactly like dead hardware.** Its only port is the ESP32-S3
  native USB-Serial/JTAG. A plain `pyserial` open **resets the chip into the ROM
  downloader** (`rst:0x15 USB_UART_CHIP_RESET, boot:0x21 DOWNLOAD`), and
  esptool's `Hard resetting via RTS pin` is a **no-op** — there is no RTS line to
  pull. So the app is not running while you listen, and every read returns zero
  bytes. **That is the whole cause.** An earlier version of this bullet also
  blamed the console routing — that `CONFIG_ESP_CONSOLE_UART_DEFAULT` puts stdout
  on UART0's pins and so needs `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`. **That was
  wrong and is corrected here**: the secondary USB console carries app output
  anyway. Measured both ways — `00_bsp_quickstart` ships the IDF default and logs
  fine over USB, and `touch_check` rebuilt with the setting removed still printed
  every `printf`. The setting is harmless, not required. What works:
  `idf.py -p <port> flash` (this really does leave the app running), then attach
  with `idf_monitor --no-reset` from a loop that reattaches when the port
  reappears — never a bare `pyserial` open. A cold power-cycle also boots the
  app; only the operator can do that.

  **The lesson is bigger than the board: six consecutive "no output" readings
  were taken before anyone checked the listener could produce a positive.** Each
  silence was reported as a fact about the firmware. The thing that broke it was
  flashing `13_display_colorbar` — an app whose success is visible *with your
  eyes*, independent of serial — and getting the **identical** silence from
  known-good code. Before believing any negative from an instrument, run the
  known-good case through it. `#103` A3 was answered within minutes of doing so.

- **An instrument can lie by dropping PART of its input, and that failure
  invents findings rather than hiding them.** Diagnosing inconsistent swipes,
  the log parser matched `\((\d+),(\d+)\)` while the firmware pads
  coordinates to three characters — so `(  1,326)` never matched and every
  point with **x <= 99 was silently dropped**. That is the left third of the
  panel, which is exactly where a rightward swipe BEGINS. It ate 48 of 79
  points, reported 13 gestures where there were 22, and the write-up published
  *"not one right swipe registered"* as a finding while `panel: swipe right`
  appeared three times in the log shipped beside it.

  A total failure is loud; a partial one is silent AND biased, because what
  drops is usually systematic — one region of the input space — not random. So:
  **print coverage before conclusions**, parsed against the producer's own
  sequence numbers, with a loud warning when they disagree (and account for the
  offset when a counter is cumulative and the capture starts mid-stream, or the
  warning cries wolf and gets ignored). The same shape bit a mutation harness
  the same day: it classified on `grep "0 failures"`, which matches
  `10 failures`, so mutants the tests really killed were reported as survivors.
  Anchor the match.

- **Read the system's OWN verdict before reconstructing it.** The same capture
  contained `panel: swipe left` and `panel: tap -> DIAGNOSTICS` — the panel's
  decisions, ground truth, never grepped for. Reconstruction feels like rigour
  and silently substitutes your model of the system for the system. Grep for
  what it SAID first; use that as the expectation in fixtures; reconstruct only
  to EXPLAIN a verdict, never instead of reading one. If a diagnosis needs
  reconstruction at all, that is the signal to add the missing decision log —
  the panel now logs every gesture's verdict with the numbers behind it.

- **An analysis script whose numbers get published must be committed.** The
  first swipe write-up cited a segmenter that lived only in a scratch directory,
  so the one piece of evidence the whole design rested on could not be re-run —
  and when it turned out to be wrong, nobody could have caught it from the repo.
  It lives in `firmware/panel/tools/` now. Same rule as *if it cites code, it
  goes in the repo*.

  **And the tail of that same mistake: once the real cause was found, a second
  plausible mechanism was written up beside it as though it were also a cause.**
  The console-routing story above was reasoned from a config diff, never
  isolated, and shipped into this file and a firmware comment before a two-minute
  rebuild refuted it. A cause you have actually isolated explains the whole
  observation; if you find yourself writing "necessary but not sufficient",
  that is usually the tell that only one of the two was tested.

- **`firmware/panel` has TWO build configurations and only one of them ships,
  and the one you run while iterating is the other one.** `idf.py
  -DPANEL_SHOT_TOUR=OFF` is what goes on the droid; `=ON` drives the panel
  through nine screenshots. **The flag is sticky in the CMake cache**, so a
  bare `idf.py build` after a tour build is *still a tour build*.

  That combination broke the shipping build for **nine consecutive green
  builds, a flash, a live hardware run with R2 linked, and a negative control**
  — a constant moved into `panel_ui.h` landed inside the tour's own `#ifdef`
  while `panel_ui.c` used it unconditionally. Every compile in that stretch was
  of the one configuration that could not see it. The merge check found it.

  Run **`firmware/panel/tools/build_both.sh`** before pushing anything under
  `firmware/panel`. It builds both and leaves the cache on the SHIPPING
  configuration, so the next bare `flash` is not a tour build going on the
  droid by accident. "It builds" is not a fact here until you say which one.

- **Panel brightness is a CO5300 register that SURVIVES A REFLASH, so a black
  screen is not a dead app.** `bsp_display_start()` does not set a level and
  `bsp_display_backlight_on()` was not enough; the panel came up black on a
  perfectly healthy app because the *previous* app's brightness slider had been
  left low. Call `bsp_display_brightness_set()` explicitly at startup, the way
  `00_bsp_quickstart` does — it is the display's equivalent of *assert the
  status on connect, never inherit it*, and the same rule as the LED colour that
  outlives the session that set it.

## Look at the screen before designing for it

The panel's STOP control was built 44 px tall "to match the tap target the rest
of the file measures against". On the glass it was amber text at the same size
and left margin as the rung names — no fill, no border, no rule — so beside six
rows reading `LOCKED` it read as **a label describing a state**, not a control.
The operator rejected it on sight.

One screenshot also showed two things reading the code had missed: the control
sat flush to the bottom edge, and the `NOT YET` / `LOCKED` distinction built
across two earlier PRs was **invisible** at the shipped ceiling.

"44 px, matches the rungs" is a correct sentence and the wrong design. The gap
between those is only visible in pixels.

**This panel can show you, with no finger and no operator:**

```bash
idf.py -DPANEL_SHOT_TOUR=ON build && idf.py -p <port> flash   # walks all 9 views
tools/grab_shot.sh <port> out.png <slot>                       # reads one back
```

**How to apply:** for any change to what the panel draws, capture a frame and
LOOK at it before asking for approval — and iterate on the real renderer rather
than a mock, so what gets approved is what ships. Two traps, both hit:
`panel_shot_take()` is hardcoded to **slot 0** (`panel_shot_take_slot()` is the
one you want), and painting a control on a screen where it is hidden repaints
nothing while logging success. **Open the PNG. The log line saying the capture
worked is written by the same code that got it wrong.**

## Wire one path end-to-end before building the layer above it

**Four PRs of the LED stack merged completely inert.** The state table, the
colour fixes, the animation player and the wake assertion all landed before
anything imported `r2_lights` except its own test — while the older beat layer
went on writing LEDs directly on the live path. Two systems described one droid
and only the wrong one ran. It took the operator asking *"if we landed our led
stack, why is it still using defaults?"* to surface it.

Each PR was individually correct, tested, and reviewed. That is what makes this
worth a rule: **correctness does not detect deadness.** A layer with no caller
passes every check a layer with a caller passes.

**How to apply:** before starting the layer above, make ONE path reach the
hardware, the screen, or the user — however thin. A single state asserted on
connect would have exposed the gap at the first PR instead of the fifth. When a
PR adds no call site for what it builds, say so in the body in those words
("nothing calls this yet"), so the inertness is a stated fact rather than a
discovery three PRs later. And when the operator asks why a shipped thing is
not visible, treat that as the highest-priority signal in the thread — it is
the CX gap the pipeline is structurally blind to.

## Research discipline

- **Read source, not README.** A README claim is a hypothesis.
- Record commit SHAs for every external repo (`docs/research/source-map.md`).
- Label every claim: `OBSERVED` / `INFERRED` / `UNKNOWN` / `SPECULATION`.
  Do not let an INFERRED claim silently graduate to OBSERVED.
- Prefer current official hardware code over blog/forum assumptions.
- When two sources disagree, record the disagreement rather than picking
  silently. Both conflicts `r2-capabilities.md` once tracked as *live* are now
  settled — `enable_idle_animations` REFUTED on hardware, and the animation-id
  table adjudicated id by id. Neither settled cleanly in one source's favour,
  which is the point: **record the adjudication, including `inconclusive`,
  rather than retiring the disagreement.**
- Cross-check protocol constants against at least two independent
  implementations before trusting them. The packet layer in
  `mac-prototype/r2_probe.py` was validated three ways (spherov2, the
  `claude-r2d2-buddy` C firmware, and a clean-room trace) — keep that bar.
- **`decisions.md` is a summary, and summaries omit. Read
  `research/r2-capabilities.md` alongside it before designing against either.**
  A design language was built on a smooth LED fade that the fixtures cannot
  render: D-012 said modulation was *untested*, while the capability doc had
  already measured that every value change flickers, and closed the section
  "Ruled on in D-012" — a pointer to a ruling D-012 did not contain. Both docs
  were individually honest and the pair was misleading. **A citation is a claim
  to verify, not evidence.** If you write "ruled on in D-0NN", open it and check,
  or write the ruling there yourself. And "untested" is weaker than a
  measurement saying it failed — go looking for the measurement.

## Where things are written

Two homes, and the split is not negotiable — putting a doc in the wrong one is
how plans go stale and decisions get lost.

**Obsidian vault — `R2Z2-vault/R2Z2-vault/` (gitignored, local):** planning and
thinking. The roadmap and per-stage notes, session logs, raw experiment results
from hardware sessions, open questions, behavior designs in progress, hardware
quirk notes, upstream reading notes.

**Repo `docs/` (committed, PR-reviewed):** things that must survive a fresh
clone and travel with the code. `intent.md`, `architecture.md`, `decisions.md`,
and `research/*.md` findings cited to `file:line`.

> **Rule of thumb: if it cites code, it goes in the repo. If it is thinking, it
> goes in the vault.** When a vault note hardens into a decision, write the ADR
> in `docs/decisions.md` — the vault note is the reasoning, the ADR is the ruling.

Consequence to accept knowingly: vault content is **not** in git, so it is not
backed up by the remote and not reviewable in a PR. That is the deliberate
trade for a fast, linkable thinking space.

### Writing for Obsidian

Vault notes are Obsidian-native, not plain markdown dropped in a folder:

- **YAML frontmatter on every note** — `type`, `status`, `updated`, `tags`.
  Keep `updated` honest; it is the only staleness signal.
- **`[[Wikilinks]]`, not relative paths.** Backlinks and the graph are the
  point. A link to a note that does not exist yet is fine — it marks work.
- **Exit criteria and checklists are `- [ ]` tasks**, so progress is visible and
  searchable rather than prose to re-read.
- **Hierarchical tags** — `#stage/s1`, `#status/blocked`, `#track/robot`.
- **Callouts** (`> [!warning]`, `> [!info]`) for the things that bite.
- Conventions live in `Meta/Vault Conventions.md`; templates in `Templates/`.
  Only core plugins are enabled — do not write notes that require Dataview.

## The working tree is usually somebody else's

Concurrent sessions share one checkout, and the branch under you is routinely
theirs. Across one session it read `main`, `s1f/touch-triggers-curious`,
`d015-voice-cloud-apis` and `d017-panel-wake-frame` — none of them mine, and
each carried uncommitted work I must not disturb.

So **land from a worktree off `main`, never by committing where you stand:**

```bash
git worktree add -b feat/<thing> "$WT" origin/main
```

Two rules that cost something to learn:

- **Copy the HUNK, not the file.** Copying an edited file carries everything
  else in it, including another branch's unmerged commits. `docs/decisions.md`
  came across at **+341 lines for a 67-line ADR**; the rest was two ADRs
  belonging to a branch that had not merged. Rebuild from the base
  (`git show origin/main:<path> > <path>`) and re-apply your own change, then
  check `git diff --stat` against the size you expect.
- **Renumber anything sequential.** ADRs and migrations get claimed by
  unmerged branches. D-017 and D-018 existed only on someone else's branch, so
  the new one is D-019 and `main` carries a temporary gap.

Clean the shared tree only after proving your edits are byte-identical to what
merged, and `git stash` rather than discard — `git checkout <file>` is guarded
for a reason.

## Reach for the skill that already exists

Two procedures got hand-rolled in one session that skills already cover, and
both improvisations cost something:

- **Mutation testing → `/mutation-battery`.** A hand-written battery shipped a
  classifier that matched `"0 failures"` inside `"10 failures"`, so mutants the
  tests really killed came back as survivors. It was caught by a contradiction,
  not by checking.
- **Adversarial review to convergence → `/grill`.** Seven rounds were driven by
  hand, re-deriving the dispatch-apply-reverify loop each time.
- **Human-as-instrument hardware work → `/survey-session`**, which is already
  documented above and was invoked correctly.

The tell is improvising a multi-step procedure you would run the same way next
time. Before hand-rolling one, check the skill list — and note that the work
arrives in words that match no skill's triggers ("check log", "swipes
inconsistent"), which is exactly why the check has to be deliberate.

## Working agreements

- `reference/` holds the upstream clones and is gitignored. Never vendor a
  clone into our history.
- Prefer adding a project-side tool over patching an external clone.
- Prototype in Python on the Mac; do **not** assume Python code ports to the
  ESP32. The Mac layer exists to learn behavior abstractions, not to be moved.
