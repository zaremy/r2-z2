#include "r2_uuids.h"
#include "coex_stats.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "R2D2";

/* Sequence counter for our own packets; upstream kept this in the bridge. */
static uint8_t g_sphero_seq = 0;
volatile bool g_r2d2_connected = false;

// R2-D2 connection state
// freer2 characteristic roles:
//   g_r2d2_handle_chr  — HANDLE_CHAR  (00020002-...) — subscribe for notifications only
//   g_r2d2_connect_chr — CONNECT_CHAR (00020005-...) — write magic "usetheforce...band"
//   g_r2d2_cmd_chr     — MAIN_CHAR    (00010002-...) — send commands + receive responses
static uint16_t  g_r2d2_conn_handle    = BLE_HS_CONN_HANDLE_NONE;
static uint16_t  g_r2d2_handle_chr     = 0;
static uint16_t  g_r2d2_connect_chr    = 0;
static uint16_t  g_r2d2_cmd_chr        = 0;
static bool      g_r2d2_ready          = false;
static bool      g_handshake_sent      = false;

// ─── UUIDs ───────────────────────────────────────────────────────────────────
static const ble_uuid128_t r2d2_main_svc_uuid = {
    .u = { .type = BLE_UUID_TYPE_128 },
    .value = R2D2_MAIN_SVC,
};
static const ble_uuid128_t r2d2_cmd_chr_uuid = {
    .u = { .type = BLE_UUID_TYPE_128 },
    .value = R2D2_CMD_CHR,
};
static const ble_uuid128_t r2d2_connect_svc_uuid = {
    .u = { .type = BLE_UUID_TYPE_128 },
    .value = R2D2_CONNECT_SVC,
};
static const ble_uuid128_t r2d2_handle_chr_uuid = {
    .u = { .type = BLE_UUID_TYPE_128 },
    .value = R2D2_HANDLE_CHR,
};
static const ble_uuid128_t r2d2_connect_chr_uuid = {
    .u = { .type = BLE_UUID_TYPE_128 },
    .value = R2D2_CONNECT_CHR,
};

// ─── Forward decls ───────────────────────────────────────────────────────────
static void r2d2_send_wake(void);
static int  r2d2_keepalive_write_cb(uint16_t conn_handle,
                                   const struct ble_gatt_error *error,
                                   struct ble_gatt_attr *attr, void *arg);
static int  r2d2_magic_write_cb(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 struct ble_gatt_attr *attr, void *arg);
static int  r2d2_svc_disc_cb(uint16_t conn_handle,
                               const struct ble_gatt_error *error,
                               const struct ble_gatt_svc *svc, void *arg);
static void r2d2_disc_next_svc_chrs(uint16_t conn_handle);
void r2d2_start_scan(void);

// ─── Sequential characteristic discovery state ───────────────────────────────
#define MAX_PENDING_SVCS 4
static struct { uint16_t start; uint16_t end; } s_pending_svcs[MAX_PENDING_SVCS];
static int s_pending_svc_count = 0;
static int s_pending_svc_idx   = 0;

static void r2d2_disc_state_reset(void)
{
    s_pending_svc_count = 0;
    s_pending_svc_idx   = 0;
}

// ─── RX packet reassembly ─────────────────────────────────────────────────────
// R2-D2 streams response bytes as individual 1-byte ATT notifications.
// Accumulate until EOP (0xD8) then log the complete packet.
#define R2D2_RX_BUF 64
static uint8_t s_rx_buf[R2D2_RX_BUF];
static uint8_t s_rx_len = 0;

static void r2d2_rx_reset(void) { s_rx_len = 0; }

static void r2d2_rx_push(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        // Resync on SOP if idle
        if (s_rx_len == 0 && b != 0x8D) continue;
        if (s_rx_len < R2D2_RX_BUF)
            s_rx_buf[s_rx_len++] = b;
        if (b == 0xD8) {
            // Complete packet — log it
            char hex[R2D2_RX_BUF * 3 + 1] = {0};
            for (uint8_t j = 0; j < s_rx_len; j++)
                snprintf(hex + j * 3, 4, "%02X ", s_rx_buf[j]);
            ESP_LOGI(TAG, "R2D2 rx (%d bytes): %s", s_rx_len, hex);
            s_rx_len = 0;
        }
    }
}

// Magic write callback — fires when R2-D2 ATT-ACKs the "usetheforce...band" write.
// R2-D2 only exposes MAIN_SERVICE after this write, so kick off a second
// service discovery here to find CMD_CHAR.
static int r2d2_magic_write_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGE(TAG, "Magic write rejected: status=%d", error->status);
        return 0;
    }
    ESP_LOGI(TAG, "Magic write ACKed — discovering MAIN_SERVICE");
    r2d2_disc_state_reset();
    ble_gattc_disc_all_svcs(conn_handle, r2d2_svc_disc_cb, NULL);
    return 0;
}

