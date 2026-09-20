#include "panel_state.h"

#include <stddef.h>

/* THE PANEL'S COLOUR VALUES, not the body's.
 *
 * D-012 Amendment A: the panel inherits D-012's colour SEMANTICS and not its
 * values. Yellow means needs-monitoring on the body and on the glass; what
 * luminance and hue deliver that meaning is the medium's business, and an
 * OLED read at arm's length is not a diffused lens seen across a room. These
 * are the v5 prototype's values.
 *
 * TWO PLACES WHERE THIS TABLE DIFFERS FROM `behaviour-states.md`, both
 * deliberate, both recorded here so the next reader does not "fix" them:
 *
 *   `offline` is AMBER here and RED on the body, and D-012 Amendment A rules
 *   that split by name: a dead link goes to yellow ON THE PANEL. The body
 *   keeps red deliberately and is NOT stale -- it has a discriminator the
 *   glass does not, namely blink rate, so red at 1.0 s reads as a lost link
 *   against red at 0.25 s for a physical fault. A panel has one steady swatch
 *   and no rate to spend, so it must carry the distinction in hue instead.
 *   (An earlier version of this comment called the body table pre-narrowing
 *   and claimed the discrepancy had been flagged. Both were wrong: the body
 *   doc post-dates the narrowing and cites it, and nothing was flagged
 *   anywhere. The conclusion was right and the reasoning was invented.)
 */
/* MAGENTA, and not the grey this table used to carry.
 *
 * D-012's own colour table puts rest and low power on magenta. `sleep` and
 * `released` were rendered in 0x7C8A8D -- byte-identical to `panel_ui.c`'s
 * V5_LABEL, which D-012 Amendment A defines as the panel's colour for "we
 * cannot say" and calls "deliberately not a colour claim: it is the absence of
 * one". So both states, which are positive claims about a deliberate act, were
 * being drawn in the panel's own no-claim colour, chromatically identical to
 * the inert key labels beside them.
 *
 * That is exactly the failure Amendment A warned about in the other direction:
 * inherit the SEMANTICS. Rest has a colour in the light language, and it is
 * not the absence of one. */

typedef struct {
    const char *name;
    const char *word;
    const char *since;
    uint32_t    colour;
} row_t;

/* Indexed by panel_state_t.
 *
 * An earlier version of this comment promised a compile-time size check that
 * DID NOT EXIST. Designated initialisers grow the array to fit, so adding an
 * enumerator compiled clean under -Wall -Wextra -Werror and left a zero-filled
 * row -- and `panel_state_word()` then handed LVGL a NULL, which segfaults the
 * host tests and would blank the word on the glass.
 *
 * What replaced it is ONE static assert, on the count, and the block below
 * says exactly what that can and cannot catch. (An intermediate version of
 * this comment promised "both halves below are real" and shipped a second
 * assert that was a tautology -- the same defect again, one commit later. The
 * sentence is left here as the reason to check that a comment about a guard
 * and the guard agree.) */
static const row_t k_row[PANEL_ST_COUNT] = {
    [PANEL_ST_DANGER]       = { "danger",       "DANGER",    "R2 FAULT",        PANEL_C_RED   },
    [PANEL_ST_OFFLINE]      = { "offline",      "OFFLINE",   NULL,              PANEL_C_AMBER },
    [PANEL_ST_ATTENTION]    = { "attention",    "ATTENTION", "NEEDS YOU",       PANEL_C_AMBER },
    [PANEL_ST_MISHEARD]     = { "misheard",     "MISHEARD",  "SAY AGAIN",       PANEL_C_AMBER },
    [PANEL_ST_WAITING]      = { "waiting",      "WAITING",   "ON YOU",          PANEL_C_BLUE  },
    /* The line under the word is the reply's MOOD at runtime; "REPLYING" is
     * only what shows before one is known. D-031. */
    [PANEL_ST_ANSWERING]    = { "answering",    "ANSWERING", "REPLYING",        PANEL_C_CYAN  },
    [PANEL_ST_THINKING]     = { "thinking",     "THINKING",  "WORKING",         PANEL_C_CYAN  },
    /* state `listen`, word "LISTENING" -- Amendment B renamed the state, not
     * the rendering. This is the one place the two differ. */
    [PANEL_ST_LISTEN]       = { "listen",       "LISTENING", "GO AHEAD",        PANEL_C_CYAN  },
    [PANEL_ST_IDLE]         = { "idle",         "IDLE",      "NOTHING ENGAGED", PANEL_C_GREEN },
    [PANEL_ST_SLEEP]        = { "sleep",        "ASLEEP",    "",                PANEL_C_MAGENTA },

    /* `released` says what WE did, never what he is (D-023). "STOPPED HOLDING
     * HIM AWAKE" rather than "ASLEEP": the same observation that looks like
     * sleep is what a failure looks like, and the panel must not promote one
     * to the other. */
    [PANEL_ST_RELEASED]     = { "released",     "RELEASED",  "HOLD TO WAKE",    PANEL_C_MAGENTA },
    [PANEL_ST_WAKING]       = { "waking",       "WAKING",    "FINDING HIM",     PANEL_C_BLUE  },
    [PANEL_ST_UNPROVISIONED]= { "unprovisioned","SETUP",     "NOT PAIRED YET",  PANEL_C_BLUE  },
};

