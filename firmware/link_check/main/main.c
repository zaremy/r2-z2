/* link_check -- the first time this stack talks to R2. #114 slice 4.
 *
 * Slices 1-3 are 1,095 host checks that have never sent a byte. This app is
 * the call site that makes them real, and it is deliberately the smallest one
 * that can prove it:
 *
 *   r2_ops  ->  r2_gate  ->  r2_packet  ->  r2_link  ->  NimBLE  ->  R2
 *
 * It connects, keeps him awake, and reads three things. Everything it prints
 * is DECODED -- volts, degrees -- not hex. Hex is what the old firmware
 * printed, and hex is why a 6.5% response loss sat in the logs for weeks
 * without anyone noticing it was there.
 *
 * SAFETY. Every op here is READ or LEDS tier. The LED phase is opt-in at build
 * time (-DLINK_CHECK_LEDS) and raises the ceiling exactly one rung, to 'leds',
 * for the duration of one scripted sequence -- then puts it back. Nothing in
 * this app can move him at any point: dome, stance, leg and drive commands sit
 * above 'leds' AND are in the gate's FORBIDDEN table, which is checked first
 * and applies at every ceiling. That is not a promise in a comment, it
 * is the default ceiling plus a FORBIDDEN table that rejects drive, leg and
 * animation commands at every tier. The one write is the wake keepalive, which
 * is idempotent and is what stops him sleeping mid-run.
 */
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

#include "r2_gate.h"
#include "r2_link.h"
#include "r2_ops.h"
#include "r2_packet.h"
#include "r2_telemetry.h"

static const char *TAG = "link_check";

/* What we asked for and have not yet heard back about. Without this, a lost
 * response is invisible -- which is the entire lesson of the A1 logs. */
static volatile uint32_t s_req_battery = 0, s_rsp_battery = 0;
static volatile uint32_t s_req_head    = 0, s_rsp_head    = 0;
static volatile uint32_t s_req_version = 0, s_rsp_version = 0;
static volatile uint32_t s_unmatched   = 0;
static volatile uint32_t s_wake_acks   = 0;

/* The version probe fires once and its answer is the whole point of the run.
 * Printed once, it scrolls past before a --no-reset attach can even connect --
 * which is exactly what happened on the first two runs. So it is STICKY: held
 * here and repeated in every report line. A result you can miss by attaching
 * late is a badly designed diagnostic. */
static volatile bool     s_version_seen = false;
static r2_version_t      s_version;
static volatile uint8_t  s_version_err  = 0;

/* THE ESCAPE GAUNTLET.
 *
 * The old firmware built packets by hand and escaped nothing. A Sphero frame
 * must escape 0x8D, 0xD8 and 0xAB anywhere in the body, and the sequence byte
 * walks through all three. In the A1 endurance logs, ALL EIGHT lost battery
 * replies across two independent arms landed on exactly the six sequence values
 * that encoder mangles -- eight for eight, no false positives.
 *
 * So do not wait for chance to walk the counter onto one. Send a battery read
 * at each of the three sequence numbers that MUST be escaped, and at the two
 * that only the checksum makes bad. Under the old encoder every one of these
 * was guaranteed lost. If they all come back, the fix is proved on hardware
 * rather than argued from a diff. */
static const uint8_t k_gauntlet[] = { 0x8D, 0xAB, 0xD8, 0x07, 0x34, 0x52 };
static volatile uint32_t s_gauntlet_sent = 0, s_gauntlet_ok = 0;
static volatile bool s_in_gauntlet = false;

/* The panel's view of the world (#114 AC6). Nothing renders it yet -- the
 * display stack is #101 -- but it is FED from the live path here rather than
 * built alongside it, because this repo's most expensive habit is landing a
 * layer with no caller and discovering four PRs later that it was inert. */
