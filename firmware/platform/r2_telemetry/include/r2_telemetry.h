/* What the panel is allowed to show — #114 AC6.
 *
 * "Telemetry the panel needs is exposed as a struct with explicit staleness,
 * so a value cannot outlive the link that carried it."
 *
 * That sentence is the whole design, and the word doing the work is OUTLIVE.
 * A battery reading is not a fact about R2; it is a fact about R2 AT A MOMENT,
 * carried by a link that may since have dropped. A panel that renders 4.42 V
 * next to a dead link is not stale, it is lying -- and it lies most
 * convincingly at exactly the moment someone is looking to find out whether
 * anything is wrong.
 *
 * This project has the scar. An LED colour set in the afternoon was still lit
 * an hour later across a daemon kill and a fresh connect, because a colour we
 * set is state that survives its session; the rule that came out of it was
 * ASSERT THE STATUS ON CONNECT, NEVER INHERIT IT. This is the same rule one
 * layer up: a reading does not survive the link that produced it.
 *
 * So every value here is a triple -- have we ever had one, what was it, and how
 * old is it -- and the link falling invalidates all of them at once. There is
 * no path that yields a value without its age.
 *
 * Pure logic, no ESP dependency: time is passed in, never read. That is what
 * makes staleness host-testable, and staleness is the part most likely to be
 * wrong in a way nobody notices until the panel is showing a comforting number
 * about a droid that is not there.
 */
#ifndef R2_TELEMETRY_H
#define R2_TELEMETRY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    R2_TM_DOWN = 0,
    R2_TM_SCANNING,
    R2_TM_CONNECTING,
    R2_TM_HANDSHAKING,
    R2_TM_UP,
} r2_tm_link_t;

/* One reading and everything needed to distrust it. */
typedef struct {
    bool     valid;      /* false until a reading arrives; false again on link loss */
    uint32_t at_ms;      /* when it arrived, on the caller's monotonic clock */
} r2_tm_stamp_t;

typedef struct {
    r2_tm_link_t  link;
    uint32_t      link_since_ms;   /* when the link last entered its current state */
    /* When the current attempt to reach him BEGAN. Not the same as
     * link_since_ms, which restarts on every step of a reconnect: everything
     * between losing him and having him back is ONE attempt, however many
     * scans, connects and dropped handshakes it takes. Read it through
     * r2_telemetry_unreachable_ms. */
    uint32_t      unreachable_since_ms;
    bool          attempt_open;    /* an attempt is running; closed only by UP */
    /* Did the current attempt begin by LOSING a live link? False when it
     * began at boot: then its age is how long WE have been looking, not how
     * long he has been gone, and a panel that rebooted while he was off for
     * hours must not report that as "down 4 s". */
    bool          attempt_from_up;
    /* We have let go of him on purpose (D-023 GOODNIGHT). While set, no
     * attempt is open and none opens: being released is not failing to reach
     * him, and an attempt clock running through it would make the first WAKE
     * after an hour read as an hour-long outage -- OFFLINE instead of WAKING. */
    bool          released;

    r2_tm_stamp_t battery;
    uint16_t      battery_centivolts;

    r2_tm_stamp_t dome;
    float         dome_degrees;

    r2_tm_stamp_t version;
    uint16_t      version_major, version_minor, version_revision;

    /* Accounting, so a panel can show that we are asking and not being
     * answered -- the condition that hid the escaping bug for a whole
     * endurance run because nothing ever displayed the ratio. */
    uint32_t requests, responses, refused, dropped;
} r2_telemetry_t;

void r2_telemetry_reset(r2_telemetry_t *t);

/* Link transitions. Falling out of UP invalidates every reading -- that is the
 * single most important line in this module. */
void r2_telemetry_link(r2_telemetry_t *t, r2_tm_link_t state, uint32_t now_ms);

/* GOODNIGHT sets it, WAKE clears it. Clearing opens a FRESH attempt timed from
 * now, as a boot does -- attempt_from_up is false, because we are looking,
 * not recovering from a loss. */
void r2_telemetry_released(r2_telemetry_t *t, bool released, uint32_t now_ms);

void r2_telemetry_battery(r2_telemetry_t *t, uint16_t centivolts, uint32_t now_ms);
void r2_telemetry_dome(r2_telemetry_t *t, float degrees, uint32_t now_ms);
void r2_telemetry_version(r2_telemetry_t *t, uint16_t major, uint16_t minor,
                          uint16_t revision, uint32_t now_ms);

void r2_telemetry_note_request(r2_telemetry_t *t);
void r2_telemetry_note_refused(r2_telemetry_t *t);
void r2_telemetry_note_dropped(r2_telemetry_t *t);

/* Age of a reading. Returns false when there is nothing to age -- so a caller
 * cannot get a number without also learning whether it means anything.
 * Handles the monotonic clock wrapping at 2^32 ms (~49.7 days), which a
 * household droid WILL reach. */
bool r2_telemetry_age_ms(const r2_tm_stamp_t *s, uint32_t now_ms, uint32_t *age_ms);

/* Should the panel show this reading at all? A reading older than max_age_ms,
 * or taken before the current link came up, is not displayable. */
bool r2_telemetry_displayable(const r2_telemetry_t *t, const r2_tm_stamp_t *s,
                              uint32_t now_ms, uint32_t max_age_ms);

/* How long we have been trying to reach him, or 0 while the link is UP.
 *
 * AN ATTEMPT OPENS ON THE FIRST TRANSITION INTO A NON-UP STATE AND CLOSES
 * ONLY ON UP. Every step in between -- scan, connect, a connect that fails and
 * rescans, a handshake that drops back through DOWN -- is the same attempt
 * continuing. An earlier version restarted the clock on leaving DOWN, and a
 * droid that connected and dropped every 1.5 s then read 100 ms unreachable
 * forever: `waking` without end, which is the bug the bound exists to end.
 *
 * At boot the first transition is DOWN -> SCANNING, so a boot is timed from
 * its first scan -- the interval P2 measured -- rather than from power-on,
 * which would bill the radio's own ~1.1 s bring-up to R2. Before that nothing
 * has opened, so this is the time since reset: a radio that never comes up
 * reads as unreachable rather than as forever about to connect.
 *
 * Wrap-safe across 2^32 ms. */
uint32_t r2_telemetry_unreachable_ms(const r2_telemetry_t *t, uint32_t now_ms);

const char *r2_telemetry_link_name(r2_tm_link_t s);

#ifdef __cplusplus
}
#endif
#endif /* R2_TELEMETRY_H */
