#include "panel_service.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* The menu, in order. Uppercase only: the panel's fonts carry 0x20-0x5F and
 * nothing else, so a lowercase letter anywhere here would render as a gap. */
static const struct {
    const char       *title;
    panel_svc_kind_t  kind;
} k_svc[PANEL_SVC_COUNT] = {
    [PANEL_SVC_NETWORK]      = { "NETWORK",       PANEL_SVC_JUMP   },
    [PANEL_SVC_R2_LINK]      = { "R2 LINK",       PANEL_SVC_LIST   },
    [PANEL_SVC_DIAGNOSTICS]  = { "DIAGNOSTICS",   PANEL_SVC_LIST   },
    [PANEL_SVC_HW_TEST]      = { "HARDWARE TEST", PANEL_SVC_LADDER },
    [PANEL_SVC_PROVISIONING] = { "PROVISIONING",  PANEL_SVC_LIST   },
    [PANEL_SVC_VOICE]        = { "VOICE",         PANEL_SVC_NOTE   },
    [PANEL_SVC_CAMERA]       = { "CAMERA",        PANEL_SVC_NOTE   },
    [PANEL_SVC_ABOUT]        = { "ABOUT",         PANEL_SVC_LIST   },
};

_Static_assert(PANEL_SVC_COUNT == 8,
               "a SERVICE row was added or removed: give it a title and a kind");

static bool in_range(panel_svc_t s) { return (unsigned)s < (unsigned)PANEL_SVC_COUNT; }

const char *panel_service_title(panel_svc_t s)
{
    return in_range(s) ? k_svc[s].title : "";
}

panel_svc_kind_t panel_service_kind(panel_svc_t s)
{
    /* Out of range is a NOTE with no lines rather than a LIST, so a bad index
     * can never be asked for rows. */
    return in_range(s) ? k_svc[s].kind : PANEL_SVC_NOTE;
}

/* ---- rows ----------------------------------------------------------------- */

static void put(panel_kv_t *out, int *n, int max, const char *key,
                panel_tone_t tone, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));

#include <stdarg.h>
static void put(panel_kv_t *out, int *n, int max, const char *key,
                panel_tone_t tone, const char *fmt, ...)
{
    if (*n >= max) return;
    panel_kv_t *kv = &out[*n];
    kv->key = key;
    kv->tone = tone;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(kv->val, sizeof kv->val, fmt, ap);
    va_end(ap);
    /* Uppercase whatever arrived -- a git-describe version is lowercase hex,
     * and this font would draw it as blanks. */
    for (char *c = kv->val; *c; c++) *c = (char)toupper((unsigned char)*c);
    (*n)++;
}

#define NONE_VAL "----"     /* the face's own "no reading" mark */

static void fmt_age(char *buf, size_t n, uint32_t ms)
{
    const uint32_t s = ms / 1000u;
    if (s < 60u)         snprintf(buf, n, "%uS AGO", (unsigned)s);
    else if (s < 3600u)  snprintf(buf, n, "%uM AGO", (unsigned)(s / 60u));
    else                 snprintf(buf, n, "%uH AGO", (unsigned)(s / 3600u));
}

static void fmt_uptime(char *buf, size_t n, uint32_t ms)
{
    const uint32_t s = ms / 1000u;
    if (s < 3600u)
        snprintf(buf, n, "%uM %02uS", (unsigned)(s / 60u), (unsigned)(s % 60u));
    else if (s < 100u * 3600u)
        snprintf(buf, n, "%uH %02uM", (unsigned)(s / 3600u),
                 (unsigned)((s / 60u) % 60u));
    else
        snprintf(buf, n, "%uD %02uH", (unsigned)(s / 86400u),
                 (unsigned)((s / 3600u) % 24u));
}

/* A reading R2 gave us, if it is still displayable. EVERYTHING in this file
 * that came from R2 goes through here, which is how AC7 holds: the link
 * check lives in r2_telemetry_displayable, once, and there is no second copy
 * of it here to drift. `max_age` is the caller's staleness budget. */
static bool r2_says(const r2_telemetry_t *tm, const r2_tm_stamp_t *s,
                    uint32_t now, uint32_t max_age)
{
    return tm != NULL && r2_telemetry_displayable(tm, s, now, max_age);
}

