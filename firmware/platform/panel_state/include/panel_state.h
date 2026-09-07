/* WHAT THE PANEL CAN SAY, and which thing wins when several are true.
 *
 * #101 child 5, AC2 / AC2b / AC2c. Pure logic, no LVGL: the rank is the part
 * worth testing and a rank you can only exercise by drawing it is a rank
 * nobody tests. `panel_ui.c` renders what this decides.
 *
 * THE STATE UNIVERSE IS NOT OURS TO INVENT. D-017 Amendment B ruled
 * `docs/behaviour-states.md` the single source of truth for what states exist,
 * and the panel a VIEW of them. Two consequences are load-bearing here:
 *
 *   1. `offline` is ONE state with three DISPLAY MODES (R2 / NET / LLM). Not
 *      three states. The droid is offline; the panel says which thing is
 *      unreachable, because the screen is the detail view and the LED is the
 *      glance. Three states would have put that distinction on the body, where
 *      nothing can render it.
 *   2. `wake` is a BEHAVIOUR state and is not a panel state at all. It is what
 *      he does; D-023's `waking` is what the panel shows while the link comes
 *      up. One word apart, and most of why the two documents looked like they
 *      contradicted each other.
 *
 * The DISPLAY WORD is a separate thing from the STATE NAME, and they differ in
 * exactly one place: the state is `listen`, the word on the glass is
 * "LISTENING". Amendment B renamed the state, not the rendering.
 */
#ifndef PANEL_STATE_H
#define PANEL_STATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* THE NINE RANKED STATES, most severe first, then the three unranked.
 *
 * The enum order IS D-017's severity rank for the first nine, deliberately:
 * a rank held in a separate table is a rank that can silently disagree with
 * the enum. `PANEL_ST_RANKED_COUNT` is the boundary, and the three below it
 * are outside the ordering BY DESIGN (D-023) -- not unranked because nobody
 * got round to ranking them. The wake frame exists to surface severity on a
 * running system, and none of these three is a severity: `released` is
 * deliberate rather than wrong, `waking` resolves itself in seconds, and
 * `unprovisioned` is a mode the panel sits in rather than an event. Ranking
 * any of them would let the panel wake the household to announce that it is
 * doing what it was told. */
typedef enum {
    PANEL_ST_DANGER = 0,
    PANEL_ST_OFFLINE,
    PANEL_ST_ATTENTION,
    PANEL_ST_MISHEARD,
    PANEL_ST_WAITING,
    PANEL_ST_THINKING,
    PANEL_ST_LISTEN,
    PANEL_ST_IDLE,
    PANEL_ST_SLEEP,

    PANEL_ST_RANKED_COUNT,          /* the closed set ends here */

    PANEL_ST_RELEASED = PANEL_ST_RANKED_COUNT,
    PANEL_ST_WAKING,
    PANEL_ST_UNPROVISIONED,

    PANEL_ST_COUNT,
} panel_state_t;

/* The three ways ONE `offline` presents. Applies to no other state. */
typedef enum {
    PANEL_OFF_R2 = 0,               /* no route to R2 -- the BLE link */
    PANEL_OFF_NET,                  /* no internet */
    PANEL_OFF_LLM,                  /* no reasoning service */
    PANEL_OFF_COUNT,
} panel_offline_mode_t;

/* The BACKPACK DISPLAY axis (AC2c), which is not the R2 axis above.
 *
 * D-023 named two axes and observed that the reason "power everything except
 * the backpack down" felt like one switch is that only one of them had ever
 * been named. Keeping them in one enum would rebuild that confusion in code:
 * `released` is a fact about R2, `resting` is a fact about this screen, and
 * they are independently true. */
typedef enum {
    PANEL_DISP_RESTING = 0,         /* lit, dimmed by the burn-in timer */
    PANEL_DISP_UI_ASLEEP,           /* screen dark, board awake */
    PANEL_DISP_OFF,                 /* board down */
    PANEL_DISP_COUNT,
} panel_display_state_t;

/* Severity rank: 0 is most severe, and -1 means "outside the ordering".
 * -1 is not "least severe" -- an unranked state must never lose or win a
 * comparison by accident, which is why callers must test for it. */
int panel_state_rank(panel_state_t s);

bool panel_state_is_ranked(panel_state_t s);

/* Does this state fire the wake frame? True for exactly `danger`, `offline`
 * and `attention` (#101 AC3), and false for every unranked state. */
bool panel_state_wakes(panel_state_t s);

/* Resolve a set of simultaneously-true states to the one the panel shows.
 *
 * `active` is a bitmask of (1u << panel_state_t). UNRANKED BITS ARE IGNORED:
 * they cannot win a conflict against a ranked state (AC2b), so a caller that
 * wants one displayed sets it alone. Returns PANEL_ST_COUNT when no ranked
 * state is set, which the caller must handle -- there is no default state
 * here, because inventing one is how a panel ends up asserting `idle` about a
 * droid it has heard nothing from. */
panel_state_t panel_state_resolve(uint32_t active);

/* WHAT THE LINK ALONE IMPLIES.
 *
 * This lives here rather than in the renderer for one reason: it is the only
 * part of the live path with a decision in it, and a decision inside
 * `panel_ui.c` can only be exercised by drawing it on a panel. The rendering
 * is then a pure function of the result -- which is also why a screenshot of
 * `IDLE` can never be evidence that the derivation works. Both pictures look
 * the same whether the wiring is live or dead.
 *
 * `link_up`/`link_down` are the two link facts this board has. Anything else
 * -- scanning, connecting, handshaking -- is a transition, and both false is
 * how a caller says so.
 *
 * Writes the resolved state to `*out_state` and, when that is `offline`, the
 * view to `*out_mode`. Returns the mask it built, so a caller (and a test)
 * can see WHICH states were candidates rather than only which one won. */
uint32_t panel_state_from_link(bool link_up, bool link_down,
                               panel_state_t *out_state,
                               panel_offline_mode_t *out_mode);

/* Rendering. `since` for `offline` depends on the display mode; for every
 * other state the mode is ignored. */
const char *panel_state_name(panel_state_t s);        /* the STATE id */
const char *panel_state_word(panel_state_t s);        /* the word on the glass */
const char *panel_state_since(panel_state_t s, panel_offline_mode_t m);
uint32_t    panel_state_colour(panel_state_t s);      /* 0xRRGGBB */

#ifdef __cplusplus
}
#endif
#endif /* PANEL_STATE_H */
