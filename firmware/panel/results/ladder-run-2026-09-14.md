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
- **One test at a time**; a second tap while one is running is ignored.
- **The verdict counts answers, not sends.** A send proves only that the gate
  admitted it. And it counts the readings THIS test asked for, arriving since
  it started -- raw reply totals include the panel's own periodic battery poll
  and would hand a failing test a passing mark it did not earn.
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

## Still not proven

No finger has touched this panel yet. The tour enters below `panel_touch.c`,
so the controller, the press and release edges, and the tap-versus-scroll
thresholds remain unobserved -- including on this rung, where a real tap is
the last untested step between a person and a command reaching R2.
