#include "panel_ui.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "lvgl.h"
#include "panel_state.h"
#include "panel_wake.h"
#include "panel_service.h"
#include "panel_probe.h"
#include "r2_gate.h"
#include "panel_touch.h"
#include "panel_fonts.h"

/* THE v5 PALETTE. Taken from the reference prototype the vault calls "the
 * reference the LVGL firmware should match", not invented.
 *
 * The panel was built to the epic's PROSE -- row names, page names -- and
 * never to the design itself, which is why it looked nothing like it: a
 * neutral black/grey scheme with generic accents, where v5 is a cool
 * blue-grey system. Values below are lifted from panel-v5-interactive.html. */
/* MEASURED off the reference: the status screen's ground is BLACK. 0x14191B
 * was read from the prototype's palette block, where it names a surface the
 * face does not actually use -- so the panel shipped a visibly lighter slate
 * behind everything. On an OLED this is not cosmetic: black is pixels that
 * are off. */
#define V5_GROUND     0x000000   /* page ground */
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
 * them. SERVICE's rows and its seven interiors are child 6 and are built
 * below; what each one may honestly say is decided in `panel_service`.
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

static lv_obj_t *s_face_word, *s_face_since, *s_face_swatch;
static lv_obj_t *s_waking_fill;       /* AC8: the rule, filling while waking */
static lv_obj_t *s_chrome_wifi, *s_chrome_llm, *s_chrome_batt;
/* EVERY NUMBER BELOW WAS MEASURED off panel-v5-interactive.html, by reading
 * getBoundingClientRect on each element and scaling to the panel's 368 px
 * width -- not estimated from a screenshot and not read off the CSS, which
 * carries rem and flex values that say nothing about where a thing lands.
 * The face was assembled from eyeballed positions before this and drifted
 * from the reference in fifteen places at once. */
#define PWR_BARS   18
#define PWR_X     132       /* first bar's left edge */
#define PWR_PITCH   7
#define PWR_BAR_W   3
#define PWR_TALL_H 30       /* every 5th bar: the scale's major tick */
#define PWR_SHORT_H 20
#define PWR_TOP   167       /* top of a TALL bar; short ones sit on the same
                             * baseline, so they start 10 lower */
#define PWR_BASE_Y 200      /* the 2 px rule the bars stand on */

#define DIAL_D   108
#define DIAL_R   (DIAL_D / 2)
#define DIAL_X   130
#define DIAL_Y   236
#define DIAL_CX  (DIAL_X + DIAL_R)
#define DIAL_TICK_H 10      /* the 12 o'clock mark, ABOVE the rim */

#define VAL_RIGHT (PANEL_W - V5_PAD)   /* values are right-aligned to 342 */
#define PWR_VAL_Y 165
#define DOME_VAL_Y 274
static lv_obj_t *s_pwr_bar[PWR_BARS];
static lv_obj_t *s_dome_needle, *s_dome_hub, *s_dome_wedge;
static lv_obj_t *s_kv_val[2];                 /* PWR, DOME -- the number */
static lv_obj_t *s_kv_unit[2];                /* and its unit, smaller */

/* ONE FAULT CHAIN, TWO PLACES. The resting face and the wake frame draw the
 * same four nodes 24 px apart, so the chain is built and painted by one pair
 * of functions rather than copied -- two copies of the broken-link rules
 * below is how the wake frame would come to disagree with the face about
 * which link is down. */
typedef struct {
    lv_obj_t *row;
    lv_obj_t *pip[4];
    lv_obj_t *lbl[4];
} chain_ui_t;
static chain_ui_t s_face_chain, s_wake_chain;
static void paint_chain(const chain_ui_t *ui, panel_state_t st,
                        panel_offline_mode_t mode);

/* The wake frame (#101 AC3). See build_wake(). */
static lv_obj_t *s_wake, *s_wake_swatch, *s_wake_word, *s_wake_reason;
static lv_obj_t *s_wake_row, *s_wake_box, *s_wake_subject;
static lv_obj_t *s_wake_what, *s_wake_down, *s_wake_sweep;
static panel_wake_t s_wake_trk;
static bool s_wake_up;
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
#define PIP_W    20     /* v5 .dot-a { width: 20px } */
#define PIP_DOT   6     /* v5 .dot   { width: 6px }  */
#define PIP_H     6
#define PIP_GAP  10     /* measured: 158->188 is 20+10, 188->204 is 6+10 */
#define PIP_Y   422

/* A GAP between dots, not a fixed pitch.
 *
 * The pill is 20 wide and a dot is 6, so a constant pitch leaves a different
 * amount of air on either side of the active one depending on where it is --
 * the row visibly shuffles as you swipe. v5 spaces by the GAP and lets the
 * row's width change, which keeps the spacing even and the row centred.
 * Returns each dot's left edge for the CURRENT active page. */
static int pip_x(int i, int active)
{
    int total = 0;
    for (int k = 0; k < PAGE_COUNT; k++)
        total += (k == active ? PIP_W : PIP_DOT) + (k ? PIP_GAP : 0);

    int x = PANEL_W / 2 - total / 2;
    for (int k = 0; k < i; k++)
        x += (k == active ? PIP_W : PIP_DOT) + PIP_GAP;
    return x;
}

static void make_pips(lv_obj_t *parent)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *d = lv_obj_create(parent);
        /* Sized and positioned by set_page: v5 draws the ACTIVE one as a 20x6
         * pill and the rest as 6 px dots, so the geometry is state, not
         * construction. See pip_x() for the spacing -- it is a gap, not a
         * pitch. (This comment described the fixed 22 px pitch pip_x
         * replaced, and sat 25 lines under the block arguing against it: a
         * comment falsified by an edit rather than wrong when written, which
         * is the harder kind to notice.) */
        lv_obj_set_size(d, PIP_DOT, PIP_H);
        lv_obj_set_pos(d, pip_x(i, PAGE_STATUS), PIP_Y);
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

/* ---- SERVICE (#101 child 6) ----------------------------------------------
 *
 * Measured off the reference like the face: rows 87 px with a 2 px rule under
 * each, the text's baseline 43.5 px into the row, the list from y=51 to the
 * pager at 410; an interior's header 74 px tall with the back label's
 * baseline at 45.3, key/value rows 52 px plus a hairline, a ladder rung 56
 * plus a hairline. Each label is set on its baseline, as the wake frame's
 * are. WHAT the rows say is `panel_service`'s, and host-tested there. */
#define SVC_X        26
#define SVC_W       316
#define SVC_LIST_Y   51
#define SVC_LIST_H  359                   /* to the pager at 410 */
#define SVC_ROW_H    89                   /* 87 + the 2 px rule */
#define INT_HEAD_H   74
#define INT_BODY_Y   76
#define INT_KV_H     53                   /* 52 + hairline */
#define INT_RUNG_H   54                   /* 53 + hairline: see the ladder */
#define CHEVRONS     "\xE2\x80\xBA\xE2\x80\xBA"       /* U+203A x2 */
#define BACK         "\xE2\x80\xB9\xE2\x80\xB9 "      /* U+2039 x2 */

/* panel_service's rungs mirror r2_tier_t INDEX FOR INDEX, and each one is
 * pinned here -- the count alone would let a reordered enum compile and draw
 * STANCE as allowed under a DOME ceiling. */
_Static_assert(R2_TIER__COUNT == 5, "a gate tier was added: give it a rung first");
_Static_assert(R2_TIER_READ == 0 && R2_TIER_LEDS == 1 && R2_TIER_AUDIO == 2 &&
               R2_TIER_DOME == 3 && R2_TIER_STANCE == 4,
               "r2_tier_t was reordered: panel_service's rung order must follow");

static lv_obj_t *s_svc_list, *s_svc_row[PANEL_SVC_COUNT], *s_svc_link_sq;
static lv_obj_t *s_int, *s_int_back, *s_int_right, *s_int_body, *s_int_foot;
static lv_obj_t *s_int_val[PANEL_SVC_MAX_ROWS];
static const char *s_int_key[PANEL_SVC_MAX_ROWS];
/* What was last SET on each value label, and in what tone. Compared against
 * rather than lv_label_get_text(): once DOTS truncates a value, LVGL rewrites
 * the label's own buffer to end in dots, so the label never matches the value
 * again -- and a refresh keyed on it re-set the text, and redrew the row,
 * every 40 ms for as long as the interior was open. */
static char s_int_last[PANEL_SVC_MAX_ROWS][PANEL_SVC_VAL_LEN];
static int  s_int_tone[PANEL_SVC_MAX_ROWS];
static int       s_int_rows;
static panel_svc_t s_int_open = PANEL_SVC_COUNT;       /* none */

/* THE LADDER'S RUN (#101 child 6). A tap on a rung the gate would admit asks
 * main.c to send that tier's ops; `panel_probe` decides what the rung then
 * says. Only rungs panel_service_ladder called allowed are tappable, and the
 * gate refuses anything above its ceiling in any case -- two refusals, and
 * this one is the softer of them. */
static lv_obj_t   *s_rung_row[PANEL_LADDER_RUNGS];
static lv_obj_t   *s_rung_word[PANEL_LADDER_RUNGS];
static bool        s_rung_allowed[PANEL_LADDER_RUNGS];

/* AND WHY EACH SHUT RUNG IS SHUT. The word is written in two places -- when the
 * ladder is built, and again when a REFUSED flash expires and the rung has to
 * go back to saying what it says for itself. The second only knew `allowed`,
 * so tapping a NOT YET rung relabelled it LOCKED for the life of the interior:
 * the operator is told to raise a ceiling that was never the obstacle. Keeping
 * the reason beside the verdict is what makes the two renderers agree. */
static panel_rung_block_t s_rung_why[PANEL_LADDER_RUNGS];

/* WHICH TIERS HAVE BEEN EXERCISED THIS SESSION (#168). Bit i for tier i; the
 * ladder opens a rung only when every tier below it is set. Deliberately a
 * plain static and not NVS: persisting it would unlock STANCE on a droid
 * nobody has looked at since last week. A reboot re-walks the ladder, which is
 * the point -- see panel_service.h.
 *
 * SET ON A PASS, NOT ON A TAP. A tier that was asked and did not answer has
 * not been exercised; advancing past a DOME that timed out is exactly the
 * thing bring-up order exists to prevent. */
static uint32_t    s_tiers_run;
static panel_probe_t s_probe;
static int         s_probe_rung = -1;      /* which rung the verdict belongs to */
static int         s_probe_request = -1;   /* a tier main.c has yet to send */

/* THE HANDOVER, and it crosses tasks on two cores: the touch tap and the
 * renderer live in ui_task, the send in link_task. Three words that must be
 * read together are not made safe by `volatile`, which orders the compiler and
 * nothing else -- so they are written and read under one spinlock. It also
 * makes take-and-clear atomic, where a plain read-modify-write could drop a
 * tap that landed between the two statements.
 *
 * The probe is NOT started by the sender, because the sender does not hold the
 * display lock and must not be able to fail to report: an earlier version
 * called into the renderer under a 100 ms lock attempt, and a lock that timed
 * out left three ops away with the rung still reading a cheerful green RUN. A
 * flag the tick loop cannot miss is the difference between "nothing happened"
 * and "nothing was SHOWN to have happened". */
