/* WHAT A PRESS MEANT -- #101, and the first finding a real finger produced.
 *
 * Pure logic, no I2C and no LVGL, so a test can reach it. It lived inside
 * panel_touch_poll's release branch, which is a decision with no test in a
 * file with no host harness -- the same shape as the RUN guard before it was
 * lifted out into panel_probe_may_start, and the same fix.
 *
 * THE MEASUREMENT THAT PUT IT HERE (2026-09-14, 13 gestures from an operator's
 * finger, raw points in results/touch-sampling-2026-09-14.md):
 *
 *   samples >= 3  ->  5 of 5 registered as swipes
 *   samples <= 2  ->  0 of 8 registered
 *
 * Sample count was the whole story -- not distance, not direction. The poll
 * runs on the 40 ms UI tick and a flick is 40-80 ms of contact, so a flick
 * got one or two looks at the finger.
 *
 * AND A ONCE-SEEN PRESS WAS A TAP, which is why this reads as "inconsistent"
 * rather than "swipes don't work". With one sample the press position IS the
 * last position, so dx and dy are 0, the stray distance is 0, and the old
 * chain fell through to "moved less than PANEL_TAP_PX" and fired a tap
 * wherever the finger happened to be caught mid-flight. A fast swipe across
 * SERVICE opened whatever row it flew over; a fast swipe back out of HARDWARE
 * TEST could land on the READ rung, which is the one control on this panel
 * that sends R2 a command. Harmless at READ. Not harmless at the tiers above
 * it, which is the argument for fixing it now rather than then.
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
 * both. Two is the least that can carry a direction, so one is refused and
 * the gesture does nothing, which is what the dead zone between PANEL_TAP_PX
 * and PANEL_SWIPE_PX already does for every other ambiguous press. */
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

#ifdef __cplusplus
}
#endif
#endif /* PANEL_GESTURE_H */
