# The first finding a real finger produced — 2026-09-14

Seven review rounds shipped the RUN control with the touch layer marked
"no finger has touched this panel". The operator used it and said **"swipes
inconsistent"**. This is what that was.

Raw captures: `touch-sampling-before-2026-09-14.txt`,
`touch-sampling-after-2026-09-14.txt`, taken with `esp_idf_monitor --no-reset`
(a plain port open resets this chip into the ROM downloader). Re-derive every
number below with:

```
python3 firmware/panel/tools/seg_touch.py \
    firmware/panel/results/touch-sampling-before-2026-09-14.txt
```

## Read the panel's own verdicts. They were in the log the whole time.

The first pass at this reconstructed gestures from raw points and never looked
at the `panel: swipe left` / `panel: tap ->` lines sitting in the same file.
That was the mistake, and a second one compounded it: the parser's regex was
`\((\d+),(\d+)\)`, while the firmware pads coordinates to three characters, so
every point with **x <= 99 was silently dropped** — the left third of the
panel, which is exactly where a rightward swipe begins.

It consumed 48 of 79 points and reported 13 gestures where there were 22. Right
swipes lost their opening samples and collapsed into what looked like
single-sample flicks, and the write-up then published *"not one right swipe
appeared"* as a finding — while `panel: swipe right` appears three times in the
file it shipped beside. `seg_touch.py` now prints parsed-vs-expected coverage
first, because a parser dropping 39% of its input reads exactly like a panel
dropping 39% of its samples.

**The mechanism below survived the correction. The quantification did not.**

## What the finger actually measured

22 presses, 9 logged verdicts: 3 swipe left, 3 swipe right, 2 taps that opened
a page, 1 tap refused on a LOCKED rung.

| samples in the press | what happened |
|---|---|
| **exactly 1** (8 presses) | **every one classified TAP.** Three did something visible the operator had not asked for: opened HARDWARE TEST, opened DIAGNOSTICS, tapped a LOCKED rung. The other five happened to land on nothing. |
| **2 or more** (14 presses) | every verdict the geometry allowed, including a **2-sample** swipe — `(358,280) → (87,253)`, 271 px in 72 ms — that the panel called correctly. |

So the broken class is **a press seen exactly once**, and it is broken
absolutely: one sample cannot carry a direction, because the press position IS
the last position. `dx`, `dy` and the stray distance are all zero, and the old
chain fell through its swipe tests into `else if (s_press_max < PANEL_TAP_PX)`
and fired a tap at whatever point the poll caught mid-flight.

**Why a whole swipe gets seen once:** `panel_touch_poll()` ran on the 40 ms UI
tick and a flick is 40–80 ms of contact. The deliberate swipes in the capture
got 4–15 samples; the flicks got one.

Note what is *not* true, and was claimed in the first draft: sample count is not
the whole story. A 3-sample press of 287 px was correctly **refused** for
arcing 175 px vertically, and a 2-sample press of 271 px was correctly
**accepted**. Geometry decides; one sample just means there is no geometry.

## Why it matters beyond a missed page change