// Called after each phase of characteristic discovery finishes.
//
// Phase 1 (g_handshake_sent == false): CONNECT_SVC chars found.
//   → write magic to CONNECT_CHR; notify translator of connection.
//
// Phase 2 (g_handshake_sent == true): MAIN_SVC chars found.
//   → subscribe CMD_CHR CCCD, set ready, send wake.
static void r2d2_discovery_done(uint16_t conn_handle)
{
    if (!g_handshake_sent) {
        // ── Phase 1: write magic ─────────────────────────────────────────────
        ESP_LOGI(TAG, "Phase 1 done (handle=%d connect=%d)",
                 g_r2d2_handle_chr, g_r2d2_connect_chr);
        if (!g_r2d2_connect_chr) {
            ESP_LOGW(TAG, "CONNECT_CHR not found");
            return;
        }
        g_handshake_sent = true;
        const char *magic = R2D2_MAGIC;
        int rc = ble_gattc_write_flat(conn_handle, g_r2d2_connect_chr,
                                      magic, strlen(magic),
                                      r2d2_magic_write_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gattc_write_flat rc=%d", rc);
            return;
        }
        ESP_LOGI(TAG, "Magic write initiated: '%s'", magic);
    } else {
        // ── Phase 2: MAIN_SVC found after magic write ────────────────────────
        ESP_LOGI(TAG, "Phase 2 done (cmd=%d)", g_r2d2_cmd_chr);
        if (!g_r2d2_cmd_chr) {
            ESP_LOGW(TAG, "CMD_CHR not found after magic write");
            return;
        }
        g_r2d2_ready     = true;
        g_r2d2_connected = true;
        coex_note_connected();
        r2d2_send_wake();
    }
}

// ─── GATT characteristic discovery callback ───────────────────────────────────
static int r2d2_chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status != 0) {
        if (error->status == BLE_HS_EDONE) {
            s_pending_svc_idx++;
            r2d2_disc_next_svc_chrs(conn_handle);
        } else {
            ESP_LOGW(TAG, "chr disc error: %d", error->status);
            s_pending_svc_idx++;
            r2d2_disc_next_svc_chrs(conn_handle);
        }
        return 0;
    }

    if (ble_uuid_cmp(&chr->uuid.u, &r2d2_handle_chr_uuid.u) == 0) {
        g_r2d2_handle_chr = chr->val_handle;
        ESP_LOGI(TAG, "Handle chr found (handle=%d) — subscribing notifications",
                 g_r2d2_handle_chr);
        uint16_t cccd_handle = chr->val_handle + 1;
        uint8_t  cccd_val[]  = { 0x01, 0x00 };  // notify
        ble_gattc_write_flat(conn_handle, cccd_handle,
                              cccd_val, sizeof(cccd_val), NULL, NULL);
    }

    if (ble_uuid_cmp(&chr->uuid.u, &r2d2_connect_chr_uuid.u) == 0) {
        g_r2d2_connect_chr = chr->val_handle;
        ESP_LOGI(TAG, "Connect chr found (handle=%d) — magic string goes here",
                 g_r2d2_connect_chr);
    }

    if (ble_uuid_cmp(&chr->uuid.u, &r2d2_cmd_chr_uuid.u) == 0) {
        g_r2d2_cmd_chr = chr->val_handle;
        ESP_LOGI(TAG, "Cmd chr found (handle=%d) — subscribing + sending commands here",
                 g_r2d2_cmd_chr);
        uint16_t cccd_handle = chr->val_handle + 1;
        uint8_t  cccd_val[]  = { 0x01, 0x00 };  // notify
        ble_gattc_write_flat(conn_handle, cccd_handle,
                              cccd_val, sizeof(cccd_val), NULL, NULL);
        ESP_LOGI(TAG, "Subscribed to cmd notifications (CCCD handle=%d)", cccd_handle);
    }

    return 0;
}

// Start discovering chrs for the next pending service, or finalise if done.
static void r2d2_disc_next_svc_chrs(uint16_t conn_handle)
{
    if (s_pending_svc_idx >= s_pending_svc_count) {
        r2d2_discovery_done(conn_handle);
        return;
    }
    uint16_t sh = s_pending_svcs[s_pending_svc_idx].start;
    uint16_t eh = s_pending_svcs[s_pending_svc_idx].end;
    ESP_LOGI(TAG, "Discovering chrs for svc [%d/%d] handles %d-%d",
             s_pending_svc_idx + 1, s_pending_svc_count, sh, eh);
    ble_gattc_disc_all_chrs(conn_handle, sh, eh, r2d2_chr_disc_cb, NULL);
}

