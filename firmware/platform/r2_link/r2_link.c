#include "sdkconfig.h"

/* r2_link is NimBLE glue, and it lives in the shared platform/ component
 * directory that every firmware here adds via EXTRA_COMPONENT_DIRS --
 * including display-only diagnostics with no radio. Compiling unconditionally
 * broke touch_check outright ("fatal error: host/ble_gap.h") for an app that
 * never asked for BLE.
 *
 * So the whole translation unit is empty without NimBLE. An app that actually
 * CALLS r2_link then fails at link time naming the missing symbols, which
 * points at the missing config, rather than failing to compile a component it
 * does not use. */
#if CONFIG_BT_NIMBLE_ENABLED

/* NimBLE glue for the R2 link. See r2_link.h for why this slice exists.
 *
 * Adapted from firmware/coex_check/main/r2_central.c, which held R2 for three
 * separate hours with zero disconnects (#103 A1) -- so the connect sequence
 * below is not new code, it is measured code with the packet building taken
 * out of it and handed to r2_packet.
 *
 * Deliberately NOT kept from the original:
 *   - hand-built, unescaped packets (see the header, and D-024)
 *   - the ad-hoc byte reassembler; r2_stream_feed does that and is fuzzed
 *   - coex_stats coupling; callers get callbacks instead
 */
#include "r2_link.h"
#include "r2_packet.h"
#include "r2_uuids.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"

#include <string.h>

static const char *TAG = "r2_link";

static r2_link_cbs_t   s_cbs;
static r2_link_state_t s_state = R2_LINK_DOWN;
static uint32_t        s_sent = 0, s_dropped = 0;

static uint16_t s_conn      = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_cmd_chr   = 0;
static uint16_t s_connect_chr = 0;
static bool     s_magic_written = false;

static r2_stream_t s_rx;
static bool s_scan_only;
static uint32_t s_adverts, s_last_advert_ms;

static const ble_uuid128_t k_main_svc    = { .u = {.type=BLE_UUID_TYPE_128}, .value = R2D2_MAIN_SVC };
static const ble_uuid128_t k_cmd_chr     = { .u = {.type=BLE_UUID_TYPE_128}, .value = R2D2_CMD_CHR };
static const ble_uuid128_t k_connect_svc = { .u = {.type=BLE_UUID_TYPE_128}, .value = R2D2_CONNECT_SVC };
static const ble_uuid128_t k_handle_chr  = { .u = {.type=BLE_UUID_TYPE_128}, .value = R2D2_HANDLE_CHR };
static const ble_uuid128_t k_connect_chr = { .u = {.type=BLE_UUID_TYPE_128}, .value = R2D2_CONNECT_CHR };

#define MAX_PENDING_SVCS 4
static struct { uint16_t start, end; } s_pending[MAX_PENDING_SVCS];
static int s_pending_n = 0, s_pending_i = 0;

static int  gap_event(struct ble_gap_event *ev, void *arg);
static int  svc_disc_cb(uint16_t conn, const struct ble_gatt_error *err,
                        const struct ble_gatt_svc *svc, void *arg);
static void disc_next_svc(uint16_t conn);

static void set_state(r2_link_state_t s, int reason)
{
    if (s == s_state) return;
    s_state = s;
    ESP_LOGI(TAG, "state -> %s%s", r2_link_state_name(s),
             (s == R2_LINK_DOWN && reason) ? " (disconnected)" : "");
    if (s_cbs.on_state) s_cbs.on_state(s, reason, s_cbs.ctx);
}

/* r2_stream_feed hands us whole frames; pass them straight up. */
static void frame_cb(const uint8_t *frame, size_t len, void *ctx)
{
    (void)ctx;
    if (s_cbs.on_frame) s_cbs.on_frame(frame, len, s_cbs.ctx);
}

/* ---- discovery ---------------------------------------------------------- */

static int magic_write_cb(uint16_t conn, const struct ble_gatt_error *err,
                          struct ble_gatt_attr *attr, void *arg)
{
    (void)attr; (void)arg;
    if (err->status != 0) {
        ESP_LOGE(TAG, "magic write rejected: status=%d", err->status);
        return 0;
    }
    /* R2 only exposes MAIN_SERVICE after this write, hence a second pass. */
    s_pending_n = s_pending_i = 0;
    ble_gattc_disc_all_svcs(conn, svc_disc_cb, NULL);
    return 0;
}