/* Defined in main.c, which owns the radio and therefore the ledger: how many
 * of the RUNNING test's own questions R2 has answered. Declared here rather
 * than in a header because it is one function across one seam, and the seam is
 * the same one panel_ui_probe_sent already crosses in the other direction. */
unsigned panel_main_test_answered(void);

static portMUX_TYPE s_probe_mux = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_probe_pending_expected;
static uint32_t s_probe_pending_ms;
static bool     s_probe_pending_link;   /* was he there when they went out? */
static bool     s_probe_pending;

/* AND A GENERATION, because closing the interior cannot recall a send already
 * in flight. Clearing the flags is not enough: link_task may already have
 * taken the request and be inside its three GATT writes, and the `pending` it
 * sets on the way out lands AFTER the clear. That resurrected start was then
 * consumed by the refresh which re-opened the ladder, starting a probe against
 * rung -1 that nothing steps -- RUNNING forever, and every later tap refused.
 * The tap stamps the generation it was issued under; the close bumps it; a
 * start whose generation has moved on is dropped. */
static unsigned s_probe_gen;

/* TAKEN, AND ITS OPS ARE GOING OUT RIGHT NOW. Nothing represented this: the
 * request is cleared the moment the link task takes it and `pending` is not
 * set until after three GATT writes, so for that stretch every guard condition
 * read clear and a second tap bought three more ops. Closed by name rather
 * than by timing. */
static bool     s_probe_in_flight;

/* A REFUSED TAP, SHOWN. The refusals were ESP_LOGI only, on a board whose
 * serial cannot be read without resetting it into the ROM downloader
 * (CLAUDE.md) -- so the operator standing at the droid saw nothing at all,
 * and the comment claiming they "see the same refusal the gate would give"
 * described something that was never rendered. A locked rung says so on
 * itself for a moment instead. */
#define PANEL_REFUSE_FLASH_MS 1200u
static int      s_refuse_rung = -1;
static uint32_t s_refuse_at;


/* Bumped every time a tap is accepted, so the tour can tell "this tap took"
 * from "a request happens to be queued" -- the request may already have been
 * taken by the link task microseconds later. */
static unsigned s_probe_accepted;

static uint32_t tone_colour(panel_tone_t t)
{
    switch (t) {
    case PANEL_TONE_GOOD: return PANEL_C_GREEN;
    case PANEL_TONE_WARN: return PANEL_C_AMBER;
    case PANEL_TONE_NONE: return V5_DIM;
    case PANEL_TONE_PLAIN:
    default:              return V5_TEXT;
    }
}

