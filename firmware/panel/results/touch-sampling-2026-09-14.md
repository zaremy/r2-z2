# The first finding a real finger produced — 2026-09-14

Seven review rounds shipped the RUN control with the touch layer marked
"no finger has touched this panel". The operator used it and said: **"swipes
inconsistent"**. This is what that was.

Raw logs: `touch-sampling-before-2026-09-14.txt`,
`touch-sampling-after-2026-09-14.txt`. Both captured with
`esp_idf_monitor --no-reset` (a plain port open resets this chip into the ROM
downloader). Segmentation and the re-run of the decision:
`scratchpad/seg.py` in the session, reproduced below.

## What the finger measured

13 gestures, segmented by the gaps between samples, with `panel_touch`'s own
decision recomputed from the endpoints:

| samples in the gesture | registered as a swipe |
|---|---|
| **>= 3** (89–633 ms) | **5 of 5** |
| **<= 2** (0–46 ms)   | **0 of 8** |

Sample count was the whole story. Not distance, not direction, not the
horizontality rule — every gesture the poll saw three times worked, and every
gesture it saw once or twice failed.

**The cause: `panel_touch_poll()` ran once per UI tick, every 40 ms, and a
flick is 40–80 ms of contact.** The slow deliberate swipes got 10–15 samples.
The flicks got one. The UI's frame rate has nothing to do with how fast a
finger moves, and tying the two together is the whole bug.

## Why it read as "inconsistent" rather than "swipes don't work"

A press seen EXACTLY ONCE has `press_at == last_touch`. So `dx` and `dy` are
both zero, `s_press_max` is zero, and the old chain fell through its swipe
tests into `else if (s_press_max < PANEL_TAP_PX)` — and fired a **TAP**, at
whatever point the poll happened to catch mid-flight.

Seven of the thirteen gestures did this. A fast swipe across SERVICE does not
fail; it opens whatever row the finger flew over.

It also reaches the control this epic spent seven rounds hardening: a fast
back-swipe out of HARDWARE TEST that gets caught over the READ rung is a tap on
RUN — the gesture meant to LEAVE a screen instead fires the one control on this
panel that sends R2 a command. READ cannot move him, so this is harmless today.
It is the same argument as every other guard here: it is the tiers above READ
that make it worth fixing before then, not after. See #168.

## The fix, and what proved it

Three changes:

1. **A press seen fewer than twice is not a gesture.** One sample cannot tell a
   still finger from a fast one — it reports zero travel for both — so the
   panel does nothing, which is what the dead band between `PANEL_TAP_PX` and
   `PANEL_SWIPE_PX` already does for every other ambiguous press.
2. **Touch polls in its own task at 10 ms**, off the UI tick. ONE owner:
   `panel_touch_poll` accumulates a press across calls, so polling it from both
   tasks would race them over `s_pressing` and the sample count — the sampling
   bug's own shape, one layer up.
3. **The decision moved out of the I2C poll loop** into `panel_gesture`, which
   is pure and has host tests. It was a decision no test could reach, in a file
   with no host harness, and it shipped a bug only a finger could find. Same
   lesson and same fix as `panel_probe_may_start` two PRs ago.

`test_panel_gesture.c` replays all 13 captured gestures as fixtures, so a fix
that cured the flicks by breaking the working swipes fails. Mutation battery:
11 of 11 mutants killed by test failure — after it found that the RIGHT swipe
threshold had no test at all and could be deleted with everything green.

**On glass, after the fix:**

```
touch: point 1 (357,248) ... point 9 (172,258)     <- 10-20 ms apart, not 40
touch: press: n=12 (357,248)->(172,258) d=(-185,10) dev=185 -> SWIPE LEFT
panel: swipe left -> page SERVICE
touch: press: n=8 (237,237)->(237,228) d=(0,-9) dev=9 -> TAP
panel: tap -> DIAGNOSTICS
```

Operator's verdict: **"swipes seem better now."**

Every release now logs its own verdict with the numbers behind it. The entire
diagnosis above came from raw points plus a reconstruction written afterwards,
because nothing logged the decision — that line is the single most useful thing
this session produced for the next one.

## The controller computes gestures itself, and we were discarding it

`rd(REG_GESTURE, buf, 6)` has always read `buf[0]` — register 0x01, the
CST816's own gesture output — and thrown it away. Observed during the same
capture, on a left swipe:

```
touch: controller gesture byte: 0x03
```

0x03 arriving on a leftward swipe is consistent with the part computing slide
gestures at its own scan rate, which is not bounded by how often we poll. That
makes it a better swipe source than anything reconstructed from samples we may
not have taken. It is **logged, not used**: one observation of one value on one
direction is not enough to move the gesture path onto it, and a datasheet is a
hypothesis. Worth a short follow-up that sweeps all four directions.

## NOT explained: the screen went black

Once, during the post-fix capture, the operator reported the screen black while
the firmware was demonstrably healthy — the periodic screenshot taken at t+141 s
shows a correctly rendered DIAGNOSTICS interior, and `panel_shot` reads LVGL's
buffer rather than the glass. So the app was rendering and the DISPLAY was dark.

What it was not:
- the burn-in dimmer — it drops to 65%, not 0, and its 120 s timer would not
  have fired until t+230 s; no `brightness ->` line appears in the log
- a crash — `link_task` logged stats at t+123 s and `shot_task` at t+141 s
- anything in this change — it touches gesture classification and poll timing,
  and calls nothing in the display path

The one link that exists is that this change quadruples I2C traffic on the
shared bus (40 ms -> 10 ms). **That is a hypothesis with no evidence for it**,
recorded here so the next occurrence is not diagnosed from scratch, and so
nobody reads this file as saying the cause is known. It did not recur.

Also unruled-out and worth stating because this session used the tool
repeatedly: esptool's "hard reset via RTS" is a no-op on this board, so a
screenshot read can leave the chip in the ROM downloader with the app stopped —
which looks exactly like this from the outside. The instrument causing the
symptom is a failure mode this project has already been bitten by.

## Still not proven

- **Right swipes.** Not one appeared in the 13 captured gestures, before or
  after. The mutation battery independently found the rightward threshold had
  no test. Both point the same way and neither is evidence of a defect yet —
  it needs a capture that deliberately sweeps rightward.
- **The controller's gesture byte** beyond one observation of 0x03.
- **Tap-versus-scroll on a moving list**, which this session never exercised.