static void discovery_done(uint16_t conn)
{
    if (!s_magic_written) {
        if (!s_connect_chr) { ESP_LOGW(TAG, "CONNECT_CHR not found"); return; }
        s_magic_written = true;
        const char *magic = R2D2_MAGIC;
        const int rc = ble_gattc_write_flat(conn, s_connect_chr, magic,
                                            strlen(magic), magic_write_cb, NULL);
        if (rc != 0) ESP_LOGE(TAG, "magic write rc=%d", rc);
        return;
    }
    if (!s_cmd_chr) { ESP_LOGW(TAG, "CMD_CHR not found after magic write"); return; }
    r2_stream_reset(&s_rx);
    set_state(R2_LINK_UP, 0);
}

static int chr_disc_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (err->status != 0) {          /* BLE_HS_EDONE or a real error: both advance */
        s_pending_i++;
        disc_next_svc(conn);
        return 0;
    }
    /* The CCCD sits immediately after the value handle on this device. */
    const uint8_t cccd_notify[2] = { 0x01, 0x00 };
    if (ble_uuid_cmp(&chr->uuid.u, &k_handle_chr.u) == 0) {
        ble_gattc_write_flat(conn, chr->val_handle + 1,
                             cccd_notify, sizeof cccd_notify, NULL, NULL);
    } else if (ble_uuid_cmp(&chr->uuid.u, &k_connect_chr.u) == 0) {
        s_connect_chr = chr->val_handle;
    } else if (ble_uuid_cmp(&chr->uuid.u, &k_cmd_chr.u) == 0) {
        s_cmd_chr = chr->val_handle;
        ble_gattc_write_flat(conn, chr->val_handle + 1,
                             cccd_notify, sizeof cccd_notify, NULL, NULL);
    }
    return 0;
}

static void disc_next_svc(uint16_t conn)
{
    if (s_pending_i >= s_pending_n) { discovery_done(conn); return; }
    ble_gattc_disc_all_chrs(conn, s_pending[s_pending_i].start,
                            s_pending[s_pending_i].end, chr_disc_cb, NULL);
}

static int svc_disc_cb(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (err->status != 0) {
        if (err->status == BLE_HS_EDONE) { s_pending_i = 0; disc_next_svc(conn); }
        else ESP_LOGW(TAG, "svc disc error: %d", err->status);
        return 0;
    }
    const bool match = s_magic_written
        ? (ble_uuid_cmp(&svc->uuid.u, &k_main_svc.u) == 0)
        : (ble_uuid_cmp(&svc->uuid.u, &k_connect_svc.u) == 0);
    if (match && s_pending_n < MAX_PENDING_SVCS) {
        s_pending[s_pending_n].start = svc->start_handle;
        s_pending[s_pending_n].end   = svc->end_handle;
        s_pending_n++;
    }
    return 0;
}

/* ---- GAP ---------------------------------------------------------------- */

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, ev->disc.data, ev->disc.length_data) != 0) break;
        if (f.name == NULL) break;
        char name[32] = {0};
        const uint8_t n = f.name_len < 31 ? f.name_len : 31;
        memcpy(name, f.name, n);
        if (strncmp(name, R2D2_NAME, strlen(R2D2_NAME)) != 0) break;

        if (s_scan_only) {
            /* Log and keep scanning. RSSI is included because a droid that has
             * moved or been switched off looks the same as one asleep, and the
             * signal level is the only thing here that can tell them apart. */
            s_adverts++;
            s_last_advert_ms = (uint32_t)(esp_timer_get_time() / 1000);
            break;
        }
        ESP_LOGI(TAG, "found %s", name);
        ble_gap_disc_cancel();
        set_state(R2_LINK_CONNECTING, 0);
        struct ble_gap_conn_params cp = {
            .scan_itvl = 0x0010, .scan_window = 0x0010,
            .itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN,
            .itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX,
            .latency = 0, .supervision_timeout = 0x0100,
            .min_ce_len = 0, .max_ce_len = 0,
        };
        ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &ev->disc.addr, 5000, &cp, gap_event, NULL);
        break;
    }
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            set_state(R2_LINK_HANDSHAKING, 0);
            s_pending_n = s_pending_i = 0;
            ble_gattc_disc_all_svcs(s_conn, svc_disc_cb, NULL);
        } else {
            ESP_LOGW(TAG, "connect failed (%d), rescanning", ev->connect.status);
            r2_link_start();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_cmd_chr = s_connect_chr = 0;
        s_magic_written = false;
        s_pending_n = s_pending_i = 0;
        r2_stream_reset(&s_rx);
        set_state(R2_LINK_DOWN, ev->disconnect.reason);
        r2_link_start();
        break;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        struct os_mbuf *om = ev->notify_rx.om;
        const uint16_t len = OS_MBUF_PKTLEN(om);
        /* R2 streams responses one byte per notification, so this is normally
         * len == 1. The stream reassembler does not care either way. */
        uint8_t buf[64];
        if (len > 0 && len <= sizeof buf) {
            os_mbuf_copydata(om, 0, len, buf);
            r2_stream_feed(&s_rx, buf, len, frame_cb, NULL);
        }
        break;
    }
    default: break;
    }
    return 0;
}