static int rows_r2_link(const panel_svc_facts_t *f, panel_kv_t *out, int max)
{
    int n = 0;
    const r2_telemetry_t *tm = f->tm;
    const bool up = tm != NULL && tm->link == R2_TM_UP;

    if (up) {
        put(out, &n, max, "STATE", PANEL_TONE_GOOD, "LINKED");
    } else {
        put(out, &n, max, "STATE", PANEL_TONE_WARN, "%s",
            tm ? r2_telemetry_link_name(tm->link) : "down");
    }

    put(out, &n, max, "CEILING", PANEL_TONE_PLAIN, "%s",
        panel_service_ceiling_name(f->ceiling)[0]
            ? panel_service_ceiling_name(f->ceiling) : NONE_VAL);

    /* Session accounting, not a reading: it describes what WE did, so it
     * survives the link -- the same rule r2_telemetry applies to it. */
    if (tm)
        put(out, &n, max, "ANSWERED", PANEL_TONE_PLAIN, "%u/%u",
            (unsigned)tm->responses, (unsigned)tm->requests);
    else
        put(out, &n, max, "ANSWERED", PANEL_TONE_NONE, NONE_VAL);

    if (up)
        put(out, &n, max, "KEEPALIVE", PANEL_TONE_PLAIN, "%uS",
            (unsigned)(f->keepalive_ms / 1000u));
    else
        put(out, &n, max, "KEEPALIVE", PANEL_TONE_NONE, NONE_VAL);

    /* The age of the newest READING -- battery, dome or version -- that is
     * still displayable. Named for what it measures: keepalive replies are
     * not timestamped, so on a healthy link this is up to the 15 s battery
     * period, and "heard" would have claimed more than it knows. */
    uint32_t newest = UINT32_MAX;
    const r2_tm_stamp_t *st[3] = { tm ? &tm->battery : NULL,
                                   tm ? &tm->dome : NULL,
                                   tm ? &tm->version : NULL };
    for (int i = 0; i < 3; i++) {
        uint32_t age;
        if (st[i] && r2_says(tm, st[i], f->now_ms, UINT32_MAX) &&
            r2_telemetry_age_ms(st[i], f->now_ms, &age) && age < newest)
            newest = age;
    }
    if (newest != UINT32_MAX) {
        char buf[PANEL_SVC_VAL_LEN];
        fmt_age(buf, sizeof buf, newest);
        put(out, &n, max, "LAST READ", PANEL_TONE_PLAIN, "%s", buf);
    } else {
        put(out, &n, max, "LAST READ", PANEL_TONE_NONE, NONE_VAL);
    }

    if (r2_says(tm, tm ? &tm->version : NULL, f->now_ms, UINT32_MAX))
        put(out, &n, max, "R2 FW", PANEL_TONE_PLAIN, "%u.%u.%u",
            tm->version_major, tm->version_minor, tm->version_revision);
    else
        put(out, &n, max, "R2 FW", PANEL_TONE_NONE, NONE_VAL);
    return n;
}

/* V2 IS PROBED, NOT ASSUMED. CLAUDE.md requires the board revision to be
 * verified at runtime, and D-005 Amendment A records that the V2-only BSP
 * drives a V1 board's panel with the CO5300 init anyway -- so "the firmware
 * is built for V2" is not evidence the board is V2. What is: a controller
 * answering at I2C 0x15 (board-revision.md's probe), which panel_touch reads
 * at boot. No answer and the board, panel and touch rows all say they could
 * not verify rather than repeating what the build assumes. */
static bool board_is_v2(const panel_svc_facts_t *f) { return f->touch_id != 0; }

static void put_board(panel_kv_t *out, int *n, int max, const panel_svc_facts_t *f)
{
    if (board_is_v2(f)) put(out, n, max, "BOARD", PANEL_TONE_PLAIN, "S3 V2");
    else                put(out, n, max, "BOARD", PANEL_TONE_WARN, "UNVERIFIED");
}

static int rows_diagnostics(const panel_svc_facts_t *f, panel_kv_t *out, int max)
{
    int n = 0;
    put_board(out, &n, max, f);
    if (board_is_v2(f)) put(out, &n, max, "PANEL", PANEL_TONE_PLAIN, "CO5300");
    else                put(out, &n, max, "PANEL", PANEL_TONE_WARN, "UNVERIFIED");
    /* 0xB7 is the CST820, the part #104 read on this unit. Anything else at
     * 0x15 is reported by its id rather than renamed to what we expected. */
    if (f->touch_id == 0xB7)
        put(out, &n, max, "TOUCH", PANEL_TONE_PLAIN, "CST820");
    else if (f->touch_id == 0)
        put(out, &n, max, "TOUCH", PANEL_TONE_WARN, "NO ANSWER");
    else
        put(out, &n, max, "TOUCH", PANEL_TONE_WARN, "ID 0X%02X", (unsigned)f->touch_id);
    put(out, &n, max, "FLASH", PANEL_TONE_PLAIN, "%u MB", (unsigned)f->flash_mb);
    if (f->psram_mb)
        put(out, &n, max, "PSRAM", PANEL_TONE_PLAIN, "%u MB", (unsigned)f->psram_mb);
    else
        put(out, &n, max, "PSRAM", PANEL_TONE_WARN, "NONE FOUND");
    put(out, &n, max, "FREE HEAP", PANEL_TONE_PLAIN, "%u KB", (unsigned)f->heap_kb);
    return n;
}

