#include "panel_ui.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"
#include "panel_state.h"

/* THE v5 PALETTE. Taken from the reference prototype the vault calls "the
 * reference the LVGL firmware should match", not invented.
 *
 * The panel was built to the epic's PROSE -- row names, page names -- and
 * never to the design itself, which is why it looked nothing like it: a
 * neutral black/grey scheme with generic accents, where v5 is a cool
 * blue-grey system. Values below are lifted from panel-v5-interactive.html. */
#define V5_GROUND     0x14191B   /* page ground */
#define V5_HAIRLINE   0x14191B   /* 1 px row separator */
#define V5_RULE       0x1E2628   /* 2 px separator, chain line */
#define V5_SURFACE    0x2A3438   /* borders, inactive dots */
#define V5_DIM        0x43535A
#define V5_MID        0x5E7276   /* captions */
#define V5_LABEL      0x7C8A8D   /* key labels, active dot */
#define V5_TEXT       0xF2F6F7   /* values */
#define V5_TEXT_HI    0xE8F2F3
/* The STATE colours are not here. They live in `panel_state.h` as PANEL_C_*,
 * and this file reads those rather than keeping its own copy: the face and the
 * chain pips must agree on amber, and once the state table moved to another
 * file a duplicate set of hexes here is how they would stop agreeing. What
 * remains below is chrome -- ground, rules, labels -- which no state owns. */


/* Panel geometry, MEASURED not assumed (#104, board-revision.md):
 * 368 x 448 px, 29.0 x 35.3 mm at 322 ppi. A 44 pt tap target is 87 px here,
 * which is why the rows are 87 tall -- the row IS the tap target, so child 4
 * adds behaviour to these rows rather than re-laying them out.
 *
 * The V2 panel's 16 px column offset applies to the DISPLAY, not to touch
 * (#104). Nothing here compensates for it, deliberately. */
#define PANEL_W       368
#define PANEL_H       448
#define HEADER_H       64
#define ROW_H          87
#define ROW_PAD        14



static lv_obj_t *s_screen;
static lv_obj_t *s_root;

/* THE THREE LATERAL PAGES: STATUS, SERVICE, NETWORK.
 *
 * Named in the vault's Prototypes/README.md:18 -- "Three lateral pages --
 * STATUS / SERVICE / NETWORK, swipe or tap the dots." I spent two loop ticks
 * calling these undefined and building around the gap, because I had assumed
 * the gitignored vault was unreachable. It is a directory.
 *
 * What is built here is the FRAME of each page and the navigation between
 * them, not the contents of SERVICE and NETWORK. SERVICE's seven interiors are
 * named in #101 and are rendered as titles; their interiors are child 6.
 * NETWORK's contents are not defined in any source I can find, so the page
 * says so rather than inventing them -- the same choice as STORAGE and BRAIN
 * on the status page, and the one D-017 Amendment B just ruled for. */
/* THE STATE FACE, from v5. This is what the STATUS page actually is, and
 * getting it wrong is why the panel "looked nothing like the spec".
 *
 * The four rows LINK / R2 / STORAGE / BRAIN were built as the whole page. AC4
 * calls them TOP-BAR rows, and the prototype does not have a BRAIN row at all
 * -- the status page is a FACE: one large state word, the reason under it, a
 * key/value pair for power and dome, and a four-node MIC/R2/NET/LLM fault
 * chain along the bottom.
 *
 * `offline` appears three times here with different chains and reasons. Those
 * are the three DISPLAY MODES of one canonical `offline` that D-017 Amendment
 * B ruled on -- the chain is exactly the mechanism by which one state shows
 * which thing is unreachable. The prototype was already right about this; the
 * ruling made it legible. */
typedef enum { CH_OK = 0, CH_DOWN, CH_FAULT, CH_UNK } chain_t;

/* THE THREE HARDCODED FACES ARE GONE, replaced by `panel_state`.
 *
 * They carried their own word, reason and colour, which made this file a
 * second normative source for a universe D-017 Amendment B had already ruled
 * belongs to `docs/behaviour-states.md`. Twelve states now come from one
 * table with host tests over the severity rank, and this file renders it.
 *
 * The `pwr_known` / `dome_known` flags went with them, and their removal is
 * a fix rather than a tidy: they duplicated a rule `r2_telemetry_displayable`
 * already enforces -- no reading survives its link -- so the panel had two
 * places that could disagree about whether a value was showable. One of them
 * was a hand-maintained bool per face, which is the half that would have
 * drifted the first time somebody added a state.
 *
 * What stays here is the part that IS the rendering: which chain pips light,
 * and whether the chain appears at all. */

/* The chain appears for `danger` and `offline` ONLY.
 *
 * Not for `attention`, although attention also fires the wake frame -- the
 * two are different questions. The wake frame asks "should this interrupt
 * you"; the chain asks "which link in the path is broken", which attention
 * does not answer, because attention is about him and not about the path.
 * v5 draws a chain for exactly these four renderings. */
static bool face_has_chain(panel_state_t st)
{
    return st == PANEL_ST_DANGER || st == PANEL_ST_OFFLINE;
}

