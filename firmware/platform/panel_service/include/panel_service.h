/* WHAT THE SERVICE MENU SAYS -- #101 child 6. Pure logic, no LVGL.
 *
 * The seven interiors behind SERVICE are the panel's "behind the door" half
 * (CLAUDE.md: indicator outside, diagnostics behind the door), which makes
 * them the place a person goes to find out what is wrong -- and therefore the
 * place a plausible invented value does the most damage. The v5 reference
 * fills every row with sample data ("HOMENET", "API KEY SET", "41°C"); this
 * module decides what THIS board can honestly put there, and that decision is
 * worth host tests in a way that drawing it is not.
 *
 * Three rules, and the tests pin each one:
 *   1. NO READING OUTLIVES ITS LINK (#101 AC7). Anything R2 told us goes to
 *      "----" the moment the link is not up, through r2_telemetry's own
 *      displayable() rather than a second copy of that rule.
 *   2. A THING THIS BUILD DOES NOT HAVE SAYS SO. No Wi-Fi stack, no API key,
 *      no camera, no microphone: the row reads NOT IN BUILD in the no-claim
 *      grey, never a sample value.
 *   3. THE LADDER NEVER BUNDLES (AC6). Every tier is its own rung, anything
 *      above the gate's ceiling is LOCKED, and there is no "all" rung.
 */
#ifndef PANEL_SERVICE_H
#define PANEL_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "r2_telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The SERVICE rows, top to bottom. NETWORK is first and is NOT an interior:
 * the epic is explicit that it jumps to the lateral NETWORK page rather than
 * being a second copy of it. The reference's last row, EXIT TO OS, is absent
 * on purpose -- D-022 removed the vendor launcher, so there is no OS to exit
 * to, and a row that promised one would be a door painted on a wall. */
typedef enum {
    PANEL_SVC_NETWORK = 0,
    PANEL_SVC_R2_LINK,
    PANEL_SVC_DIAGNOSTICS,
    PANEL_SVC_HW_TEST,
    PANEL_SVC_PROVISIONING,
    PANEL_SVC_VOICE,
    PANEL_SVC_CAMERA,
    PANEL_SVC_ABOUT,
    PANEL_SVC_COUNT,
} panel_svc_t;

typedef enum {
    PANEL_SVC_JUMP = 0,    /* goes somewhere else (NETWORK) */
    PANEL_SVC_LIST,        /* key / value rows */
    PANEL_SVC_NOTE,        /* a null state: two lines, nothing to list */
    PANEL_SVC_LADDER,      /* HARDWARE TEST */
} panel_svc_kind_t;

/* What colour a value may claim. There is deliberately no RED: D-012 keeps it
 * for danger and stop, and nothing behind this door is either. NONE is the
 * panel's no-claim grey (D-012 Amendment A) -- absence, not a colour. */
typedef enum {
    PANEL_TONE_PLAIN = 0,  /* a fact, no verdict */
    PANEL_TONE_GOOD,       /* healthy */
    PANEL_TONE_WARN,       /* needs monitoring */
    PANEL_TONE_NONE,       /* we cannot say */
} panel_tone_t;

#define PANEL_SVC_VAL_LEN   16
#define PANEL_SVC_MAX_ROWS   8

typedef struct {
    const char  *key;
    char         val[PANEL_SVC_VAL_LEN];
    panel_tone_t tone;
} panel_kv_t;

/* Everything the rows are made from, gathered by the caller so this file
 * reads no clock and touches no hardware. */
typedef struct {
    const r2_telemetry_t *tm;
    uint32_t    now_ms;
    uint32_t    keepalive_ms;   /* main.c's keepalive period, not a copy of it */
    int         ceiling;        /* the gate's ceiling, as an r2_tier_t index */
    const char *panel_fw;       /* this app's version string */
    uint32_t    flash_mb, psram_mb;
    /* INTERNAL free heap. Total free counts PSRAM, which on this board is 8 MB
     * -- a number that reads reassuring while the internal heap, the one that
     * actually runs out, is small. Seen on the glass: FREE HEAP 7203 KB. */
    uint32_t    heap_kb;
    /* The touch controller's chip id as read at boot from I2C 0x15, or 0
     * when it did not answer. It is the board-revision probe
     * (board-revision.md): an answer at 0x15 IS a V2 board. */
    uint8_t     touch_id;
} panel_svc_facts_t;