/* `offline`'s reason is the whole point of the state having display modes:
 * one state, and the screen says which thing is unreachable. */
static const char *k_offline_since[PANEL_OFF_COUNT] = {
    [PANEL_OFF_R2]  = "R2 LINK DOWN",
    [PANEL_OFF_NET] = "NO INTERNET",
    [PANEL_OFF_LLM] = "LLM DOWN",
};

/* The count is pinned, so ADDING a state fails the build here until somebody
 * adds its row and decides whether it is ranked.
 *
 * That is ALL a static assert can do for this table, and the previous version
 * of this block pretended otherwise. It carried a `ROW_PRESENT` macro under a
 * comment claiming every row was checked non-empty; both of its assertions
 * (`sizeof "" #s > 1`, `PANEL_ST_##s < PANEL_ST_COUNT`) are true for any
 * non-empty macro argument and neither reads `k_row` at all. Blanking a row to
 * `{ 0 }` compiled clean. It was a false guard shipped in the commit written
 * to remove a false guard, which is the whole reason it is described here
 * instead of quietly deleted.
 *
 * A blank row is caught at test time instead, by `test_every_state_renders`,
 * and survived at runtime by `or_empty()` below. */
_Static_assert(PANEL_ST_COUNT == 13,
               "a state was added or removed: give it a row in k_row, decide "
               "whether it is ranked, and update this count deliberately");

/* `(unsigned)s < (unsigned)PANEL_ST_COUNT`, not `(int)s >= 0 && ...`.
 *
 * panel_state_t has no negative enumerator, so the compiler is free to pick an
 * unsigned underlying type -- and does. The old signed form therefore never
 * saw a negative value at all: `(panel_state_t)-1` arrived as 4294967295 and
 * was rejected by the upper bound, so the guard passed its test for a reason
 * unrelated to what it read as doing. The unsigned comparison is exact under
 * either choice of underlying type. */
static bool in_range(panel_state_t s)
{
    return (unsigned)s < (unsigned)PANEL_ST_COUNT;
}

/* A row that exists but is blank must degrade, not crash: an out-of-range
 * state already renders as nothing, and a missing row should look the same
 * rather than being the one path that hands LVGL a NULL.
 *
 * THIS GUARD IS LOAD-BEARING, and an earlier version of this comment said the
 * opposite -- that it was "unreachable by construction" because the static
 * assert made a missing row a build failure. It does not: the assert pins the
 * COUNT, so a row blanked to `{ 0 }` compiles clean, and with the `? :`
 * removed the suite segfaults through the public API. On the panel that is a
 * NULL handed to lv_label_set_text.
 *
 * What IS true is narrower: no test reaches it, because deleting the `? :`
 * alone passes all 246 checks while every row is populated. So it is an
 * untested guard rather than an unreachable one, and the distinction matters
 * -- "unreachable" is an invitation to delete it. */
static const char *or_empty(const char *s) { return s ? s : ""; }

int panel_state_rank(panel_state_t s)
{
    if (!in_range(s)) return -1;
    /* The enum order IS the rank for the ranked prefix. Anything at or past
     * the boundary is outside the ordering and says so with -1 rather than
     * with a large number, which would make it merely "least severe". */
    return ((int)s < (int)PANEL_ST_RANKED_COUNT) ? (int)s : -1;
}

bool panel_state_is_ranked(panel_state_t s)
{
    return panel_state_rank(s) >= 0;
}

bool panel_state_wakes(panel_state_t s)
{
    return s == PANEL_ST_DANGER || s == PANEL_ST_OFFLINE || s == PANEL_ST_ATTENTION;
}

panel_state_t panel_state_resolve(uint32_t active)
{
    /* Lowest rank wins, and the enum order lets that be a scan from the top.
     * Unranked bits are never consulted: the loop bound is the ranked count,
     * not PANEL_ST_COUNT, so an unranked state cannot win by being set. */
    for (int i = 0; i < (int)PANEL_ST_RANKED_COUNT; i++)
        if (active & (1u << i)) return (panel_state_t)i;
    return PANEL_ST_COUNT;
}