static r2_telemetry_t s_tm;
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void telemetry_report(void)
{
    uint32_t age;
    const uint32_t t = now_ms();
    ESP_LOGI(TAG, "PANEL WOULD SHOW:  link=%s", r2_telemetry_link_name(s_tm.link));

    if (r2_telemetry_displayable(&s_tm, &s_tm.battery, t, 60000)
        && r2_telemetry_age_ms(&s_tm.battery, t, &age))
        ESP_LOGI(TAG, "   battery  %u.%02u V   (%u ms old)",
                 s_tm.battery_centivolts / 100u, s_tm.battery_centivolts % 100u, age);
    else
        ESP_LOGI(TAG, "   battery  --  (nothing we can vouch for)");

    if (r2_telemetry_displayable(&s_tm, &s_tm.dome, t, 60000)
        && r2_telemetry_age_ms(&s_tm.dome, t, &age))
        ESP_LOGI(TAG, "   dome     %.2f deg  (%u ms old)", (double)s_tm.dome_degrees, age);
    else
        ESP_LOGI(TAG, "   dome     --");

    if (r2_telemetry_displayable(&s_tm, &s_tm.version, t, 3600000))
        ESP_LOGI(TAG, "   firmware %u.%u.%u", s_tm.version_major,
                 s_tm.version_minor, s_tm.version_revision);
    else
        ESP_LOGI(TAG, "   firmware --");

    ESP_LOGI(TAG, "   asked %"PRIu32"  answered %"PRIu32"  refused %"PRIu32
                  "  dropped %"PRIu32,
             s_tm.requests, s_tm.responses, s_tm.refused, s_tm.dropped);
}

static uint8_t next_seq(void)
{
    static uint8_t seq = 0;
    return seq++;
}

static void on_state(r2_link_state_t s, int reason, void *ctx)
{
    (void)ctx;
    r2_telemetry_link(&s_tm, (r2_tm_link_t)s, now_ms());
    if (s == R2_LINK_DOWN && reason)
        ESP_LOGW(TAG, "LINK DOWN (reason=%d)", reason);
    else if (s == R2_LINK_UP)
        ESP_LOGI(TAG, "LINK UP -- R2 is listening");
}

static void on_frame(const uint8_t *frame, size_t len, void *ctx)
{
    (void)ctx;
    uint8_t scratch[64];
    r2_response_t r;
    const int rc = r2_packet_decode(frame, len, scratch, sizeof scratch, &r);
    if (rc != R2_OK) {
        /* A frame that will not decode is a finding, not noise. The old
         * firmware printed these as hex and they read as normal traffic. */
        ESP_LOGW(TAG, "undecodable frame (%d bytes, err %d)", (int)len, rc);
        return;
    }

    r2_battery_t b;
    r2_head_t    h;
    r2_version_t v;

    if (r2_ops_parse_battery(&r, &b) == R2_OPS_OK) {
        s_rsp_battery++;
        r2_telemetry_battery(&s_tm, b.centivolts, now_ms());
        for (size_t i = 0; i < sizeof k_gauntlet; i++)
            if (r.seq == k_gauntlet[i]) { s_gauntlet_ok++; break; }
        ESP_LOGI(TAG, "battery   %u.%02u V   (seq=0x%02X)",
                 b.centivolts / 100u, b.centivolts % 100u, r.seq);
        return;
    }
    if (r2_ops_parse_head(&r, &h) == R2_OPS_OK) {
        s_rsp_head++;
        r2_telemetry_dome(&s_tm, h.degrees, now_ms());
        ESP_LOGI(TAG, "dome      %.2f deg  (seq=0x%02X)", (double)h.degrees, r.seq);
        return;
    }
    if (r2_ops_parse_version(&r, &v) == R2_OPS_OK) {
        s_rsp_version++;
        s_version = v;
        s_version_seen = true;
        r2_telemetry_version(&s_tm, v.major, v.minor, v.revision, now_ms());
        ESP_LOGI(TAG, "*** VERSION ANSWERED: %u.%u.%u -- the library claim holds",
                 v.major, v.minor, v.revision);
        return;
    }

    /* The keepalive's own acknowledgement. Our wake carries REQUESTS_RESPONSE,
     * so R2 answers every one with an empty 0x13/0x0D. Counting these as
     * "unmatched" buries a genuinely unmatched frame under fifty of them --
     * an alarm that fires constantly is one nobody reads. */
    if (r.did == 0x13 && r.cid == 0x0D && r.err == 0 && r.data_len == 0) {
        s_wake_acks++;
        return;
    }

    /* Not one of ours, or an error reply. Say WHICH, because "R2 refused the
     * version probe" is the single most likely outcome of this run and it is a
     * result, not a malfunction. */
    if (r.err != 0) {
        if (r.did == 0x11 && r.cid == 0x00) s_version_err = r.err;
        ESP_LOGW(TAG, "R2 refused did=0x%02X cid=0x%02X: %s",
                 r.did, r.cid, r2_ops_device_error_name(r.err));
    } else {
        s_unmatched++;
        ESP_LOGI(TAG, "unmatched response did=0x%02X cid=0x%02X len=%u",
                 r.did, r.cid, (unsigned)r.data_len);
    }
}