/* MIC / R2 / NET / LLM.
 *
 * Everything is CH_UNK unless this build has a source for it, and this build
 * has exactly one: the BLE link to R2. There is no microphone, no network
 * stack and no LLM client here, so a green pip beside any of those three
 * would be a certification of a subsystem that does not exist -- shown, by
 * design, at the very moment a human is looking to find out what is wrong. */
static void face_chain(panel_state_t st, panel_offline_mode_t mode, chain_t out[4])
{
    for (int i = 0; i < 4; i++) out[i] = CH_UNK;

    if (st == PANEL_ST_DANGER) { out[1] = CH_FAULT; return; }
    if (st != PANEL_ST_OFFLINE) return;

    switch (mode) {
    case PANEL_OFF_R2:  out[1] = CH_DOWN; break;
    case PANEL_OFF_NET: out[2] = CH_DOWN; break;
    case PANEL_OFF_LLM: out[3] = CH_DOWN; break;
    default: break;
    }
}

static const char *k_chain_label[4] = { "MIC", "R2", "NET", "LLM" };

static void set_face(panel_state_t st, panel_offline_mode_t mode,
                     const r2_telemetry_t *t, uint32_t now_ms);

enum { PAGE_STATUS = 0, PAGE_SERVICE, PAGE_NETWORK, PAGE_COUNT };
static lv_obj_t *s_page[PAGE_COUNT];
static lv_obj_t *s_pip[PAGE_COUNT];
static int s_page_at = PAGE_STATUS;

static const char *k_page_name[PAGE_COUNT] = { "STATUS", "SERVICE", "NETWORK" };

/* #101: "The 7 interiors from SERVICE are unchanged." NETWORK is deliberately
 * absent -- the epic says it is not an interior, it jumps to the lateral page. */
static const char *k_service_rows[] = {
    "R2 LINK", "DIAGNOSTICS", "HARDWARE TEST", "PROVISIONING",
    "VOICE", "CAMERA", "ABOUT",
};
#define N_SERVICE_ROWS (sizeof k_service_rows / sizeof k_service_rows[0])
static lv_obj_t *s_face_word, *s_face_since, *s_face_swatch;
static lv_obj_t *s_chrome_wifi, *s_chrome_llm, *s_chrome_batt;
#define PWR_BARS 18
#define DIAL_X   132
#define DIAL_Y   198
#define DIAL_D   104
#define DIAL_R   (DIAL_D / 2)
static lv_obj_t *s_pwr_bar[PWR_BARS];
static lv_obj_t *s_dome_needle, *s_dome_hub;
static lv_obj_t *s_chain_row;
static lv_obj_t *s_kv_val[2];                 /* PWR, DOME */
static lv_obj_t *s_chain_pip[4];
static panel_state_t        s_state_now = PANEL_ST_COUNT;
static panel_offline_mode_t s_mode_now  = PANEL_OFF_COUNT;

/* Severity colours: D-012's SEMANTICS, not its values (D-012 Amendment A).
 *
 * An earlier version of this comment argued the opposite -- that the panel
 * should not reuse the LED language because that scheme is tuned for a fixture
 * glanced across a room. Half right. The RULING is that what a colour MEANS is
 * one vocabulary and must not fork (yellow is needs-monitoring on the body and
 * on the glass); what luminance and hue best deliver that meaning is the
 * medium's business, so the values here are chosen for an OLED read at arm's
 * length rather than inherited from a diffused lens.
 *
 * RED IS DANGER AND STOP, ONLY. D-012 narrowed it deliberately, citing IEC
 * 60073, and moved pending-attention to yellow. That constrains this table
 * more than it looks: see the LINK row below. */
/* The page dots. The vault's description is "swipe or tap the dots", so they
 * are an affordance and not decoration -- but they are drawn here and made
 * tappable in child 4 proper. Drawn small and low-contrast: this is an
 * instrument, and a navigation cue that competes with the reading is a
 * navigation cue in the wrong place. */
#define PIP_W   20      /* v5 .dot-a { width: 20px } */
#define PIP_DOT  6      /* v5 .dot   { width: 6px }  */
#define PIP_H    6
#define PIP_PITCH 22

static int pip_slot_x(int i)
{
    return PANEL_W / 2 - (PAGE_COUNT * PIP_PITCH - (PIP_PITCH - PIP_W)) / 2
           + i * PIP_PITCH;
}

static void make_pips(lv_obj_t *parent)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *d = lv_obj_create(parent);
        /* Sized and positioned by set_page: v5 draws the ACTIVE one as a 20x6
         * pill and the rest as 6 px dots, so the geometry is state, not
         * construction. Slots are a fixed 22 px pitch and the dot is centred
         * inside its own slot, which keeps the row centred whichever one is
         * wide. */
        lv_obj_set_size(d, PIP_DOT, PIP_H);
        lv_obj_set_pos(d, pip_slot_x(i) + (PIP_W - PIP_DOT) / 2, PANEL_H - 22);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        s_pip[i] = d;
    }
}

