/* The panel becomes the droid's own screen — #101 children 2 and 3.
 *
 * Child 2: it boots as the DEVICE'S OWN SURFACE, on our partition table
 * (D-022), with no vendor launcher underneath and nothing to return to
 * (D-021 -- we own idle).
 *
 * Child 3: the resting STATUS frame on real glass.
 *
 * And child 7 does not exist any more. The epic budgeted "live telemetry
 * replaces mock" as a late child because it assumed the panel would be built
 * against fabricated data for weeks first. #114 landed the link before the UI,
 * so the mock layer was never written: these rows have never shown anything
 * but R2. That was the stated reason for doing S5 first, and it is the cheaper
 * order -- a rendering bug can be told apart from a data bug.
 *
 * NOT ESP-BROOKESIA. #101 child 1, decided: plain LVGL 9. Its value was "our
 * app beside the vendor's" and D-022 removed the vendor's.
 *
 * SAFETY: the gate ceiling is never raised. This panel reads; it cannot move
 * him, light him, or sound him.
 */
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "lvgl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

#include "panel_shot.h"
#include "panel_ui.h"
#include "r2_gate.h"
#include "r2_link.h"
#include "r2_ops.h"
#include "r2_packet.h"
#include "r2_telemetry.h"

static const char *TAG = "panel";

static r2_telemetry_t s_tm;
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint8_t next_seq(void) { static uint8_t s; return s++; }

static void on_state(r2_link_state_t s, int reason, void *ctx)
{
    (void)ctx; (void)reason;
    /* The telemetry layer forgets every reading here. That is what keeps a
     * voltage from outliving the link that carried it, and it is why the R2
     * row goes to "--" rather than holding the last good number. */
    r2_telemetry_link(&s_tm, (r2_tm_link_t)s, now_ms());
    ESP_LOGI(TAG, "link -> %s", r2_link_state_name(s));
}

static void on_frame(const uint8_t *frame, size_t len, void *ctx)
{
    (void)ctx;
    uint8_t scratch[64];
    r2_response_t r;
    if (r2_packet_decode(frame, len, scratch, sizeof scratch, &r) != R2_OK) return;

    r2_battery_t b;
    r2_head_t    h;
    r2_version_t v;
    if (r2_ops_parse_battery(&r, &b) == R2_OPS_OK)
        r2_telemetry_battery(&s_tm, b.centivolts, now_ms());
    else if (r2_ops_parse_head(&r, &h) == R2_OPS_OK)
        r2_telemetry_dome(&s_tm, h.degrees, now_ms());
    else if (r2_ops_parse_version(&r, &v) == R2_OPS_OK)
        r2_telemetry_version(&s_tm, v.major, v.minor, v.revision, now_ms());
}

/* Talks to R2. Never touches LVGL. */
static void link_task(void *arg)
{
    (void)arg;
    int tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (!r2_link_is_up()) continue;

        /* Keepalive. This is also what stops him sleeping, which is a real
         * cost and a deliberate one for now: D-023 records that powering him
         * down is us stopping, and the panel's RELEASE control is the place
         * that gets decided -- not here. */
        r2_gate_send(0x13, 0x0D, next_seq(), NULL, 0, r2_link_send, NULL);

        if (tick % 5 == 0) {
            r2_telemetry_note_request(&s_tm);
            r2_ops_request_battery(next_seq(), r2_link_send, NULL);
        }
        /* The ratio, logged as well as shown. The first run on glass read
         * "35/55 answered" where link_check -- the same stack with no display
         * -- ran 138/138. Either the display is costing us responses or the
         * accounting is wrong, and a number on a panel nobody can screenshot
         * mid-run cannot tell me which. */
        if (tick % 20 == 0) {
            uint32_t sent, dropped, admitted, refused;
            r2_link_stats(&sent, &dropped);
            r2_gate_stats(&admitted, &refused);
            ESP_LOGI(TAG, "t+%us  answered %u/%u  | gate adm=%u ref=%u  link sent=%u drop=%u",
                     (unsigned)(now_ms() / 1000), (unsigned)s_tm.responses,
                     (unsigned)s_tm.requests, (unsigned)admitted, (unsigned)refused,
                     (unsigned)sent, (unsigned)dropped);
        }
        tick++;
    }
}

/* Injected into panel_ui so the UI file stays free of the BSP. */
static void set_brightness_pct(int percent) { bsp_display_brightness_set(percent); }

/* Draws. Never touches the radio. The two never share anything but the
 * telemetry struct, which is written on the NimBLE host task and read here --
 * every field is word-sized or smaller and a torn read shows one stale value
 * for one frame, which is a redraw away from correct. */
static void ui_task(void *arg)
{
    (void)arg;
    while (1) {
        if (bsp_display_lock(100)) {
            const bool changed = panel_ui_update(&s_tm, now_ms());
            panel_ui_burn_in(now_ms(), changed, set_brightness_pct);
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(250));

        /* A screenshot every 15 s, so a visual check costs a serial capture
         * instead of the operator walking to the droid with a phone. */
        /* The snapshot itself needs the lock; the emit takes seconds and must
         * NOT hold it, or the UI stalls for the whole dump. */

    }
}

/* Screenshots get their OWN task, with a generous stack.
 *
 * They started life inside the UI task and overflowed its 4 KB in about a
 * minute: lv_snapshot_take plus the partition writes are far heavier than a
 * redraw, and the crash-loop was INVISIBLE in the thing I was using to check
 * the panel -- the captured frame looked perfect every time, because a frame
 * captured two seconds before a reboot looks exactly like a healthy one. Only
 * the serial log showed five boots in four minutes.
 *
 * So the diagnostic is isolated from the thing it diagnoses: a screenshot can
 * now fail, or run out of stack, without taking the panel down with it. */
static void shot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(15000));   /* let the link settle first */
    while (1) {
        panel_shot_take();
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

static void on_sync(void) { r2_link_start(); }
static void host_task(void *p) { (void)p; nimble_port_run(); nimble_port_freertos_deinit(); }

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    bsp_display_start();
    /* Explicitly, every boot. Brightness is a CO5300 register that SURVIVES A
     * REFLASH, so a previous app's low slider leaves a healthy app looking
     * like dead hardware. The display's version of "assert the status on
     * connect, never inherit it". */
    ESP_ERROR_CHECK(bsp_display_brightness_set(80));

    if (bsp_display_lock(2000)) {
        panel_ui_create();
        bsp_display_unlock();
    }
    ESP_LOGI(TAG, "STATUS frame drawn; ceiling is '%s'",
             r2_gate_tier_name(r2_gate_get_ceiling()));

    r2_telemetry_reset(&s_tm);
    const r2_link_cbs_t cbs = { .on_state = on_state, .on_frame = on_frame, .ctx = NULL };
    r2_link_init(&cbs);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);

    xTaskCreate(link_task, "r2_link_task", 4096, NULL, 4, NULL);
    xTaskCreate(ui_task,   "panel_ui",     4096, NULL, 3, NULL);
    xTaskCreate(shot_task, "panel_shot",   8192, NULL, 2, NULL);
}
