/* Touch: the swipe mechanism and the edge recorder — #101 child 4 and P1.
 *
 * WHY THESE ARE ONE FILE. P1 (corner-to-corner calibration) was going to be a
 * separate bare-metal app, and running it that way means the operator's touches
 * are measured through a DIFFERENT code path from the one the panel will
 * actually use. Measuring here instead makes the calibration evidence apply to
 * the thing being calibrated, and removes the app-swapping that would otherwise
 * make a trip to the droid land on whichever firmware happened to be flashed.
 *
 * WHAT IS DELIBERATELY NOT HERE: what a gesture MEANS. This file reports a
 * swipe, a tap, a press; which page or row that reaches is panel_ui's call.
 */
#ifndef PANEL_TOUCH_H
#define PANEL_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The thresholds and the decision itself live in panel_gesture, which is pure
 * and has host tests. They were here, inside an I2C poll loop, which is a
 * decision no test could reach -- and it shipped a bug that only a finger
 * could find. PANEL_SWIPE_PX and PANEL_TAP_PX come in from there. */
#include "panel_gesture.h"

typedef enum { PANEL_SWIPE_NONE = 0, PANEL_SWIPE_LEFT, PANEL_SWIPE_RIGHT } panel_swipe_t;

void panel_touch_init(void);

/* Read the controller. Call every UI tick, and NOT under the display lock:
 * it does a blocking I2C transaction, and holding the LVGL lock across that
 * lets a wedged controller stall every redraw. Touches no LVGL state. */
void panel_touch_poll(void);

/* Draw the touch feedback. Must hold the display lock. Separate from poll()
 * precisely because the two need different locks. */
void panel_touch_render(void);

/* Every observed press extreme, for P1. `points` is how many presses have been
 * seen at all -- zero means the operator has not touched it, which must not be
 * confused with a touch that reported (0,0). */
typedef struct {
    uint32_t points;
    int16_t  min_x, max_x, min_y, max_y;
} panel_touch_extremes_t;

void panel_touch_extremes(panel_touch_extremes_t *out);

/* The most recent completed swipe, consumed by reading it. */
panel_swipe_t panel_touch_take_swipe(void);

/* True if a finger has been seen since the last call. Feeds the burn-in
 * dimmer: a person touching the panel is the clearest possible signal that
 * someone is looking at it. */
bool panel_touch_take_activity(void);

/* True once per finger LANDING -- not while it stays down. Activity says
 * a finger is on the glass; this says a new touch began, which is what
 * separates "I tapped the frame" from "my finger was already there when
 * it appeared". */
bool panel_touch_take_press(void);

/* The press in progress will not become a swipe on release, and any swipe
 * not yet taken is dropped. For a touch that has been consumed by
 * something else -- the wake frame's dismissal. */
void panel_touch_void_gesture(void);

/* The most recent tap, consumed by reading it: where the finger went down.
 * A voided gesture never produces one. */
bool panel_touch_take_tap(int16_t *x, int16_t *y);

/* The touch controller's chip id read at boot, 0 if it never answered. An
 * answer at 0x15 is also the board-revision probe: it means V2. */
uint8_t panel_touch_chip_id(void);

#ifdef __cplusplus
}
#endif
#endif /* PANEL_TOUCH_H */