static lv_obj_t *make_page(lv_obj_t *parent)
{
    lv_obj_t *pg = lv_obj_create(parent);
    lv_obj_set_size(pg, PANEL_W, PANEL_H);
    lv_obj_set_pos(pg, 0, 0);
    lv_obj_set_style_bg_color(pg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(pg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pg, 0, 0);
    lv_obj_set_style_radius(pg, 0, 0);
    lv_obj_set_style_pad_all(pg, 0, 0);
    lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    return pg;
}

static void build_service_page(lv_obj_t *pg)
{
    lv_obj_t *t = lv_label_create(pg);
    lv_label_set_text(t, "SERVICE");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xF0F0F4), 0);
    lv_obj_set_pos(t, ROW_PAD, 20);

    /* VERTICAL SCROLL, keeping 87 px rows. Operator ruling, 2026-09-07.
     *
     * Seven 87 px rows are 609 px and the panel is 448. Something had to give,
     * and the alternative on the table was shrinking the rows -- which would
     * have traded the 44 pt tap target #106 actually measured as clickable for
     * a tidier screen. Measured hit accuracy beats a screen that fits.
     *
     * It also matches the design: the vault's v5 says "vertical scroll inside
     * a page, horizontal swipe reserved for back". Accepted cost, stated: the
     * panel stops being wholly glanceable here, because something is always
     * off-screen. */
    lv_obj_t *list = lv_obj_create(pg);
    lv_obj_set_size(list, PANEL_W, PANEL_H - 64 - 30);
    lv_obj_set_pos(list, 0, 64);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    for (unsigned i = 0; i < N_SERVICE_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, PANEL_W, ROW_H);
        lv_obj_set_pos(row, 0, (int)i * ROW_H);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x101014), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x282830), 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *r = lv_label_create(row);
        lv_label_set_text(r, k_service_rows[i]);
        lv_obj_set_style_text_font(r, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(r, lv_color_hex(0xF0F0F4), 0);
        lv_obj_set_pos(r, ROW_PAD + 12, (ROW_H - 28) / 2);
    }
}

