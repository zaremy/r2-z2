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
 * SAFETY. Every op here is READ tier and the gate's ceiling is never raised,
 * so nothing in this app can move him. That is not a promise in a comment, it
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

static uint8_t next_seq(void)
{
    static uint8_t seq = 0;
    return seq++;
}

static void on_state(r2_link_state_t s, int reason, void *ctx)
{
    (void)ctx;
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
        for (size_t i = 0; i < sizeof k_gauntlet; i++)
            if (r.seq == k_gauntlet[i]) { s_gauntlet_ok++; break; }
        ESP_LOGI(TAG, "battery   %u.%02u V   (seq=0x%02X)",
                 b.centivolts / 100u, b.centivolts % 100u, r.seq);
        return;
    }
    if (r2_ops_parse_head(&r, &h) == R2_OPS_OK) {
        s_rsp_head++;
        ESP_LOGI(TAG, "dome      %.2f deg  (seq=0x%02X)", (double)h.degrees, r.seq);
        return;
    }
    if (r2_ops_parse_version(&r, &v) == R2_OPS_OK) {
        s_rsp_version++;
        s_version = v;
        s_version_seen = true;
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

static void ops_task(void *arg)
{
    (void)arg;
    int tick = 0;
    bool probed_version = false;
    bool ran_gauntlet   = false;

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
            r2_ops_request_battery(next_seq(), r2_link_send, NULL);
        }

        /* The dome, every 30 s. READ ONLY -- this asks where he is looking, it
         * does not turn him. */
        if (tick % 10 == 3) {
            s_req_head++;
            r2_ops_request_head(next_seq(), r2_link_send, NULL);
        }

        /* The version probe ONCE, ten seconds in. It has never been sent by
         * this project and the likely answer is bad_command_id. Sending it in
         * a loop would just repeat one unknown; sending it once answers it. */
        if (!probed_version && tick == 1) {
            probed_version = true;
            s_req_version++;
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
                r2_ops_request_battery(k_gauntlet[i], r2_link_send, NULL);
                vTaskDelay(pdMS_TO_TICKS(400));   /* let each reply land */
            }
            vTaskDelay(pdMS_TO_TICKS(1500));
            s_in_gauntlet = false;
            ESP_LOGW(TAG, "ESCAPE GAUNTLET: %"PRIu32"/%"PRIu32" answered",
                     s_gauntlet_ok, s_gauntlet_sent);
        }

        if (++tick % 10 == 0) report();
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

    const r2_link_cbs_t cbs = { .on_state = on_state, .on_frame = on_frame, .ctx = NULL };
    r2_link_init(&cbs);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);

    xTaskCreate(ops_task, "r2_ops", 4096, NULL, 4, NULL);
}
