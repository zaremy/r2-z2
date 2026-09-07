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
#include "panel_touch.h"
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
    int ticks = 0;
    while (1) {
        if (bsp_display_lock(100)) {
            const bool changed = panel_ui_update(&s_tm, now_ms());
            panel_ui_burn_in(now_ms(), changed, set_brightness_pct);
            bsp_display_unlock();
        }
        /* P1 progress, once a minute (#101). Without this the panel records
         * touch extremes and never says so, which makes the measurement
         * INVISIBLE -- and an operator who has done the corners has no way to
         * know whether it worked. It also distinguishes "nobody touched it"
         * from "touch is not wired", which look identical from here.
         *
         * This block was lost once already: the edit that added it targeted an
         * anchor a previous edit had changed, the replace silently did nothing,
         * and only `int ticks = 0;` survived -- set, never read, and not loud
         * enough to fail the build. */
        if (++ticks % 240 == 0) {
            panel_touch_extremes_t ex;
            panel_touch_extremes(&ex);
            if (ex.points == 0) {
                ESP_LOGI(TAG, "P1: no touch points yet");
            } else {
                ESP_LOGI(TAG, "P1: %u points  x %d..%d  y %d..%d",
                         (unsigned)ex.points, ex.min_x, ex.max_x,
                         ex.min_y, ex.max_y);
                ESP_LOGI(TAG, "    edge gaps: L%d R%d T%d B%d  (0 = bezel reached)",
                         ex.min_x, 367 - ex.max_x, ex.min_y, 447 - ex.max_y);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
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

#ifdef PANEL_P2_RECONNECT
/* P2 — time the PANEL'S OWN reconnect (#101, gates AC8).
 *
 * AC8 wants `waking` to show BOUNDED PROGRESS against a real duration, and the
 * epic is explicit that the ~12 s figure floating around is the MAC DAEMON'S
 * and must not calibrate this bar. Nobody has ever timed the board's.
 *
 * Behind a build flag because dropping the link on purpose, repeatedly, is the
 * last thing a resting panel should do. It is safe -- every op here is read
 * tier, and a dropped link is what "default to STOP" already contemplates --
 * but it is not resting behaviour and must not be reachable by accident.
 *
 * n > 1 deliberately. A single reconnect is an anecdote, and a progress bar
 * calibrated to one sample will overrun or stall for every user of it. */
#define P2_TRIALS 8

static void p2_task(void *arg)
{
    (void)arg;
    uint32_t ms[P2_TRIALS];
    int n = 0;

    while (!r2_link_is_up()) vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "P2: timing %d deliberate reconnects", P2_TRIALS);

    while (n < P2_TRIALS) {
        vTaskDelay(pdMS_TO_TICKS(4000));      /* settle between trials */
        if (!r2_link_is_up()) continue;

        const uint32_t t0 = now_ms();
        r2_link_disconnect();
        /* Wait for the link to LEAVE up first. Timing from the request would
         * fold our own teardown into his reconnect and quietly inflate it. */
        while (r2_link_is_up()) vTaskDelay(pdMS_TO_TICKS(5));
        const uint32_t dropped = now_ms();

        while (!r2_link_is_up()) vTaskDelay(pdMS_TO_TICKS(5));
        ms[n] = now_ms() - dropped;
        ESP_LOGW(TAG, "P2 trial %d/%d: %"PRIu32" ms  (teardown %"PRIu32" ms)",
                 n + 1, P2_TRIALS, ms[n], dropped - t0);
        n++;
    }

    uint32_t lo = ms[0], hi = ms[0], sum = 0;
    for (int i = 0; i < n; i++) {
        if (ms[i] < lo) lo = ms[i];
        if (ms[i] > hi) hi = ms[i];
        sum += ms[i];
    }
    ESP_LOGW(TAG, "P2 RESULT: n=%d  min %"PRIu32" ms  max %"PRIu32
                  " ms  mean %"PRIu32" ms", n, lo, hi, sum / (uint32_t)n);
    ESP_LOGW(TAG, "  AC8's bar should be bounded by the MAX, not the mean:");
    ESP_LOGW(TAG, "  a bar that finishes early and waits reads as broken, and");
    ESP_LOGW(TAG, "  one that overruns reads as a hang.");
    vTaskDelete(NULL);
}
#endif

#ifdef PANEL_P4_IDLE
/* P4 — his idle timeout (#101, gates how `released` is presented over time).
 *
 * We never connect. The keepalive IS the wake command, so any session that
 * connects has already destroyed the thing it wanted to measure.
 *
 * WHAT THIS CAN CONCLUDE IS NARROWER THAN IT LOOKS, and saying so up front is
 * the point: the only sleep this project has ever observed was VISUAL -- he
 * reverted to his resting alternation and faded out. Whether a sleeping droid
 * stops advertising has never been established. So:
 *
 *   advertising STOPS   -> a real, machine-readable transition worth timing
 *   advertising CONTINUES -> INSTRUMENT-LIMITED. Not "he stayed awake".
 *
 * The second outcome is the likely one and must not be written up as a
 * finding about the droid. */
static void p4_task(void *arg)
{
    (void)arg;
    r2_link_set_scan_only(true);
    /* RESTART the scan. The flag is read when ble_gap_disc is called, and
     * on_sync already started one with filter_duplicates=1 -- so without this
     * we see him ONCE and never again, and the very comment in r2_link warning
     * about that was written by the same hand that then wired it wrong. The
     * first run reported "1 advert this minute", which is the dedup filter, not
     * his advertising rate, and would have made a real silence unmeasurable. */
    vTaskDelay(pdMS_TO_TICKS(500));
    r2_link_start();
    ESP_LOGW(TAG, "P4: scan-only. We will NOT connect, because the keepalive");
    ESP_LOGW(TAG, "    is his wake command and connecting destroys the thing");
    ESP_LOGW(TAG, "    being measured. Watching the advertisement only.");
    const uint32_t t0 = now_ms();
    uint32_t last_seen = 0, adverts = 0, prev_adverts = 0;
    bool ever_seen = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        r2_link_adverts(&adverts, &last_seen);
        const uint32_t mins = (now_ms() - t0) / 60000u;
        const uint32_t since = adverts - prev_adverts;
        prev_adverts = adverts;
        if (since > 0) {
            ever_seen = true;
            ESP_LOGW(TAG, "P4 t+%2umin: %u adverts this minute (still advertising)",
                     (unsigned)mins, (unsigned)since);
        } else if (!ever_seen) {
            /* Never seen him AT ALL. That is an instrument failure, not a
             * sleeping droid, and the two are indistinguishable unless this
             * says so -- the first P4 run reported zero adverts because a
             * stale scan was deduplicating him, and it read exactly like
             * sleep. */
            ESP_LOGE(TAG, "P4 t+%2umin: NO ADVERTS EVER SEEN. The scanner has "
                          "not produced a single positive, so silence here is "
                          "an INSTRUMENT FAILURE and proves nothing about him.",
                     (unsigned)mins);
        } else {
            ESP_LOGW(TAG, "P4 t+%2umin: *** NO ADVERTS THIS MINUTE *** "
                          "(last seen t+%umin, after %u total)",
                     (unsigned)mins, (unsigned)((last_seen - t0) / 60000u),
                     (unsigned)adverts);
        }
    }
}
#endif

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
        panel_touch_init();
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
#ifdef PANEL_P4_IDLE
    xTaskCreate(p4_task,   "panel_p4",     4096, NULL, 4, NULL);
#endif
#ifdef PANEL_P2_RECONNECT
    xTaskCreate(p2_task,   "panel_p2",     4096, NULL, 4, NULL);
#endif
}
