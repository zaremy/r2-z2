#include "panel_probe.h"

#include <stdio.h>

bool panel_probe_may_start(panel_probe_gate_t g)
{
    return !g.queued && !g.in_flight && !g.pending &&
           g.state != PANEL_PROBE_RUNNING &&
           /* HE HAS TO HAVE STOPPED, not merely to have been judged. The four
            * conditions above all ask about the test; this one asks about the
            * droid, and on a moving tier it outlasts every one of them. */
           g.motion_settled;
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

uint32_t panel_probe_move_ms(int tier)
{
    /* Same tier order as above. DOME is the only tier that both moves him and
     * has a measured duration; READ is 0 because reading cannot move him at
     * all. A 0 on a tier that moves him is "unmeasured", and
     * panel_motion_settled holds on it rather than releasing. */
    switch (tier) {
    case 3: return PANEL_PROBE_DOME_MOVE_MS;  /* measured: D-013, longest 2.19 s */
    default: break;
    }
    return 0u;
}

bool panel_probe_needs_completion(int tier)
{
    /* The tiers that move him. STANCE and LOCOMOTION are here although
     * neither is timed and LOCOMOTION is not even a gate tier: this predicate
     * must describe the tier, not the current build's ceiling, or it goes
     * quietly wrong on the commit that opens one. It also decides what the
     * guard holds for (panel_motion_note_sent), so a mover left out of it is a
     * mover the guard releases on. */
    return tier == 3 || tier == 4 || tier == 5;   /* DOME, STANCE, LOCOMOTION */
}

void panel_motion_note_sent(panel_motion_t *m, int tier, unsigned sent,
                            uint32_t sent_ms)
{
    if (m == NULL) return;
    /* NOTHING THAT MOVES HIM: leave the record alone. A READ sent a second
     * after a dome move must not overwrite the dome's stamp with a tier that
     * cannot move him -- that would release the hold on the move still running.
     * A tier that DOES move him is recorded whether or not its move is
     * measured; panel_motion_settled decides what an unmeasured one means.
     * NOTHING WENT OUT: leave it alone too -- nothing is moving, so there is
     * nothing to hold for. */
    if (sent == 0u || !panel_probe_needs_completion(tier)) return;
    m->sent = true;
    m->tier = tier;
    m->sent_ms = sent_ms;
}

bool panel_motion_settled(const panel_motion_t *m, uint32_t now_ms)
{
    if (m == NULL) return false;        /* lost track of it: still moving */
    if (!m->sent) return true;          /* nothing that moves him went out */
    const uint32_t move = panel_probe_move_ms(m->tier);
    /* A TIER THAT MOVES HIM WITH NO MEASURED MOVE HOLDS -- until reboot. It is
     * NOT refused before it is sent: the only refusal is panel_probe_start's
     * zero window, on ui_task, after link_task has already sent the ops, and
     * it settles NO REPLY. Releasing here would open the guard straight after
     * an untimed stance change. The lock is the friction: measure the tier. */
    if (move == 0u) return false;
    /* A CLOCK BEHIND THE START IS "STILL MOVING", not "long since finished".
     * These are uint32_t, so `now_ms - sent_ms` on a now_ms EARLIER than
     * the start wraps to roughly 4.3e9 and clears any window -- the guard
     * opens mid-travel, silently, in the one direction that matters.
     *
     * And it CAN happen: sent_ms is stamped on link_task and now_ms is
     * ui_task's last tick, so a tap landing between a send and the next
     * refresh compares a fresh stamp against an older clock.
     *
     * BEHIND BY A LITTLE, NOT BY ANY AMOUNT. Two cheaper forms each fail
     * closed for weeks. `now_ms < sent_ms` calls a clock that has WRAPPED
     * behind: a send in the last 2.2 s before the ~49.7-day wrap held every
     * row shut for the next 49 days. A signed difference goes negative 2^31 ms
     * after ANY send, and nothing clears `sent`, so every row locked for 24.8
     * days on any board left up that long after one dome move. Bounding
     * "behind" by the skew the two tasks can actually produce fixes both. */
    const uint32_t behind = m->sent_ms - now_ms;
    if (behind != 0u && behind <= PANEL_MOTION_SKEW_MS) return false;
    return now_ms - m->sent_ms >= move;
}

void panel_probe_arm_completion(panel_probe_t *p, bool armed)
{
    if (p == NULL) return;
    p->completion_armed = armed;
}

void panel_probe_note_complete(panel_probe_t *p)
{
    if (p == NULL) return;
    p->completed = true;
}

void panel_probe_abort(panel_probe_t *p)
{
    if (p == NULL) return;
    /* IDLE counts: the caller aborts a test that was tapped and cancelled
     * before its ops went, and that row must say so rather than fall back to
     * a fresh RUN as though nothing had been asked. */
    if (p->state == PANEL_PROBE_IDLE || p->state == PANEL_PROBE_RUNNING)
        p->state = PANEL_PROBE_STOPPED;
}

void panel_probe_init(panel_probe_t *p)
{
    if (p == NULL) return;
    p->state = PANEL_PROBE_IDLE;
    p->started_ms = 0;
    p->expected = 0;
    p->got = 0;
    p->tier = 0;
    /* CLEARED, SO THE DEFAULT REFUSES. An un-armed completion channel makes a
     * moving tier refuse to run; leaving this set across runs would let one
     * armed test vouch for the next unarmed one. */
    p->completion_armed = false;
    p->completed = false;
}

void panel_probe_start(panel_probe_t *p, uint32_t now_ms, uint8_t expected,
                       bool link_up, int tier)
{
    if (p == NULL) return;
    p->tier = tier;
    p->completed = false;               /* last run's completion is not this one's */
    if (panel_probe_needs_completion(tier) && !p->completion_armed) {
        /* THE CHANNEL WAS NEVER ARMED. `leg_action_complete` does not fire
         * unless the notify went out, and a test that never armed it would
         * wait out its whole window, see silence, and report NO REPLY for a
         * move that worked -- or, worse, be "fixed" later by dropping the
         * completion requirement. Refuse instead: prove the channel live,
         * then believe its silence. Same refusal shape, and the same reason,
         * as an unmeasured window. */
        p->state = PANEL_PROBE_NO_REPLY;
        p->started_ms = now_ms;
        p->expected = expected;
        p->got = 0;
        return;
    }
    if (panel_probe_timeout_ms(tier) == 0u) {
        /* NOBODY HAS MEASURED THIS TIER. Running it on a borrowed constant is
         * how the single 2 s window came to call a 2.0-2.2 s dome move
         * PARTIAL. Refuse, and make whoever raises the ceiling measure. */
        p->state = PANEL_PROBE_NO_REPLY;
        p->started_ms = now_ms;
        p->expected = expected;
        p->got = 0;
        return;
    }
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

    if (p->got >= p->expected &&
        /* ANSWERS ARE NOT ARRIVAL. On a moving tier the replies say the op was
         * heard; only the completion says he finished the move. Passing on the
         * reply count alone is what let a verdict settle mid-travel. */
        (!panel_probe_needs_completion(p->tier) || p->completed)) {
        p->state = PANEL_PROBE_PASS;
    } else if (!link_up) {
        /* THE LINK IS ITS OWN ANSWER. Saying NO REPLY here would blame him
         * for a silence that is ours: the question never reached him. */
        p->state = PANEL_PROBE_LINK_LOST;
    } else if (now_ms - p->started_ms >= panel_probe_timeout_ms(p->tier)) {
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
    case PANEL_PROBE_STOPPED:   snprintf(buf, n, "STOPPED"); break;
    default:                    snprintf(buf, n, "RUN"); break;
    }
    return buf;
}

bool panel_probe_settled(const panel_probe_t *p)
{
    return p != NULL && p->state != PANEL_PROBE_IDLE &&
           p->state != PANEL_PROBE_RUNNING;
}

bool panel_probe_passed(const panel_probe_t *p)
{
    return p != NULL && p->state == PANEL_PROBE_PASS;
}