const char       *panel_service_title(panel_svc_t s);   /* "" out of range */
panel_svc_kind_t  panel_service_kind(panel_svc_t s);

/* Fill a LIST interior's rows. Returns how many were written, never more than
 * `max`; 0 for anything that is not a LIST or when `f` is NULL. */
int panel_service_rows(panel_svc_t s, const panel_svc_facts_t *f,
                       panel_kv_t *out, int max);

/* A NOTE interior's two lines. False for anything that is not a NOTE. */
bool panel_service_note(panel_svc_t s, const char **line1, const char **line2);

/* The menu row's status square for R2 LINK: GOOD while linked, WARN otherwise.
 * The only menu row that makes a claim, because it is the only one whose
 * interior can go wrong while you are looking at the menu. */
panel_tone_t panel_service_link_tone(const r2_telemetry_t *tm);

/* THE PERMISSION LADDER, bring-up order: READ, LEDS, AUDIO, DOME, STANCE,
 * LOCOMOTION (CLAUDE.md). `ceiling` is the gate's tier index; every rung above
 * it is locked, an out-of-range ceiling locks everything, and LOCOMOTION is
 * locked at every ceiling because it is not a rung of the gate at all -- it
 * has no allowlist entry and is refused as unlisted. Returns the rung count. */
/* A CEILING IS NOT AN ORDER, WHICH IS WHAT THE RULE ACTUALLY SAYS (#168).
 * CLAUDE.md states bring-up as a SEQUENCE -- "read-only -> LEDs -> audio ->
 * small dome -> stance -> locomotion" -- and a ceiling only caps how far up
 * you may reach. At ceiling = STANCE the old ladder marked all five allowed,
 * so an operator could tap STANCE having never once run DOME: the rule's exact
 * prohibition. It was invisible while the ceiling sat at READ and only one
 * rung was tappable, which is why it had to be fixed before the first commit
 * that raises it.
 *
 * `done` is the set of tiers already exercised this session, bit i for tier i.
 * A rung opens only when the ceiling admits it AND every tier below it is in
 * that set. READ has nothing below it, so it opens whenever the ceiling
 * allows -- the sequence has to start somewhere.
 *
 * PER SESSION, NOT PERSISTED, and that is the safety-relevant half. A stored
 * "DOME ran fine" would unlock STANCE on a droid nobody has looked at since
 * last week -- and he has no resting posture, parks himself in bipod a minute
 * after the link drops, and an animation has put him on the floor once already
 * (CLAUDE.md). Re-walking the ladder after a boot costs one tap per rung and
 * re-proves what a stored bit only remembers.
 *
 * The rung says WHY it is shut, because "locked" and "not yet" ask different
 * things of the operator: one is a ceiling they must deliberately raise, the
 * other is a rung they have simply not reached. */
#define PANEL_LADDER_RUNGS 6
typedef enum {
    PANEL_RUNG_OPEN = 0,     /* tappable */
    PANEL_RUNG_CEILING,      /* above the gate's ceiling */
    PANEL_RUNG_SEQUENCE,     /* within the ceiling, but a lower tier is unrun */
} panel_rung_block_t;
typedef struct {
    const char        *tier;
    bool               allowed;
    panel_rung_block_t why;      /* PANEL_RUNG_OPEN exactly when allowed */
} panel_rung_t;

/* Bit i marks tier i as exercised; `done` is a mask of these. */
#define PANEL_RUNG_BIT(tier) (1u << (tier))

int panel_service_ladder(int ceiling, uint32_t done,
                         panel_rung_t out[PANEL_LADDER_RUNGS]);

/* How many of R2's three readings -- battery, dome, version -- have arrived
 * SINCE `since_ms`, on a link that is still up. This is what a hardware test
 * counts, instead of raw reply totals, which include every reply since boot.
 *
 * IT NARROWS THE WINDOW; IT DOES NOT CLOSE IT. The panel's own periodic polls
 * land in these same three stamps, so a reply the test did not ask for can
 * still count toward it. Closing that needs per-request accounting the
 * telemetry layer does not carry -- see panel_probe.h, which says what a PASS
 * is therefore worth. Wrap-safe. */
unsigned panel_service_fresh_readings(const r2_telemetry_t *tm, uint32_t since_ms,
                                      uint32_t now_ms);

/* The ceiling's name for the ladder header, "" when out of range. */
const char *panel_service_ceiling_name(int ceiling);

#ifdef __cplusplus
}
#endif
#endif /* PANEL_SERVICE_H */
