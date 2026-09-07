/* Touch: the swipe mechanism and the edge recorder — #101 child 4 and P1.
 *
 * WHY THESE ARE ONE FILE. P1 (corner-to-corner calibration) was going to be a
 * separate bare-metal app, and running it that way means the operator's touches
 * are measured through a DIFFERENT code path from the one the panel will
 * actually use. Measuring here instead makes the calibration evidence apply to
 * the thing being calibrated, and removes the app-swapping that would otherwise
 * make a trip to the droid land on whichever firmware happened to be flashed.
 *
 * WHAT IS DELIBERATELY NOT HERE: the three lateral pages' CONTENT. The epic
 * names page 2 (`NETWORK`) and nothing else; pages 1 and 3 exist only in the
 * gitignored vault. Inventing them is exactly the failure D-017 Amendment B
 * just ruled against -- a view inventing its own model. The mechanism is built;
 * the pages wait for their definition.
 */
#ifndef PANEL_TOUCH_H
#define PANEL_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AC5: the swipe threshold, in raw panel pixels. */
#define PANEL_SWIPE_PX 60

typedef enum { PANEL_SWIPE_NONE = 0, PANEL_SWIPE_LEFT, PANEL_SWIPE_RIGHT } panel_swipe_t;

void panel_touch_init(void);

/* Call every UI tick. Polls the input device directly rather than relying on
 * LVGL events, which a full-screen container silently swallows. */
void panel_touch_poll(void);

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

#ifdef __cplusplus
}
#endif
#endif /* PANEL_TOUCH_H */
