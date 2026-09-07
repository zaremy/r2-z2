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
static lv_obj_t *s_root;
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

    lv_obj_t *header = lv_obj_create(s_root);
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
        make_row(s_root, i);
        s_row_sev[i] = PANEL_UNKNOWN;
        lv_obj_set_style_bg_color(s_row_dot[i], sev_colour(PANEL_UNKNOWN), 0);
    }
}

static bool s_changed;

static void set_row(int i, panel_sev_t sev, const char *text)
{
    if (s_row_sev[i] != sev) {
        s_row_sev[i] = sev;
        s_changed = true;
        lv_obj_set_style_bg_color(s_row_dot[i], sev_colour(sev), 0);
    }
    const char *cur = lv_label_get_text(s_row_value[i]);
    if (cur == NULL || strcmp(cur, text) != 0) {
        s_changed = true;
        lv_label_set_text(s_row_value[i], text);
    }
}

bool panel_ui_update(const r2_telemetry_t *t, uint32_t now_ms)
{
    char buf[48];
    s_changed = false;

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
    if (cur == NULL || strcmp(cur, buf) != 0) {
        s_changed = true;
        lv_label_set_text(s_header_val, buf);
    }
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
#define BURN_DIM_PERCENT   40
#define BURN_FULL_PERCENT  80

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
    }
}

int panel_ui_drift_step(void) { return s_drift_step; }
bool panel_ui_is_dimmed(void) { return s_dimmed; }