// ─── GATT service discovery callback ─────────────────────────────────────────
static int r2d2_svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status != 0) {
        if (error->status == BLE_HS_EDONE) {
            s_pending_svc_idx = 0;
            r2d2_disc_next_svc_chrs(conn_handle);
        } else {
            ESP_LOGW(TAG, "svc disc error: %d", error->status);
        }
        return 0;
    }

    // Phase 1: look only for CONNECT_SVC (before magic write)
    // Phase 2: look only for MAIN_SVC   (after magic write — R2D2 exposes it then)
    bool match = g_handshake_sent
        ? (ble_uuid_cmp(&svc->uuid.u, &r2d2_main_svc_uuid.u) == 0)
        : (ble_uuid_cmp(&svc->uuid.u, &r2d2_connect_svc_uuid.u) == 0);

    if (match) {
        char uuid_str[BLE_UUID_STR_LEN];
        ble_uuid_to_str(&svc->uuid.u, uuid_str);
        ESP_LOGI(TAG, "Queuing svc for chr discovery: %s (handles %d-%d)",
                 uuid_str, svc->start_handle, svc->end_handle);
        if (s_pending_svc_count < MAX_PENDING_SVCS) {
            s_pending_svcs[s_pending_svc_count].start = svc->start_handle;
            s_pending_svcs[s_pending_svc_count].end   = svc->end_handle;
            s_pending_svc_count++;
        }
    }
    return 0;
}

// ─── Wake command (Sphero V2 packet, no data_len byte) ───────────────────────
// Format: SOP FLAGS DID CID SEQ [payload] CHK EOP
static void r2d2_send_wake(void)
{
    uint8_t seq = g_sphero_seq++;
    uint8_t chk = (~(0x0A + 0x13 + 0x0D + seq)) & 0xFF;
    uint8_t pkt[] = { 0x8D, 0x0A, 0x13, 0x0D, seq, chk, 0xD8 };
    ble_gattc_write_no_rsp_flat(g_r2d2_conn_handle, g_r2d2_cmd_chr,
                                 pkt, sizeof(pkt));
    ESP_LOGI(TAG, "Wake command sent (seq=%d)", seq);
}

// ─── GAP event handler (central role) ────────────────────────────────────────
static int r2d2_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_gap_disc_desc *desc = &event->disc;
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) != 0)
            break;
        if (fields.name == NULL) break;

        char name[32] = {0};
        uint8_t name_len = fields.name_len < 31 ? fields.name_len : 31;
        memcpy(name, fields.name, name_len);

        if (strncmp(name, R2D2_NAME, strlen(R2D2_NAME)) == 0) {
            ESP_LOGI(TAG, "Found R2-D2: %s", name);
            ble_gap_disc_cancel();

            struct ble_gap_conn_params conn_params = {
                .scan_itvl     = 0x0010,
                .scan_window   = 0x0010,
                .itvl_min      = BLE_GAP_INITIAL_CONN_ITVL_MIN,
                .itvl_max      = BLE_GAP_INITIAL_CONN_ITVL_MAX,
                .latency       = 0,
                .supervision_timeout = 0x0100,
                .min_ce_len    = 0,
                .max_ce_len    = 0,
            };
            ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &desc->addr,
                            5000, &conn_params, r2d2_gap_event, NULL);
        }
        break;
    }

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "Connected to R2-D2 (handle=%d)",
                     event->connect.conn_handle);
            g_r2d2_conn_handle = event->connect.conn_handle;
            r2d2_disc_state_reset();
            ble_gattc_disc_all_svcs(g_r2d2_conn_handle, r2d2_svc_disc_cb, NULL);
        } else {
            ESP_LOGW(TAG, "R2-D2 connect failed (%d), retrying scan",
                     event->connect.status);
            r2d2_start_scan();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "R2-D2 disconnected (reason=%d)", event->disconnect.reason);
        g_r2d2_conn_handle  = BLE_HS_CONN_HANDLE_NONE;
        g_r2d2_handle_chr   = 0;
        g_r2d2_connect_chr  = 0;
        g_r2d2_cmd_chr      = 0;
        g_r2d2_ready        = false;
        g_r2d2_connected    = false;
        g_handshake_sent    = false;
        r2d2_disc_state_reset();
        r2d2_rx_reset();
        coex_note_disconnected(event->disconnect.reason);
        r2d2_start_scan();
        break;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        struct os_mbuf *om = event->notify_rx.om;
        uint16_t pkt_len   = OS_MBUF_PKTLEN(om);
        if (pkt_len > 0 && pkt_len <= R2D2_RX_BUF) {
            uint8_t buf[R2D2_RX_BUF];
            os_mbuf_copydata(om, 0, pkt_len, buf);
            r2d2_rx_push(buf, pkt_len);
        }
        break;
    }

    default:
        break;
    }
    return 0;
}

