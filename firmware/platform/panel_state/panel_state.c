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
 *   `offline` is AMBER here and RED on the body. D-012 narrowed red to danger
 *   and stop ONLY, citing IEC 60073, and moved pending-attention to yellow. A
 *   dropped link is not danger -- nothing is going to hurt him because the
 *   radio stopped answering -- so on the glass it is the definition of
 *   needs-monitoring. The body table still carries the pre-narrowing red; that
 *   is a discrepancy in the body doc, flagged rather than silently propagated.
 *
 *   `sleep` is grey here and dim blue on the body. Same rule: the semantics
 *   are "nothing is engaged and this is fine", and grey delivers that on a
 *   panel where a 0.2-scaled blue would read as a fault-coloured smudge.
 */
#define C_RED     0xF0574A
#define C_AMBER   0xF2B23C
#define C_BLUE    0x4A7BE8
#define C_CYAN    0x3FD8E8
#define C_GREEN   0x4ED18B
#define C_GREY    0x7C8A8D

typedef struct {
    const char *name;
    const char *word;
    const char *since;
    uint32_t    colour;
} row_t;

/* Indexed by panel_state_t. The compile-time size check below is the only
 * thing standing between "somebody added a state" and "the panel reads a row
 * off the end of this table", so it is not decoration. */
static const row_t k_row[PANEL_ST_COUNT] = {
    [PANEL_ST_DANGER]       = { "danger",       "DANGER",    "R2 FAULT",        C_RED   },
    [PANEL_ST_OFFLINE]      = { "offline",      "OFFLINE",   NULL,              C_AMBER },
    [PANEL_ST_ATTENTION]    = { "attention",    "ATTENTION", "NEEDS YOU",       C_AMBER },
    [PANEL_ST_MISHEARD]     = { "misheard",     "MISHEARD",  "SAY AGAIN",       C_AMBER },
    [PANEL_ST_WAITING]      = { "waiting",      "WAITING",   "ON YOU",          C_BLUE  },
    [PANEL_ST_THINKING]     = { "thinking",     "THINKING",  "WORKING",         C_CYAN  },
    /* state `listen`, word "LISTENING" -- Amendment B renamed the state, not
     * the rendering. This is the one place the two differ. */
    [PANEL_ST_LISTEN]       = { "listen",       "LISTENING", "GO AHEAD",        C_CYAN  },
    [PANEL_ST_IDLE]         = { "idle",         "IDLE",      "NOTHING ENGAGED", C_GREEN },
    [PANEL_ST_SLEEP]        = { "sleep",        "ASLEEP",    "",                C_GREY  },

    /* `released` says what WE did, never what he is (D-023). "STOPPED HOLDING
     * HIM AWAKE" rather than "ASLEEP": the same observation that looks like
     * sleep is what a failure looks like, and the panel must not promote one
     * to the other. */
    [PANEL_ST_RELEASED]     = { "released",     "RELEASED",  "KEEPALIVE OFF",   C_GREY  },
    [PANEL_ST_WAKING]       = { "waking",       "WAKING",    "FINDING HIM",     C_BLUE  },
    [PANEL_ST_UNPROVISIONED]= { "unprovisioned","SETUP",     "NOT PAIRED YET",  C_BLUE  },
};

/* `offline`'s reason is the whole point of the state having display modes:
 * one state, and the screen says which thing is unreachable. */
static const char *k_offline_since[PANEL_OFF_COUNT] = {
    [PANEL_OFF_R2]  = "R2 LINK DOWN",
    [PANEL_OFF_NET] = "NO INTERNET",
    [PANEL_OFF_LLM] = "LLM DOWN",
};

static bool in_range(panel_state_t s)
{
    return (int)s >= 0 && (int)s < (int)PANEL_ST_COUNT;
}

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

uint32_t panel_state_from_link(bool link_up, bool link_down,
                               panel_state_t *out_state,
                               panel_offline_mode_t *out_mode)
{
    uint32_t active = 0;
    panel_offline_mode_t mode = PANEL_OFF_COUNT;

    if (link_down) {
        active |= 1u << PANEL_ST_OFFLINE;
        /* PANEL_OFF_R2 and not NET or LLM: the BLE link is the only one this
         * build has. Reporting NET here would be the panel diagnosing a
         * subsystem it cannot see. */
        mode = PANEL_OFF_R2;
    } else if (link_up) {
        /* `idle`, never `listen` / `thinking` / `waiting`. Those need a
         * reasoning layer that does not run on this board, and claiming one
         * would be the panel inventing an interaction that never happened. */
        active |= 1u << PANEL_ST_IDLE;
    }

    const panel_state_t won = panel_state_resolve(active);

    /* No ranked state means the link is mid-transition. `waking` is unranked
     * BY DESIGN (D-023), so it can never win a contest against a real fault;
     * being chosen deliberately here is the only way an unranked state ever
     * reaches the glass. */
    if (out_state) *out_state = (won == PANEL_ST_COUNT) ? PANEL_ST_WAKING : won;
    if (out_mode)  *out_mode  = mode;
    return active;
}

const char *panel_state_name(panel_state_t s)
{
    return in_range(s) ? k_row[s].name : "";
}

const char *panel_state_word(panel_state_t s)
{
    return in_range(s) ? k_row[s].word : "";
}

const char *panel_state_since(panel_state_t s, panel_offline_mode_t m)
{
    if (!in_range(s)) return "";
    if (s == PANEL_ST_OFFLINE) {
        if ((int)m < 0 || (int)m >= (int)PANEL_OFF_COUNT) return "";
        return k_offline_since[m];
    }
    return k_row[s].since ? k_row[s].since : "";
}

uint32_t panel_state_colour(panel_state_t s)
{
    /* 0 is not a legal panel colour -- black on black -- so an out-of-range
     * state renders as nothing rather than as a plausible hue. */
    return in_range(s) ? k_row[s].colour : 0u;
}
