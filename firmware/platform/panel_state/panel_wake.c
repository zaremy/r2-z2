#include "panel_wake.h"

#include <stdio.h>

void panel_wake_init(panel_wake_t *w)
{
    if (w == NULL) return;
    w->st       = PANEL_ST_COUNT;      /* nothing seen: the first state is an entry */
    w->mode     = PANEL_OFF_COUNT;
    w->since_ms = 0;
    w->showing  = false;
    w->fired    = false;
}

bool panel_wake_step(panel_wake_t *w, panel_state_t st,
                     panel_offline_mode_t mode, uint32_t now_ms)
{
    if (w == NULL) return false;

    if (st != w->st || mode != w->mode) {
        w->st       = st;
        w->mode     = mode;
        w->since_ms = now_ms;
        /* Leaving a waking state takes the frame down in the same step --
         * it is decided by the NEW state, never carried over from the old. */
        w->showing  = panel_state_wakes(st);
        w->fired    = w->showing;
    }

    /* Unsigned subtraction, so a frame that fires just before the 49.7-day
     * wrap still decays on time rather than never. */
    if (w->showing && now_ms - w->since_ms >= PANEL_WAKE_DECAY_MS)
        w->showing = false;

    return w->showing;
}

bool panel_wake_take_fired(panel_wake_t *w)
{
    if (w == NULL) return false;
    const bool f = w->fired;
    w->fired = false;
    return f;
}

void panel_wake_dismiss(panel_wake_t *w)
{
    if (w == NULL) return;
    w->showing = false;
    w->fired   = false;   /* a dismissed frame is not painted */
}

bool panel_wake_showing(const panel_wake_t *w)
{
    return w != NULL && w->showing;
}

bool panel_wake_box(panel_state_t st, panel_offline_mode_t mode,
                    const char **subject, const char **what)
{
    static const char *const k_subject[PANEL_OFF_COUNT] = {
        [PANEL_OFF_R2] = "R2", [PANEL_OFF_NET] = "NET", [PANEL_OFF_LLM] = "LLM",
    };
    /* The reference's own words for what each subject is: the R2 link is
     * BLE, the network is a LINK, the reasoning service is a SVC. */
    static const char *const k_what[PANEL_OFF_COUNT] = {
        [PANEL_OFF_R2] = "BLE", [PANEL_OFF_NET] = "LINK", [PANEL_OFF_LLM] = "SVC",
    };

    if (st != PANEL_ST_OFFLINE) return false;
    if ((unsigned)mode >= (unsigned)PANEL_OFF_COUNT) return false;
    if (subject) *subject = k_subject[mode];
    if (what)    *what    = k_what[mode];
    return true;
}

void panel_wake_format_down(uint32_t ms, char *buf, size_t n)
{
    if (buf == NULL || n == 0) return;
    const uint32_t s = ms / 1000u;
    if (s < 60u)          snprintf(buf, n, "DOWN %uS", (unsigned)s);
    else if (s < 3600u)   snprintf(buf, n, "DOWN %uM", (unsigned)(s / 60u));
    else if (s < 86400u)  snprintf(buf, n, "DOWN %uH", (unsigned)(s / 3600u));
    else                  snprintf(buf, n, "DOWN %uD", (unsigned)(s / 86400u));
}
