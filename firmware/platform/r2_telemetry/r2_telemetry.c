#include "r2_telemetry.h"

#include <string.h>

void r2_telemetry_reset(r2_telemetry_t *t)
{
    if (t == NULL) return;
    memset(t, 0, sizeof *t);
    t->link = R2_TM_DOWN;
}

/* Invalidate every reading. Not "mark them old" -- old is a number a panel can
 * decide to show anyway. These readings describe a droid we are no longer
 * talking to, and the only honest rendering is absence. */
static void forget_readings(r2_telemetry_t *t)
{
    t->battery.valid = false;
    t->dome.valid    = false;
    t->version.valid = false;
}

void r2_telemetry_link(r2_telemetry_t *t, r2_tm_link_t state, uint32_t now_ms)
{
    if (t == NULL) return;

    /* Anything that is not UP means we cannot vouch for a reading, so every
     * transition out of UP forgets them -- including UP -> SCANNING, which is
     * what a reconnect looks like and is easy to miss by only handling DOWN.
     *
     * And re-entering UP forgets them too: a new link is a new droid as far as
     * this struct is concerned, and the first reading must come from the new
     * session. That is "assert the status on connect, never inherit it" for
     * data instead of for LEDs. Without it, a reconnect silently republishes
     * the previous session's battery voltage as current. */
    if (state != t->link) {
        forget_readings(t);
        t->link_since_ms = now_ms;

        /* One attempt from losing him to having him back -- see the header.
         * Only UP closes it, so no path through DOWN can restart the clock:
         * a droid that keeps half-answering must not hold the panel in
         * `waking` forever. */
        if (state == R2_TM_UP) {
            t->attempt_open = false;
        } else if (!t->attempt_open) {
            t->attempt_open = true;
            t->unreachable_since_ms = now_ms;
        }
    }
    t->link = state;
}

uint32_t r2_telemetry_unreachable_ms(const r2_telemetry_t *t, uint32_t now_ms)
{
    if (t == NULL || t->link == R2_TM_UP) return 0;
    return now_ms - t->unreachable_since_ms;   /* unsigned: wrap-safe */
}

static void stamp(r2_tm_stamp_t *s, uint32_t now_ms)
{
    s->valid = true;
    s->at_ms = now_ms;
}

/* A reading that arrives while the link is not UP is discarded, not stored.
 * Frames can be in flight across a disconnect, so this is reachable rather
 * than theoretical, and storing one would resurrect exactly the value the
 * transition just forgot. */
void r2_telemetry_battery(r2_telemetry_t *t, uint16_t centivolts, uint32_t now_ms)
{
    if (t == NULL || t->link != R2_TM_UP) return;
    t->battery_centivolts = centivolts;
    stamp(&t->battery, now_ms);
    t->responses++;
}

void r2_telemetry_dome(r2_telemetry_t *t, float degrees, uint32_t now_ms)
{
    if (t == NULL || t->link != R2_TM_UP) return;
    t->dome_degrees = degrees;
    stamp(&t->dome, now_ms);
    t->responses++;
}

void r2_telemetry_version(r2_telemetry_t *t, uint16_t major, uint16_t minor,
                          uint16_t revision, uint32_t now_ms)
{
    if (t == NULL || t->link != R2_TM_UP) return;
    t->version_major    = major;
    t->version_minor    = minor;
    t->version_revision = revision;
    stamp(&t->version, now_ms);
    t->responses++;
}

void r2_telemetry_note_request(r2_telemetry_t *t) { if (t) t->requests++; }
void r2_telemetry_note_refused(r2_telemetry_t *t) { if (t) t->refused++;  }
void r2_telemetry_note_dropped(r2_telemetry_t *t) { if (t) t->dropped++;  }

bool r2_telemetry_age_ms(const r2_tm_stamp_t *s, uint32_t now_ms, uint32_t *age_ms)
{
    if (s == NULL || age_ms == NULL) return false;
    if (!s->valid) return false;
    /* Unsigned subtraction is correct across the 2^32 ms wrap (~49.7 days).
     * A droid that lives in a household reaches that, and the naive
     * (now < at) ? 0 : now - at reports a wrapped reading as brand new --
     * the failure mode being that the panel looks most confident exactly when
     * its clock has just rolled over. */
    *age_ms = now_ms - s->at_ms;
    return true;
}

bool r2_telemetry_displayable(const r2_telemetry_t *t, const r2_tm_stamp_t *s,
                              uint32_t now_ms, uint32_t max_age_ms)
{
    if (t == NULL || s == NULL) return false;
    /* A reading is only displayable while we are still connected. This is the
     * rule the whole module exists for: 4.42 V beside a dead link is not a
     * stale reading, it is a false statement about the present. */
    if (t->link != R2_TM_UP) return false;
    uint32_t age;
    if (!r2_telemetry_age_ms(s, now_ms, &age)) return false;
    return age <= max_age_ms;
}

const char *r2_telemetry_link_name(r2_tm_link_t s)
{
    switch (s) {
    case R2_TM_DOWN:        return "down";
    case R2_TM_SCANNING:    return "scanning";
    case R2_TM_CONNECTING:  return "connecting";
    case R2_TM_HANDSHAKING: return "handshaking";
    case R2_TM_UP:          return "up";
    default:                return "?";
    }
}