static void report(void)
{
    uint32_t admitted, refused, sent, dropped;
    r2_gate_stats(&admitted, &refused);
    r2_link_stats(&sent, &dropped);
    ESP_LOGI(TAG,
             "-- battery %"PRIu32"/%"PRIu32"  dome %"PRIu32"/%"PRIu32
             "  version %"PRIu32"/%"PRIu32"  wake-acks %"PRIu32"  unmatched %"PRIu32
             "  | gate admitted=%"PRIu32" refused=%"PRIu32
             "  link sent=%"PRIu32" dropped=%"PRIu32,
             s_rsp_battery, s_req_battery, s_rsp_head, s_req_head,
             s_rsp_version, s_req_version, s_wake_acks, s_unmatched,
             admitted, refused, sent, dropped);

    if (s_version_seen)
        ESP_LOGI(TAG, "   main app version = %u.%u.%u  (LIBRARY CLAIM CONFIRMED "
                      "ON HARDWARE -- first time this project has ever sent it)",
                 s_version.major, s_version.minor, s_version.revision);
    else if (s_version_err)
        ESP_LOGW(TAG, "   version probe REFUSED: %s -- the library claim does not "
                      "hold on this firmware", r2_ops_device_error_name(s_version_err));
    else
        ESP_LOGW(TAG, "   version probe: no answer and no refusal (silence)");
}

#ifdef LINK_CHECK_LEDS
/* ONE LED STEP PER FLASH, and the state HOLDS until the next flash.
 *
 * The first attempt ran all six steps back to back on five-second timers. That
 * is the "fixed lead-in" failure the survey skill warns about, wearing a
 * different hat: the observer gets one five-second window per step and no way
 * to ask for a second look. It produced zero observations.
 *
 * So: -DLINK_CHECK_LED_STEP=N fires exactly step N, once, and then does
 * nothing. The colour stays on him for as long as it takes to look, walk
 * closer, and say what it is. The ceiling is raised for the one write and put
 * back immediately -- a rung that stays open because it is convenient is a rung
 * that has stopped meaning anything.
 */
#ifndef LINK_CHECK_LED_STEP
#define LINK_CHECK_LED_STEP 1
#endif

