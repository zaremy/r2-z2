/* The resting STATUS frame — #101 child 3, on real glass with real data.
 *
 * D-017: the panel is an instrument. A fixed header plus FOUR ROWS THAT NEVER
 * SCROLL, each answering one question, each a tap target. Rows are
 * LINK · R2 · STORAGE · BRAIN (Amendment A dropped PACK: #93 found no cell on
 * the board).
 *
 * The character boundary in CLAUDE.md binds here and is worth restating,
 * because a screen on a droid invites exactly the thing it forbids: this is a
 * SERVICE PANEL, not a face. No eyes, no expression, no character rendering.
 * R2's body is the character interface; this is the door you open to find out
 * what is wrong.
 *
 * And the corollary that constrains what a row may be: NOTHING THAT NEEDS A
 * GLANCE MAY LIVE ONLY HERE. Nobody reads a diagnostic display to discover
 * whether anything is wrong. The LED says THAT something is up; this says
 * WHAT. So a row may be the only place a detail lives, but never the only
 * place a problem is announced.
 */
#ifndef PANEL_UI_H
#define PANEL_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "r2_telemetry.h"
#include "panel_service.h"   /* panel_op_t */

#ifdef __cplusplus
extern "C" {
#endif

/* The four-row severity API is GONE, with the four rows it served.
 *
 * `panel_sev_t`, `sev_colour()` and `panel_sev_name()` existed only to colour
 * LINK / R2 / STORAGE / BRAIN. The v5 status page is a face, so nothing calls
 * them -- and a severity API left exported in a header is one a reader will
 * reasonably build against. Deleted rather than reserved: the face carries its
 * own colour per state, and the fault chain its own four-way `chain_t`.
 *
 * D-017's severity RANK is untouched by this. It orders which state wins a
 * conflict and is a different thing from a row's colour. */

/* Build the frame. Call once, with the display already started and locked. */
void panel_ui_create(void);

/* Redraw from telemetry. Safe to call often; only touches what changed.
 * Must hold the LVGL lock. Returns true if anything on the frame actually
 * changed -- which is what resets the burn-in dim timer, so that something
 * worth looking at is never dimmed the instant it appears. */
bool panel_ui_update(const r2_telemetry_t *t, uint32_t now_ms);

/* Burn-in mitigation (#101 AC9). Call every UI tick.
 *
 * `state_changed` resets the dim timer -- something the operator would want to
 * see must not be dimmed the instant it appears. The brightness setter is
 * injected rather than called directly so this file stays free of the BSP and
 * the behaviour is inspectable without a panel. */
typedef void (*panel_brightness_fn)(int percent);
void panel_ui_burn_in(uint32_t now_ms, bool state_changed,
                      panel_brightness_fn set_brightness);

/* For evidence and tests: which of the 4 drift positions is current, and
 * whether the panel is currently dimmed. */
int  panel_ui_drift_step(void);

/* The active (undimmed) brightness percent, so nothing hardcodes a second
 * copy of it. */
int  panel_ui_full_brightness(void);
bool panel_ui_is_dimmed(void);

/* The three lateral pages (vault Prototypes/README.md:18). */
void        panel_ui_show_page(int page);
int         panel_ui_page(void);

/* The number of lateral pages. Exported so callers clamp against the real
 * count rather than a literal: a hardcoded last-index makes a page added
 * later unreachable, with no compile error and no test to catch it. */
int         panel_ui_page_count(void);
const char *panel_ui_page_name(int page);

/* The wake frame (#101 AC3). Showing: whether it is on the glass right now.
 * Dismiss: a tap took it down -- it stays down until the state next
 * changes, so dismissing one fault never mutes the next. */
bool        panel_ui_wake_showing(void);
void        panel_ui_wake_dismiss(void);

/* NAVIGATION (#101 child 6). A swipe is +1 (left: the next page) or -1
 * (right: back). Inside a SERVICE interior a right swipe is BACK and a
 * left swipe does nothing -- the reference reserves horizontal swipe for
 * back in there. A tap is in panel pixels, where the finger went down. */
void        panel_ui_swipe(int dir);
void        panel_ui_tap(int x, int y);
/* Call on every finger LANDING, before the tap it may become is routed:
 * it records whether the list was moving at that moment. */
void        panel_ui_note_press(void);

/* THE LADDER'S RUN (#101 child 6, and #168 part 2). The renderer never sends
 * anything: a tap leaves a request here, main.c takes it, sends it through the
 * gate like everything else, and reports how many ops the gate admitted.
 * Returns the tier, or -1 when nothing was asked for.
 *
 * `op` IS WHICH OP OF THAT TIER, and it is the half that makes this consent
 * rather than rationing. A rung no longer runs a tier -- it opens the tier's
 * named ops and the operator taps one, so what arrives here is a thing they
 * chose by name. Written only when the return value is >= 0, so a caller that
 * ignores the return cannot act on a stale op. */
int         panel_ui_take_probe_request(unsigned *gen, panel_op_t *op);

/* THE STOP, across the same seam. Read-and-clear on the link task; the report
 * comes back the other way. `sent` is how many of the three halts reached the
 * transport -- 3 is the only complete stop. */
bool        panel_ui_take_stop_request(void);
void        panel_ui_stop_sent(unsigned sent, bool link_up, uint32_t now_ms);
void        panel_ui_probe_sent(unsigned expected, uint32_t now_ms, unsigned gen,
                                bool link_up);

#ifdef PANEL_SHOT_TOUR
/* Screenshot-tour build only. `panel_ui_debug_open_row` scrolls a SERVICE row
 * into view and taps its centre through the real hit test, returning false if
 * that did not open the row asked for. `panel_ui_debug_showing` answers what
 * is on the glass NOW, and `panel_ui_debug_restore` puts a view back after an
 * interruption -- the wake frame can fire mid-tour. The two non-interior
 * views have their own ids; anything >= 0 is a SERVICE row. */
#define PANEL_TOUR_STATUS (-2)
#define PANEL_TOUR_MENU   (-1)
/* THE OP LIST IS ITS OWN VIEW, not "HARDWARE TEST with something over it".
 * s_int_open stays HW TEST while the overlay is up, so asking for row 3 would
 * answer yes with the op list gone -- and panel_ui_debug_restore reopens the
 * interior, which closes the overlay. The rig would then photograph a bare
 * ladder under the op list's name, having checked twice that it was right. */
#define PANEL_TOUR_OPS    (-3)
void        panel_ui_debug_to_menu(void);
bool        panel_ui_debug_open_row(int row);
/* Tap a ladder rung through the hit test; false if it was locked, off screen,
 * or the tap did not register a request. */
bool        panel_ui_debug_open_rung(int rung);
bool        panel_ui_debug_run_op(int row);
int         panel_ui_debug_op_count(void);
bool        panel_ui_debug_probe_settled(void);
bool        panel_ui_debug_probe_passed(void);
bool        panel_ui_debug_showing(int want);
void        panel_ui_debug_restore(int want);
#endif

/* The link task's period, which IS the keepalive: one number, so the
 * R2 LINK interior cannot describe a period the link does not use. */
#define PANEL_KEEPALIVE_MS 3000u


#ifdef __cplusplus
}
#endif
#endif /* PANEL_UI_H */
