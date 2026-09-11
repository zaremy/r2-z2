/* THE WAKE FRAME'S DECISIONS -- #101 AC3. Pure logic, no LVGL.
 *
 * The wake frame is what the panel shows for the first seconds of a state
 * worth interrupting someone for: the condition, large, in its own colour,
 * and nothing tappable (D-017). `panel_ui.c` draws it; this decides WHEN it is
 * up and WHAT its box says, because both are exactly the kind of decision that
 * looks right on a screenshot whether or not it works.
 *
 * WHEN, as the v5 reference implements it (`pick()` and the 6000 ms timer in
 * panel-v5-interactive.html):
 *   - it fires on ENTERING a state that wakes (panel_state_wakes: danger,
 *     offline, attention), and on no other state, ranked or not;
 *   - it decays after 6000 ms, and does not re-fire while the state holds;
 *   - a tap dismisses it early;
 *   - leaving the state takes it down at once -- a wake frame outliving its
 *     condition would be the panel announcing a fault that has cleared.
 *
 * "Entering" includes a change of offline VIEW. D-017 Amendment B makes the
 * three views one state, but the view is what tells you which thing broke,
 * so R2 -> LLM is a new fault and gets a new interruption -- the same reason
 * `panel_ui.c` repaints on a mode change. */
#ifndef PANEL_WAKE_H
#define PANEL_WAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "panel_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 6000 ms, from the reference's own timer rather than its caption -- the two
 * agree ("STATES WAKE THEN DECAY 6S"), and the timer is the one that runs. */
#define PANEL_WAKE_DECAY_MS 6000u

typedef struct {
    panel_state_t        st;         /* last state seen */
    panel_offline_mode_t mode;       /* and its view */
    uint32_t             since_ms;   /* when the current one was entered */
    bool                 showing;
    bool                 fired;      /* rose since last taken */
} panel_wake_t;

/* Start with no state seen, so the first real one counts as an entry. */
void panel_wake_init(panel_wake_t *w);

/* Feed the current state every tick. Returns whether the frame is up. */
bool panel_wake_step(panel_wake_t *w, panel_state_t st,
                     panel_offline_mode_t mode, uint32_t now_ms);

/* Did the frame (re)fire since the last call? Read-and-clear. True on
 * entering a waking state, including an escalation while it is already up
 * -- the one moment it must be repainted. The renderer paints on THIS,
 * not on its own change detector, so the two can never disagree about
 * whether a frame that is showing was ever drawn. */
bool panel_wake_take_fired(panel_wake_t *w);

/* A tap. Holds until the state next changes -- dismissing is not muting. */
void panel_wake_dismiss(panel_wake_t *w);

bool panel_wake_showing(const panel_wake_t *w);

/* THE BOX: a subject set large, and the line beside it that says what it is.
 *
 * Returns false when the state has no box, and that is most of them: the
 * reference boxes a fault CODE for danger and a battery PERCENT for
 * attention, and this board has a source for neither. A box is the most
 * confident thing on the frame -- 72 px, in the fault colour -- so it is the
 * last place to put a number nobody measured. `offline` has one because its
 * subject is structural: WHICH link, which the view already knows. */
bool panel_wake_box(panel_state_t st, panel_offline_mode_t mode,
                    const char **subject, const char **what);

/* "DOWN 4S" / "DOWN 12M" / "DOWN 3H" / "DOWN 2D" -- the box's second line.
 * The largest unit that reads as a whole number, floored: a link down for
 * 119 s says 1M, not 2M, because rounding up would claim time that has not
 * passed. Always NUL-terminated; truncates rather than overruns.
 *
 * Only the seconds branch reaches the glass today: the count is drawn while
 * the frame is up, which is the first 6 s of an absence that began 4.1 s
 * earlier. The larger units are for the same count on a surface that
 * outlasts the frame, and are tested so that surface inherits them right. */
void panel_wake_format_down(uint32_t ms, char *buf, size_t n);

#ifdef __cplusplus
}
#endif
#endif /* PANEL_WAKE_H */