static void led_step(void)
{
    const uint8_t logic_on[1]  = { 255 };
    const uint8_t holo_half[1] = { 128 };
    const uint8_t off8[8]      = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const int n = LINK_CHECK_LED_STEP;
    int rc = 0;

    ESP_LOGW(TAG, "=== LED STEP %d ===", n);
    if (n != 8) r2_gate_set_ceiling(R2_TIER_LEDS);

    switch (n) {
    case 1:
        ESP_LOGW(TAG, "expect: front AND back lenses RED, and they STAY red");
        rc = r2_ops_set_rgb(255, 0, 0, next_seq(), r2_link_send, NULL);
        break;
    case 2:
        ESP_LOGW(TAG, "expect: front AND back lenses GREEN");
        rc = r2_ops_set_rgb(0, 255, 0, next_seq(), r2_link_send, NULL);
        break;
    case 3:
        ESP_LOGW(TAG, "expect: front AND back lenses BLUE");
        rc = r2_ops_set_rgb(0, 0, 255, next_seq(), r2_link_send, NULL);
        break;
    case 4:
        ESP_LOGW(TAG, "expect: RGB lenses dark, LOGIC PANELS lit (square grids)");
        r2_ops_set_leds(R2_LED_MASK_ALL, off8, 8, next_seq(), r2_link_send, NULL);
        vTaskDelay(pdMS_TO_TICKS(400));
        rc = r2_ops_set_leds(1u << R2_LED_LOGIC, logic_on, 1, next_seq(),
                             r2_link_send, NULL);
        break;
    case 5:
        ESP_LOGW(TAG, "expect: everything dark except the HOLO lens at HALF (128/255)");
        r2_ops_set_leds(R2_LED_MASK_ALL, off8, 8, next_seq(), r2_link_send, NULL);
        vTaskDelay(pdMS_TO_TICKS(400));
        rc = r2_ops_set_leds(1u << R2_LED_HOLO, holo_half, 1, next_seq(),
                             r2_link_send, NULL);
        break;
    case 6:
        ESP_LOGW(TAG, "expect: ALL EIGHT channels dark. He does NOT return to his");
        ESP_LOGW(TAG, "        own red/blue idiom -- a colour we set is state and");
        ESP_LOGW(TAG, "        it holds (r2-capabilities.md:852). Dark is where he");
        ESP_LOGW(TAG, "        stays until something sets him otherwise.");
        rc = r2_ops_leds_off(next_seq(), r2_link_send, NULL);
        break;
    case 7:
        ESP_LOGW(TAG, "expect: HOLO at FULL (255) -- the comparison step 5 needs,");
        ESP_LOGW(TAG, "        because 'half' is not a judgement anyone can make");
        ESP_LOGW(TAG, "        without seeing full.");
        r2_ops_set_leds(R2_LED_MASK_ALL, off8, 8, next_seq(), r2_link_send, NULL);
        vTaskDelay(pdMS_TO_TICKS(400));
        { const uint8_t full[1] = { 255 };
          rc = r2_ops_set_leds(1u << R2_LED_HOLO, full, 1, next_seq(),
                               r2_link_send, NULL); }
        break;
    case 9:
        /* The correct ENDING, as opposed to step 6's correct teardown.
         *
         * Step 6 sets all eight channels to zero and that state HOLDS -- he
         * does not return to his own red/blue idiom. Measured: he sat dark for
         * 25 minutes, awake and answering reads the whole time, and read to the
         * operator as POWERED OFF. So "off" is the right primitive and the
         * wrong place to stop.
         *
         * Blue is not a colour invented here. D-012 assigns blue steady to
         * 'idle -- nothing engaged', chosen because it is the dimmest corner
         * (0.072 relative luminance) and idle is the state that runs for
         * hours. */
        ESP_LOGW(TAG, "expect: front AND back BLUE -- D-012 idle, the state a");
        ESP_LOGW(TAG, "        session should LEAVE him in. Never dark: dark");
        ESP_LOGW(TAG, "        reads as broken to anyone walking past.");
        rc = r2_ops_set_rgb(0, 0, 255, next_seq(), r2_link_send, NULL);
        break;
    case 8:
        /* THE NULL CONTROL, and the only step that tests the safety property
         * rather than the hardware.
         *
         * Every step above raised the ceiling first, so all of them prove the
         * same thing: that an ALLOWED write renders. None of them shows that a
         * REFUSED write does not -- and a gate that returns the right verdict
         * while the frame still reaches the radio is a log line, not a gate.
         *
         * So: leave the ceiling at its default and ask for bright red on a
         * droid that is currently dark. The host tests say tx is never called.
         * This asks the only instrument that can see the difference. */
        ESP_LOGW(TAG, "NULL CONTROL: ceiling deliberately LEFT at 'read'");
        ESP_LOGW(TAG, "expect: NOTHING. He must stay dark. If he goes red, the");
        ESP_LOGW(TAG, "        ceiling is decorative and slice 2 is a fiction.");
        break;
    default:
        ESP_LOGE(TAG, "no such step");
        break;
    }

    if (n == 8) {
        /* Ceiling never raised. This must be refused, and nothing must go out. */
        uint32_t sent_before, sent_after, dropped;
        r2_link_stats(&sent_before, &dropped);
        rc = r2_ops_set_rgb(255, 0, 0, next_seq(), r2_link_send, NULL);
        r2_link_stats(&sent_after, &dropped);
        ESP_LOGW(TAG, "  set_rgb(255,0,0) at ceiling '%s' -> %s",
                 r2_gate_tier_name(r2_gate_get_ceiling()),
                 r2_gate_verdict_name((r2_gate_verdict_t)rc));
        ESP_LOGW(TAG, "  frames that reached the radio: %"PRIu32
                      "  (MUST be 0)", sent_after - sent_before);
    }

    r2_gate_set_ceiling(R2_TIER_READ);
    ESP_LOGW(TAG, "step %d sent (rc=%d), ceiling back to '%s'. Holding.",
             n, rc, r2_gate_tier_name(r2_gate_get_ceiling()));
    ESP_LOGW(TAG, "NOTE: rc>0 means R2 ACCEPTED the write. It does NOT mean any");
    ESP_LOGW(TAG, "      fixture lit -- only the operator can say that.");
}
#endif

