/* #103 A2 — does current ESP-Brookesia build AND run on this board?
 *
 * Modelled on the component's own phone_s3_box_3 example, reduced to the part
 * that answers the question: bring the BSP display + touch up, construct a
 * Brookesia phone on it, and begin(). No apps installed -- if the shell renders,
 * A2 is answered; installed apps would only add ways to fail for other reasons.
 *
 * WHAT THIS ALREADY ESTABLISHED, before a single line ran:
 *   - The dependency set RESOLVES: BSP 2.0.3 + esp-brookesia 0.5.0 forces
 *     lvgl down to 9.2.2 (Brookesia pins 9.2.*; the BSP permits >=8,<10).
 *   - It does NOT build with the esp_lvgl_port the BSP resolves on its own.
 *     Port 2.9.0 and 2.8.0 both reference LV_COLOR_FORMAT_RGB565_SWAPPED, which
 *     does not exist in LVGL 9.2.2 (0 occurrences in its source). Their declared
 *     constraint is lvgl >=8,<10 -- true of the manifest, false of the code, so
 *     the solver accepts a combination the compiler rejects. Pinning
 *     esp_lvgl_port to 2.6.0 in OUR manifest fixes it without patching the BSP.
 *
 * AND THE ONE THAT MATTERS FOR #101: Brookesia ships no stylesheet for this
 * panel. It has 320x240, 320x480, 480x480, 800x480, 1024x600, 720x1280,
 * 1280x800 -- and ours is 368x448. The example only adds a stylesheet when the
 * resolution matches exactly, so this runs on Brookesia's built-in default.
 * Whether that is usable at 368x448 is precisely what this app is for.
 */
#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "esp_log.h"

#include <new>

static const char *TAG = "a2_brookesia";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "#103 A2: esp-brookesia 0.5.0 on LVGL 9.2.2, panel %dx%d",
             BSP_LCD_H_RES, BSP_LCD_V_RES);

    /* Plain bsp_display_start(), not start_with_config(): the example's config
     * references CONFIG_BSP_LCD_DRAW_BUF_HEIGHT, which is another board's
     * Kconfig symbol -- this BSP exposes BSP_LCD_DRAW_BUFF_SIZE instead. The
     * no-argument form is also the one already proven to render on this board
     * by 00_bsp_quickstart, so it removes a variable rather than adding one. */
    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start failed -- A2 fails at the BSP, not at Brookesia");
        return;
    }
    /* Explicit level, not backlight_on: brightness is a CO5300 register that
     * survives a reflash, so an inherited 0 looks exactly like a dead app. */
    bsp_display_brightness_set(80);

    /* POSITIVE CONTROL, before Brookesia exists.
     *
     * phone->begin() returned OK and the panel stayed black. That has two very
     * different causes and they need separating before anything is concluded:
     *   (a) the render path itself is broken -- plausible, because this build
     *       pins esp_lvgl_port back to 2.6.0 to satisfy LVGL 9.2.2, and the BSP
     *       normally resolves 2.9.0;
     *   (b) the render path is fine and Brookesia's home screen is simply empty,
     *       because no apps were installed (the vendor example installs three).
     * A plain magenta rectangle drawn by LVGL, with no Brookesia involvement,
     * tells (a) from (b) in one look. */
    /* Wait for the lock rather than a 0-timeout try: a 0 means "fail if the
     * LVGL task holds it", and the calls below mutate LVGL unconditionally.
     * firmware/tap_target_check uses 1000 for the same reason. */
    if (!bsp_display_lock(1000)) {
        ESP_LOGE(TAG, "could not acquire the LVGL lock -- aborting rather than "
                      "mutating LVGL unlocked");
        return;
    }
    {
        lv_obj_t *probe = lv_obj_create(lv_scr_act());
        lv_obj_set_size(probe, BSP_LCD_H_RES, BSP_LCD_V_RES);
        lv_obj_set_style_bg_color(probe, lv_color_hex(0xFF00FF), LV_PART_MAIN);
        lv_obj_set_style_border_width(probe, 0, LV_PART_MAIN);
        lv_obj_center(probe);
        lv_obj_t *lbl = lv_label_create(probe);
        lv_label_set_text(lbl, "LVGL RENDER OK");
        lv_obj_center(lbl);
    }
    bsp_display_unlock();
    ESP_LOGI(TAG, "positive control drawn: panel should be MAGENTA for 12 s");
    vTaskDelay(pdMS_TO_TICKS(12000));
    if (!bsp_display_lock(1000)) {
        ESP_LOGE(TAG, "could not re-acquire the LVGL lock before constructing the phone");
        return;
    }
    lv_obj_clean(lv_scr_act());

    /* nothrow: CONFIG_COMPILER_CXX_EXCEPTIONS is on, so a plain `new` would
     * THROW on exhaustion and the null check below would be dead code. */
    ESP_Brookesia_Phone *phone = new (std::nothrow) ESP_Brookesia_Phone(disp);
    if (phone == nullptr) {
        ESP_LOGE(TAG, "Create phone failed (allocation)");
        bsp_display_unlock();
        return;
    }
    ESP_LOGI(TAG, "phone object created");

    if (!phone->setTouchDevice(bsp_display_get_input_dev())) {
        ESP_LOGE(TAG, "setTouchDevice failed");
    }
    phone->registerLvLockCallback((ESP_Brookesia_GUI_LockCallback_t)(bsp_display_lock), 0);
    phone->registerLvUnlockCallback((ESP_Brookesia_GUI_UnlockCallback_t)(bsp_display_unlock));

    if (!phone->begin()) {
        ESP_LOGE(TAG, "phone->begin() FAILED -- record this as A2's failure mode");
    } else {
        ESP_LOGI(TAG, "phone->begin() OK -- Brookesia shell is up on a %dx%d panel "
                      "with no matching stylesheet", BSP_LCD_H_RES, BSP_LCD_V_RES);
    }

    bsp_display_unlock();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "[alive] heap=%u", (unsigned)esp_get_free_heap_size());
    }
}
