#include "coex_stats.h"

#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "COEX";

static const char *s_arm         = "?";
static int64_t     s_t0          = 0;
static int64_t     s_last_ack_us = 0;   /* last SUCCESSFUL keepalive */
static int64_t     s_max_gap_us  = 0;
static int64_t     s_gap_at_us   = 0;   /* when the worst gap ended */
static uint32_t    s_sent        = 0;
static uint32_t    s_acked       = 0;
static uint32_t    s_failed      = 0;
static uint32_t    s_disconnects = 0;
static uint32_t    s_connects    = 0;
static bool        s_linked      = false;

/* The pre-declared threshold from #103: three missed keepalives at 3 s. */
#define GAP_LIMIT_US (10 * 1000000LL)

static int64_t now_us(void) { return esp_timer_get_time(); }

void coex_stats_init(const char *arm_name)
{
    s_arm = arm_name;
    s_t0 = now_us();
    /* Deliberately NOT seeded to "now": the gap clock starts at the first
     * successful keepalive, so time spent connecting is never miscounted as a
     * coexistence stall. */
    s_last_ack_us = 0;
    ESP_LOGI(TAG, "arm=%s  gap limit %.0f s  (PASS needs 0 disconnects and no gap over it)",
             s_arm, (double)GAP_LIMIT_US / 1e6);
}

void coex_note_connected(void)
{
    s_connects++;
    s_linked = true;
    ESP_LOGI(TAG, "LINK UP   (connect #%" PRIu32 ", t+%.1fs)",
             s_connects, (double)(now_us() - s_t0) / 1e6);
}

void coex_note_disconnected(int reason)
{
    s_disconnects++;
    s_linked = false;
    /* A disconnect is an automatic FAIL in arms 1-2 and in arm 3. Say so at
     * the moment it happens, so it can never be lost in a summary later. */
    ESP_LOGE(TAG, "LINK DOWN reason=%d  (disconnect #%" PRIu32 ", t+%.1fs)  *** FAIL CONDITION ***",
             reason, s_disconnects, (double)(now_us() - s_t0) / 1e6);
}

void coex_note_keepalive_sent(uint8_t seq)
{
    (void)seq;
    s_sent++;
}

void coex_note_keepalive_ack(uint8_t seq, int status)
{
    const int64_t t = now_us();
    if (status != 0) {
        s_failed++;
        ESP_LOGW(TAG, "keepalive seq=%u FAILED status=%d  (t+%.1fs)",
                 seq, status, (double)(t - s_t0) / 1e6);
        return;   /* a failure does not close a gap -- that is the point */
    }
    s_acked++;
    if (s_last_ack_us != 0) {
        const int64_t gap = t - s_last_ack_us;
        if (gap > s_max_gap_us) {
            s_max_gap_us = gap;
            s_gap_at_us  = t;
            if (gap > GAP_LIMIT_US) {
                ESP_LOGE(TAG, "GAP %.1fs EXCEEDS THE %.0fs LIMIT (t+%.1fs) *** FAIL CONDITION ***",
                         (double)gap / 1e6, (double)GAP_LIMIT_US / 1e6,
                         (double)(t - s_t0) / 1e6);
            }
        }
    }
    s_last_ack_us = t;
}

void coex_stats_report(void)
{
    const int64_t t   = now_us();
    const double elapsed = (double)(t - s_t0) / 1e6;
    /* An open gap counts. Without this a link that died five minutes ago still
     * reports its last CLOSED gap and looks healthy. */
    const int64_t open_gap = (s_last_ack_us != 0) ? (t - s_last_ack_us) : 0;
    const int64_t worst    = (open_gap > s_max_gap_us) ? open_gap : s_max_gap_us;

    ESP_LOGI(TAG,
             "[%s] t+%.0fs  link=%s  sent=%" PRIu32 " ack=%" PRIu32 " fail=%" PRIu32
             "  disconnects=%" PRIu32 "  worst gap=%.1fs%s",
             s_arm, elapsed, s_linked ? "UP" : "DOWN",
             s_sent, s_acked, s_failed, s_disconnects,
             (double)worst / 1e6,
             (worst > GAP_LIMIT_US || s_disconnects > 0) ? "   <-- FAIL CONDITION MET" : "");
    (void)s_gap_at_us;
}
