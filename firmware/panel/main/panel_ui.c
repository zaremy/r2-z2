#include "panel_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

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
#define N_ROWS          4

enum { ROW_LINK = 0, ROW_R2, ROW_STORAGE, ROW_BRAIN };

static const char *k_row_label[N_ROWS] = { "LINK", "R2", "STORAGE", "BRAIN" };

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
static lv_obj_t *s_header_val;
static lv_obj_t *s_row_value[N_ROWS];
static lv_obj_t *s_row_dot[N_ROWS];
static panel_sev_t s_row_sev[N_ROWS];

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
static lv_color_t sev_colour(panel_sev_t s)
{
    switch (s) {
    case PANEL_OK:      return lv_color_hex(0x35C46A);
    case PANEL_WARN:    return lv_color_hex(0xE0A020);
    /* NOTHING CURRENTLY RENDERS PANEL_BAD, and that is the point rather than
     * an oversight. Red is danger and stop only (D-012), and no state the
     * panel can reach today qualifies: a dead link is needs-monitoring, an
     * unwired row is unknown. Kept because the ladder needs a rung above
     * warn the day something genuinely alarming exists -- a fallen droid, a
     * thermal fault. Declared here so its absence reads as deliberate. */
    case PANEL_BAD:     return lv_color_hex(0xE04040);
    case PANEL_UNKNOWN:
    default:
        /* Grey is NOT a colour claim -- it is the absence of one. D-012 has
         * six colours for six meanings and none of them is "we cannot say".
         * Adding a seventh would be a new decision about the BODY made for a
         * screen's convenience, which D-012 Amendment A explicitly declines. */
        return lv_color_hex(0x606060);
    }
}

const char *panel_sev_name(panel_sev_t s)
{
    switch (s) {
    case PANEL_OK:      return "ok";
    case PANEL_WARN:    return "warn";
    case PANEL_BAD:     return "bad";
    case PANEL_UNKNOWN: return "unknown";
    default:            return "?";
    }
}

static lv_obj_t *make_row(lv_obj_t *parent, int index)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, PANEL_W, ROW_H);
    lv_obj_set_pos(row, 0, HEADER_H + index * ROW_H);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x101014), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x282830), 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* The severity dot. It carries the state; the text explains it. A row must
     * be readable as ok/not-ok before any word is read. */
    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_pos(dot, ROW_PAD, (ROW_H - 14) / 2);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    s_row_dot[index] = dot;

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, k_row_label[index]);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x8A8A98), 0);
    lv_obj_set_pos(label, ROW_PAD + 26, 16);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text(value, "--");
    lv_obj_set_style_text_font(value, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(value, lv_color_hex(0xF0F0F4), 0);
    lv_obj_set_pos(value, ROW_PAD + 26, 42);
    s_row_value[index] = value;

    return row;
}

/* The page dots. The vault's description is "swipe or tap the dots", so they
 * are an affordance and not decoration -- but they are drawn here and made
 * tappable in child 4 proper. Drawn small and low-contrast: this is an
 * instrument, and a navigation cue that competes with the reading is a
 * navigation cue in the wrong place. */
static void make_pips(lv_obj_t *parent)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *d = lv_obj_create(parent);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_pos(d, PANEL_W / 2 - 22 + i * 16, PANEL_H - 22);
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
        if (s_pip[i])
            lv_obj_set_style_bg_color(s_pip[i],
                lv_color_hex(i == page ? 0xF0F0F4 : 0x404048), 0);
    }
}

int panel_ui_page(void) { return s_page_at; }
int panel_ui_page_count(void) { return PAGE_COUNT; }

