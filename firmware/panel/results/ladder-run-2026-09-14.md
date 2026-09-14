# The permission ladder runs a tier — 2026-09-14

`ladder-run-read-2026-09-14.png`: **READ 3/3 OK** in green, LEDS through
LOCOMOTION still **LOCKED**, captured on hardware with R2 linked.

The tour tapped the READ rung itself, through the same hit test a finger
would use. What the picture shows is therefore the whole path: tap -> the
renderer's request -> `main.c` sending three read ops through `r2_gate` ->
R2's three answers -> the verdict.

## What RUN will and will not do

- **Only rungs the gate would admit are tappable.** On this firmware that is
  READ alone, because the ceiling is never raised. A tap on a locked rung is
  refused by the panel and logged; if one ever got past, `r2_gate_send` would
  refuse it too. Two independent refusals.
- **READ is three questions that cannot move him**: his battery, his dome's
  position, his firmware version.
- **One test at a time**, and the guard names every state that means one is
  under way: queued, its ops in flight, sent but not yet clocked, or running.
  Three review rounds went into that list, because each shorter version left a
  window in which a second tap bought three more ops -- 2 s wide, then ~140 ms,
  then the length of three GATT writes. `panel_probe_may_start` is a pure
  function with host tests, rather than a condition inside an LVGL callback.
- **The verdict counts answers, not sends.** A send proves only that the gate
  admitted it. It counts the readings that arrived since this test started,
  rather than raw reply totals, which include every reply since boot.
- **That narrows the window; it does not close it.** The panel's own periodic
  polls -- battery every 15 s, dome every 30 s -- land in the same three
  stamps, so a reply this test did not ask for can still count toward it
  inside the 2 s window. A PASS therefore means "three readings arrived while
  the test was running", which is weaker than "R2 answered all three of these
  questions". Closing it would need per-request accounting the telemetry layer
  does not carry; it is stated here rather than papered over.
- **A lost link reads LINK LOST, not NO REPLY**: the question never reached
  him, and blaming him for our silence is the wrong answer in the reassuring
  direction.
- **A test that asked nothing settles as NO REPLY**, never as running and
  never as a pass.
- **The tour fails rather than photographing a `...`.** It checks the verdict
  settled before taking the picture, so an unfinished test can never be filed
  as a result.

Re-run on hardware AFTER review, because the review moved the send into the
link task, took the start stamp before the ops leave, and made the tick loop
start the probe rather than the sender -- all of them on the path between the
tap and the verdict. The second run's frame is **byte-identical** to this one
(sha1 `b634553c`), which is the strongest form the answer comes in: not "it
still passes", but "the panel drew the same pixels".

FOUR RUNS, one per review round, because each round changed the path between
the tap and the verdict: the send moving to the link task, the poll cadence
returning to link-up beats, the one-at-a-time guard moving to the tap and then
being made a tested predicate, and the generation that lets a closed interior
disown a send already in flight. The ladder frame is byte-identical across all
four; the frames that differ between runs are the ones carrying live values --
voltage, dome angle, uptime.

## Still not proven

No finger has touched this panel yet. The tour enters below `panel_touch.c`,
so the controller, the press and release edges, and the tap-versus-scroll
thresholds remain unobserved -- including on this rung, where a real tap is
the last untested step between a person and a command reaching R2.
