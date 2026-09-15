#include "panel_probe.h"

#include <stdio.h>

bool panel_probe_may_start(panel_probe_gate_t g)
{
    return !g.queued && !g.in_flight && !g.pending &&
           g.state != PANEL_PROBE_RUNNING;
}

/* ---- the ledger: which of THIS test's questions have been answered ------- */

void panel_ledger_reset(panel_ledger_t *l)
{
    if (l == NULL) return;
    for (unsigned i = 0; i < PANEL_LEDGER_MAX; i++) {
        l->seq[i]  = 0;
        l->open[i] = false;
    }
    l->asked = 0;
    l->answered = 0;
}

bool panel_ledger_add(panel_ledger_t *l, uint8_t seq)
{
    if (l == NULL) return false;
    if (l->asked >= PANEL_LEDGER_MAX) return false;
    /* A SEQ ALREADY OUTSTANDING IS REFUSED. Two open slots on one seq would
     * let a single reply strike both, and a three-question test would pass on
     * two answers -- the exact overcounting this ledger exists to end. */
    for (unsigned i = 0; i < l->asked; i++)
        if (l->open[i] && l->seq[i] == seq) return false;
    l->seq[l->asked]  = seq;
    l->open[l->asked] = true;
    l->asked++;
    return true;
}

bool panel_ledger_note(panel_ledger_t *l, uint8_t seq)
{
    if (l == NULL) return false;
    for (unsigned i = 0; i < l->asked; i++) {
        if (l->open[i] && l->seq[i] == seq) {
            l->open[i] = false;     /* struck ONCE: a duplicate reply is not a
                                     * second answer */
            l->answered++;
            return true;
        }
    }
    return false;                   /* a poll, a keepalive, or an echo */
}

unsigned panel_ledger_asked(const panel_ledger_t *l)
{
    return l ? l->asked : 0u;
}

unsigned panel_ledger_answered(const panel_ledger_t *l)
{
    return l ? l->answered : 0u;
}

/* ---- how long a tier may take ------------------------------------------- */

uint32_t panel_probe_timeout_ms(int tier)
{
    /* Indices are the gate's tier order, the same one the ladder walks:
     * 0 READ, 1 LEDS, 2 AUDIO, 3 DOME, 4 STANCE. Not the enum, to keep this
     * component free of r2_gate -- the order is asserted by the ladder's own
     * test, which writes the names out longhand. */
    switch (tier) {
    case 0: return PANEL_PROBE_TIMEOUT_MS;        /* measured: 138/138 sub-second */
    case 3: return PANEL_PROBE_DOME_TIMEOUT_MS;   /* measured: D-013, 2.0-2.2 s */
    default: break;
    }
    /* LEDS, AUDIO, STANCE -- and anything out of range. UNMEASURED, and a
     * guessed number would be indistinguishable from the two above in six
     * months. The caller refuses a 0. */
    return 0u;
}

void panel_probe_init(panel_probe_t *p)
{
    if (p == NULL) return;
    p->state = PANEL_PROBE_IDLE;
    p->started_ms = 0;
    p->expected = 0;
    p->got = 0;
}

void panel_probe_start(panel_probe_t *p, uint32_t now_ms, uint8_t expected,
                       bool link_up)
{
    if (p == NULL) return;
    if (!link_up) {
        /* THE LINK IS ITS OWN ANSWER, at the start as well as during. Every
         * op fails to send when he is away, which used to arrive here as
         * `expected == 0` and settle NO REPLY -- blaming him for a silence
         * that is entirely ours, which is exactly what this module says
         * elsewhere it must not do. Tapping RUN with the link down is the
         * COMMON case of that, not the rare one: the rare one is a link that
         * drops mid-window, and only that was handled. */
        p->state = PANEL_PROBE_LINK_LOST;
        p->started_ms = now_ms;
        p->expected = expected;
        p->got = 0;
        return;
    }
    if (expected == 0) {
        /* Nothing went out on a live link -- the gate refused every op. NO
         * REPLY rather than RUNNING: a test that never asked must not sit
         * there looking busy, and must never settle as a pass. */
        p->state = PANEL_PROBE_NO_REPLY;
        p->started_ms = now_ms;
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