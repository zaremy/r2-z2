#include <math.h>
#include <string.h>

#include "r2_lights.h"

/* The corners, from mac-prototype/r2_behavior.py:134-138. Seven reachable
 * colours, not a continuum (D-012 Amendment A); these are the five with a
 * status meaning. */
#define NEUTRAL {0, 0, 255}      /* blue   -- idle, nothing engaged */
#define ENGAGED {0, 255, 255}    /* cyan   -- engaged with you */
#define SUCCESS {0, 255, 0}      /* green  -- wake-sweep terminus */
#define PENDING {255, 255, 0}    /* yellow -- needs monitoring */
#define DANGER  {255, 0, 0}      /* red    -- danger and stop, ONLY */

typedef enum { K_ABSENT = 0, K_STEADY, K_BLINK, K_ALTERNATE, K_SWEEP } kind_t;

/* One fixture's behaviour over time; r2_lights.py `Pattern`. A PSI takes RGB
 * triples; the holo and logic take a level in v[i][0]. `period_s` means a
 * full CYCLE for blink/alternate and the DWELL per member for a sweep -- the
 * one non-uniform field, exactly as in the Python. */
typedef struct {
    kind_t  kind;
    uint8_t n;
    uint8_t v[6][3];
    double  period_s;
    double  phase;
    double  scale;
} pattern_t;

typedef struct {
    const char *name;
    pattern_t   front, back, holo, logic;
    bool        dimmable;
} row_t;

#define STEADY(...)        { K_STEADY, 1, { __VA_ARGS__ }, 0.0, 0.0, 1.0 }
#define STEADY_AT(sc, ...) { K_STEADY, 1, { __VA_ARGS__ }, 0.0, 0.0, (sc) }
#define BLINK(p, ...)      { K_BLINK, 1, { __VA_ARGS__ }, (p), 0.0, 1.0 }
#define ALT(p, ph, a, b)   { K_ALTERNATE, 2, { a, b }, (p), (ph), 1.0 }
#define SWEEP(p, n, ...)   { K_SWEEP, (n), { __VA_ARGS__ }, (p), 0.0, 1.0 }
#define ABSENT             { K_ABSENT, 0, {{0}}, 0.0, 0.0, 1.0 }
#define BREATHE {16}, {64}, {160}, {255}, {160}, {64}

/* r2_lights.py STATES, row for row and in the same order. */
static const row_t ROWS[R2L__COUNT] = {
    [R2L_IDLE] = { "idle",
        STEADY(NEUTRAL), STEADY(NEUTRAL), ABSENT, ABSENT, true },
    [R2L_WAKE] = { "wake",
        SWEEP(0.45, 3, NEUTRAL, ENGAGED, SUCCESS), STEADY(NEUTRAL),
        SWEEP(0.45, 3, {16}, {96}, {255}), STEADY({255}), true },
    [R2L_LISTEN] = { "listen",
        STEADY(ENGAGED), STEADY(ENGAGED),
        SWEEP(2.2, 6, BREATHE), BLINK(0.9, {255}), true },
    [R2L_THINKING] = { "thinking",
        ALT(0.9, 0.0, ENGAGED, NEUTRAL), ALT(0.9, 0.5, ENGAGED, NEUTRAL),
        SWEEP(1.2, 6, BREATHE), BLINK(0.45, {255}), true },
    [R2L_ANSWERING] = { "answering",
        STEADY(ENGAGED), STEADY(ENGAGED), STEADY({255}), STEADY({255}), true },
    [R2L_ATTENTION] = { "attention",
        BLINK(2.4, PENDING), BLINK(2.4, PENDING), ABSENT, ABSENT, true },
    [R2L_DANGER] = { "danger",
        BLINK(0.25, DANGER), BLINK(0.25, DANGER), ABSENT, ABSENT, false },
    [R2L_MISHEARD] = { "misheard",
        BLINK(1.2, PENDING), BLINK(1.2, PENDING), ABSENT, ABSENT, true },
    [R2L_OFFLINE] = { "offline",
        BLINK(1.0, DANGER), BLINK(1.0, DANGER), ABSENT, ABSENT, false },
    [R2L_WAITING] = { "waiting",
        BLINK(3.0, NEUTRAL), STEADY_AT(0.25, NEUTRAL), ABSENT, ABSENT, true },
    [R2L_SLEEP] = { "sleep",
        STEADY_AT(0.2, NEUTRAL), STEADY_AT(0.2, NEUTRAL), ABSENT, ABSENT, true },
};

