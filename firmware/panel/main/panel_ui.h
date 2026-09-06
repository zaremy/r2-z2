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

#include "r2_telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Severity, which is the only thing the rows are sorted by and the only thing
 * the wake frame reads. Ordered: worse is greater. */
typedef enum {
    PANEL_OK = 0,
    PANEL_UNKNOWN,   /* we cannot say -- distinct from fine, and drawn as such */
    PANEL_WARN,
    PANEL_BAD,
} panel_sev_t;

/* Build the frame. Call once, with the display already started and locked. */
void panel_ui_create(void);

/* Redraw from telemetry. Safe to call often; only touches what changed.
 * Must hold the LVGL lock. */
void panel_ui_update(const r2_telemetry_t *t, uint32_t now_ms);

const char *panel_sev_name(panel_sev_t s);

#ifdef __cplusplus
}
#endif
#endif /* PANEL_UI_H */