A fast swipe across SERVICE does not fail — it opens whatever row the finger
flew over. And a fast swipe back out of HARDWARE TEST that gets caught over the
READ rung is a tap on RUN: the gesture meant to *leave* a screen firing the one
control on this panel that sends R2 a command. READ cannot move him, so this is
harmless today. The tiers above it are the reason to fix it now (#168).

## The fix

1. **A press seen fewer than twice is not a gesture.** Two, not three: the
   2-sample swipe above is real and a floor of 3 would destroy it.
2. **Touch polls in its own task at 10 ms**, off the UI tick. One owner —
   `panel_touch_poll` accumulates a press across calls, so polling from both
   tasks would race them over the press state, which is the sampling bug's own
   shape a layer up.
3. **The decision moved into `panel_gesture`**, pure and host-tested. It was a
   decision no test could reach, in a file with no host harness. Same lesson
   and same fix as `panel_probe_may_start`.

`test_panel_gesture.c` replays all 22 presses, and where the panel logged a
verdict at the time, **that verdict is the expectation** — ground truth from
the same file rather than something recomputed. 46 checks.

Mutation battery, 14 mutants, all killed by test failure — including the two
that survived the first brief (`SWIPE_PX 60→50`, `TAP_PX 24→29`), which is why
there is now one test written in literal pixels instead of in terms of the
constants. `TAP_PX → 40` is killed at compile time by a new `_Static_assert`
that the tap radius stays inside the swipe threshold; that is a stronger guard
than a test, but it is a compile kill and not a test kill, and the distinction
is worth keeping honest.

## What the after-capture does and does not show

```
touch: point 1 (357,248) ... point 9 (172,258)     <- 10-20 ms apart, not 40
touch: press: n=12 (357,248)->(172,258) d=(-185,10) dev=185 -> SWIPE LEFT
panel: swipe left -> page SERVICE
touch: press: n=8 (237,237)->(237,228) d=(0,-9) dev=9 -> TAP
panel: tap -> DIAGNOSTICS
```

It shows the 10 ms poll working (points 10–20 ms apart, not 42) and the new
per-release verdict line. **It does not contain a single 40–80 ms flick** — the
one swipe in it spans 120 ms, which would have registered before the fix too.
The evidence that the fix cures the reported symptom is the operator's
**"swipes seem better now"**, and nothing stronger. A capture that deliberately
flicks would settle it.

Every release now logs its own verdict with the numbers behind it. The entire
diagnosis above came from raw points plus a reconstruction written afterwards,
because nothing logged the decision — and the reconstruction was wrong twice.
That line is the most useful thing this session produced for the next one.

## The controller computes gestures itself, and we were discarding it

`rd(REG_GESTURE, buf, 6)` has always read `buf[0]` — register 0x01, the
CST816's own gesture output — and thrown it away. Observed on a left swipe:

```
touch: controller gesture byte: 0x03
```

Consistent with the part computing slide gestures at its own scan rate, which
is not bounded by how often we poll — which would make it a better swipe source
than anything reconstructed from samples we may not have taken. **Logged, not
used:** one observation of one value on one direction, and a datasheet is a
hypothesis. Worth a follow-up that sweeps all four directions.

## NOT explained: the screen went black

Once, during the post-fix capture, the operator reported the screen black while
the firmware was demonstrably rendering — the periodic screenshot at t+141 s is
a correct DIAGNOSTICS interior, and `panel_shot` reads LVGL's buffer rather
than the glass. So the app was drawing and the DISPLAY was dark.

Ruled out: the burn-in dimmer (it drops to 65%, not 0, its 120 s timer would
not have fired until t+230 s, and no `brightness ->` line appears); a crash
(`link_task` logged at t+123 s, `shot_task` at t+141 s).

Two candidates, neither isolated, recorded so the next occurrence is not
diagnosed from scratch:

- **Console contention.** This change quadrupled the poll rate while `note()`
  still logged every point at INFO — up to 100 lines/s on a 115200 UART with no
  driver installed, so `ESP_LOGI` busy-waits, from a task above both the UI and
  the link. `note()` is `ESP_LOGD` now, which removes the mechanism whether or
  not it was the cause.
- **The instrument.** esptool's "hard reset via RTS" is a no-op on this board,
  so a screenshot read can leave the chip in the ROM downloader with the app
  stopped — which looks exactly like this from outside. This session used that
  tool repeatedly.

It did not recur. Nobody should read this section as saying the cause is known.

## Still not proven

- **The fix against a real flick.** See above: the after-capture has none.
- **The controller's gesture byte** beyond one observation of `0x03`.
- **Tap-versus-scroll on a moving list**, never exercised this session.
- Right swipes are **not** on this list. They were demonstrated working on
  hardware three times, before the change.
