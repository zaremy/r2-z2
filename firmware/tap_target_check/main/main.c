/* tap_target_check -- is an 87 px tap row reliably clickable on this panel?
 *
 * The panel design (docs/research + the v5 prototype) is built on 87 px rows,
 * which is 44 pt at this display's 322 ppi. Nothing had ever tested whether a
 * target that size actually delivers LV_EVENT_CLICKED on this hardware.
 *
 * What prompted this: 00_bsp_quickstart's "Refresh SD" button is 132x38 px
 * (10.4 x 3.0 mm). Its press style engaged on every touch -- so hit-testing and
 * the input device are fine -- but it delivered a CLICK on roughly one tap in
 * six. LVGL cancels a click when the pointer leaves the object before release,
 * and a fingertip centroid wandering off a 3 mm-tall target is unremarkable.
 * Scroll-cancel was ruled out: that example clears LV_OBJ_FLAG_SCROLLABLE on
 * both the card and the screen.
 *
 * The hypothesis was TARGET SIZE, not a driver defect. This app tests it
 * directly by putting both sizes on one screen, under one finger, in one
 * session. Each button counts its own presses and clicks and prints both, so
 * the ratio is measured rather than remembered:
 *
 *   presses = LV_EVENT_PRESSED   (did the touch land on the widget at all)
 *   clicks  = LV_EVENT_CLICKED   (did press+release-inside-bounds survive)
 *
 * RESULT 2026-08-31, operator tapping, results/tap-target-2026-08-31.txt:
 *
 *     87px  10 presses / 10 clicks
 *     38px   9 presses /  9 clicks
 *
 * The HEIGHT hypothesis is REFUTED: across those 19 taps not one click was lost
 * at either height. What the run establishes, and it is the thing worth having,
 * is that the panel's 87 px row delivered a click on all 10 of its taps -- the
 * interaction model rests on that and it had never been tested.
 *
 * Scope it honestly. make_target() fixes the WIDTH at PANEL_W - 40 = 328 px and
 * varies only the height, so both targets here are 328 wide while the vendor's
 * button is 132x38. Height is exonerated; WIDTH IS UNTESTED. Do not read this
 * as "size was never the cause" -- add a 132 px-wide target before claiming
 * that. Note also that this counts LVGL PRESSED vs CLICKED, so a physical tap
 * that never became a PRESSED event is invisible to it.
 *
 * The vendor button's 1-in-6 remains UNEXPLAINED. Leading candidate, INFERRED
 * and untested: its callback runs example_probe_sdcard(), which mounts, writes
 * and unmounts the SD card synchronously INSIDE the LVGL event handler. That
 * blocks the LVGL task for hundreds of ms, and taps arriving in that window are
 * never processed at all. Whether or not that is the cause there, the rule it
 * implies is one we want anyway: never do blocking I/O in an LVGL callback --
 * hand it to a task and let the UI stay live.
 */

#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "tap_target";

#define PANEL_W 368
#define ROW_BIG 87      /* 44 pt at 322 ppi -- the panel spec's tap row */
#define ROW_SMALL 38    /* what 00_bsp_quickstart used */

typedef struct {
    const char *name;
    int height;
    int presses;
    int clicks;
    lv_obj_t *label;
} target_t;

static target_t s_big   = {.name = "87px", .height = ROW_BIG};
static target_t s_small = {.name = "38px", .height = ROW_SMALL};

static void refresh_label(target_t *t)
{
    if (t->label) {
        lv_label_set_text_fmt(t->label, "%s  press %d / click %d",
                              t->name, t->presses, t->clicks);
    }
}

static void on_event(lv_event_t *e)
{
    target_t *t = (target_t *)lv_event_get_user_data(e);
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        t->presses++;
    } else if (code == LV_EVENT_CLICKED) {
        t->clicks++;
    } else {
        return;
    }
    refresh_label(t);
    /* Printed on BOTH counters every time, so a lost click is visible in the
     * log as a press that never gained a matching click -- not inferred later
     * from a total. */
    ESP_LOGI(TAG, "%-5s presses=%d clicks=%d  %s",
             t->name, t->presses, t->clicks,
             (code == LV_EVENT_CLICKED) ? "<-- CLICK" : "press");
}

static void make_target(lv_obj_t *parent, target_t *t, int y)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, PANEL_W - 40, t->height);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2f9bff), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, on_event, LV_EVENT_PRESSED, t);
    lv_obj_add_event_cb(btn, on_event, LV_EVENT_CLICKED, t);

    t->label = lv_label_create(btn);
    lv_obj_center(t->label);
    lv_obj_set_style_text_color(t->label, lv_color_hex(0x001018), LV_PART_MAIN);
    refresh_label(t);
}

void app_main(void)
{
    ESP_LOGI(TAG, "tap_target_check: 87 px vs 38 px, press vs click");

    bsp_display_start();
    /* Set the level explicitly, exactly as 00_bsp_quickstart does. The
     * backlight helper alone left the panel black: brightness is a CO5300
     * register that survives a reflash, so whatever the previous app's slider
     * was left on is what you inherit. Never assume a lit panel. */
    esp_err_t bret = bsp_display_brightness_set(80);
    ESP_LOGI(TAG, "brightness_set(80) -> %s", esp_err_to_name(bret));

    if (bsp_display_lock(1000)) {
        lv_obj_t *scr = lv_scr_act();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x00060B), LV_PART_MAIN);
        /* Scrolling off, exactly as the quickstart does -- so a cancelled click
         * can never be blamed on scroll-drag interception. */
        lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(scr);
        lv_label_set_text(title, "tap each 10x");
        lv_obj_set_style_text_color(title, lv_color_hex(0xF2F6F7), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

        make_target(scr, &s_big, 50);
        make_target(scr, &s_small, 50 + ROW_BIG + 24);

        bsp_display_unlock();
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "[alive] 87px %d/%d   38px %d/%d  (press/click)",
                 s_big.presses, s_big.clicks, s_small.presses, s_small.clicks);
    }
}
