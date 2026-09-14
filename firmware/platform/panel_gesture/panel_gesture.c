#include "panel_gesture.h"

#include <stddef.h>

/* THE TWO THRESHOLDS MUST NOT MEET. A tap is a press that never strayed
 * PANEL_TAP_PX; a swipe must travel PANEL_SWIPE_PX. If those ever overlap, a
 * gesture could satisfy both and the order of the tests would decide -- which
 * is the kind of thing that reads fine and behaves randomly. PANEL_TAP_PX is
 * also handed to lv_indev_set_scroll_limit, so widening it moves LVGL's
 * scrolling too. */
_Static_assert(PANEL_TAP_PX * 2 < PANEL_SWIPE_PX,
               "the tap radius must stay well inside the swipe threshold");

static int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

panel_gesture_t panel_gesture_classify(const panel_press_t *p)
{
    if (p == NULL) return PANEL_GESTURE_NONE;

    /* The UI asked for this press to mean nothing -- a finger already on the
     * glass when a frame rose under it, which must not dismiss what it never
     * saw. */
    if (p->voided) return PANEL_GESTURE_NONE;

    /* TOO FEW LOOKS TO SAY. This is the finding: a flick seen once reports
     * zero travel, which used to read as a tap at wherever it was caught. */
    if (p->samples < PANEL_MIN_SAMPLES) return PANEL_GESTURE_NONE;

    const int32_t dx = p->last_x - p->press_x;
    const int32_t dy = p->last_y - p->press_y;
    const int32_t adx = iabs(dx), ady = iabs(dy);

    /* A SWIPE IS HORIZONTAL: at least twice as far across as down. Scrolling
     * the SERVICE list is the commonest gesture on the panel, and a scroll
     * with 60 px of sideways drift used to change the page. */
    const bool across = adx > 2 * ady;
    if (across && dx <= -PANEL_SWIPE_PX) return PANEL_GESTURE_SWIPE_LEFT;
    if (across && dx >= PANEL_SWIPE_PX)  return PANEL_GESTURE_SWIPE_RIGHT;

    /* A TAP is a release that barely moved in EITHER axis, and that never
     * LEFT that radius: a drag out past the scroll limit and back scrolled the
     * list, and ending near where it started does not make it a tap. */
    if (p->max_dev < PANEL_TAP_PX) return PANEL_GESTURE_TAP;

    /* Far enough to be deliberate, not straight enough to be a swipe. The gap
     * is dead on purpose. */
    return PANEL_GESTURE_NONE;
}