static void build_network_page(lv_obj_t *pg)
{
    lv_obj_t *t = lv_label_create(pg);
    lv_label_set_text(t, "NETWORK");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xF0F0F4), 0);
    lv_obj_set_pos(t, ROW_PAD, 20);

    /* Wi-Fi status and provisioning state. Operator ruling, 2026-09-07,
     * replacing the "not specified" placeholder.
     *
     * EVERY FIELD READS "not wired", and that is accurate rather than lazy:
     * this firmware compiles NO Wi-Fi stack at all. The panel holds its own
     * BLE link to R2 (D-006) and nothing else, so there is no SSID to show
     * and no provisioning state to read.
     *
     * The fields are drawn anyway because the page now has a defined SHAPE,
     * and a reader deserves to see what will appear here rather than a blank.
     * Same choice as STORAGE and BRAIN on the status page: name the thing,
     * admit it is not connected, never invent a plausible value. */
    static const char *k_fields[] = { "SSID", "SIGNAL", "ADDRESS", "PROVISIONED" };
    for (unsigned i = 0; i < 4; i++) {
        lv_obj_t *k = lv_label_create(pg);
        lv_label_set_text(k, k_fields[i]);
        lv_obj_set_style_text_font(k, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(k, lv_color_hex(0x8A8A98), 0);
        lv_obj_set_pos(k, ROW_PAD, 78 + (int)i * 72);

        lv_obj_t *v = lv_label_create(pg);
        lv_label_set_text(v, "not wired");
        lv_obj_set_style_text_font(v, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(v, lv_color_hex(0x606060), 0);
        lv_obj_set_pos(v, ROW_PAD, 78 + (int)i * 72 + 24);
    }

    lv_obj_t *n = lv_label_create(pg);
    lv_label_set_text(n, "no Wi-Fi in this build");
    lv_obj_set_style_text_font(n, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(n, lv_color_hex(0x505058), 0);
    lv_obj_set_pos(n, ROW_PAD, PANEL_H - 52);
}

void panel_ui_show_page(int page)
{
    if (page < 0 || page >= PAGE_COUNT) return;
    s_page_at = page;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (s_page[i] == NULL) continue;
        if (i == page) lv_obj_remove_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
        if (s_pip[i]) {
            const bool on = (i == page);
            lv_obj_set_size(s_pip[i], on ? PIP_W : PIP_DOT, PIP_H);
            lv_obj_set_pos(s_pip[i],
                pip_slot_x(i) + (PIP_W - (on ? PIP_W : PIP_DOT)) / 2,
                PANEL_H - 22);
            lv_obj_set_style_bg_color(s_pip[i],
                lv_color_hex(on ? V5_LABEL : V5_SURFACE), 0);
        }
    }
}

int panel_ui_page(void) { return s_page_at; }
int panel_ui_page_count(void) { return PAGE_COUNT; }

const char *panel_ui_page_name(int page)
{
    return (page >= 0 && page < PAGE_COUNT) ? k_page_name[page] : "?";
}


/* THE STATUS FACE (v5). Replaces the four-row page.
 *
 * A SPEC CONFLICT IS BEING RESOLVED HERE AND MUST NOT BE BURIED. D-017 says
 * the panel is "a fixed header plus four rows that never scroll", and #101's
 * AC4 names them LINK / R2 / STORAGE / BRAIN. The v5 prototype -- which the
 * vault calls "the reference the LVGL firmware should match" -- has NO BRAIN
 * ROW and no four-row status page at all. It is a face: one large state word,
 * the reason beneath it, power and dome as key/value, and a four-node
 * MIC/R2/NET/LLM fault chain.
 *
 * Two normative sources describing different screens, exactly like the states
 * disagreement P3 resolved. The operator ruled that the UI must match the
 * panel spec, so the face wins here -- and AC4 needs the same treatment P3
 * got: a ruling, in writing, about which document governs the LAYOUT.
 *
 * Geometry follows v5: 26 px side padding, 52 px key rows, 58 px chain. */
#define V5_PAD 26

static void build_status_face(lv_obj_t *pg)
{
    lv_obj_set_style_bg_color(pg, lv_color_hex(V5_GROUND), 0);

    /* ---- TOP CHROME: wifi, LLM, battery ------------------------------
     * Always present, and v5 is explicit about why: "the system is still
     * running, so the status chrome stays true". It describes the BOARD --
     * its network, its reasoning service, its own power -- not R2, which is
     * what the face below is about. Missing it entirely was the most visible
     * gap between this panel and the spec.
     *
     * BUT: this build has NO Wi-Fi stack, NO LLM client and NO battery ADC.
     * The first version of this bar drew LV_SYMBOL_WIFI and
     * LV_SYMBOL_BATTERY_2 -- a half-full battery glyph is a QUANTITATIVE
     * claim -- in normal text colour, one swipe away from a NETWORK page that
     * says "no Wi-Fi in this build" in so many words. The panel would have
     * contradicted itself inside one build, in exactly the way the dome
     * needle below refuses to.
     *
     * So the three slots exist and are drawn at V5_SURFACE, which is the
     * colour chain_colour() already uses for CH_UNK: unknown is ABSENCE, not
     * a claim. And the battery slot carries the word PWR rather than a level
     * glyph, because there is no level to render. When a real source appears,
     * these get a driver and a colour -- until then they are placeholders
     * that say so. */
    s_chrome_wifi = lv_label_create(pg);
    lv_label_set_text(s_chrome_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_chrome_wifi, lv_color_hex(V5_SURFACE), 0);
    lv_obj_set_pos(s_chrome_wifi, 120, 16);

    s_chrome_llm = lv_label_create(pg);
    lv_label_set_text(s_chrome_llm, "LLM");
    lv_obj_set_style_text_font(s_chrome_llm, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_chrome_llm, lv_color_hex(V5_SURFACE), 0);
    lv_obj_set_pos(s_chrome_llm, 160, 18);

    s_chrome_batt = lv_label_create(pg);
    lv_label_set_text(s_chrome_batt, "PWR");
    lv_obj_set_style_text_font(s_chrome_batt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_chrome_batt, lv_color_hex(V5_SURFACE), 0);
    lv_obj_set_pos(s_chrome_batt, 208, 18);

    /* ---- SWATCH + WORD + SINCE ---------------------------------------- */
    s_face_swatch = lv_obj_create(pg);
    lv_obj_set_size(s_face_swatch, 22, 22);
    lv_obj_set_pos(s_face_swatch, V5_PAD, 54);
    lv_obj_set_style_radius(s_face_swatch, 2, 0);
    lv_obj_set_style_border_width(s_face_swatch, 0, 0);

    s_face_word = lv_label_create(pg);
    lv_obj_set_style_text_font(s_face_word, &lv_font_montserrat_34, 0);
    lv_obj_set_pos(s_face_word, V5_PAD + 34, 44);

    s_face_since = lv_label_create(pg);
    lv_obj_set_style_text_font(s_face_since, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_face_since, lv_color_hex(V5_LABEL), 0);
    lv_obj_set_pos(s_face_since, V5_PAD + 34, 88);

    lv_obj_t *rule = lv_obj_create(pg);
    lv_obj_set_size(rule, PANEL_W - 2 * V5_PAD, 1);
    lv_obj_set_pos(rule, V5_PAD, 128);
    lv_obj_set_style_bg_color(rule, lv_color_hex(V5_SURFACE), 0);
    lv_obj_set_style_border_width(rule, 0, 0);

    /* ---- R2 PWR: label, bar graph, value ------------------------------ */
    lv_obj_t *pk = lv_label_create(pg);
    lv_label_set_text(pk, "R2 PWR");
    lv_obj_set_style_text_font(pk, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(pk, lv_color_hex(V5_LABEL), 0);
    lv_obj_set_pos(pk, V5_PAD, 160);

    for (int i = 0; i < PWR_BARS; i++) {
        lv_obj_t *b = lv_obj_create(pg);
        lv_obj_set_size(b, 4, 26);
        lv_obj_set_pos(b, 118 + i * 7, 152);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        s_pwr_bar[i] = b;
    }

    s_kv_val[0] = lv_label_create(pg);
    lv_obj_set_style_text_font(s_kv_val[0], &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_kv_val[0], lv_color_hex(V5_TEXT), 0);
    lv_obj_set_pos(s_kv_val[0], 254, 154);

    /* ---- DOME: label, dial, value -------------------------------------
     * v5 draws the dial with NO NEEDLE when the heading is unknown rather
     * than hiding the dial or parking the needle at zero -- "[FIX] heading
     * may be unknown (link down)". A needle at zero is a confident lie; an
     * empty dial is the truth. */
    lv_obj_t *dk = lv_label_create(pg);
    lv_label_set_text(dk, "DOME");
    lv_obj_set_style_text_font(dk, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(dk, lv_color_hex(V5_LABEL), 0);
    lv_obj_set_pos(dk, V5_PAD, 250);

    lv_obj_t *dial = lv_obj_create(pg);
    lv_obj_set_size(dial, DIAL_D, DIAL_D);
    lv_obj_set_pos(dial, DIAL_X, DIAL_Y);
    lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dial, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dial, 2, 0);
    lv_obj_set_style_border_color(dial, lv_color_hex(V5_SURFACE), 0);

    s_dome_hub = lv_obj_create(pg);
    lv_obj_set_size(s_dome_hub, 6, 6);
    lv_obj_set_pos(s_dome_hub, DIAL_X + DIAL_R - 3, DIAL_Y + DIAL_R - 3);
    lv_obj_set_style_radius(s_dome_hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dome_hub, 0, 0);
    lv_obj_set_style_bg_color(s_dome_hub, lv_color_hex(V5_MID), 0);

    /* The needle's points are DIAL-RELATIVE, and the line object is pinned
     * over the dial to make that true. lv_line draws its points relative to
     * its own origin; writing page coordinates into them worked only because
     * the object happened to land at 0,0 with no padding, so any later
     * set_pos, align or layout on this page would have silently displaced the
     * needle with no error anywhere. */
    static lv_point_precise_t needle_pts[2];
    s_dome_needle = lv_line_create(pg);
    lv_obj_set_pos(s_dome_needle, DIAL_X, DIAL_Y);
    lv_obj_set_size(s_dome_needle, DIAL_D, DIAL_D);
    needle_pts[0].x = DIAL_R; needle_pts[0].y = DIAL_R;
    needle_pts[1].x = DIAL_R; needle_pts[1].y = DIAL_R;
    lv_line_set_points(s_dome_needle, needle_pts, 2);
    lv_obj_set_style_line_width(s_dome_needle, 3, 0);
    lv_obj_set_style_line_color(s_dome_needle, lv_color_hex(PANEL_C_CYAN), 0);
    lv_obj_add_flag(s_dome_needle, LV_OBJ_FLAG_HIDDEN);

    s_kv_val[1] = lv_label_create(pg);
    lv_obj_set_style_text_font(s_kv_val[1], &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_kv_val[1], lv_color_hex(V5_TEXT), 0);
    lv_obj_set_pos(s_kv_val[1], 254, 236);

    /* ---- FAULT CHAIN: only when something is actually wrong -----------
     * I had this permanently on the resting face. v5 defines `chain` for
     * exactly four states -- the three offline modes and danger -- so a
     * healthy panel does not carry it. A fault indicator that is always
     * visible is one nobody reads. */
    s_chain_row = lv_obj_create(pg);
    lv_obj_set_size(s_chain_row, PANEL_W, 58);
    lv_obj_set_pos(s_chain_row, 0, 336);
    lv_obj_set_style_bg_opa(s_chain_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chain_row, 0, 0);
    lv_obj_set_style_pad_all(s_chain_row, 0, 0);
    lv_obj_clear_flag(s_chain_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *line = lv_obj_create(s_chain_row);
    lv_obj_set_size(line, PANEL_W - 60, 2);
    lv_obj_set_pos(line, 30, 40);
    lv_obj_set_style_bg_color(line, lv_color_hex(V5_RULE), 0);
    lv_obj_set_style_border_width(line, 0, 0);

    for (int i = 0; i < 4; i++) {
        const int cx = 40 + i * ((PANEL_W - 80) / 3);
        lv_obj_t *lbl = lv_label_create(s_chain_row);
        lv_label_set_text(lbl, k_chain_label[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(V5_MID), 0);
        lv_obj_set_pos(lbl, cx - 14, 8);

        lv_obj_t *pip = lv_obj_create(s_chain_row);
        lv_obj_set_size(pip, 16, 16);
        lv_obj_set_pos(pip, cx - 8, 33);
        lv_obj_set_style_radius(pip, 2, 0);
        lv_obj_set_style_border_width(pip, 0, 0);
        s_chain_pip[i] = pip;
    }
    lv_obj_add_flag(s_chain_row, LV_OBJ_FLAG_HIDDEN);
}

void panel_ui_create(void)
{
    s_screen = lv_scr_act();
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Everything lives in a root container so burn-in drift moves the whole
     * frame as one unit. Drifting elements independently would change the
     * layout as it moved, which is a different and worse thing: the panel
     * would look subtly broken rather than subtly displaced. */
    s_root = lv_obj_create(s_screen);
    lv_obj_set_size(s_root, PANEL_W, PANEL_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_page[PAGE_STATUS] = make_page(s_root);
    s_page[PAGE_SERVICE] = make_page(s_root);
    s_page[PAGE_NETWORK] = make_page(s_root);
    build_service_page(s_page[PAGE_SERVICE]);
    build_network_page(s_page[PAGE_NETWORK]);

    build_status_face(s_page[PAGE_STATUS]);

    /* DRIVE THE FACE ONCE, before anything can be seen.
     *
     * build_status_face creates the word, the reason and the two values as
     * empty labels and lets set_face fill them, which left them showing
     * LVGL's default label text -- the literal string "Text", four times, at
     * 34 px. Not a sub-frame flicker either: main.c releases the display lock
     * here and only starts the UI task after NimBLE is up, so that frame is
     * what sits on the glass for the whole BLE bring-up.
     *
     * The synthetic telemetry below is all-invalid on purpose, so every
     * readout takes its unknown path: "----" against an empty gauge and a
     * dial with no needle. The panel's first statement about R2 is that it
     * does not know anything about him yet, which is true. */
    {
        const r2_telemetry_t empty = { 0 };
        set_face(PANEL_ST_WAKING, PANEL_OFF_COUNT, &empty, 0);
    }

    make_pips(s_root);
    panel_ui_show_page(PAGE_STATUS);
}

/* TWO KINDS OF CHANGE, and conflating them killed the dimmer.
 *
 * `s_changed` used to mean "any label text differs", and it fed the burn-in
 * dim timer. R2's battery alternates between 4.42 V and 4.43 V -- both values
 * appear throughout the A1 logs -- so a reading every 15 s reset a 120 s timer
 * forever. AC9'S DIMMING HALF HAS NEVER ONCE EXECUTED, on any build. The drift
 * worked and was measured; the dimming looked alive and was dead.
 *
 * So: a SEVERITY change is something worth looking at and resets the timer. A
 * value's last digit wobbling is not, and merely repaints. */
static bool s_changed;      /* worth looking at: severity moved */

static uint32_t chain_colour(chain_t c)
{
    switch (c) {
    case CH_OK:    return PANEL_C_GREEN;
    case CH_DOWN:  return PANEL_C_AMBER;
    case CH_FAULT: return PANEL_C_RED;
    case CH_UNK:
    default:       return V5_SURFACE;   /* unknown is absence, not a claim */
    }
}

/* Drive the face. Only the states this board can REACH are selectable: the
 * rest need a reasoning layer that does not run here, and a face driven by an
 * invented source would be worse than an honest three. */
static void set_face(panel_state_t st, panel_offline_mode_t mode,
                     const r2_telemetry_t *t, uint32_t now_ms)
{
    char buf[32];

    /* The MODE is part of the identity, not a detail under it: the three
     * offline views are what makes one `offline` state able to say which
     * thing is unreachable, so a change from R2 to LLM must repaint even
     * though the state has not moved. Comparing only the state would leave
     * the panel reading "R2 LINK DOWN" after the fault moved elsewhere. */
    if (st != s_state_now || mode != s_mode_now) {
        s_state_now = st;
        s_mode_now  = mode;
        s_changed = true;                       /* worth looking at */

        const uint32_t colour = panel_state_colour(st);
        lv_label_set_text(s_face_word, panel_state_word(st));
        lv_obj_set_style_text_color(s_face_word, lv_color_hex(colour), 0);
        lv_obj_set_style_bg_color(s_face_swatch, lv_color_hex(colour), 0);
        lv_label_set_text(s_face_since, panel_state_since(st, mode));

        if (face_has_chain(st)) {
            chain_t chain[4];
            face_chain(st, mode, chain);
            lv_obj_remove_flag(s_chain_row, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < 4; i++)
                lv_obj_set_style_bg_color(s_chain_pip[i],
                                          lv_color_hex(chain_colour(chain[i])), 0);
        } else {
            lv_obj_add_flag(s_chain_row, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* ---- R2 PWR ------------------------------------------------------- */
    const bool pwr_ok = r2_telemetry_displayable(t, &t->battery, now_ms, 60000);
    if (pwr_ok)
        snprintf(buf, sizeof buf, "%u.%02u V",
                 t->battery_centivolts / 100u, t->battery_centivolts % 100u);
    else
        snprintf(buf, sizeof buf, "----");
    if (strcmp(lv_label_get_text(s_kv_val[0]), buf) != 0)
        lv_label_set_text(s_kv_val[0], buf);

    /* The bar graph's fill needs a percentage and we have VOLTS.
     *
     * Mapping one to the other needs his discharge curve, which is UNMEASURED
     * -- every reading this project has ever taken is 4.42 or 4.43 V on a
     * charger. The provisional range below (3.60 V empty, 4.50 V full, a
     * nominal 1S Li-ion) is INFERRED and labelled as such; it must not
     * graduate to OBSERVED because a bar looked plausible.
     *
     * When the value is not knowable, ALL bars go to the track colour -- v5's
     * treatment, and the honest one: an empty gauge beside "----". */
    int filled = 0;
    if (pwr_ok) {
        const int mv = (int)t->battery_centivolts * 10;
        int pct = (mv - 3600) * 100 / (4500 - 3600);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        filled = pct * PWR_BARS / 100;
        /* A known battery must never render as an empty track, because the
         * track is ALSO what "we have no reading" looks like. The floor is
         * unconditional inside this branch, NOT gated on pct > 0: pct is
         * clamped to zero at 3.60 V and below, so gating on it left the one
         * case that matters most -- a genuinely flat droid -- pixel-identical
         * to no reading at all. Having a reading is what lights a bar; what
         * the reading says is the bar count above it. */
        if (filled == 0) filled = 1;
    }
    /* V5_LABEL, not PANEL_C_GREEN. Green is a VERDICT -- "this is healthy" -- and
     * the mapping behind this bar is inferred from an unmeasured curve, so it
     * has no business rendering a verdict. A neutral instrument level is what
     * we can actually justify. */
    for (int i = 0; i < PWR_BARS; i++)
        lv_obj_set_style_bg_color(s_pwr_bar[i],
            lv_color_hex(i < filled ? V5_LABEL : V5_RULE), 0);

    /* ---- DOME --------------------------------------------------------- */
    /* isfinite() because dome_degrees is a float written straight off a BLE
     * frame. Casting a NaN to int is undefined behaviour, and cosf/sinf of a
     * NaN would put arbitrary values into the needle's points -- the one
     * place in this file where an unvalidated wire value reached arithmetic.
     * lroundf rather than a truncating cast: the dome has been observed at
     * -0.06 deg, which truncates to 000 when the answer is 000 either way but
     * would truncate -0.6 to 000 when it should read 359. The magnitude bound
     * is there because isfinite() admits 1e30, and lroundf() of a value
     * outside long's range is unspecified -- finite is not the same as
     * sane, and a corrupt frame is finite. */
    const bool dome_ok = isfinite(t->dome_degrees) &&
                         fabsf(t->dome_degrees) < 1.0e6f &&
                         r2_telemetry_displayable(t, &t->dome, now_ms, 60000);
    if (dome_ok)
        snprintf(buf, sizeof buf, "%03d",
                 (int)((lroundf(t->dome_degrees) % 360 + 360) % 360));
    else
        snprintf(buf, sizeof buf, "----");
    if (strcmp(lv_label_get_text(s_kv_val[1]), buf) != 0)
        lv_label_set_text(s_kv_val[1], buf);

    /* No needle when the heading is unknown. A needle parked at zero is a
     * confident lie; an empty dial is the truth. v5 does exactly this. */
    if (dome_ok) {
        static lv_point_precise_t pts[2];
        const float rad = ((float)t->dome_degrees - 90.0f) * 3.14159265f / 180.0f;
        pts[0].x = DIAL_R; pts[0].y = DIAL_R;
        pts[1].x = DIAL_R + (int)(42.0f * cosf(rad));
        pts[1].y = DIAL_R + (int)(42.0f * sinf(rad));
        lv_line_set_points(s_dome_needle, pts, 2);
        lv_obj_remove_flag(s_dome_needle, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_dome_needle, LV_OBJ_FLAG_HIDDEN);
    }
}

bool panel_ui_update(const r2_telemetry_t *t, uint32_t now_ms)
{
    s_changed = false;

    /* The derivation lives in `panel_state` and is host-tested there.
     *
     * It was written here first, and that was wrong for a reason worth
     * keeping: a screenshot of `IDLE` looks identical whether the wiring is
     * live or dead, so a decision that only exists inside the renderer has no
     * evidence available to it short of an operator unplugging the droid. The
     * decision moved to where a test can reach it and this stayed a renderer.
     *
     * Only three of the twelve states are reachable from this board -- it has
     * one sensor, the BLE link. The other nine are defined and rendered by a
     * table nothing selects yet, and that is stated rather than discovered
     * later: four PRs of the LED stack once merged completely inert because
     * each was built above the last without one path reaching the hardware. */
    panel_state_t st;
    panel_offline_mode_t mode;
    panel_state_from_link(t->link == R2_TM_UP, t->link == R2_TM_DOWN,
                          &st, &mode);
    set_face(st, mode, t, now_ms);

    /* The four rows LINK / R2 / STORAGE / BRAIN are GONE from this page.
     * They were built as the whole status screen from AC4's wording; the v5
     * reference has no BRAIN row and no four-row status page. See
     * build_status_face() for the conflict this resolves and the ruling it
     * still needs. */

    /* Only the severity kind. The dim timer must not be resettable by noise,
     * or it never expires and the panel never rests. */
    return s_changed;
}


/* ---- burn-in mitigation (#101 AC9, D-021) --------------------------------
 *
 * This panel is an always-on OLED showing a largely static instrument, and
 * D-021 makes that "the first design constraint" on the resting frame. The
 * epic is explicit that mitigation belongs in the render from child 3 onward
 * rather than retrofitted: A STATIC FRAME SHIPPED FOR WEEKS IS NOT REVERSIBLE
 * BY A LATER PATCH. Child 3 shipped in #134 without it -- caught here rather
 * than after another nine days of UI work on top.
 *
 * What moves, by how much, on what period -- stated because AC9 requires the
 * evidence to say, and "it is dim" is explicitly not mitigation on its own:
 *
 *   DRIFT   the entire frame, +/-4 px in x and y, on a 240 s cycle, walking a
 *           4-position square (0,0) -> (4,0) -> (4,4) -> (0,4). So every pixel
 *           of every glyph edge sees background within four minutes, and no
 *           element is pixel-static across an idle period.
 *
 *           4 px because the rows are 87 px and the type sits well inside
 *           them: a shift this small never clips and never reads as movement
 *           to someone glancing at it, which matters because this is an
 *           instrument and a drifting instrument looks broken.
 *
 *   DIM     brightness drops to 40% after 120 s with no state change, and
 *           returns immediately when anything changes. The prototype used 0.42
 *           for sleep chrome; 40 is the nearest whole percent.
 *
 * NOT measured: this panel's actual luminance-decay characteristics are
 * unknown, so the numbers are a mitigation designed against the risk rather
 * than one calibrated against it. Recorded as UNKNOWN, not as sufficient.
 */
#define BURN_DRIFT_PX      4
#define BURN_DRIFT_MS  60000u    /* per step; 4 steps = a 240 s cycle */
#define BURN_DIM_AFTER_MS 120000u
/* 65, not 40. The operator reported the panel as "off" at 40%.
 *
 * 40 is a real 40% (the BSP maps percent to 0-255, so 102), which would be
 * unremarkable on a bright UI. This one is near-black BY DESIGN -- 0x000000
 * ground, 0x101014 rows, grey labels -- so dimming it 60% leaves a dark screen
 * with dim grey text, and on an AMOLED whose blacks are true black that reads
 * as POWERED OFF rather than as resting.
 *
 * The number came from D-021's prototype "dims sleep chrome to 0.42", and that
 * was the mistake: sleep chrome is a different surface from a resting
 * instrument someone is meant to be able to READ at a glance. A dim value
 * borrowed from one does not transfer to the other.
 *
 * The burn-in argument survives the change. Drift is the mitigation that does
 * the work -- every glyph edge sees background within 240 s -- and this design
 * lights very few pixels to begin with. Dimming is a secondary measure and is
 * not worth making the panel unreadable for. */
#define BURN_DIM_PERCENT   65
#define BURN_FULL_PERCENT  90

static uint32_t s_last_change_ms;
static int      s_drift_step = -1;
static bool     s_dimmed;

void panel_ui_burn_in(uint32_t now_ms, bool state_changed,
                      panel_brightness_fn set_brightness)
{
    if (state_changed) s_last_change_ms = now_ms;

    /* Drift. The step index is derived from the clock rather than counted, so
     * it cannot accumulate error and is correct after any missed tick. */
    const int step = (int)((now_ms / BURN_DRIFT_MS) % 4u);
    if (step != s_drift_step && s_root != NULL) {
        s_drift_step = step;
        static const int8_t dx[4] = {0, BURN_DRIFT_PX, BURN_DRIFT_PX, 0};
        static const int8_t dy[4] = {0, 0, BURN_DRIFT_PX, BURN_DRIFT_PX};
        lv_obj_set_pos(s_root, dx[step], dy[step]);
    }

    /* Dim at rest. Guarded on the transition so it is not written every tick:
     * brightness is an I2C register on this panel, not a variable. */
    if (set_brightness == NULL) return;
    const bool want_dim = (now_ms - s_last_change_ms) > BURN_DIM_AFTER_MS;
    if (want_dim != s_dimmed) {
        s_dimmed = want_dim;
        set_brightness(want_dim ? BURN_DIM_PERCENT : BURN_FULL_PERCENT);
        /* Logged, because "the screen is off" and "the screen is dim" are the
         * same observation from arm's length and the log is the only place
         * they differ. */
        ESP_LOGI("panel", "brightness -> %d%% (%s)",
                 want_dim ? BURN_DIM_PERCENT : BURN_FULL_PERCENT,
                 want_dim ? "resting" : "active");
    }
}

int panel_ui_drift_step(void) { return s_drift_step; }

/* The active brightness, so boot and the burn-in module cannot disagree.
 * main.c used to hardcode 80 while this file's active level was 90 -- two
 * numbers for one concept, and the kind that drift apart silently. */
int panel_ui_full_brightness(void) { return BURN_FULL_PERCENT; }
bool panel_ui_is_dimmed(void) { return s_dimmed; }