const char *r2_lights_name(r2_lights_state_t s)
{
    if ((int)s < 0 || s >= R2L__COUNT) return "none";
    return ROWS[s].name;
}

/* Pattern.sample(): the member shown at t, or NULL for a blink's dark half. */
static const uint8_t *sample(const pattern_t *p, double t)
{
    switch (p->kind) {
    case K_STEADY:
        return p->v[0];
    case K_SWEEP: {
        /* ONE SHOT: walks its members once and holds the last forever. */
        const double i = floor(t / p->period_s + p->phase);
        return p->v[i >= p->n - 1 ? p->n - 1 : (int)i];
    }
    case K_BLINK:
    case K_ALTERNATE: {
        const double q = fmod(t / p->period_s + p->phase, 1.0);
        if (q < 0.5) return p->v[0];
        return p->kind == K_BLINK ? NULL : p->v[1];
    }
    default:
        return NULL;
    }
}

/* Python's round() is round-half-to-EVEN, and nearbyint under the default
 * rounding mode is the same. `(int)(x + 0.5)` would disagree at every .5. */
static uint8_t level(uint8_t v, double k) { return (uint8_t)nearbyint(v * k); }

bool r2_lights_sample(r2_lights_state_t s, uint32_t t_ms, double value,
                      r2_lights_frame_t *out)
{
    if ((int)s < 0 || s >= R2L__COUNT || out == NULL) return false;
    const row_t *r = &ROWS[s];
    const double t = t_ms / 1000.0;
    const double quiet = r->dimmable ? value : 1.0;
    r2_lights_frame_t f;
    memset(&f, 0, sizeof f);

    const pattern_t *psi[2] = { &r->front, &r->back };
    const int base[2] = { 0, 4 };                   /* front R, back R */
    for (int i = 0; i < 2; i++) {
        const uint8_t *c = sample(psi[i], t);
        const double k = psi[i]->scale * quiet;
        for (int j = 0; j < 3; j++)
            f.v[base[i] + j] = c ? level(c[j], k) : 0;
    }
    if (r->holo.kind != K_ABSENT) {
        const uint8_t *c = sample(&r->holo, t);
        f.v[7] = c ? level(c[0], r->holo.scale * quiet) : 0;
    }
    if (r->logic.kind != K_ABSENT) {
        const uint8_t *c = sample(&r->logic, t);
        f.v[3] = c ? level(c[0], r->logic.scale * quiet) : 0;
    }
    *out = f;
    return true;
}

/* ---- the player ---------------------------------------------------------- */

void r2_lights_player_init(r2_lights_player_t *p)
{
    memset(p, 0, sizeof *p);
    p->state = R2L_NONE;
}

void r2_lights_player_enter(r2_lights_player_t *p, r2_lights_state_t s,
                            uint32_t now_ms)
{
    if (s == p->state) return;
    p->state = s;
    p->entered_ms = now_ms;
    p->owes_write = (s != R2L_NONE);
}

void r2_lights_player_forget(r2_lights_player_t *p)
{
    p->owes_write = (p->state != R2L_NONE);
}

bool r2_lights_player_step(r2_lights_player_t *p, uint32_t now_ms,
                           double value, r2_lights_frame_t *out)
{
    if (p->state == R2L_NONE) return false;
    /* Unsigned subtraction, so a clock that wraps between writes still
     * measures the gap correctly. */
    if (p->wrote_any && (uint32_t)(now_ms - p->last_write_ms) < R2_LIGHTS_MIN_INTERVAL_MS)
        return false;
    r2_lights_frame_t f;
    if (!r2_lights_sample(p->state, (uint32_t)(now_ms - p->entered_ms), value, &f))
        return false;
    if (!p->owes_write && memcmp(&f, &p->last, sizeof f) == 0) return false;
    *out = f;
    return true;
}

void r2_lights_player_failed(r2_lights_player_t *p, uint32_t now_ms)
{
    p->last_write_ms = now_ms;
    p->wrote_any = true;
    p->owes_write = true;
}

void r2_lights_player_sent(r2_lights_player_t *p, const r2_lights_frame_t *f,
                           uint32_t now_ms)
{
    p->last = *f;
    p->last_write_ms = now_ms;
    p->wrote_any = true;
    p->owes_write = false;
}