uint32_t panel_state_from_link(bool link_up, uint32_t unreachable_ms,
                               panel_state_t voice_state,
                               panel_state_t *out_state,
                               panel_offline_mode_t *out_mode)
{
    uint32_t active = 0;
    panel_offline_mode_t mode = PANEL_OFF_COUNT;

    if (link_up) {
        /* `idle` by default. `listen` / `thinking` / `misheard` / `answering`
         * are real now (E2E v0 slice 3.2): `voice_state` is what the hold and
         * the exchange say is happening, and PANEL_ST_COUNT is their honest
         * "nothing" -- a caller with no exchange passes that, not a guess.
         * `waiting` still has no caller; nothing sets it. */
        active |= 1u << PANEL_ST_IDLE;
        if (voice_state < PANEL_ST_RANKED_COUNT) active |= 1u << voice_state;
    } else if (unreachable_ms > PANEL_WAKING_BOUND_MS) {
        active |= 1u << PANEL_ST_OFFLINE;
        /* PANEL_OFF_R2 and not NET or LLM: the BLE link is the only one this
         * build has. Reporting NET here would be the panel diagnosing a
         * subsystem it cannot see. */
        mode = PANEL_OFF_R2;
    }

    const panel_state_t won = panel_state_resolve(active);

    /* No ranked state means the link is mid-reconnect and still inside the
     * bound. `waking` is unranked BY DESIGN (D-023), so it can never win a
     * contest against a real fault; being chosen deliberately here is the
     * only way an unranked state ever reaches the glass. */
    if (out_state) *out_state = (won == PANEL_ST_COUNT) ? PANEL_ST_WAKING : won;
    if (out_mode)  *out_mode  = mode;
    return active;
}

uint32_t panel_state_from_power(bool released, bool link_up,
                                uint32_t unreachable_ms,
                                panel_state_t voice_state,
                                panel_state_t *out_state,
                                panel_offline_mode_t *out_mode)
{
    if (!released)
        return panel_state_from_link(link_up, unreachable_ms, voice_state,
                                     out_state, out_mode);
    /* Released wins over voice too: an exchange caught mid-GOODNIGHT is the
     * caller's to retire (panel_exchange_retire, PX_RETIRE_GOODNIGHT /
     * PX_RETIRE_R2_RELEASED) before this is ever asked again, not this
     * function's to notice. */
    if (out_state) *out_state = PANEL_ST_RELEASED;
    if (out_mode)  *out_mode  = PANEL_OFF_COUNT;
    return 0;
}

unsigned panel_state_waking_permille(uint32_t unreachable_ms)
{
    /* Test the bound BEFORE multiplying: 4.3 million ms times 1000 already
     * overflows 32 bits, and he is routinely away for longer than that. */
    if (unreachable_ms >= PANEL_WAKING_BOUND_MS) return 1000u;
    return (unsigned)(unreachable_ms * 1000u / PANEL_WAKING_BOUND_MS);
}

const char *panel_state_name(panel_state_t s)
{
    return in_range(s) ? or_empty(k_row[s].name) : "";
}

const char *panel_state_word(panel_state_t s)
{
    return in_range(s) ? or_empty(k_row[s].word) : "";
}

const char *panel_state_since(panel_state_t s, panel_offline_mode_t m)
{
    if (!in_range(s)) return "";
    if (s == PANEL_ST_OFFLINE) {
        if ((int)m < 0 || (int)m >= (int)PANEL_OFF_COUNT) return "";
        return k_offline_since[m];
    }
    return or_empty(k_row[s].since);
}

r2_lights_state_t panel_state_lights(panel_state_t s)
{
    switch (s) {
    case PANEL_ST_DANGER:    return R2L_DANGER;
    case PANEL_ST_OFFLINE:   return R2L_OFFLINE;
    case PANEL_ST_ATTENTION: return R2L_ATTENTION;
    case PANEL_ST_MISHEARD:  return R2L_MISHEARD;
    case PANEL_ST_WAITING:   return R2L_WAITING;
    case PANEL_ST_ANSWERING: return R2L_ANSWERING;
    case PANEL_ST_THINKING:  return R2L_THINKING;
    case PANEL_ST_LISTEN:    return R2L_LISTEN;
    case PANEL_ST_IDLE:      return R2L_IDLE;
    case PANEL_ST_SLEEP:     return R2L_SLEEP;
    default:                 return R2L_NONE;   /* WAKING, UNPROVISIONED, junk */
    }
}

uint32_t panel_state_colour(panel_state_t s)
{
    /* 0 is not a legal panel colour -- black on black -- so an out-of-range
     * state renders as nothing rather than as a plausible hue. */
    return in_range(s) ? k_row[s].colour : 0u;
}
