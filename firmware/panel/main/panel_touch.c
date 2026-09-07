#include "panel_touch.h"

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "touch";

#define PANEL_W 368
#define PANEL_H 448

static panel_touch_extremes_t s_ex = { .points = 0, .min_x = INT16_MAX,
                                       .max_x = INT16_MIN, .min_y = INT16_MAX,
                                       .max_y = INT16_MIN };
static volatile panel_swipe_t s_swipe;
static lv_point_t s_press_at;
static bool s_pressing;

static void note(int16_t x, int16_t y)
{
    if (x < s_ex.min_x) s_ex.min_x = x;
    if (x > s_ex.max_x) s_ex.max_x = x;
    if (y < s_ex.min_y) s_ex.min_y = y;
    if (y > s_ex.max_y) s_ex.max_y = y;
    s_ex.points++;

    /* P1's whole question is the EXTREMES, so log the remaining gap to each
     * bezel rather than only the point. #104 measured x 1..362 on a 368-wide
     * panel and recorded its own limit: x never reached either endpoint, so a
     * 1:1 map onto 0..367 is "reasonable and unverified" at the edges -- which
     * is precisely what every edge-adjacent hit target depends on.
     *
     * An operator pressing a corner cannot tell a corner that does not
     * register from one they simply missed. Printing the gaps is what makes
     * that legible to them and to me. */
    ESP_LOGI(TAG, "point %4u  (%3d,%3d)   gaps: L%-3d R%-3d T%-3d B%-3d",
             (unsigned)s_ex.points, x, y,
             s_ex.min_x, (PANEL_W - 1) - s_ex.max_x,
             s_ex.min_y, (PANEL_H - 1) - s_ex.max_y);
}

static void on_input(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    /* RAW coordinates. The display's 16 px V2 column offset is NOT added:
     * #104 proved it does not apply to touch, with five samples arriving below
     * x=16 which would be impossible if it did. */
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_press_at = p;
        s_pressing = true;
        note((int16_t)p.x, (int16_t)p.y);
    } else if (code == LV_EVENT_PRESSING) {
        note((int16_t)p.x, (int16_t)p.y);
    } else if (code == LV_EVENT_RELEASED && s_pressing) {
        s_pressing = false;
        const int32_t dx = p.x - s_press_at.x;
        /* AC5: +/-60 px. Measured from press to release rather than from
         * LVGL's own gesture detector, because the threshold is specified in
         * PANEL PIXELS and a gesture API that changes its mind about units is
         * one more thing to verify. */
        if (dx <= -PANEL_SWIPE_PX)      s_swipe = PANEL_SWIPE_LEFT;
        else if (dx >= PANEL_SWIPE_PX)  s_swipe = PANEL_SWIPE_RIGHT;
    }
}

void panel_touch_init(void)
{
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev == NULL) {
        /* Say so loudly. A panel that silently has no touch looks exactly like
         * one nobody has touched -- and P1's answer would then be "no points",
         * which reads as a hardware finding rather than a wiring bug. */
        ESP_LOGE(TAG, "NO INPUT DEVICE — touch is not wired. Any 'no points'");
        ESP_LOGE(TAG, "  result below is an INSTRUMENT FAILURE, not evidence.");
        return;
    }
    lv_obj_add_event_cb(lv_screen_active(), on_input, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(lv_screen_active(), on_input, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(lv_screen_active(), on_input, LV_EVENT_RELEASED, NULL);
    ESP_LOGI(TAG, "touch wired; swipe threshold %d px; reporting edge gaps",
             PANEL_SWIPE_PX);
}

void panel_touch_extremes(panel_touch_extremes_t *out)
{
    if (out) *out = s_ex;
}

panel_swipe_t panel_touch_take_swipe(void)
{
    const panel_swipe_t s = s_swipe;
    s_swipe = PANEL_SWIPE_NONE;
    return s;
}