// ─── Start BLE scan for R2-D2 ────────────────────────────────────────────────
void r2d2_start_scan(void)
{
    struct ble_gap_disc_params disc_params = {
        .itvl              = 0x0040,
        .window            = 0x0020,
        .filter_policy     = 0,
        .limited           = 0,
        .passive           = 0,
        .filter_duplicates = 1,
    };

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER,
                          &disc_params, r2d2_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "disc error: %d", rc);
    } else {
        ESP_LOGI(TAG, "Scanning for R2-D2 (name prefix '%s')...", R2D2_NAME);
    }
}

static int r2d2_keepalive_write_cb(uint16_t conn_handle,
                                   const struct ble_gatt_error *error,
                                   struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)attr;
    coex_note_keepalive_ack((uint8_t)(uintptr_t)arg, error ? error->status : 0);
    return 0;
}

// ─── Keepalive task ───────────────────────────────────────────────────────────
static void r2d2_keepalive_task(void *arg)
{
    int battery_tick = 0;
    // Re-send wake (DID=0x13 CID=0x0D) — idempotent, resets R2D2 inactivity timer.
    // CID=0x01 is SLEEP and must NOT be used as a keepalive.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (g_r2d2_ready && g_r2d2_cmd_chr != 0) {
            uint8_t seq = g_sphero_seq++;
            uint8_t chk = (~(SPHERO_FLAGS + DID_POWER + CID_POWER_WAKE + seq)) & 0xFF;
            uint8_t ping[] = { SPHERO_SOP, SPHERO_FLAGS, DID_POWER,
                               CID_POWER_WAKE, seq, chk, SPHERO_EOP };
            coex_note_keepalive_sent(seq);
            /* WITH response, unlike upstream's write_no_rsp_flat. A keepalive
             * that cannot report failure measures nothing, and failure is the
             * entire point of this experiment. The ATT ACK is the link-health
             * signal; its absence is what a coexistence stall looks like. */
            int rc = ble_gattc_write_flat(g_r2d2_conn_handle, g_r2d2_cmd_chr,
                                          ping, sizeof(ping),
                                          r2d2_keepalive_write_cb,
                                          (void *)(uintptr_t)seq);
            if (rc != 0) {
                /* Could not even be queued -- count it now, since no callback
                 * will ever arrive to do it. */
                coex_note_keepalive_ack(seq, rc);
            }
        } else {
            coex_note_keepalive_sent(0xFF);
            coex_note_keepalive_ack(0xFF, -1);   /* link down: still a gap */
        }

        /* Every 20th cycle (~60 s), ask R2 for his battery voltage.
         *
         * This exists to protect the VERDICT, not to measure the droid. Our
         * keepalive is his wake command, so an arm holds him awake for its
         * whole hour; across three arms that is most of a morning. A sagging
         * battery would drop the link, and a dropped link is scored as a
         * disconnect -- i.e. FAIL, i.e. "D-005 is revisited". Logging the
         * voltage means a power-caused disconnect can be told apart from a
         * coexistence-caused one after the fact, instead of being argued about.
         *
         * DID 0x13 / CID 0x03 is a READ of a voltage. It cannot move him. */
        if (++battery_tick >= 20) {
            battery_tick = 0;
            if (g_r2d2_ready && g_r2d2_cmd_chr != 0) {
                uint8_t seq = g_sphero_seq++;
                uint8_t chk = (~(SPHERO_FLAGS + DID_POWER + 0x03 + seq)) & 0xFF;
                uint8_t req[] = { SPHERO_SOP, SPHERO_FLAGS, DID_POWER,
                                  0x03, seq, chk, SPHERO_EOP };
                ble_gattc_write_flat(g_r2d2_conn_handle, g_r2d2_cmd_chr,
                                     req, sizeof(req), NULL, NULL);
                ESP_LOGI(TAG, "battery voltage requested (seq=%u)", seq);
            }
        }
    }
}

// ─── Module init ──────────────────────────────────────────────────────────────
void r2d2_central_init(void)
{
    xTaskCreate(r2d2_keepalive_task, "r2d2_keepalive", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "R2-D2 central module ready");
}