static void ops_task(void *arg)
{
    (void)arg;
    int tick = 0;
    bool probed_version = false;
    bool ran_gauntlet   = false;
    bool ran_leds       = false;
    bool ran_tm_proof   = false;
    (void)ran_leds;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (!r2_link_is_up()) continue;

        /* Keepalive. DID 0x13 / CID 0x0D -- idempotent, and it is what stops
         * him sleeping. CID 0x01 is SLEEP and must never appear here. Routed
         * through the gate like everything else, so it is escaped and counted. */
        r2_gate_send(0x13, 0x0D, next_seq(), NULL, 0, r2_link_send, NULL);

        /* A battery read every 15 s. Frequent enough that a lost response
         * shows up as a ratio within a short run. */
        if (tick % 5 == 0) {
            s_req_battery++;
            r2_telemetry_note_request(&s_tm);
            r2_ops_request_battery(next_seq(), r2_link_send, NULL);
        }

        /* The dome, every 30 s. READ ONLY -- this asks where he is looking, it
         * does not turn him. */
        if (tick % 10 == 3) {
            s_req_head++;
            r2_telemetry_note_request(&s_tm);
            r2_ops_request_head(next_seq(), r2_link_send, NULL);
        }

        /* The version probe ONCE, ten seconds in. It has never been sent by
         * this project and the likely answer is bad_command_id. Sending it in
         * a loop would just repeat one unknown; sending it once answers it. */
        if (!probed_version && tick == 1) {
            probed_version = true;
            s_req_version++;
            /* Counted like any other request. Without this the panel shows
             * "asked 2 answered 3", and a ratio that can exceed 1 is worse
             * than no ratio: it makes the one number that would have exposed
             * the escaping bug look broken instead of informative. */
            r2_telemetry_note_request(&s_tm);
            ESP_LOGI(TAG, "probing main app version (never sent before) ...");
            r2_ops_probe_version(next_seq(), r2_link_send, NULL);
        }

        /* Once, after the ordinary reads have proved the path works. */
        if (!ran_gauntlet && tick == 8) {
            ran_gauntlet = true;
            s_in_gauntlet = true;
            ESP_LOGW(TAG, "ESCAPE GAUNTLET: battery reads at the 6 sequence "
                          "numbers the old unescaped encoder always lost");
            for (size_t i = 0; i < sizeof k_gauntlet; i++) {
                s_gauntlet_sent++;
                s_req_battery++;
                r2_telemetry_note_request(&s_tm);
                r2_ops_request_battery(k_gauntlet[i], r2_link_send, NULL);
                vTaskDelay(pdMS_TO_TICKS(400));   /* let each reply land */
            }
            vTaskDelay(pdMS_TO_TICKS(1500));
            s_in_gauntlet = false;
            ESP_LOGW(TAG, "ESCAPE GAUNTLET: %"PRIu32"/%"PRIu32" answered",
                     s_gauntlet_ok, s_gauntlet_sent);
        }

#ifdef LINK_CHECK_LEDS
        if (!ran_leds) { ran_leds = true; led_step(); }
#endif

        /* THE TELEMETRY PROOF (#114 AC6), once, after readings exist.
         *
         * Host tests show a reading is invalidated when the link falls. They
         * cannot show that the real link falling does it, because in a host
         * test I am the one calling r2_telemetry_link() -- I am asserting the
         * wiring by performing it. So: drop the actual BLE link and look at
         * what the panel would show a tenth of a second later. */
        if (!ran_tm_proof && s_tm.battery.valid && s_tm.dome.valid) {
            ran_tm_proof = true;
            ESP_LOGW(TAG, "=== AC6 PROOF: telemetry must not outlive its link ===");
            ESP_LOGW(TAG, "--- before: link is up, readings are real ---");
            telemetry_report();
            ESP_LOGW(TAG, "--- dropping the BLE link deliberately ---");
            r2_link_disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));
            ESP_LOGW(TAG, "--- after: every reading must read '--' ---");
            telemetry_report();
            ESP_LOGW(TAG, "    (it reconnects on its own; values return only");
            ESP_LOGW(TAG, "     when the NEW link answers, never inherited)");
            continue;
        }

        if (++tick % 10 == 0) { report(); telemetry_report(); }
    }
}

static void on_sync(void) { r2_link_start(); }
static void host_task(void *param) { (void)param; nimble_port_run(); nimble_port_freertos_deinit(); }

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "link_check -- ceiling is '%s', nothing here can move him",
             r2_gate_tier_name(r2_gate_get_ceiling()));

    r2_telemetry_reset(&s_tm);
    const r2_link_cbs_t cbs = { .on_state = on_state, .on_frame = on_frame, .ctx = NULL };
    r2_link_init(&cbs);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);

    xTaskCreate(ops_task, "r2_ops", 4096, NULL, 4, NULL);
}