static void bare(lv_obj_t *o)
{
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

/* A vertical scroller with the reference's 3 px scrollbar at the right edge. */
static void scroller(lv_obj_t *o)
{
    bare(o);
    lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(o, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(o, 0, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(o, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(o, lv_color_hex(V5_LABEL), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_SCROLLBAR);
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, int ls,
                      uint32_t colour, int x, int y, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_letter_space(l, ls, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, s);
    return l;
}

/* Right-aligned to `right`, in a box `w` wide. */
static lv_obj_t *text_r(lv_obj_t *parent, const lv_font_t *font, int ls,
                        uint32_t colour, int right, int w, int y, const char *s)
{
    lv_obj_t *l = text(parent, font, ls, colour, right - w, y, s);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    return l;
}

/* THE STATUS CHROME, on SERVICE as on the face -- v5 keeps it on every page
 * because it describes the board, not the page. The same three placeholders
 * and the same reason they are grey: this build has no Wi-Fi, no LLM client
 * and no battery ADC to put in them. */
static void make_chrome(lv_obj_t *pg)
{
    lv_obj_t *w = lv_label_create(pg);
    lv_label_set_text(w, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(w, lv_color_hex(V5_SURFACE), 0);
    lv_obj_set_pos(w, 120, 16);
    text(pg, &michroma_12, 1, V5_SURFACE, 160, 18, "LLM");
    text(pg, &michroma_12, 1, V5_SURFACE, 217, 18, "PWR");
}

static void build_interior(lv_obj_t *pg);

static void build_service_page(lv_obj_t *pg)
{
    make_chrome(pg);

    lv_obj_t *rule = lv_obj_create(pg);
    lv_obj_set_size(rule, SVC_W, 1);
    lv_obj_set_pos(rule, SVC_X, 50);
    lv_obj_set_style_bg_color(rule, lv_color_hex(V5_RULE), 0);
    lv_obj_set_style_border_width(rule, 0, 0);

    /* VERTICAL SCROLL, keeping 87 px rows. Operator ruling, 2026-09-07.
     *
     * Eight 89 px rows are 712 px and the list has 359. Something had to give,
     * and the alternative on the table was shrinking the rows -- which would
     * have traded the 44 pt tap target #106 actually measured as clickable for
     * a tidier screen. Measured hit accuracy beats a screen that fits.
     *
     * It also matches the design: the vault's v5 says "vertical scroll inside
     * a page, horizontal swipe reserved for back". Accepted cost, stated: the
     * panel stops being wholly glanceable here, because something is always
     * off-screen. The list runs to x=360 so its scrollbar sits where the
     * reference's does, outside the rows. */
    s_svc_list = lv_obj_create(pg);
    scroller(s_svc_list);
    lv_obj_set_size(s_svc_list, 334, SVC_LIST_H);
    lv_obj_set_pos(s_svc_list, SVC_X, SVC_LIST_Y);

    for (int i = 0; i < PANEL_SVC_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(s_svc_list);
        bare(row);
        lv_obj_set_size(row, SVC_W, SVC_ROW_H);
        lv_obj_set_pos(row, 0, i * SVC_ROW_H);
        if (i < PANEL_SVC_COUNT - 1) {
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(V5_RULE), 0);
        }
        /* Share Tech Mono 26, 0.04em. Baseline 43.5 into the row; line 26,
         * base line 4 -> y 21. */
        text(row, &techmono_26, 1, V5_TEXT, 0, 21, panel_service_title((panel_svc_t)i));
        text_r(row, &techmono_26, 1, V5_LABEL, SVC_W, 60, 21, CHEVRONS);

        /* R2 LINK carries a square: the one row whose interior can go wrong
         * while you are reading the menu. Coloured each tick. */
        if (i == PANEL_SVC_R2_LINK) {
            s_svc_link_sq = lv_obj_create(row);
            lv_obj_set_size(s_svc_link_sq, 14, 14);
            lv_obj_set_pos(s_svc_link_sq, SVC_W - 26 - 14 - 14, (87 - 14) / 2);
            lv_obj_set_style_radius(s_svc_link_sq, 0, 0);
            lv_obj_set_style_border_width(s_svc_link_sq, 0, 0);
            lv_obj_set_style_bg_color(s_svc_link_sq, lv_color_hex(PANEL_C_AMBER), 0);
        }
        s_svc_row[i] = row;
    }

    build_interior(pg);
}

/* ---- an interior ------------------------------------------------------------
 *
 * One view reused by all seven: a header that is the BACK button, a rule, and
 * a body rebuilt on open. Built on the SERVICE page, so leaving the page
 * leaves the interior with it. */
static void build_interior(lv_obj_t *pg)
{
    s_int = lv_obj_create(pg);
    bare(s_int);
    lv_obj_set_size(s_int, PANEL_W, PANEL_H);
    lv_obj_set_pos(s_int, 0, 0);
    lv_obj_set_style_bg_color(s_int, lv_color_hex(V5_GROUND), 0);
    lv_obj_set_style_bg_opa(s_int, LV_OPA_COVER, 0);

    /* "‹‹ TITLE", Share Tech Mono 26. Baseline 45.3; line 26, base 4 -> 23. */
    s_int_back = text(s_int, &techmono_26, 1, V5_TEXT, SVC_X, 23, "");
    /* Michroma 16 on the right. Baseline 44; line 17, base 3 -> 30. */
    s_int_right = text_r(s_int, &michroma_16, 1, PANEL_C_GREEN, SVC_X + SVC_W, 200, 30, "");

    lv_obj_t *rule = lv_obj_create(s_int);
    lv_obj_set_size(rule, SVC_W, 2);
    lv_obj_set_pos(rule, SVC_X, INT_HEAD_H);
    lv_obj_set_style_bg_color(rule, lv_color_hex(V5_RULE), 0);
    lv_obj_set_style_border_width(rule, 0, 0);

    s_int_body = lv_obj_create(s_int);
    scroller(s_int_body);
    lv_obj_set_pos(s_int_body, SVC_X, INT_BODY_Y);

    /* 14 px, the no-claim grey. Baseline 424; line 14, base 3 -> 413. */
    s_int_foot = text(s_int, &techmono_14, 1, V5_DIM, SVC_X, 413,
                      "EACH TIER OPT-IN \xC2\xB7 NEVER BUNDLED");
    lv_obj_add_flag(s_int, LV_OBJ_FLAG_HIDDEN);
}

static void set_pips_hidden(bool hidden)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (s_pip[i] == NULL) continue;
        if (hidden) lv_obj_add_flag(s_pip[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_remove_flag(s_pip[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void svc_facts(const r2_telemetry_t *t, uint32_t now_ms, panel_svc_facts_t *f)
{
    static uint32_t flash_bytes;
    if (flash_bytes == 0) esp_flash_get_size(NULL, &flash_bytes);
    const esp_app_desc_t *app = esp_app_get_description();
    *f = (panel_svc_facts_t){
        .tm = t, .now_ms = now_ms, .keepalive_ms = PANEL_KEEPALIVE_MS,
        .ceiling = (int)r2_gate_get_ceiling(),
        .panel_fw = app ? app->version : NULL,
        .flash_mb = flash_bytes / (1024u * 1024u),
        .psram_mb = (uint32_t)(esp_psram_get_size() / (1024u * 1024u)),
        /* INTERNAL only: the total counts this board's 8 MB of PSRAM and so
         * reads reassuring whatever the internal heap is doing. */
        .heap_kb  = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024u),
        .touch_id = panel_touch_chip_id(),
    };
}

/* The telemetry the last update saw, so an interior opened by a tap between
 * updates is filled from the same data as the tick after it. */
static const r2_telemetry_t *s_last_tm;
static int32_t s_svc_scroll_at;
static bool    s_press_on_moving_list;
static uint32_t s_last_now;

static void fill_list(bool build)
{
    panel_svc_facts_t f;
    svc_facts(s_last_tm, s_last_now, &f);
    panel_kv_t kv[PANEL_SVC_MAX_ROWS];
    const int n = panel_service_rows(s_int_open, &f, kv, PANEL_SVC_MAX_ROWS);

    /* REBUILD IF THE SHAPE MOVED. The labels are made once and refreshed in
     * place, so a row that appeared would be written through a freed label
     * and one that vanished would leave its last value -- possibly R2's --
     * on the glass after the link went. panel_service keeps the shape fixed
     * today (and a host test pins it); this is what holds if it ever stops. */
    bool same = !build && n == s_int_rows;
    for (int i = 0; same && i < n; i++)
        if (s_int_key[i] != kv[i].key) same = false;
    if (!build && !same) {
        lv_obj_clean(s_int_body);
        build = true;
    }

    for (int i = 0; i < n; i++) {
        if (build) {
            lv_obj_t *row = lv_obj_create(s_int_body);
            bare(row);
            lv_obj_set_size(row, SVC_W, INT_KV_H);
            lv_obj_set_pos(row, 0, 6 + i * INT_KV_H);
            if (i < n - 1) {
                lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
                lv_obj_set_style_border_width(row, 1, 0);
                lv_obj_set_style_border_color(row, lv_color_hex(V5_HAIRLINE), 0);
            }
            /* Key: Michroma 16, baseline 33 in; line 17, base 3 -> 19.
             * Value: Share Tech Mono 28, baseline 35 in; line 29, base 5 -> 11.
             * The value gets whatever the key leaves, 16 px clear of it, and
             * ends in dots rather than drawing over the key or wrapping into
             * the next row -- a git-describe version is longer than any
             * value the reference shows. */
            lv_obj_t *key = text(row, &michroma_16, 1, V5_LABEL, 0, 19, kv[i].key);
            lv_obj_update_layout(key);
            const int room = SVC_W - lv_obj_get_width(key) - 16;
            s_int_val[i] = text_r(row, &techmono_28, 0, V5_TEXT, SVC_W, room, 11, "");
            lv_label_set_long_mode(s_int_val[i], LV_LABEL_LONG_MODE_DOTS);
            /* A FIXED HEIGHT, one line. DOTS only fires when the text is
             * taller than its box, and a content-sized label grows to fit
             * the wrap -- so without this an overlong value wrapped into
             * the row below instead of ending in dots. */
            lv_obj_set_height(s_int_val[i], 29);
            s_int_key[i] = kv[i].key;
            s_int_last[i][0] = '\x01';          /* matches no value: set it */
            s_int_tone[i] = -1;
        }
        if (strcmp(s_int_last[i], kv[i].val) != 0) {
            lv_label_set_text(s_int_val[i], kv[i].val);
            memcpy(s_int_last[i], kv[i].val, sizeof s_int_last[i]);
        }
        if (s_int_tone[i] != (int)kv[i].tone) {
            lv_obj_set_style_text_color(s_int_val[i],
                lv_color_hex(tone_colour(kv[i].tone)), 0);
            s_int_tone[i] = (int)kv[i].tone;
        }
    }
    if (build) s_int_rows = n;
}

/* THE ONE PLACE A RUNG'S WORD IS DECIDED. Both renderers call it, so the
 * ladder and the post-REFUSED restore cannot drift apart. LOCKED and NOT YET
 * ask different things of the operator: a ceiling is a thing they must
 * deliberately raise, a sequence gap is a rung they have not reached yet. */
static const char *rung_word(panel_rung_block_t why)
{
    switch (why) {
    case PANEL_RUNG_OPEN:     return "RUN";
    case PANEL_RUNG_SEQUENCE: return "NOT YET";
    case PANEL_RUNG_CEILING:
    case PANEL_RUNG_UNLISTED: break;
    }
    return "LOCKED";            /* and anything unrecognised: shut, not open */
}

static void open_interior(panel_svc_t s)
{
    const panel_svc_kind_t kind = panel_service_kind(s);
    s_int_open = s;
    lv_obj_clean(s_int_body);
    s_int_rows = 0;

    char head[32];
    snprintf(head, sizeof head, BACK "%s",
             s == PANEL_SVC_HW_TEST ? "HW TEST" : panel_service_title(s));
    lv_label_set_text(s_int_back, head);
    lv_label_set_text(s_int_right, "");
    lv_obj_add_flag(s_int_foot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_int_body, 334, PANEL_H - INT_BODY_Y);

    if (kind == PANEL_SVC_LIST) {
        fill_list(true);
    } else if (kind == PANEL_SVC_NOTE) {
        const char *a = "", *b = "";
        panel_service_note(s, &a, &b);
        /* Centred in the BODY, which starts below the header. Measured off
         * the glass: the ink ran 228-273, a midpoint of 250 against the
         * body's centre of 262 -- 12 px high. It now runs 240-285. */
        lv_obj_t *l1 = text(s_int_body, &michroma_16, 2, V5_LABEL, 0, 162, a);
        lv_obj_t *l2 = text(s_int_body, &techmono_18, 1, V5_DIM, 0, 194, b);
        lv_obj_set_width(l1, SVC_W);
        lv_obj_set_width(l2, SVC_W);
        lv_obj_set_style_text_align(l1, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(l2, LV_TEXT_ALIGN_CENTER, 0);
    } else if (kind == PANEL_SVC_LADDER) {
        /* THE LADDER RUNS. Each rung says whether the gate would admit it,
         * and a rung it would admit is tappable: the first control on this
         * panel that sends R2 a command a person chose. What it may send is
         * bounded twice over -- the ladder offers only what the gate's ceiling
         * allows, and r2_gate_send refuses the rest regardless. */
        for (int i = 0; i < PANEL_LADDER_RUNGS; i++) {
            s_rung_row[i] = NULL;
            s_rung_word[i] = NULL;
            s_rung_allowed[i] = false;
            /* THE ZERO VALUE OF panel_rung_block_t IS _OPEN. Leaving it unset
             * here means a shut rung defaults to the reason that says it is
             * tappable -- fail-open on the one field the operator reads to
             * decide what to do next. Reset it with its siblings. */
            s_rung_why[i] = PANEL_RUNG_CEILING;
        }
        panel_probe_init(&s_probe);
        s_probe_rung = -1;

        const int ceiling = (int)r2_gate_get_ceiling();
        char right[24];
        snprintf(right, sizeof right, "CEIL %s", panel_service_ceiling_name(ceiling));
        lv_label_set_text(s_int_right, right);
        /* EVERY RUNG ON SCREEN AT ONCE. The reference's 56 px rungs overflow
         * its own ladder and push LOCOMOTION's LOCKED below the fold -- the
         * one rung whose lock matters most. 53 px rungs and a body that runs
         * to the footer fit all six; each is still far above the 44 pt tap
         * target for when RUN arrives. */
        lv_obj_set_size(s_int_body, 334, 410 - INT_BODY_Y);   /* footer below */
        lv_obj_remove_flag(s_int_foot, LV_OBJ_FLAG_HIDDEN);

        panel_rung_t r[PANEL_LADDER_RUNGS];
        const int n = panel_service_ladder(ceiling, s_tiers_run, r);
        for (int i = 0; i < n; i++) {
            lv_obj_t *row = lv_obj_create(s_int_body);
            bare(row);
            lv_obj_set_size(row, SVC_W, INT_RUNG_H);
            lv_obj_set_pos(row, 0, 6 + i * INT_RUNG_H);
            if (i < n - 1) {
                lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
                lv_obj_set_style_border_width(row, 1, 0);
                lv_obj_set_style_border_color(row, lv_color_hex(V5_HAIRLINE), 0);
            }
            /* Share Tech Mono 24, baseline 26 in; line 24, base 4 -> 6. The
             * lock word is 18 px, centred on the rung. ALLOWED is in the
             * text colour, not green: with no RUN yet it states what the gate
             * would admit, and green would read as armed. */
            text(row, &techmono_24, 1, r[i].allowed ? V5_TEXT : V5_DIM, 0, 6, r[i].tier);
            /* RUN in green on a rung the gate would admit -- it is a control
             * now, and the reference greens it. Every shut word (LOCKED, NOT
             * YET) stays the no-claim grey: none of them is a control. */
            /* LOCKED and NOT YET ask different things of the operator: one
             * is a ceiling they must deliberately raise, the other is a rung
             * they have not reached yet and can. Both stay the no-claim grey
             * -- neither is a control. */
            s_rung_word[i] = text_r(row, &techmono_18, 1,
                                    r[i].allowed ? PANEL_C_GREEN : V5_DIM,
                                    SVC_W, 120, 16, rung_word(r[i].why));
            s_rung_row[i] = row;
            s_rung_allowed[i] = r[i].allowed;
            s_rung_why[i]     = r[i].why;
        }
    }

    lv_obj_remove_flag(s_int, LV_OBJ_FLAG_HIDDEN);
    set_pips_hidden(true);          /* the reference's interior has no pager */
}

static void close_interior(void)
{
    if (s_int_open == PANEL_SVC_COUNT) return;
    s_int_open = PANEL_SVC_COUNT;
    /* The rung labels belong to the body about to be cleaned. Forgetting to
     * forget them left svc_refresh dereferencing freed LVGL objects on the
     * next tick -- on a panel bolted to the droid. */
    for (int i = 0; i < PANEL_LADDER_RUNGS; i++) {
        s_rung_row[i] = NULL;
        s_rung_word[i] = NULL;
        s_rung_allowed[i] = false;
        s_rung_why[i] = PANEL_RUNG_CEILING;    /* zero value is _OPEN */
    }
    s_probe_rung = -1;
    /* AND THE HANDOVER, both halves. A request left behind sends three ops
     * with no rung left to report them on; a pending start left behind is
     * consumed by the refresh that re-opens the ladder, which leaves the probe
     * RUNNING against rung -1 -- nothing steps it, and every later tap is
     * refused by the guard above for the rest of the visit. */
    portENTER_CRITICAL(&s_probe_mux);
    s_probe_request = -1;
    s_probe_pending = false;
    s_probe_in_flight = false;
    s_refuse_rung = -1;             /* its label is about to be freed */
    s_probe_gen++;                  /* disown a send already in flight */
    portEXIT_CRITICAL(&s_probe_mux);
    lv_obj_add_flag(s_int, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(s_int_body);
    s_int_rows = 0;
    set_pips_hidden(false);
}

static bool hit(lv_obj_t *o, int x, int y)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

void panel_ui_tap(int x, int y)
{
    if (s_page_at != PAGE_SERVICE || s_svc_list == NULL) return;

    if (s_int_open != PANEL_SVC_COUNT) {
        /* THE HEADER IS THE BACK BUTTON, all 74 px of it -- a tap target the
         * size of the reference's whole header rather than the width of its
         * chevrons. */
        if (y < INT_HEAD_H) { close_interior(); return; }

        /* A RUNG THE GATE WOULD ADMIT IS THE ONE OTHER CONTROL ON THIS PANEL.
         * A locked rung is inert on purpose: the refusal the operator sees is
         * the same one the gate would give, and a tap that "did nothing"
         * silently would leave them guessing which. */
        if (panel_service_kind(s_int_open) != PANEL_SVC_LADDER) return;
        for (int i = 0; i < PANEL_LADDER_RUNGS; i++) {
            if (s_rung_row[i] == NULL || !hit(s_rung_row[i], x, y)) continue;
            if (!s_rung_allowed[i]) {
                ESP_LOGI("panel", "tap on a LOCKED rung %d -- refused", i);
                s_refuse_rung = i;
                s_refuse_at = s_last_now;
                return;
            }
            /* ONE DECISION, UNDER ONE LOCK, and the running probe is part of
             * it. An earlier version committed the request first and checked
             * PANEL_PROBE_RUNNING after: that closed the 140 ms hole and
             * opened the 2 s one, because a tap during a running test still
             * armed a request the link task then sent. Anything that means a
             * test is under way -- queued, sent but not yet started, or
             * running -- refuses the tap, and the refusal is decided before
             * anything is written.
             *
             * IT IS NOT SUFFICIENT FOR A TIER THAT MOVES HIM. This guard
             * releases when the verdict settles, at 2 s, and D-013 measured a
             * dome move at 2.0-2.2 s -- so above READ it would release while
             * he was still travelling. Said here as well as in panel_probe.h
             * because whoever raises the ceiling may read only one of them,
             * and a pair of individually honest comments is how this repo has
             * misled itself before. See #168. */
            bool taken;
            portENTER_CRITICAL(&s_probe_mux);
            taken = panel_probe_may_start((panel_probe_gate_t){
                .queued    = s_probe_request >= 0,
                .in_flight = s_probe_in_flight,
                .pending   = s_probe_pending,
                .state     = s_probe.state,
            });
            if (taken) {
                s_probe_request = i;
                s_probe_rung = i;
                s_probe_accepted++;
            }
            portEXIT_CRITICAL(&s_probe_mux);

            /* AND THE OLD VERDICT GOES NOW, not when the new clock starts.
             * Deferring the rung index was not enough: at this ceiling exactly
             * one rung is tappable, so the rung tapped IS the rung that just
             * passed, and it wore a green "3/3 OK" for the ~140 ms before its
             * own test began -- a pass it had not earned, in the reassuring
             * direction. Reset here and it reads RUN until it reads "...".
             * Safe outside the spinlock: s_probe is touched only by this task,
             * under the display lock, and the start happens in the refresh --
             * also this task, so it cannot land in between. */
            if (taken) panel_probe_init(&s_probe);
            if (!taken) {
                /* The running rung already reads "..." -- the refusal of a
                 * second tap is legible there. Flashed anyway when the tap
                 * lands on a rung that is NOT the running one. */
                ESP_LOGI("panel", "RUN on rung %d ignored -- a test is under way", i);
                if (i != s_probe_rung) {
                    s_refuse_rung = i;
                    s_refuse_at = s_last_now;
                }
                return;
            }
            ESP_LOGI("panel", "RUN requested for rung %d", i);
            return;
        }
        return;
    }

    /* NOT IF THE LIST WAS MOVING WHEN THE FINGER LANDED. A tap meant to
     * stop a flick would otherwise open whatever row the momentum had carried
     * under it. Judged at the PRESS, not here: LVGL stops the flick the
     * moment a finger lands, so by release the list is still and a check
     * made now passes every tap. */
    if (s_press_on_moving_list) return;

    /* Inside the list's visible box first: a row scrolled under the rule or
     * the pager still has coordinates, and must not be tappable through them. */
    lv_obj_update_layout(s_svc_list);
    if (!hit(s_svc_list, x, y)) return;
    for (int i = 0; i < PANEL_SVC_COUNT; i++) {
        if (!hit(s_svc_row[i], x, y)) continue;
        if (panel_service_kind((panel_svc_t)i) == PANEL_SVC_JUMP)
            panel_ui_show_page(PAGE_NETWORK);
        else
            open_interior((panel_svc_t)i);
        ESP_LOGI("panel", "tap -> %s", panel_service_title((panel_svc_t)i));
        return;
    }
}

#ifdef PANEL_SHOT_TOUR
/* THE TOUR SCROLLS A ROW INTO VIEW AND TAPS WHERE IT ENDED UP.
 *
 * Exposed only for the screenshot build. The first version scrolled by
 * `row * SVC_ROW_H` and tapped a FIXED point, which a review caught: eight
 * 89 px rows in a 359 px list can only scroll 353 px, so rows 5, 6 and 7 all
 * clamped to the same offset and the tap opened PROVISIONING three times --
 * three frames that would have been filed as VOICE, CAMERA and ABOUT. A rig
 * that quietly produces plausible wrong pictures is worse than no rig.
 *
 * So: ask LVGL to bring the row into view, re-measure where it landed, tap
 * its centre through the real hit test, and REPORT whether the interior that
 * opened is the one that was asked for. The caller logs a failure loudly. */
/* Is the view the tour asked for the one actually showing? PANEL_TOUR_STATUS
 * and PANEL_TOUR_MENU are the two non-interior views; anything >= 0 is a
 * SERVICE row. The wake frame counts as "no": it is a real interruption that
 * can fire mid-tour -- on a bench with no droid the panel goes OFFLINE and
 * the frame pulls STATUS forward -- and a picture of it filed under an
 * interior's name is exactly the lie this rig exists not to tell. */
/* Has the rung's test settled, and did it pass? For the tour, which must not
 * photograph a "..." and call it a result. */
bool panel_ui_debug_probe_settled(void) { return panel_probe_settled(&s_probe); }
bool panel_ui_debug_probe_passed(void)  { return panel_probe_passed(&s_probe); }

bool panel_ui_debug_showing(int want)
{
    if (panel_ui_wake_showing()) return false;
    if (want == PANEL_TOUR_STATUS)
        return s_page_at == PAGE_STATUS && s_int_open == PANEL_SVC_COUNT;
    if (want == PANEL_TOUR_MENU)
        return s_page_at == PAGE_SERVICE && s_int_open == PANEL_SVC_COUNT;
    return s_page_at == PAGE_SERVICE && s_int_open == (panel_svc_t)want;
}

/* Put `want` back on the glass after something interrupted it. */
void panel_ui_debug_restore(int want)
{
    if (panel_ui_wake_showing()) panel_ui_wake_dismiss();
    if (want == PANEL_TOUR_STATUS) {
        panel_ui_show_page(PAGE_STATUS);
    } else if (want == PANEL_TOUR_MENU) {
        panel_ui_show_page(PAGE_SERVICE);
    } else {
        panel_ui_show_page(PAGE_SERVICE);
        panel_ui_debug_open_row(want);
    }
}

/* Tap a ladder rung through the real hit test, for the screenshot tour.
 * False when the rung does not exist, is locked, or the tap left no request.
 * Scrolls it into view first, so a ladder longer than its body still works. */
bool panel_ui_debug_run_rung(int rung)
{
    if (rung < 0 || rung >= PANEL_LADDER_RUNGS) return false;
    if (s_rung_row[rung] == NULL || !s_rung_allowed[rung]) return false;
    lv_obj_scroll_to_view(s_rung_row[rung], LV_ANIM_OFF);
    lv_obj_update_layout(s_int_body);
    lv_area_t a;
    lv_obj_get_coords(s_rung_row[rung], &a);
    /* Asked as "did MY tap take", not "is a request queued": the link task
     * can take the request microseconds later, which made the old check fail
     * on a perfectly healthy panel and the tour cry refusal. */
    unsigned before;
    portENTER_CRITICAL(&s_probe_mux);
    before = s_probe_accepted;
    portEXIT_CRITICAL(&s_probe_mux);
    panel_ui_tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    unsigned after;
    portENTER_CRITICAL(&s_probe_mux);
    after = s_probe_accepted;
    portEXIT_CRITICAL(&s_probe_mux);
    return after != before;
}

bool panel_ui_debug_open_row(int row)
{
    if (s_svc_list == NULL || row < 0 || row >= PANEL_SVC_COUNT) return false;
    lv_obj_scroll_to_view(s_svc_row[row], LV_ANIM_OFF);
    lv_obj_update_layout(s_svc_list);

    lv_area_t a;
    lv_obj_get_coords(s_svc_row[row], &a);
    const int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    panel_ui_tap(cx, cy);
    return s_int_open == (panel_svc_t)row;
}
#endif

int panel_ui_take_probe_request(unsigned *gen)
{
    portENTER_CRITICAL(&s_probe_mux);
    const int t = s_probe_request;
    s_probe_request = -1;
    if (t >= 0) s_probe_in_flight = true;   /* until the send reports back */
    if (gen) *gen = s_probe_gen;
    portEXIT_CRITICAL(&s_probe_mux);
    return t;
}

void panel_ui_probe_sent(unsigned expected, uint32_t sent_at_ms, unsigned gen,
                         bool link_up)
{
    portENTER_CRITICAL(&s_probe_mux);
    s_probe_in_flight = false;      /* they are out; it is the clock's turn */
    /* Whether anything is left to report them on is the generation's business.
     * A stale one is dropped here rather than started against a rung that no
     * longer exists. This is the load-bearing check; the refresh does not
     * repeat it, because a pending start can only exist with a live
     * generation. */
    if (gen == s_probe_gen) {
        s_probe_pending_expected = expected;
        s_probe_pending_ms = sent_at_ms;
        s_probe_pending_link = link_up;
        s_probe_pending = true;
    }
    portEXIT_CRITICAL(&s_probe_mux);
}

void panel_ui_note_press(void)
{
    /* Compared with the previous tick's sample: if the list moved between
     * that tick and this press, it was moving when the finger landed. */
    s_press_on_moving_list = s_svc_list != NULL &&
        lv_obj_get_scroll_y(s_svc_list) != s_svc_scroll_at;
}

void panel_ui_swipe(int dir)
{
    /* Inside an interior, horizontal swipe is reserved for BACK. */
    if (s_int_open != PANEL_SVC_COUNT) {
        if (dir < 0) close_interior();
        return;
    }
    int next = s_page_at + (dir > 0 ? 1 : -1);
    /* Clamp, do not wrap. The pages are an ordered strip, and the vault's own
     * argument for a fixed ring was that "position in the ring is itself an
     * orientation cue" -- wrapping destroys that cue on a strip whose ends
     * are otherwise unmarked. */
    if (next < 0) next = 0;
    if (next > PAGE_COUNT - 1) next = PAGE_COUNT - 1;
    if (next != s_page_at) {
        panel_ui_show_page(next);
        ESP_LOGI("panel", "swipe %s -> page %s", dir > 0 ? "left" : "right",
                 k_page_name[next]);
    }
}

/* Every tick: the R2 LINK square, and an open list's values. The values move
 * -- LAST READ ages, R2 LINK goes to "----" the tick the link drops -- and an
 * interior that only painted on open would be the panel vouching for a
 * reading after its link had gone, which is AC7 exactly. */
static void svc_refresh(const r2_telemetry_t *t, uint32_t now_ms)
{
    s_last_tm = t;
    s_last_now = now_ms;
    if (s_svc_list) s_svc_scroll_at = lv_obj_get_scroll_y(s_svc_list);
    if (s_svc_link_sq)
        lv_obj_set_style_bg_color(s_svc_link_sq,
            lv_color_hex(tone_colour(panel_service_link_tone(t))), 0);
    if (s_int_open != PANEL_SVC_COUNT &&
        panel_service_kind(s_int_open) == PANEL_SVC_LIST)
        fill_list(false);

    /* A test whose ops have gone out starts here, stamped with the moment
     * they left rather than the moment this tick noticed. */
    unsigned e = 0;
    uint32_t at = 0;
    bool was_up = false;
    bool start;
    portENTER_CRITICAL(&s_probe_mux);
    /* No generation test here: panel_ui_probe_sent only sets `pending` under a
     * live generation, and close_interior clears `pending` in the same
     * critical section that bumps the generation. A pending start therefore
     * always belongs to this one. Re-testing it here would read like a second
     * barrier and is a branch that cannot be taken. */
    start = s_probe_pending;
    if (start) {
        s_probe_pending = false;
        e = s_probe_pending_expected;
        at = s_probe_pending_ms;
        was_up = s_probe_pending_link;
    }
    portEXIT_CRITICAL(&s_probe_mux);
    if (start)
        panel_probe_start(&s_probe, at, e > 255u ? 255u : (uint8_t)e, was_up);

    /* THE RUNNING TEST'S VERDICT. Counted as the readings this test asked for
     * that arrived since it started, which NARROWS but does not eliminate the
     * panel's own periodic polls: they land in the same three stamps, so a
     * reply the test did not ask for can still count toward it. */
    /* THE REFUSAL FLASH, before the verdict, so a refused tap on the running
     * rung never overwrites what the test is saying. */
    if (s_refuse_rung >= 0) {
        if (s_refuse_rung == s_probe_rung) {
            /* The rung was refused and has since been tapped and accepted, so
             * the test owns its word now. Dropped rather than expired: the
             * expiry writes "RUN" back, and the verdict block below happens to
             * repaint over it in the same pass -- correct only by the order of
             * two blocks, which is not something to leave load-bearing. */
            s_refuse_rung = -1;
        } else if (now_ms - s_refuse_at >= PANEL_REFUSE_FLASH_MS) {
            /* Back to whatever the rung says for itself. */
            if (s_rung_word[s_refuse_rung] != NULL) {
                lv_label_set_text(s_rung_word[s_refuse_rung],
                                  rung_word(s_rung_why[s_refuse_rung]));
                lv_obj_set_style_text_color(
                    s_rung_word[s_refuse_rung],
                    lv_color_hex(s_rung_allowed[s_refuse_rung] ? PANEL_C_GREEN
                                                               : V5_DIM), 0);
            }
            s_refuse_rung = -1;
        } else if (s_rung_word[s_refuse_rung] != NULL) {
            lv_label_set_text(s_rung_word[s_refuse_rung], "REFUSED");
            lv_obj_set_style_text_color(s_rung_word[s_refuse_rung],
                                        lv_color_hex(PANEL_C_AMBER), 0);
        }
    }

    if (s_int_open != PANEL_SVC_COUNT &&
        panel_service_kind(s_int_open) == PANEL_SVC_LADDER &&
        s_probe_rung >= 0 && s_rung_word[s_probe_rung] != NULL) {
        /* THE LEDGER, NOT THE CLOCK (#168). This used to count READINGS that
         * had arrived since the test started, which the panel's own battery
         * and dome polls also land in -- so a PASS meant "three readings
         * arrived", not "R2 answered these three questions". With the
         * sequence gate keying on a PASS (D-025), that gap let a background
         * poll advance the ladder past a tier that never ran.
         *
         * panel_main_test_answered() counts only replies whose seq matches one
         * this test sent. A poll matches nothing and counts nothing. */
        const unsigned answered = panel_main_test_answered();
        panel_probe_step(&s_probe, now_ms, answered, t->link == R2_TM_UP);

        char word[16];
        panel_probe_word(&s_probe, word, sizeof word);
        lv_obj_t *lbl = s_rung_word[s_probe_rung];
        if (strcmp(lv_label_get_text(lbl), word) != 0)
            lv_label_set_text(lbl, word);
        uint32_t colour = PANEL_C_GREEN;                 /* RUN, and a pass */
        if (!panel_probe_settled(&s_probe))  colour = V5_TEXT;      /* running */
        else if (!panel_probe_passed(&s_probe)) colour = PANEL_C_AMBER;
        lv_obj_set_style_text_color(lbl, lv_color_hex(colour), 0);

        /* THE TIER IS EXERCISED ONLY ON A PASS (#168). Idempotent: this runs
         * every tick while the verdict stands, and setting a set bit is free.
         *
         * The ladder is rebuilt when the interior reopens, so a rung unlocked
         * by this pass shows NOT YET until then. Unobservable in this build --
         * the ceiling is READ, so there is exactly one rung and no sequence to
         * walk -- and it belongs with the per-op consent work (#168 part 2),
         * which rebuilds these rows anyway. Stated rather than left to be
         * discovered. */
        if (panel_probe_settled(&s_probe) && panel_probe_passed(&s_probe))
            s_tiers_run |= PANEL_RUNG_BIT(s_probe_rung);
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

static void close_interior(void);

void panel_ui_show_page(int page)
{
    if (page < 0 || page >= PAGE_COUNT) return;
    close_interior();          /* a page change always leaves an interior */
    s_page_at = page;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (s_page[i] == NULL) continue;
        if (i == page) lv_obj_remove_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
        if (s_pip[i]) {
            const bool on = (i == page);
            lv_obj_set_size(s_pip[i], on ? PIP_W : PIP_DOT, PIP_H);
            lv_obj_set_pos(s_pip[i], pip_x(i, page), PIP_Y);
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

/* A READING IS A BIG NUMBER AND A SMALL, DIMMER UNIT, right-aligned to the
 * same edge as the one above it.
 *
 * v5 renders the number at 34 px in the value colour and the unit at 20 px in
 * the label colour, as a separate span.
 *
 * An earlier version of this comment said 18 px and 11 px, and the panel
 * shipped 28/14 on the strength of it. THE MEASUREMENT WAS WRONG IN A
 * SPECIFIC, REUSABLE WAY: the prototype rendered about 1.9x its 368 px design
 * width in the browser, so every figure was divided by 368/renderedWidth to
 * get panel pixels. That is right for `getBoundingClientRect`, which returns
 * rendered geometry, and wrong for `getComputedStyle().fontSize`, which
 * returns the authored CSS value -- 18 is exactly 34/1.9. Every position in
 * this file survives the error; every font size taken the same way did not.
 *
 * WHAT MADE IT RENDER AT 1.9x IS NOT ISOLATED. A review looked for a
 * `transform: scale()` and found the file sets only 1 and 0.298, so the
 * obvious explanation is not the true one. The rule -- never divide an
 * authored CSS length by a rendering scale -- holds regardless, and is
 * written here without a cause attached rather than with a plausible one
 * invented to finish the sentence.
 *
 * (The number's size used to be PER-READING -- PWR at 28 and DOME at 34 --
 * because a 34 px Montserrat "4.43" ran into the meter. Share Tech Mono is
 * narrower and both are 34 now; the measured clearances are recorded at the
 * call sites. That deviation is retired, and this paragraph replaced the one
 * still arguing for it, which had become the second of two adjacent comments
 * giving opposite accounts of the same shipped behaviour.)
 *
 * Both readings were one flat string here
 * ("4.43 V"), which made the unit compete with the digits and let the two
 * rows' right edges disagree by however wide their text happened to be. The
 * flex row does the alignment, so a value that grows a digit still ends at
 * 342 like everything else. */
static void make_value(lv_obj_t *pg, int i, int y, const lv_font_t *num_font)
{
    lv_obj_t *row = lv_obj_create(pg);
    lv_obj_set_size(row, 150, 41);
    lv_obj_set_pos(row, VAL_RIGHT - 150, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    /* pad_all does NOT cover pad_column, which is the flex gap.
     *
     * The default theme's `card` style sets it to PAD_SMALL, and at this
     * panel's size that resolves through DISP_MEDIUM (max(368,448) picks the
     * branch) and LV_DPX_CALC(130, 12) to 10 px -- so without this the unit
     * would sit 13 px off its number where the reference has 3. An earlier
     * version of this comment said 8 and 11, which are the DISP_SMALL
     * numbers: right shape, wrong branch. */
    lv_obj_set_style_pad_column(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_kv_val[i] = lv_label_create(row);
    lv_label_set_text(s_kv_val[i], "----");
    lv_obj_set_style_text_font(s_kv_val[i], num_font, 0);
    lv_obj_set_style_text_color(s_kv_val[i], lv_color_hex(V5_TEXT), 0);

    s_kv_unit[i] = lv_label_create(row);
    lv_label_set_text(s_kv_unit[i], "");
    lv_obj_set_style_text_font(s_kv_unit[i], &techmono_20, 0);
    lv_obj_set_style_text_color(s_kv_unit[i], lv_color_hex(V5_LABEL), 0);
    lv_obj_set_style_pad_left(s_kv_unit[i], 3, 0);
}

/* Set a reading's number and unit together, and only when either changed.
 * The unit goes EMPTY when the value is unknown: "---- V" would still be
 * asserting volts about a droid we cannot hear. */
static void set_value(int i, const char *num, const char *unit)
{
    if (strcmp(lv_label_get_text(s_kv_val[i]), num) != 0)
        lv_label_set_text(s_kv_val[i], num);
    /* The reference dims a placeholder to V5_DIM. A "----" set as brightly as
     * a real reading competes with the ones that mean something. */
    lv_obj_set_style_text_color(s_kv_val[i],
        lv_color_hex(unit[0] == '\0' ? V5_DIM : V5_TEXT), 0);
    if (strcmp(lv_label_get_text(s_kv_unit[i]), unit) != 0)
        lv_label_set_text(s_kv_unit[i], unit);
}

/* Geometry measured off the reference: label boxes at row+6, pips 16x16 at
 * row+32, the connector from x=56 to x=312 at row+37, and the four columns at
 * x=43 / 128 / 214 / 307. The face's row is at y=352 and the wake frame's at
 * 376 -- v5 draws the same chain 24 px lower there. */
static void make_chain(lv_obj_t *parent, int y, chain_ui_t *c)
{
    c->row = lv_obj_create(parent);
    lv_obj_set_size(c->row, PANEL_W, 74);
    lv_obj_set_pos(c->row, 0, y);
    lv_obj_set_style_bg_opa(c->row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c->row, 0, 0);
    lv_obj_set_style_pad_all(c->row, 0, 0);
    lv_obj_clear_flag(c->row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *line = lv_obj_create(c->row);
    lv_obj_set_size(line, 256, 2);
    lv_obj_set_pos(line, 56, 37);
    lv_obj_set_style_bg_color(line, lv_color_hex(V5_RULE), 0);
    lv_obj_set_style_border_width(line, 0, 0);

    static const int k_pip_x[4] = { 43, 128, 214, 307 };
    for (int i = 0; i < 4; i++) {
        /* CENTRED ON THE PIP, not placed at the reference's left edge.
         *
         * The reference centres each label over its node, and the left edges
         * it reports are what Michroma 13 happens to produce from that. Copy
         * the edge and a narrower face drifts left of its pip -- worst for
         * "R2", the shortest string. Centring is metric-independent, which
         * is why it survived the real font landing on the next line.
         *
         * AND ON THE BASELINE, not the box top. The reference's label box
         * starts 6 px into the row with its baseline 15 px below that;
         * michroma_13's LVGL line is 14 px with a 2 px base line, so the
         * label goes at 6 + 15 - 12 = 9. It sat at 6 -- three pixels high --
         * and nobody saw, because until #161 no state that draws the chain
         * could be reached on hardware. */
        lv_obj_t *lbl = lv_label_create(c->row);
        lv_label_set_text(lbl, k_chain_label[i]);
        lv_obj_set_style_text_font(lbl, &michroma_13, 0);
        lv_obj_set_style_text_letter_space(lbl, 1, 0);
        lv_obj_set_width(lbl, 64);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(lbl, k_pip_x[i] + 8 - 32, 9);
        c->lbl[i] = lbl;

        lv_obj_t *pip = lv_obj_create(c->row);
        lv_obj_set_size(pip, 16, 16);
        lv_obj_set_pos(pip, k_pip_x[i], 32);
        lv_obj_set_style_radius(pip, 0, 0);
        c->pip[i] = pip;
    }
    lv_obj_add_flag(c->row, LV_OBJ_FLAG_HIDDEN);
}

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
    lv_obj_set_style_text_font(s_chrome_llm, &michroma_12, 0);
    lv_obj_set_style_text_letter_space(s_chrome_llm, 1, 0);
    lv_obj_set_style_text_color(s_chrome_llm, lv_color_hex(V5_SURFACE), 0);
    lv_obj_set_pos(s_chrome_llm, 160, 18);

    s_chrome_batt = lv_label_create(pg);
    lv_label_set_text(s_chrome_batt, "PWR");
    lv_obj_set_style_text_font(s_chrome_batt, &michroma_12, 0);
    lv_obj_set_style_text_letter_space(s_chrome_batt, 1, 0);
    lv_obj_set_style_text_color(s_chrome_batt, lv_color_hex(V5_SURFACE), 0);
    lv_obj_set_pos(s_chrome_batt, 217, 18);

    /* ---- SWATCH + WORD + SINCE ---------------------------------------- */
    /* STEADY, on this face and on the wake frame, and deliberately so. The
     * v5 reference blinks the swatch for offline and danger; D-012 gives
     * blink to escalation (slow attention, fast danger), and the operator
     * ruled D-012 governs (D-012, the 2026-09-07 Amendment A,
     * "Blink"). Do not re-add the
     * reference's blink -- it is the one place "match the reference" was
     * ruled not to reach. */
    s_face_swatch = lv_obj_create(pg);
    lv_obj_set_size(s_face_swatch, 18, 18);
    lv_obj_set_pos(s_face_swatch, V5_PAD, 56);
    lv_obj_set_style_radius(s_face_swatch, 2, 0);
    lv_obj_set_style_border_width(s_face_swatch, 0, 0);

    /* THE REFERENCE'S OWN TYPEFACES, at the reference's own sizes.
     *
     * Michroma for the display face, Share Tech Mono for the values. Both are
     * converted in `fonts/` and this file no longer approximates either. The
     * letter-spacing that remains is the reference's (3 px on the word, 1 px
     * on the labels), not the compensation it used to be -- the previous
     * values existed to stretch Montserrat toward Michroma's width and are
     * gone with the substitution that needed them.
     *
     * The sizes changed with the faces, and that is the point: the panel had
     * been carrying sizes chosen to make a SUBSTITUTE fit. The word was
     * montserrat_34 to approximate a 30 px Michroma; the PWR reading was
     * montserrat_28 because a 34 px Montserrat "4.43" collided with the
     * meter. Share Tech Mono is narrower, so both readings go to the
     * reference's 34 and the per-reading deviation is retired.
     *
     * MEASURED off the glass after the swap, because a collision is exactly
     * what a font change moves: "4.43" now clears the meter by 5 px and stops
     * 3 px inside the right margin. The reference's own gap is 12, and the
     * difference is not a layout fault -- it shows a 2-character percentage
     * where this shows a 4-character voltage, for the reason recorded above
     * the gauge. */
    s_face_word = lv_label_create(pg);
    lv_obj_set_style_text_font(s_face_word, &michroma_30, 0);
    lv_obj_set_style_text_letter_space(s_face_word, 3, 0);
    lv_obj_set_pos(s_face_word, 58, 50);

    s_face_since = lv_label_create(pg);
    lv_obj_set_style_text_font(s_face_since, &techmono_24, 0);
    lv_obj_set_style_text_letter_space(s_face_since, 1, 0);
    lv_obj_set_style_text_color(s_face_since, lv_color_hex(V5_LABEL), 0);
    lv_obj_set_pos(s_face_since, 58, 88);

    lv_obj_t *rule = lv_obj_create(pg);
    lv_obj_set_size(rule, PANEL_W - 2 * V5_PAD, 2);   /* 2 px, not 1 */
    lv_obj_set_pos(rule, V5_PAD, 135);
    lv_obj_set_style_bg_color(rule, lv_color_hex(V5_RULE), 0);
    lv_obj_set_style_border_width(rule, 0, 0);

    /* AC8: `waking` shows BOUNDED PROGRESS, and the bar is this rule.
     *
     * The reference has no `waking` state (it is D-023's), so there is no
     * measured geometry to match -- and the choice was between inventing a
     * new element and giving an existing one a second job. The rule sits
     * directly under the word and reason it qualifies, it is already the full
     * content width, and a rule that fills is read as progress without a
     * label. It fills in the state's own blue and is empty on every other
     * state, so at rest it is exactly the reference's rule. */
    s_waking_fill = lv_obj_create(pg);
    lv_obj_set_size(s_waking_fill, 0, 2);
    lv_obj_set_pos(s_waking_fill, V5_PAD, 135);
    lv_obj_set_style_bg_color(s_waking_fill, lv_color_hex(PANEL_C_BLUE), 0);
    lv_obj_set_style_border_width(s_waking_fill, 0, 0);
    lv_obj_set_style_radius(s_waking_fill, 0, 0);
    lv_obj_add_flag(s_waking_fill, LV_OBJ_FLAG_HIDDEN);

    /* ---- R2 PWR: label, bar graph, value ------------------------------ */
    lv_obj_t *pk = lv_label_create(pg);
    lv_label_set_text(pk, "R2 PWR");
    lv_obj_set_style_text_font(pk, &michroma_16, 0);
    lv_obj_set_style_text_letter_space(pk, 1, 0);
    lv_obj_set_style_text_color(pk, lv_color_hex(V5_LABEL), 0);
    lv_obj_set_pos(pk, V5_PAD, 173);

    /* Every fifth bar is TALLER. It is a scale, not a row of blocks: the tall
     * bars are major ticks, so a glance reads roughly how full without
     * counting. All eighteen were the same height here, which is why the
     * graph looked like a texture rather than an instrument. */
    for (int i = 0; i < PWR_BARS; i++) {
        const bool major = (i % 5) == 0;
        lv_obj_t *b = lv_obj_create(pg);
        lv_obj_set_size(b, PWR_BAR_W, major ? PWR_TALL_H : PWR_SHORT_H);
        lv_obj_set_pos(b, PWR_X + i * PWR_PITCH,
                       major ? PWR_TOP : PWR_TOP + (PWR_TALL_H - PWR_SHORT_H));
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        s_pwr_bar[i] = b;
    }

    /* The bars stand on a rule. Without it they float, which is most of why
     * the graph read as decoration. */
    lv_obj_t *pwr_base = lv_obj_create(pg);
    lv_obj_set_size(pwr_base, PWR_BARS * PWR_PITCH - (PWR_PITCH - PWR_BAR_W),
                    2);
    lv_obj_set_pos(pwr_base, PWR_X, PWR_BASE_Y);
    lv_obj_set_style_bg_color(pwr_base, lv_color_hex(V5_SURFACE), 0);
    lv_obj_set_style_border_width(pwr_base, 0, 0);

    make_value(pg, 0, PWR_VAL_Y, &techmono_34);

    /* ---- DOME: label, dial, value -------------------------------------
     * v5 draws the dial with NO NEEDLE when the heading is unknown rather
     * than hiding the dial or parking the needle at zero -- "[FIX] heading
     * may be unknown (link down)". A needle at zero is a confident lie; an
     * empty dial is the truth. */
    lv_obj_t *dk = lv_label_create(pg);
    lv_label_set_text(dk, "DOME");
    lv_obj_set_style_text_font(dk, &michroma_16, 0);
    lv_obj_set_style_text_letter_space(dk, 1, 0);
    lv_obj_set_style_text_color(dk, lv_color_hex(V5_LABEL), 0);
    lv_obj_set_pos(dk, V5_PAD, 282);

    /* NO INNER TICK MARKS, and that is faithful rather than an omission.
     *
     * The reference's SVG builds twelve of them and renders none: its element
     * helper lower-cases camelCase attributes, so `viewBox` is emitted as
     * `view-box` and ignored, and the tick array is appended as one node and
     * stringifies to "[object SVGLineElement],...". A later pass measuring
     * this dial should know both -- the second is why the ticks are absent,
     * and the FIRST is why the diameter is 108: with the viewBox applied
     * everything would scale by 0.97 and this would be ~105. */
    lv_obj_t *dial = lv_obj_create(pg);
    lv_obj_set_size(dial, DIAL_D, DIAL_D);
    lv_obj_set_pos(dial, DIAL_X, DIAL_Y);
    lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dial, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dial, 2, 0);
    lv_obj_set_style_border_color(dial, lv_color_hex(V5_SURFACE), 0);

    /* The 12 o'clock tick, sitting just OUTSIDE the rim. Without it the dial
     * has no zero, so a needle near vertical is unreadable -- which is
     * exactly where the dome sits most of the time. */
    lv_obj_t *tick = lv_obj_create(pg);
    lv_obj_set_size(tick, 4, DIAL_TICK_H);
    lv_obj_set_pos(tick, DIAL_CX - 2, DIAL_Y - DIAL_TICK_H);
    /* V5_TEXT and 4 px wide, from the reference's own SVG -- the brightest
     * thing on the dial. It is the zero the needle is read against, so a dim
     * hairline makes the whole instrument approximate. */
    lv_obj_set_style_bg_color(tick, lv_color_hex(V5_TEXT), 0);
    lv_obj_set_style_border_width(tick, 0, 0);
    lv_obj_set_style_radius(tick, 0, 0);

    /* THE HEADING WEDGE, and it was not here at all.
     *
     * The reference draws a translucent cyan pie slice spanning +/-10 degrees
     * around the heading, under the needle. Reconstructed from the prototype's
     * SVG path, whose two rim points sit at compass 30 and 50 for a heading of
     * 40. It is what makes the reading legible at a glance: a 4 px line on a
     * 108 px dial is a hairline, and the wedge is the part you actually see
     * from arm's length.
     *
     * An lv_arc whose arc width equals the radius fills all the way to the
     * centre, which is how you get a pie slice rather than a ring segment. */
    s_dome_wedge = lv_arc_create(pg);
    lv_obj_set_size(s_dome_wedge, DIAL_D, DIAL_D);
    lv_obj_set_pos(s_dome_wedge, DIAL_X, DIAL_Y);
    lv_obj_remove_style(s_dome_wedge, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_dome_wedge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_opa(s_dome_wedge, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_dome_wedge, DIAL_R, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_dome_wedge, lv_color_hex(PANEL_C_CYAN),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_dome_wedge, 41, LV_PART_INDICATOR); /* 0.16 */
    /* SQUARE CAPS. LVGL rounds arc ends by default, and with an arc width
     * equal to the radius each cap is a 27 px half-disc bolted onto a 20 px
     * sector -- the first build of this drew a blob roughly 90 degrees wide
     * and visibly off-centre from the needle it was meant to sit under. */
    lv_obj_set_style_arc_rounded(s_dome_wedge, false, LV_PART_INDICATOR);
    lv_obj_add_flag(s_dome_wedge, LV_OBJ_FLAG_HIDDEN);

    s_dome_hub = lv_obj_create(pg);
    lv_obj_set_size(s_dome_hub, 7, 7);
    lv_obj_set_pos(s_dome_hub, DIAL_CX - 3, DIAL_Y + DIAL_R - 3);
    lv_obj_set_style_radius(s_dome_hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dome_hub, 0, 0);
    lv_obj_set_style_bg_color(s_dome_hub, lv_color_hex(PANEL_C_CYAN), 0);

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
    lv_obj_set_style_line_width(s_dome_needle, 4, 0);
    lv_obj_set_style_line_color(s_dome_needle, lv_color_hex(PANEL_C_CYAN), 0);
    lv_obj_add_flag(s_dome_needle, LV_OBJ_FLAG_HIDDEN);

    make_value(pg, 1, DOME_VAL_Y, &techmono_34);

    /* ---- FAULT CHAIN: only when something is actually wrong -----------
     * I had this permanently on the resting face. v5 defines `chain` for
     * exactly four states -- the three offline modes and danger -- so a
     * healthy panel does not carry it. A fault indicator that is always
     * visible is one nobody reads. */
    make_chain(pg, 352, &s_face_chain);
}

/* ---- THE WAKE FRAME (#101 AC3) --------------------------------------------
 *
 * The first thing on the glass when something worth interrupting for begins:
 * the swatch, the word IN ITS COLOUR, the reason, a boxed subject, the chain,
 * and a sweep down the screen as it lands. Nothing tappable -- D-017: "a
 * glance that lands a thumb on a row is a glance that can arm something which
 * moves him". A press that LANDS while it is up dismisses it and does
 * nothing else, and no touch reaches the pages beneath it (main.c).
 *
 * WHEN is decided in `panel_wake` and host-tested there; this only draws.
 *
 * Every position is the reference's, measured with getBoundingClientRect on
 * the rendered prototype, and set on the BASELINE rather than the box top:
 * CSS centres a font's full ascent+descent in its line box, LVGL puts a
 * label's baseline at line_height - base_line, and the two disagree by up to
 * 5 px at these sizes. Worked through for each label below. */
#define WAKE_X        26
#define WAKE_CHAIN_Y 376                 /* the face's chain row + 24 */

static void wake_container(lv_obj_t *o)
{
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static void build_wake(lv_obj_t *parent)
{
    /* A child of the root, created after the pages AND the pips, so it covers
     * both and drifts with everything else for burn-in. */
    s_wake = lv_obj_create(parent);
    lv_obj_set_size(s_wake, PANEL_W, PANEL_H);
    lv_obj_set_pos(s_wake, 0, 0);
    lv_obj_set_style_bg_color(s_wake, lv_color_hex(V5_GROUND), 0);
    lv_obj_set_style_bg_opa(s_wake, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wake, 0, 0);
    lv_obj_set_style_radius(s_wake, 0, 0);
    lv_obj_set_style_pad_all(s_wake, 0, 0);
    lv_obj_clear_flag(s_wake, LV_OBJ_FLAG_SCROLLABLE);

    s_wake_swatch = lv_obj_create(s_wake);            /* 26x26 at (26,48) */
    lv_obj_set_size(s_wake_swatch, 26, 26);
    lv_obj_set_pos(s_wake_swatch, WAKE_X, 48);
    lv_obj_set_style_radius(s_wake_swatch, 0, 0);
    lv_obj_set_style_border_width(s_wake_swatch, 0, 0);

    /* Michroma 32, 0.07em. Baseline at 131; line 32, base line 4 -> y 103. */
    s_wake_word = lv_label_create(s_wake);
    lv_obj_set_style_text_font(s_wake_word, &michroma_32, 0);
    lv_obj_set_style_text_letter_space(s_wake_word, 2, 0);
    lv_obj_set_pos(s_wake_word, WAKE_X, 103);

    /* Share Tech Mono 28, 0.05em, label grey. Baseline 173.4; line 29, base
     * line 5 -> y 149. */
    s_wake_reason = lv_label_create(s_wake);
    lv_obj_set_style_text_font(s_wake_reason, &techmono_28, 0);
    lv_obj_set_style_text_letter_space(s_wake_reason, 1, 0);
    lv_obj_set_style_text_color(s_wake_reason, lv_color_hex(V5_LABEL), 0);
    lv_obj_set_pos(s_wake_reason, WAKE_X, 149);

    /* THE BOX ROW: the box, 16 px, then two grey lines centred beside it.
     * A flex row, because the box is as wide as its subject -- "R2" measures
     * 124 px and "NET" would not -- and the lines must follow it. */
    s_wake_row = lv_obj_create(s_wake);
    wake_container(s_wake_row);
    lv_obj_set_size(s_wake_row, LV_SIZE_CONTENT, 101);
    lv_obj_set_pos(s_wake_row, WAKE_X, 206);
    lv_obj_set_flex_flow(s_wake_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_wake_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_wake_row, 16, 0);

    /* 3 px border, 14 px radius, padding 6/20: 101 tall, as measured. LVGL
     * counts the border inside the padding space, as CSS border-box does. */
    s_wake_box = lv_obj_create(s_wake_row);
    lv_obj_set_size(s_wake_box, LV_SIZE_CONTENT, 101);
    lv_obj_set_style_bg_opa(s_wake_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wake_box, 3, 0);
    lv_obj_set_style_radius(s_wake_box, 14, 0);
    lv_obj_set_style_pad_hor(s_wake_box, 20, 0);
    lv_obj_set_style_pad_ver(s_wake_box, 6, 0);
    lv_obj_clear_flag(s_wake_box, LV_OBJ_FLAG_SCROLLABLE);

    /* Share Tech Mono 72. Baseline 279.4; line 55, base line 5 -> 229.4,
     * which is 14 below the box's content edge at 206 + 3 + 6. */
    s_wake_subject = lv_label_create(s_wake_box);
    lv_obj_set_style_text_font(s_wake_subject, &techmono_72, 0);
    lv_obj_set_pos(s_wake_subject, 0, 14);

    /* Two lines of Share Tech Mono 22 at line-height 1.4 (30.8 px): each
     * label is 22 tall, so 9 px between them keeps the pitch, and the pair
     * (53 px) centred in 101 puts the first baseline at 248 -- the
     * reference's 248.5. */
    lv_obj_t *lines = lv_obj_create(s_wake_row);
    wake_container(lines);
    lv_obj_set_size(lines, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lines, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(lines, 9, 0);
    s_wake_what = lv_label_create(lines);
    s_wake_down = lv_label_create(lines);
    lv_obj_t *const two[2] = { s_wake_what, s_wake_down };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_text_font(two[i], &techmono_22, 0);
        lv_obj_set_style_text_color(two[i], lv_color_hex(V5_LABEL), 0);
        lv_label_set_text(two[i], "");
    }

    make_chain(s_wake, WAKE_CHAIN_Y, &s_wake_chain);

    /* THE SWEEP: 3 px of the state colour with a 12 px glow, run once from
     * above the top edge to below the bottom as the frame lands. v5's own
     * [FIX] note is why it is the state colour and not white -- it was the
     * one element in the frame not carrying it. */
    s_wake_sweep = lv_obj_create(s_wake);
    lv_obj_set_size(s_wake_sweep, PANEL_W, 3);
    lv_obj_set_pos(s_wake_sweep, 0, -4);
    lv_obj_set_style_border_width(s_wake_sweep, 0, 0);
    lv_obj_set_style_radius(s_wake_sweep, 0, 0);
    lv_obj_set_style_shadow_width(s_wake_sweep, 12, 0);
    lv_obj_set_style_shadow_opa(s_wake_sweep, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_wake_sweep, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(s_wake, LV_OBJ_FLAG_HIDDEN);
}

/* Repaint the frame for a state that has just fired it. Called on entry
 * only: the one thing that changes while it is up is the DOWN count. */
static void wake_paint(panel_state_t st, panel_offline_mode_t mode)
{
    /* THE WORD CARRIES THE COLOUR HERE, unlike the resting face -- it is
     * part of how the frame reads as an interruption rather than a screen. */
    const lv_color_t c = lv_color_hex(panel_state_colour(st));
    lv_obj_set_style_bg_color(s_wake_swatch, c, 0);
    lv_label_set_text(s_wake_word, panel_state_word(st));
    lv_obj_set_style_text_color(s_wake_word, c, 0);
    lv_label_set_text(s_wake_reason, panel_state_since(st, mode));

    const char *subject, *what;
    if (panel_wake_box(st, mode, &subject, &what)) {
        lv_label_set_text(s_wake_subject, subject);
        lv_obj_set_style_text_color(s_wake_subject, c, 0);
        lv_obj_set_style_border_color(s_wake_box, c, 0);
        lv_label_set_text(s_wake_what, what);
        lv_label_set_text(s_wake_down, "");     /* set each tick, R2 only */
        lv_obj_remove_flag(s_wake_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_wake_row, LV_OBJ_FLAG_HIDDEN);
    }

    paint_chain(&s_wake_chain, st, mode);

    lv_obj_set_style_bg_color(s_wake_sweep, c, 0);
    lv_obj_set_style_shadow_color(s_wake_sweep, c, 0);
}

static void wake_sweep_y(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }
static void wake_sweep_done(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_wake_sweep, LV_OBJ_FLAG_HIDDEN);
}

/* One pass, 450 ms, linear: the reference's 0.5 s keyframes move it for the
 * first 90% and fade it below the bottom edge for the rest, where it cannot
 * be seen -- so the fade is not modelled. A one-shot, never a loop. */
static void wake_sweep_start(void)
{
    lv_anim_delete(s_wake_sweep, NULL);
    lv_obj_set_y(s_wake_sweep, -4);
    lv_obj_remove_flag(s_wake_sweep, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_wake_sweep);
    lv_anim_set_exec_cb(&a, wake_sweep_y);
    lv_anim_set_values(&a, -4, PANEL_H);
    lv_anim_set_duration(&a, 450);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_completed_cb(&a, wake_sweep_done);
    lv_anim_start(&a);
}

static void wake_show(bool up)
{
    if (up == s_wake_up) return;
    s_wake_up = up;
    if (up) lv_obj_remove_flag(s_wake, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(s_wake, LV_OBJ_FLAG_HIDDEN);
}

void panel_ui_wake_dismiss(void)
{
    panel_wake_dismiss(&s_wake_trk);
    wake_show(false);
}

bool panel_ui_wake_showing(void) { return s_wake_up; }

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
    build_wake(s_root);                 /* last: it covers the pips too */
    panel_wake_init(&s_wake_trk);
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

static void paint_chain(const chain_ui_t *ui, panel_state_t st,
                        panel_offline_mode_t mode)
{
    if (!face_has_chain(st)) {
        lv_obj_add_flag(ui->row, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    chain_t chain[4];
    face_chain(st, mode, chain);
    lv_obj_remove_flag(ui->row, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 4; i++) {
        /* THE BROKEN LINK IS HOLLOW, not filled.
         *
         * The reference draws a down or faulted node as a 3 px ring in the
         * fault colour around the ground, and lights its LABEL to match,
         * while every healthy node is a solid block with a grey label. It is
         * the same idea as the swatch: the eye finds the odd one out by
         * SHAPE first and colour second, which survives being glanced at and
         * being colour-blind. A filled amber square among filled green ones
         * relies on hue alone.
         *
         * THE LABEL HAS THREE STATES, not two. The reference gives a healthy
         * node's label V5_LABEL, an UNKNOWN node's V5_DIM, and the broken one
         * the fault colour. Painting both non-broken cases the same grey was
         * mine, and it contradicts this file's own rule one function up:
         * chain_colour() dims an unknown pip to V5_SURFACE because unknown is
         * absence rather than a claim, so a caption at healthy brightness
         * beside it says the opposite. It matters here more than in the
         * reference: this build has a source for R2 only, so three of the
         * four nodes are UNKNOWN in every chain it can draw. */
        const bool broken = (chain[i] == CH_DOWN || chain[i] == CH_FAULT);
        const uint32_t c = chain_colour(chain[i]);
        lv_obj_set_style_bg_color(ui->pip[i],
            lv_color_hex(broken ? V5_GROUND : c), 0);
        lv_obj_set_style_border_width(ui->pip[i], broken ? 3 : 0, 0);
        lv_obj_set_style_border_color(ui->pip[i], lv_color_hex(c), 0);

        uint32_t lc = V5_DIM;                    /* CH_UNK */
        if (broken)                  lc = c;
        else if (chain[i] == CH_OK)  lc = V5_LABEL;
        lv_obj_set_style_text_color(ui->lbl[i], lv_color_hex(lc), 0);
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

        /* THE WORD IS WHITE. Measured: the reference sets the resting face's
         * state word in V5_TEXT for every state THAT HAS ONE -- all ten of
         * them, danger included -- and puts the colour in the swatch alone.
         * (`asleep` and `off` are the scope of that caveat: the reference
         * draws no resting face for either, so it is not evidence about the
         * face this firmware does draw for PANEL_ST_SLEEP. An earlier version
         * of this comment said "every state", which claimed twelve states'
         * worth of evidence from ten.)
         * Colouring the word too was mine, and it doubles the signal at the
         * cost of the type: amber 30 px text on black is markedly harder to
         * read than white, and the swatch beside it already said amber.
         *
         * The WAKE frame is the exception and does colour the word, which is
         * part of how it reads as an interruption rather than a screen. */
        const uint32_t colour = panel_state_colour(st);
        lv_label_set_text(s_face_word, panel_state_word(st));
        lv_obj_set_style_text_color(s_face_word, lv_color_hex(V5_TEXT), 0);
        lv_obj_set_style_bg_color(s_face_swatch, lv_color_hex(colour), 0);
        lv_label_set_text(s_face_since, panel_state_since(st, mode));

        paint_chain(&s_face_chain, st, mode);
    }

    /* ---- R2 PWR ------------------------------------------------------- */
    const bool pwr_ok = r2_telemetry_displayable(t, &t->battery, now_ms, 60000);
    if (pwr_ok)
        snprintf(buf, sizeof buf, "%u.%02u",
                 t->battery_centivolts / 100u, t->battery_centivolts % 100u);
    else
        snprintf(buf, sizeof buf, "----");
    set_value(0, buf, pwr_ok ? "V" : "");

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
    /* GREEN fill on a V5_SURFACE track, both measured off the reference.
     *
     * An earlier review argued this should be neutral grey, on the grounds
     * that green is a verdict and the volts-to-percent mapping is inferred
     * from an unmeasured curve. The reasoning is sound about the NUMBER and
     * wrong about the BAR: the operator ruled the panel matches the v5
     * reference, and the reference fills this gauge green when the battery is
     * healthy. The caveat belongs where it already is -- on the mapping, in
     * the comment above -- not spent on desaturating an instrument until it
     * stops reading as one. Recorded rather than silently reversed, because
     * it IS a reversal.
     *
     * The fill is CONDITIONAL in the reference, which the first version of
     * this missed: it goes amber for the low-battery state. The threshold
     * below is INFERRED, like the voltage mapping it reads -- the reference
     * shows amber at 14% and green at 78% and nothing in between, so 20% is
     * a choice, not a measurement. It must not graduate to OBSERVED. Amber
     * rather than red because D-012 reserves red for danger and stop. */
    const uint32_t fill = (pwr_ok && filled * 100 / PWR_BARS <= 20)
                          ? PANEL_C_AMBER : PANEL_C_GREEN;
    for (int i = 0; i < PWR_BARS; i++)
        lv_obj_set_style_bg_color(s_pwr_bar[i],
            lv_color_hex(i < filled ? fill : V5_SURFACE), 0);

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
    /* The degree sign is U+00B0, and the unit is set in techmono_20, whose
     * range is `0x20-0x5F,0xB0` -- the 0xB0 is there for exactly this glyph
     * and for nothing else. Regenerate the fonts without it and this renders
     * as nothing. (This comment used to reason about LVGL's stock Montserrat,
     * which stopped being the unit's font in the same branch that wrote the
     * sentence sweeping up comments the font swap had falsified.) */
    set_value(1, buf, dome_ok ? "\xC2\xB0" : "");

    /* No needle when the heading is unknown. A needle parked at zero is a
     * confident lie; an empty dial is the truth. v5 does exactly this. */
    if (dome_ok) {
        static lv_point_precise_t pts[2];
        const float rad = ((float)t->dome_degrees - 90.0f) * 3.14159265f / 180.0f;
        pts[0].x = DIAL_R; pts[0].y = DIAL_R;
        /* Full radius: the reference's needle ends ON the rim. */
        pts[1].x = DIAL_R + (int)(DIAL_R * cosf(rad));
        pts[1].y = DIAL_R + (int)(DIAL_R * sinf(rad));
        lv_line_set_points(s_dome_needle, pts, 2);
        lv_obj_remove_flag(s_dome_needle, LV_OBJ_FLAG_HIDDEN);

        /* LVGL's zero is 3 o'clock and compass zero is 12, hence the +270.
         * The wedge spans +/-10 degrees, measured off the reference. */
        const int h = (int)((lroundf(t->dome_degrees) % 360 + 360) % 360);
        lv_arc_set_angles(s_dome_wedge, (h + 260) % 360, (h + 280) % 360);
        lv_obj_remove_flag(s_dome_wedge, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* Both go, together. A wedge with no needle would be a heading drawn
         * as a fuzzy claim rather than no claim at all. */
        lv_obj_add_flag(s_dome_needle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_dome_wedge, LV_OBJ_FLAG_HIDDEN);
    }
    /* The HUB is a claim as well, and it was left permanently cyan -- a live
     * centre dot beside a "----" reading, which is the same confident lie the
     * hidden needle exists to avoid. The reference draws it in V5_SURFACE
     * when the heading is unknown. */
    lv_obj_set_style_bg_color(s_dome_hub,
        lv_color_hex(dome_ok ? PANEL_C_CYAN : V5_SURFACE), 0);
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
    const uint32_t away = r2_telemetry_unreachable_ms(t, now_ms);
    panel_state_from_link(t->link == R2_TM_UP, away, &st, &mode);
    set_face(st, mode, t, now_ms);

    /* The bar moves every tick, not only on a state change: progress that
     * repaints only when the state flips is a bar that jumps from empty to
     * gone. Not counted as a change for the dimmer -- it is time passing,
     * the same as a value's last digit wobbling. */
    if (st == PANEL_ST_WAKING) {
        /* EMPTY UNTIL AN ATTEMPT HAS OPENED. Before the BLE host syncs the
         * count runs from reset, and the first scan restarts it at ~1.1 s --
         * so a bar drawn from it would fill to a quarter and snap back to
         * empty on every boot. Keyed on the attempt and not on DOWN: DOWN is
         * also passed through mid-attempt (a dropped handshake) and held
         * after a rescan fails to start, and emptying the bar there would
         * make an attempt that is still running look like one starting over.
         * No host test reaches this line; panel_ui.c is device-only. */
        const unsigned pm = t->attempt_open
                            ? panel_state_waking_permille(away) : 0u;
        const int w = (PANEL_W - 2 * V5_PAD) * (int)pm / 1000;
        if (lv_obj_get_width(s_waking_fill) != w)
            lv_obj_set_width(s_waking_fill, w);
        lv_obj_remove_flag(s_waking_fill, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_waking_fill, LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- THE WAKE FRAME (#101 AC3) -----------------------------------
     * `panel_wake` decides; this draws. It fires on ENTERING danger, offline
     * or attention, and paints on the tracker's own rising edge -- which
     * includes an escalation while it is already up, the one moment it must
     * be repainted. Never on a second change detector: two that agree only by
     * coincidence are how a frame rises that was never drawn. Firing also
     * brings the STATUS page forward, the reference's behaviour: when the
     * frame decays, the face underneath is the detail for what it said. */
    const bool up = panel_wake_step(&s_wake_trk, st, mode, now_ms);
    if (panel_wake_take_fired(&s_wake_trk) && up) {
        wake_paint(st, mode);
        wake_sweep_start();
        panel_ui_show_page(PAGE_STATUS);
    }
    wake_show(up);

    /* How long he has been gone, counting while the frame is up. Only for
     * the R2 view: it is the one link this board times, and a NET or LLM
     * count borrowed from R2's clock would be a number about the wrong
     * thing. And only if we SAW him go: an attempt that began at boot is as
     * old as our looking, not his absence, so a panel that restarted while
     * he was off for hours says NOT SEEN rather than "DOWN 4S". */
    if (up && st == PANEL_ST_OFFLINE && mode == PANEL_OFF_R2) {
        char down[16];
        if (t->attempt_from_up)
            panel_wake_format_down(away, down, sizeof down);
        else
            snprintf(down, sizeof down, "NOT SEEN");
        if (strcmp(lv_label_get_text(s_wake_down), down) != 0)
            lv_label_set_text(s_wake_down, down);
    }

    /* The four rows LINK / R2 / STORAGE / BRAIN are GONE from this page.
     * They were built as the whole status screen from AC4's wording; the v5
     * reference has no BRAIN row and no four-row status page. See
     * build_status_face() for the conflict this resolves and the ruling it
     * still needs. */

    svc_refresh(t, now_ms);

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