static int rows_provisioning(const panel_svc_facts_t *f, panel_kv_t *out, int max)
{
    (void)f;
    int n = 0;
    /* This build has no Wi-Fi stack and no LLM client, so it holds no SSID
     * and no key. The reference's "HOMENET" / "SET" would be the panel
     * vouching for provisioning that does not exist on this board. */
    put(out, &n, max, "WI-FI",   PANEL_TONE_NONE,  "NOT IN BUILD");
    put(out, &n, max, "API KEY", PANEL_TONE_NONE,  "NOT IN BUILD");
    /* What the link will pair with: r2_link matches any advertiser whose name
     * begins "D2-" and takes the first. Worth knowing in a house with two
     * droids. Written as a glob because "FIRST D2-" read like a string that
     * had been cut off -- seen on the glass, 2026-09-14. */
    put(out, &n, max, "PAIRS WITH", PANEL_TONE_PLAIN, "ANY D2-*");
    /* OK BY CONSTRUCTION, and said so: main.c ESP_ERROR_CHECKs nvs_flash_init,
     * so a panel drawing this row is one whose NVS came up. */
    put(out, &n, max, "NVS", PANEL_TONE_GOOD, "OK");
    return n;
}

static int rows_about(const panel_svc_facts_t *f, panel_kv_t *out, int max)
{
    int n = 0;
    char up[PANEL_SVC_VAL_LEN];
    fmt_uptime(up, sizeof up, f->now_ms);
    put(out, &n, max, "MODEL",    PANEL_TONE_PLAIN, "R2Z2");
    put(out, &n, max, "FW",       PANEL_TONE_PLAIN, "%s",
        (f->panel_fw && f->panel_fw[0]) ? f->panel_fw : NONE_VAL);
    put_board(out, &n, max, f);
    put(out, &n, max, "UPTIME",   PANEL_TONE_PLAIN, "%s", up);
    return n;
}

int panel_service_rows(panel_svc_t s, const panel_svc_facts_t *f,
                       panel_kv_t *out, int max)
{
    if (f == NULL || out == NULL || max <= 0) return 0;
    if (panel_service_kind(s) != PANEL_SVC_LIST) return 0;
    switch (s) {
    case PANEL_SVC_R2_LINK:      return rows_r2_link(f, out, max);
    case PANEL_SVC_DIAGNOSTICS:  return rows_diagnostics(f, out, max);
    case PANEL_SVC_PROVISIONING: return rows_provisioning(f, out, max);
    case PANEL_SVC_ABOUT:        return rows_about(f, out, max);
    default:                     return 0;
    }
}

bool panel_service_note(panel_svc_t s, const char **line1, const char **line2)
{
    const char *a = NULL, *b = NULL;
    switch (s) {
    /* The voice runs on the Mac (D-019/D-020). This board cannot see it, so
     * the honest interior says where it lives rather than listing settings
     * the panel has no way to read. */
    case PANEL_SVC_VOICE:  a = "ON THE MAC";  b = "NOT VISIBLE FROM HERE"; break;
    case PANEL_SVC_CAMERA: a = "NO CAMERA";   b = "NONE IN THIS BUILD";    break;
    default: return false;
    }
    if (line1) *line1 = a;
    if (line2) *line2 = b;
    return true;
}

panel_tone_t panel_service_link_tone(const r2_telemetry_t *tm)
{
    return (tm != NULL && tm->link == R2_TM_UP) ? PANEL_TONE_GOOD : PANEL_TONE_WARN;
}

/* ---- the ladder ----------------------------------------------------------- */

/* The bring-up order, CLAUDE.md's and the gate's. The first five mirror
 * r2_tier_t index for index (panel_ui.c pins that with a static assert where
 * both headers are visible); LOCOMOTION is not a gate tier at all. */
static const char *const k_rung[PANEL_LADDER_RUNGS] = {
    "READ", "LEDS", "AUDIO", "DOME", "STANCE", "LOCOMOTION",
};
#define GATE_TIERS 5        /* READ..STANCE: the rungs the gate can admit */

const char *panel_service_ceiling_name(int ceiling)
{
    return (ceiling >= 0 && ceiling < GATE_TIERS) ? k_rung[ceiling] : "";
}

int panel_service_ladder(int ceiling, panel_rung_t out[PANEL_LADDER_RUNGS])
{
    if (out == NULL) return 0;
    /* An out-of-range ceiling locks EVERYTHING rather than being clamped: a
     * corrupt ceiling that clamped to STANCE would draw legs as allowed. */
    const bool sane = ceiling >= 0 && ceiling < GATE_TIERS;
    for (int i = 0; i < PANEL_LADDER_RUNGS; i++) {
        out[i].tier = k_rung[i];
        out[i].allowed = sane && i < GATE_TIERS && i <= ceiling;
    }
    return PANEL_LADDER_RUNGS;
}
