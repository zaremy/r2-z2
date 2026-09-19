/* WHAT A PRESS MEANT -- #101, and the first finding a real finger produced.
 *
 * Pure logic, no I2C and no LVGL, so a test can reach it. It lived inside
 * panel_touch_poll's release branch, which is a decision with no test in a
 * file with no host harness -- the same shape as the RUN guard before it was
 * lifted out into panel_probe_may_start, and the same fix.
 *
 * THE MEASUREMENT THAT PUT IT HERE (2026-09-14, 22 presses from an operator's
 * finger; raw points and the panel's own verdicts in
 * results/touch-sampling-2026-09-14.md, re-derivable with
 * tools/seg_touch.py):
 *
 *   samples == 1  ->  8 presses, EVERY one classified TAP, none a swipe
 *   samples >= 2  ->  every verdict the geometry allowed, including a
 *                     2-sample swipe of 271 px that registered correctly
 *
 * A press seen ONCE is the broken class, and it is broken absolutely: one
 * sample cannot carry a direction, because the press position IS the last
 * position. The poll ran on the 40 ms UI tick and a flick is 40-80 ms of
 * contact, which is how a whole swipe came to be seen once.
 *
 * AND A ONCE-SEEN PRESS WAS A TAP, which is why this reads as "inconsistent"
 * rather than "swipes don't work". With one sample the press position IS the
 * last position, so dx and dy are 0, the stray distance is 0, and the old
 * chain fell through to "moved less than PANEL_TAP_PX" and fired a tap
 * wherever the finger happened to be caught mid-flight. OBSERVED: of the
 * eight 1-sample presses in the capture, three produced a visible effect the
 * operator did not ask for -- HARDWARE TEST opened, DIAGNOSTICS opened, and a
 * LOCKED rung was tapped -- and the other five were taps that happened to land
 * on nothing.
 *
 * That last one is the point. A fast swipe back out of HARDWARE TEST that gets
 * caught over the READ rung is a tap on RUN: the gesture meant to LEAVE a
 * screen firing the one control on this panel that sends R2 a command.
 * Harmless at READ, which cannot move him. Not harmless at the tiers above it,
 * which is the argument for fixing it now rather than then (#168).
 */
#ifndef PANEL_GESTURE_H
#define PANEL_GESTURE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AC5: the swipe threshold, in raw panel pixels. */
#define PANEL_SWIPE_PX 60

/* A tap moves less than this in both axes. Well under the swipe threshold so
 * the two can never both be true. It is also LVGL's scroll limit, and a tap is
 * a press that never strayed this far, so a gesture that scrolled the list is
 * never also a tap. Chosen, not measured: this controller's jitter has never
 * been recorded. */
#define PANEL_TAP_PX 24

/* HOW MANY LOOKS AT THE FINGER A VERDICT NEEDS. One sample cannot tell a
 * still finger from a flick caught mid-flight -- it reports zero travel for
 * both. Two is the least that can carry a direction at all, so one is refused
 * and the gesture does nothing, which is what the dead zone between
 * PANEL_TAP_PX and PANEL_SWIPE_PX already does for every other ambiguous
 * press.
 *
 * TWO, NOT THREE, AND THE LOG IS WHY. The capture contains a 2-sample press
 * -- (358,280) to (87,253), 271 px across in 72 ms -- that the panel
 * correctly called a left swipe. A floor of 3 would have destroyed it. An
 * earlier draft of this comment reasoned from a table that said no gesture
 * under 3 samples ever registered; that table came from a parser which
 * silently dropped every point with x <= 99 and it was wrong.
 *
 * AND IT NARROWS THE FAILURE RATHER THAN CLOSING IT. A press caught by
 * exactly two polls that travels less than PANEL_TAP_PX between them is still
 * a TAP at the press position. At a 10 ms poll that is a 20-30 ms contact,
 * far shorter than the flicks that caused this, but it is not nothing. */
#define PANEL_MIN_SAMPLES 2

typedef enum {
    PANEL_GESTURE_NONE = 0,     /* ambiguous, or deliberately voided */
    PANEL_GESTURE_TAP,
    PANEL_GESTURE_SWIPE_LEFT,
    PANEL_GESTURE_SWIPE_RIGHT,
} panel_gesture_t;

typedef struct {
    int32_t  press_x, press_y;  /* where the finger landed */
    int32_t  last_x, last_y;    /* last position that HAD a finger on it --
                                 * never the release packet, whose coordinates
                                 * this controller has never been shown to
                                 * populate */
    int32_t  max_dev;           /* furthest it strayed from the press, either
                                 * axis: a drag out and back is not a tap */
    uint32_t samples;           /* polls that saw a finger during this press */
    bool     voided;            /* the UI asked for this press to mean nothing */
} panel_press_t;

/* What the press meant. NONE for anything ambiguous -- the panel would rather
 * do nothing than do the wrong thing, because a wrong gesture here is not a
 * no-op, it is a different control. */
panel_gesture_t panel_gesture_classify(const panel_press_t *p);

/* HOLD TO UNLOCK (E2E v0 slice 1). WAKE and GOODNIGHT are a finger held on
 * the state word, not a tap: D-023 ruled there is no confirm dialog and that a
 * thumb steadying the droid is an argument for the control being HARD TO HIT
 * -- a hold is that, with the progress shown so the operator learns it from
 * the first brush. Chosen, not measured. */
#define PANEL_HOLD_MS 700u

typedef struct {
    bool     active;     /* a qualifying press is being held */
    bool     spent;      /* this press fired or was cancelled; wait for a lift */
    uint32_t since_ms;
} panel_hold_t;

/* One look at the finger. `down` is whether one is on the glass now,
 * `in_target` whether THIS press landed on the control (fixed for the press),
 * `max_dev` how far it has strayed. Returns progress in permille, 0..1000, and
 * sets *fire exactly once, on the look that completes the hold.
 *
 * A press that strays past PANEL_TAP_PX is cancelled and cannot restart until
 * the finger lifts: a swipe that pauses is not a hold.
 *
 * `voided` is the press having been consumed by something else -- D-017's
 * wake frame, dismissed by it. A voided press is spent whole, even if it was
 * already holding. Without this the finger that dismissed the OFFLINE frame
 * could stay put and fire GOODNIGHT 700 ms later (review of #190). */
unsigned panel_hold_step(panel_hold_t *h, bool down, bool in_target,
                         bool voided, int32_t max_dev, uint32_t now_ms,
                         bool *fire);

#ifdef __cplusplus
}
#endif
#endif /* PANEL_GESTURE_H */