/* ---- public ------------------------------------------------------------- */

void r2_link_init(const r2_link_cbs_t *cbs)
{
    s_cbs = *cbs;
    r2_stream_reset(&s_rx);
    s_state = R2_LINK_DOWN;
    s_sent = s_dropped = 0;
}

void r2_link_start(void)
{
    struct ble_gap_disc_params dp = {
        .itvl = 0x0040, .window = 0x0020, .filter_policy = 0,
        .limited = 0, .passive = 0,
        /* Duplicate filtering is what makes a normal scan cheap, and it is
         * exactly wrong here: with it on we would see him ONCE and then never
         * again, and his silence at minute 40 would be indistinguishable from
         * the radio having stopped reporting a device it already knew about. */
        .filter_duplicates = s_scan_only ? 0 : 1,
    };
    /* Cancel any scan already running, so a restart actually applies these
     * parameters. Without this ble_gap_disc returns BLE_HS_EALREADY, which the
     * check below treats as success -- and the OLD scan continues with the OLD
     * settings. That silently defeated P4: scan-only wants duplicate filtering
     * OFF, the running scan had it ON, and once he had been seen once he was
     * suppressed forever. Zero adverts then looks exactly like a sleeping
     * droid, which is the wrong answer in the most convincing direction. */
    ble_gap_disc_cancel();

    const int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &dp, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
        return;
    }
    set_state(R2_LINK_SCANNING, 0);
}

bool            r2_link_is_up(void)  { return s_state == R2_LINK_UP; }
r2_link_state_t r2_link_state(void)  { return s_state; }

const char *r2_link_state_name(r2_link_state_t s)
{
    switch (s) {
    case R2_LINK_DOWN:        return "down";
    case R2_LINK_SCANNING:    return "scanning";
    case R2_LINK_CONNECTING:  return "connecting";
    case R2_LINK_HANDSHAKING: return "handshaking";
    case R2_LINK_UP:          return "up";
    default:                  return "?";
    }
}

int r2_link_send(const uint8_t *frame, size_t len, void *ctx)
{
    (void)ctx;
    if (frame == NULL || len == 0) return -1;
    if (s_state != R2_LINK_UP || s_cmd_chr == 0) { s_dropped++; return -1; }
    /* WITH response. An unacknowledged write cannot report failure, and a
     * transport that cannot fail is indistinguishable from one that works --
     * which is exactly how the escaping bug stayed invisible for a whole
     * endurance run. */
    const int rc = ble_gattc_write_flat(s_conn, s_cmd_chr, frame, len, NULL, NULL);
    if (rc != 0) { s_dropped++; return -1; }
    s_sent++;
    return 0;
}

int r2_link_disconnect(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return -1;
    /* No Sphero packet is sent. We stop talking; he does the rest. */
    return ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

void r2_link_set_scan_only(bool on) { s_scan_only = on; }

void r2_link_adverts(uint32_t *count, uint32_t *last_ms)
{
    if (count)   *count   = s_adverts;
    if (last_ms) *last_ms = s_last_advert_ms;
}

void r2_link_stats(uint32_t *sent, uint32_t *dropped)
{
    if (sent)    *sent    = s_sent;
    if (dropped) *dropped = s_dropped;
}

#else  /* !CONFIG_BT_NIMBLE_ENABLED */

/* ISO C forbids an empty translation unit. */
typedef int r2_link_needs_nimble_t;

#endif
