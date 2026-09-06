#include "panel_ui.h"

#include <stdio.h>
#include <string.h>

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
static lv_obj_t *s_header_val;
static lv_obj_t *s_row_value[N_ROWS];
static lv_obj_t *s_row_dot[N_ROWS];
static panel_sev_t s_row_sev[N_ROWS];

/* Severity colours. Deliberately NOT D-012's LED language: that scheme is
 * tuned for a fixture glanced from across a room, where blue is idle because
 * it is the dimmest corner. This is read up close and deliberately, and its
 * job is legibility, not mood. Reusing the LED palette here would make two
 * different vocabularies look like one. */
static lv_color_t sev_colour(panel_sev_t s)
{
    switch (s) {
    case PANEL_OK:      return lv_color_hex(0x35C46A);
    case PANEL_WARN:    return lv_color_hex(0xE0A020);
    case PANEL_BAD:     return lv_color_hex(0xE04040);
    case PANEL_UNKNOWN:
    default:            return lv_color_hex(0x606060);
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

void panel_ui_create(void)
{
    s_screen = lv_scr_act();
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(s_screen);
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
        make_row(s_screen, i);
        s_row_sev[i] = PANEL_UNKNOWN;
        lv_obj_set_style_bg_color(s_row_dot[i], sev_colour(PANEL_UNKNOWN), 0);
    }
}

static void set_row(int i, panel_sev_t sev, const char *text)
{
    if (s_row_sev[i] != sev) {
        s_row_sev[i] = sev;
        lv_obj_set_style_bg_color(s_row_dot[i], sev_colour(sev), 0);
    }
    const char *cur = lv_label_get_text(s_row_value[i]);
    if (cur == NULL || strcmp(cur, text) != 0)
        lv_label_set_text(s_row_value[i], text);
}

void panel_ui_update(const r2_telemetry_t *t, uint32_t now_ms)
{
    char buf[48];

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
        set_row(ROW_LINK, PANEL_BAD, "no link");
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
    if (cur == NULL || strcmp(cur, buf) != 0)
        lv_label_set_text(s_header_val, buf);
}