const char *panel_ui_page_name(int page)
{
    return (page >= 0 && page < PAGE_COUNT) ? k_page_name[page] : "?";
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

    lv_obj_t *header = lv_obj_create(s_page[PAGE_STATUS]);
    lv_obj_set_size(header, PANEL_W, HEADER_H);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(header);
    lv_label_set_text(name, "R2-Z2");
    lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0xF0F0F4), 0);
    lv_obj_set_pos(name, ROW_PAD, 20);

    s_header_val = lv_label_create(header);
    lv_label_set_text(s_header_val, "starting");
    lv_obj_set_style_text_font(s_header_val, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_header_val, lv_color_hex(0x8A8A98), 0);
    lv_obj_set_pos(s_header_val, 150, 25);

    for (int i = 0; i < N_ROWS; i++) {
        make_row(s_page[PAGE_STATUS], i);
        s_row_sev[i] = PANEL_UNKNOWN;
        lv_obj_set_style_bg_color(s_row_dot[i], sev_colour(PANEL_UNKNOWN), 0);
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
static bool s_repainted;    /* a value differs; not a reason to wake the panel */

static void set_row(int i, panel_sev_t sev, const char *text)
{
    if (s_row_sev[i] != sev) {
        s_row_sev[i] = sev;
        s_changed = true;
        lv_obj_set_style_bg_color(s_row_dot[i], sev_colour(sev), 0);
    }
    const char *cur = lv_label_get_text(s_row_value[i]);
    if (cur == NULL || strcmp(cur, text) != 0) {
        s_repainted = true;
        lv_label_set_text(s_row_value[i], text);
    }
}

bool panel_ui_update(const r2_telemetry_t *t, uint32_t now_ms)
{
    char buf[48];
    s_changed = s_repainted = false;

    /* ---- LINK ---------------------------------------------------------- */
    switch (t->link) {
    case R2_TM_UP:
        set_row(ROW_LINK, PANEL_OK, "connected");
        break;
    case R2_TM_SCANNING:
        set_row(ROW_LINK, PANEL_WARN, "looking for him");
        break;
    case R2_TM_CONNECTING:
    case R2_TM_HANDSHAKING:
        set_row(ROW_LINK, PANEL_WARN, "connecting");
        break;
    case R2_TM_DOWN:
    default:
        /* YELLOW, not red. A dropped link is not danger -- nothing is going to
         * hurt him or anyone because the radio stopped answering -- and D-012
         * reserves red for danger and stop only. This row WAS red, because it
         * felt bad, which is exactly the reasoning D-012 rejected when it took
         * red away from "issue pending resolution". Red stays available for a
         * state that genuinely warrants alarm. */
        set_row(ROW_LINK, PANEL_WARN, "no link");
        break;
    }

    /* ---- R2 ------------------------------------------------------------
     * Battery, and the staleness rule is the whole reason this reads "--"
     * rather than the last number we saw. r2_telemetry_displayable() refuses a
     * reading whose link has dropped, so a dead link CANNOT leave a comforting
     * voltage on the glass. That is the failure this panel exists to not have:
     * it would look most trustworthy at the exact moment it was wrong. */
    if (r2_telemetry_displayable(t, &t->battery, now_ms, 60000)) {
        snprintf(buf, sizeof buf, "%u.%02u V",
                 t->battery_centivolts / 100u, t->battery_centivolts % 100u);
        /* Thresholds are PLACEHOLDERS and are marked as such. 4.42 V is the
         * only value ever observed and he was on the charger for all of it, so
         * the discharge curve is unmeasured -- there is no evidence for where
         * "low" begins. Showing a warn colour from a guessed threshold would be
         * inventing a fact on the glass, so everything readable is OK until
         * somebody measures it. */
        set_row(ROW_R2, PANEL_OK, buf);
    } else if (t->link == R2_TM_UP) {
        set_row(ROW_R2, PANEL_UNKNOWN, "asking...");
    } else {
        set_row(ROW_R2, PANEL_UNKNOWN, "--");
    }

    /* ---- STORAGE -------------------------------------------------------
     * Nothing writes to storage yet, so this row states that rather than
     * showing a plausible number. A row that invents content to look finished
     * is worse than one that admits it is not wired. */
    set_row(ROW_STORAGE, PANEL_UNKNOWN, "not wired");

    /* ---- BRAIN ---------------------------------------------------------
     * Likewise: no reasoning layer runs on this board yet. */
    set_row(ROW_BRAIN, PANEL_UNKNOWN, "not wired");

    /* The header carries LOSSES, not a ratio -- and only real ones.
     *
     * It first showed "N/N+1 answered", which is always one short because the
     * most recent request has not been answered yet: at any instant one is in
     * flight. Over six minutes it read 4/5, 8/9, 12/13, 16/17, 20/21, 24/25
     * with ZERO actual losses. On an instrument meant to be glanced at, a
     * permanent one-short reads as a standing fault, and a fault indicator
     * that is always on is one nobody reads -- the same failure as the fifty
     * keepalive acks that buried a real unmatched frame.
     *
     * So: one outstanding request is normal and invisible. More than one is
     * the condition worth naming, and it is the number that would have made
     * the escaping bug legible instead of it hiding in an endurance run. */
    const uint32_t outstanding = t->requests - t->responses;
    if (outstanding > 1)
        snprintf(buf, sizeof buf, "%u unanswered", (unsigned)(outstanding - 1));
    else
        snprintf(buf, sizeof buf, "%s", r2_telemetry_link_name(t->link));
    const char *cur = lv_label_get_text(s_header_val);
    if (cur == NULL || strcmp(cur, buf) != 0) {
        s_repainted = true;
        lv_label_set_text(s_header_val, buf);
    }
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
