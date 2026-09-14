#include "panel_probe.h"

#include <stdio.h>

bool panel_probe_may_start(panel_probe_gate_t g)
{
    return !g.queued && !g.in_flight && !g.pending &&
           g.state != PANEL_PROBE_RUNNING;
}

void panel_probe_init(panel_probe_t *p)
{
    if (p == NULL) return;
    p->state = PANEL_PROBE_IDLE;
    p->started_ms = 0;
    p->expected = 0;
    p->got = 0;
}

void panel_probe_start(panel_probe_t *p, uint32_t now_ms, uint8_t expected)
{
    if (p == NULL) return;
    if (expected == 0) {
        /* Nothing went out -- the gate refused every op, or the link did.
         * NO REPLY rather than RUNNING: a test that never asked must not sit
         * there looking busy, and must never settle as a pass. */
        p->state = PANEL_PROBE_NO_REPLY;
        p->expected = 0;
        p->got = 0;
        return;
    }
    p->state = PANEL_PROBE_RUNNING;
    p->started_ms = now_ms;
    p->expected = expected;
    p->got = 0;
}

panel_probe_state_t panel_probe_step(panel_probe_t *p, uint32_t now_ms,
                                     unsigned answered, bool link_up)
{
    if (p == NULL) return PANEL_PROBE_IDLE;
    if (p->state != PANEL_PROBE_RUNNING) return p->state;

    p->got = (answered > 255u) ? 255u : (uint8_t)answered;

    if (p->got >= p->expected) {
        p->state = PANEL_PROBE_PASS;
    } else if (!link_up) {
        /* THE LINK IS ITS OWN ANSWER. Saying NO REPLY here would blame him
         * for a silence that is ours: the question never reached him. */
        p->state = PANEL_PROBE_LINK_LOST;
    } else if (now_ms - p->started_ms >= PANEL_PROBE_TIMEOUT_MS) {
        p->state = p->got > 0 ? PANEL_PROBE_PARTIAL : PANEL_PROBE_NO_REPLY;
    }
    return p->state;
}

const char *panel_probe_word(const panel_probe_t *p, char *buf, unsigned n)
{
    if (buf == NULL || n == 0) return "";
    buf[0] = '\0';
    if (p == NULL) return buf;
    switch (p->state) {
    case PANEL_PROBE_IDLE:      snprintf(buf, n, "RUN"); break;
    case PANEL_PROBE_RUNNING:   snprintf(buf, n, "..."); break;
    case PANEL_PROBE_PASS:      snprintf(buf, n, "%u/%u OK",
                                         (unsigned)p->got, (unsigned)p->expected); break;
    case PANEL_PROBE_PARTIAL:   snprintf(buf, n, "%u/%u",
                                         (unsigned)p->got, (unsigned)p->expected); break;
    case PANEL_PROBE_NO_REPLY:  snprintf(buf, n, "NO REPLY"); break;
    case PANEL_PROBE_LINK_LOST: snprintf(buf, n, "LINK LOST"); break;
    default:                    snprintf(buf, n, "RUN"); break;
    }
    return buf;
}

bool panel_probe_settled(const panel_probe_t *p)
{
    return p != NULL && p->state != PANEL_PROBE_IDLE &&
           p->state != PANEL_PROBE_RUNNING;
}

uint32_t panel_probe_started_ms(const panel_probe_t *p)
{
    return (p != NULL && p->state == PANEL_PROBE_RUNNING) ? p->started_ms : 0u;
}

bool panel_probe_passed(const panel_probe_t *p)
{
    return p != NULL && p->state == PANEL_PROBE_PASS;
}