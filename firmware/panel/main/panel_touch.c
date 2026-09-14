#include "panel_touch.h"

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_touch.h"
#include "lvgl.h"
#include "driver/i2c_master.h"

static const char *TAG = "touch";

#define PANEL_W 368
#define PANEL_H 448

static panel_touch_extremes_t s_ex = { .points = 0, .min_x = INT16_MAX,
                                       .max_x = INT16_MIN, .min_y = INT16_MAX,
                                       .max_y = INT16_MIN };
static volatile panel_swipe_t s_swipe;
static lv_point_t s_press_at;
static lv_point_t s_last_touch;
/* VOLATILE because three tasks read it now. Written only by touch_task, but
 * read by the LVGL timer task (indev_read, which decides whether LVGL sees a
 * finger at all, and so whether the SERVICE list scrolls) and by ui_task.
 * Every other cross-task scalar in this file was already volatile; this one
 * was not, and moving the poll off ui_task is what made that matter. */
static volatile bool s_pressing;

/* A dot under the finger.
 *
 * Not decoration -- the operator tapped all four corners and reported
 * "nothing happened", because the panel acknowledged touch in NO WAY
 * WHATSOEVER. 47 points had in fact been recorded. Asking someone to
 * calibrate a surface that cannot tell them it felt anything is a bad
 * instruction, and the silence was indistinguishable from dead hardware.
 *
 * It doubles as the diagnostic: if the dot follows a dragged finger, the
 * coordinates are live. If it sticks in one place while the finger moves,
 * the mapping is broken -- which is what the first P1 reading suggested
 * (47 points, all inside x 1..3, y 7..9, from four different corners). */
static lv_obj_t *s_dot;
static volatile int32_t s_dot_x = -1, s_dot_y = -1;

static void dot_to(int16_t x, int16_t y)
{
    if (s_dot == NULL) {
        s_dot = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_dot, 22, 22);
        lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_dot, lv_color_hex(0x35C46A), 0);
        lv_obj_set_style_bg_opa(s_dot, LV_OPA_80, 0);
        lv_obj_set_style_border_width(s_dot, 0, 0);
        lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_pos(s_dot, x - 11, y - 11);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
}

/* How long the dot lingers after the finger lifts.
 *
 * NOT forever, which is what the first version did. A 22 px circle at 80%
 * opacity, parented to lv_layer_top() so the burn-in drift never moves it,
 * sitting on the same pixels of an always-on OLED for the rest of the
 * session -- the single most pixel-static element on the panel, added by the
 * same hand that implemented AC9's mitigation two PRs earlier.
 *
 * Four seconds is the whole point of the linger: someone who taps once and
 * looks up still sees the confirmation, which is exactly what was missing
 * when the operator reported "nothing happened" three times. It just does not
 * outlive their attention. */
#define DOT_LINGER_MS 4000
static uint32_t s_dot_shown_at;

/* Set by any touch, cleared when read. The burn-in dimmer needs to know a
 * HUMAN did something, and its only other input is whether the DATA changed --
 * so without this, someone who picks up the droid and taps the panel is
 * looking at a 40% screen because R2's battery happened to read the same
 * voltage as a minute ago. Deliberate interaction is the strongest evidence
 * anyone is looking, and it was the one signal the dimmer ignored. */
static volatile bool s_activity;
static volatile bool s_press_began;   /* a finger landed since last taken */
static volatile bool s_gesture_void;  /* this press must become neither swipe nor tap */
static volatile bool s_tap;           /* a press that barely moved, released */
static volatile int16_t s_tap_x, s_tap_y;
static int32_t s_press_max;           /* furthest this press strayed */
/* HOW MANY POLLS SAW THIS PRESS. The measurement that forced it: of 22 presses
 * from a finger, the eight seen EXACTLY ONCE were every one classified a tap --
 * a press seen once reports zero travel, because its press position is also its
 * last position, so it fired a tap wherever the finger was caught mid-flight.
 * Presses seen twice or more produced whatever the geometry allowed, including
 * a 2-sample swipe of 271 px. Counted here, judged in panel_gesture. */
static uint32_t s_press_samples;
static uint8_t s_chip_id;             /* 0 = the controller did not answer */
static uint8_t s_last_hw_gesture;     /* so the log says it once, not 25x/s */

static void dot_hide(void)
{
    if (s_dot != NULL) lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
}

static void note(int16_t x, int16_t y)
{
    if (x < s_ex.min_x) s_ex.min_x = x;
    if (x > s_ex.max_x) s_ex.max_x = x;
    if (y < s_ex.min_y) s_ex.min_y = y;
    if (y > s_ex.max_y) s_ex.max_y = y;
    s_ex.points++;

    /* P1's whole question is the EXTREMES, so log the remaining gap to each
     * bezel rather than only the point. #104 measured x 1..362 on a 368-wide
     * panel and recorded its own limit: x never reached either endpoint, so a
     * 1:1 map onto 0..367 is "reasonable and unverified" at the edges -- which
     * is precisely what every edge-adjacent hit target depends on.
     *
     * An operator pressing a corner cannot tell a corner that does not
     * register from one they simply missed. Printing the gaps is what makes
     * that legible to them and to me. */
    /* Log EVERY point, not a summary. The first P1 attempt reported only
     * min/max, and "x 1..3" from four separate corners is a diagnosis that
     * arrives too late to act on -- the individual points would have shown
     * immediately that the coordinate was not tracking. */
    /* DEBUG, NOT INFO. At the 10 ms poll this fires up to 100 times a second
     * during a drag, and the console is UART0 at 115200 with no driver
     * installed -- so ESP_LOGI busy-waits on the TX FIFO rather than yielding,
     * from a task above the UI and the link. ~77 bytes a line is ~6.7 ms of
     * UART each; at 100/s that is most of the wire. It was written when the
     * poll ran 25 times a second, and the P1 calibration that wanted every
     * point is long finished. Turn it back on with a log-level override when
     * measuring touch, not by default. */
    ESP_LOGD(TAG, "point %4u  (%3d,%3d)   gaps: L%-3d R%-3d T%-3d B%-3d",
             (unsigned)s_ex.points, x, y,
             s_ex.min_x, (PANEL_W - 1) - s_ex.max_x,
             s_ex.min_y, (PANEL_H - 1) - s_ex.max_y);
}

/* READ THE CONTROLLER DIRECTLY. Do not use LVGL for this at all.
 *
 * Three attempts, three failures, three of the operator's trips to the droid:
 *
 *   1. LVGL EVENTS on lv_screen_active(). Never fired: the burn-in work put a
 *      full-size container over the screen, and LVGL does not propagate a
 *      press to a parent without LV_OBJ_FLAG_EVENT_BUBBLE. Worse, it still
 *      recorded 47 "points" clustered in x 1..3 / y 7..9, which looked like a
 *      coordinate-mapping bug and sent me after the driver. They were never
 *      the operator's taps.
 *   2. POLLING lv_indev. The indev exists -- bsp_display_get_input_dev()
 *      returns non-NULL and bsp_display_start() would have failed otherwise --
 *      and it reports nothing. Zero points through a full drag.
 *   3. This: the same register read touch_check uses, which #104 proved on
 *      hardware with 174 real points and 116 distinct x values.
 *
 * The lesson is the one CLAUDE.md already states and I did not apply: #104
 * validated the BARE-METAL path, and I moved P1 onto an LVGL path I had never
 * seen produce a single point from a finger. "It builds and the indev is
 * non-NULL" is not the same evidence.
 *
 * Registers from touch_check: 0x01 gesture, 0x02 finger count (low nibble),
 * 0x03-0x06 the 12-bit x and y. */
#define CST816_ADDR   0x15
#define REG_GESTURE   0x01

static i2c_master_dev_handle_t s_dev;
static lv_indev_t *s_lv_indev;
static void indev_read(lv_indev_t *indev, lv_indev_data_t *data);

static esp_err_t rd(uint8_t reg, uint8_t *out, size_t n)
{
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
    /* FINDING 2 (review of #150): 20 ms, not 200. It bounds the damage from
     * a wedged controller: far longer than a 6-byte read at 400 kHz needs,
     * short enough that a dead controller costs one cycle rather than the
     * session. It no longer runs on the UI tick -- the poll has its own task
     * -- so what it protects now is the touch rate, not the redraw. */
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, n, 20);
}

void panel_touch_poll(void)
{
    uint8_t buf[6];
    static int last_x = -1, last_y = -1;

    if (rd(REG_GESTURE, buf, sizeof buf) != ESP_OK) return;

    /* buf[0] IS THE CONTROLLER'S OWN GESTURE, register 0x01, and this code has
     * always read it and thrown it away. The CST816 computes slide-left and
     * slide-right at its own scan rate, which is not bounded by how often we
     * poll -- so if it reports usefully here it is a better swipe source than
     * anything reconstructed from samples we may not have taken. Logged rather
     * than used: what this particular part emits has never been observed, and
     * a gesture source is not something to adopt on the datasheet's word. */
    const uint8_t hw_gesture = buf[0];
    if (hw_gesture != 0 && hw_gesture != s_last_hw_gesture)
        ESP_LOGI(TAG, "controller gesture byte: 0x%02X", hw_gesture);
    s_last_hw_gesture = hw_gesture;

    const uint8_t fingers = buf[1] & 0x0Fu;
    const int x = ((buf[2] & 0x0F) << 8) | buf[3];
    const int y = ((buf[4] & 0x0F) << 8) | buf[5];

    if (fingers > 0) {
        if (!s_pressing) {
            s_press_at.x = x; s_press_at.y = y; s_pressing = true;
            s_press_began = true;
            s_gesture_void = false;
            s_press_max = 0;
            s_press_samples = 0;
        }
        s_press_samples++;
        {
            const int32_t ax = x > s_press_at.x ? x - s_press_at.x : s_press_at.x - x;
            const int32_t ay = y > s_press_at.y ? y - s_press_at.y : s_press_at.y - y;
            if (ax > s_press_max) s_press_max = ax;
            if (ay > s_press_max) s_press_max = ay;
        }
        s_last_touch.x = x; s_last_touch.y = y;   /* the last REAL position */
        s_activity = true;
        s_dot_x = x; s_dot_y = y;
        if (x != last_x || y != last_y) {
            note((int16_t)x, (int16_t)y);
            last_x = x; last_y = y;
        }
    } else if (s_pressing) {
        s_pressing = false;
        last_x = last_y = -1;
        /* FINDING 3 (review of #150): measure from the last position that had
         * a FINGER on it, never from the release packet. touch_check only ever
         * used a fingers==0 read to mark a LIFT; it never established that x/y
         * mean anything in that packet. If they are stale it happens to work;
         * if they are zeroed, every swipe becomes a false left-swipe. Not a
         * coin worth flipping for a gesture that changes pages. */
        const panel_press_t press = {
            .press_x = s_press_at.x,   .press_y = s_press_at.y,
            .last_x  = s_last_touch.x, .last_y  = s_last_touch.y,
            .max_dev = s_press_max,
            .samples = s_press_samples,
            .voided  = s_gesture_void,
        };
        const panel_gesture_t g = panel_gesture_classify(&press);
        s_gesture_void = false;

        /* The tap lands where the finger went DOWN, which is the row it was
         * aimed at, not wherever it drifted to. */
        switch (g) {
        case PANEL_GESTURE_SWIPE_LEFT:  s_swipe = PANEL_SWIPE_LEFT;  break;
        case PANEL_GESTURE_SWIPE_RIGHT: s_swipe = PANEL_SWIPE_RIGHT; break;
        case PANEL_GESTURE_TAP:
            s_tap_x = (int16_t)s_press_at.x;
            s_tap_y = (int16_t)s_press_at.y;
            s_tap = true;
            break;
        case PANEL_GESTURE_NONE:
            break;
        }

        /* EVERY release, with the numbers the verdict was made from. The whole
         * diagnosis of the swipe bug came from raw points and a reconstruction
         * afterwards, because nothing logged the decision -- so the one thing
         * worth keeping from that session is this line. */
        ESP_LOGI(TAG, "press: n=%u (%3d,%3d)->(%3d,%3d) d=(%d,%d) dev=%d -> %s",
                 (unsigned)s_press_samples, (int)s_press_at.x, (int)s_press_at.y,
                 (int)s_last_touch.x, (int)s_last_touch.y,
                 (int)(s_last_touch.x - s_press_at.x),
                 (int)(s_last_touch.y - s_press_at.y), (int)s_press_max,
                 g == PANEL_GESTURE_SWIPE_LEFT  ? "SWIPE LEFT"  :
                 g == PANEL_GESTURE_SWIPE_RIGHT ? "SWIPE RIGHT" :
                 g == PANEL_GESTURE_TAP         ? "TAP" : "nothing");
        /* The dot STAYS where the finger left it. Hiding it on release is what
         * makes a working panel look dead to someone who taps once and looks
         * up -- they see nothing, exactly as reported three times. */
    }
}

/* LVGL work, and ONLY LVGL work. Must hold the display lock.
 *
 * Split from panel_touch_poll because fixing review finding 2 -- do not do a
 * blocking I2C read under the display lock -- immediately created the mirror
 * bug: dot_to() calls LVGL, so moving the whole poll outside the lock would
 * have raced the UI task against itself. The read and the render want
 * different locks, so they are different functions. */
void panel_touch_render(void)
{
    static int32_t drawn_x = -1, drawn_y = -1;

    if (s_dot_x < 0) return;

    /* Only when it MOVED. Re-issuing lv_obj_set_pos with identical values 25
     * times a second marks the area dirty every tick and redraws a region that
     * did not change -- forever, once any touch has happened. */
    if (s_dot_x != drawn_x || s_dot_y != drawn_y) {
        drawn_x = s_dot_x; drawn_y = s_dot_y;
        dot_to((int16_t)s_dot_x, (int16_t)s_dot_y);
        s_dot_shown_at = (uint32_t)(esp_timer_get_time() / 1000);
    } else if (!s_pressing && s_dot_shown_at != 0 &&
               (uint32_t)(esp_timer_get_time() / 1000) - s_dot_shown_at > DOT_LINGER_MS) {
        dot_hide();
        s_dot_shown_at = 0;
        drawn_x = drawn_y = -1;
        s_dot_x = s_dot_y = -1;
    }
}

void panel_touch_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus != NULL) {
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = CST816_ADDR,
            .scl_speed_hz    = 400000,
        };
        if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) s_dev = NULL;
    }
    if (s_dev != NULL) {
        /* Prove the controller answers BEFORE trusting any silence from it.
         * touch_check reads 0xB7 = CST820 here; if we cannot, "no touch" is an
         * instrument failure and not a fact about anyone's finger. */
        uint8_t id = 0;
        const bool id_ok = rd(0xA7, &id, 1) == ESP_OK;
        if (id_ok) s_chip_id = id;
        if (id_ok)
            ESP_LOGI(TAG, "CST816 chip id 0x%02X %s", id,
                     id == 0xB7 ? "(CST820 -- matches #104)" : "(UNEXPECTED)");
        else
            ESP_LOGE(TAG, "chip id read FAILED -- silence below proves nothing");

        /* FINDING 1 (review of #150), and the most important of the three:
         * bsp_display_start() has ALREADY registered
         * esp_lcd_touch_new_i2c_cst816s on this same 0x15 device and driven it
         * from an LVGL indev. Adding our own handle put TWO DRIVERS on ONE
         * CONTROLLER.
         *
         * That is not merely untidy. The CST816 CLEARS its touch data on read,
         * so two pollers steal each other's events and whichever asks second
         * sees fingers == 0. It is a plausible root cause of this entire
         * episode -- and it would certainly have broken this attempt, which is
         * why the review catching it mattered more than the other two findings
         * combined.
         *
         * We do not use LVGL for touch at all, so delete its indev. One
         * reader, and it is ours. */
        lv_indev_t *indev = bsp_display_get_input_dev();
        if (indev != NULL) {
            /* lvgl_port_remove_touch(), NOT lv_indev_delete().
             *
             * The first fix used the bare LVGL call and the re-review blocked
             * it: esp_lvgl_port's lvgl_port_add_touch() allocated a touch_ctx
             * and registered an interrupt callback, and deleting the indev
             * underneath it leaks both and leaves the BSP's static disp_indev
             * pointer dangling. Reaching past a wrapper to free the thing it
             * owns is how you get a use-after-free that only shows up when
             * something later asks the BSP for its input device.
             *
             * This is the paired teardown for the paired constructor. */
            lvgl_port_remove_touch(indev);
            ESP_LOGI(TAG, "removed the BSP's LVGL touch indev via "
                          "lvgl_port_remove_touch -- two drivers on one "
                          "controller steal each other's reads");
        }

        /* ...and give LVGL one back, fed from OUR reads. Without this LVGL has
         * no input device at all, and the SERVICE list cannot scroll: the rows
         * below the fold could not be reached by any gesture. Found in review
         * of #153. Taps and swipes do NOT go through it -- they are read here
         * and routed by panel_ui -- so LVGL's only job is scrolling. */
        s_lv_indev = lv_indev_create();
        if (s_lv_indev != NULL) {
            lv_indev_set_type(s_lv_indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(s_lv_indev, indev_read);
            /* LVGL starts scrolling after 10 px by default, and a tap is
             * anything under PANEL_TAP_PX. Between the two a finger both
             * scrolled the list AND tapped it, and the tap then landed on
             * whichever row had moved under the press point. Matching the
             * limits makes them exclusive. */
            lv_indev_set_scroll_limit(s_lv_indev, PANEL_TAP_PX);
            ESP_LOGI(TAG, "LVGL indev re-created, fed from our register reads "
                          "-- one hardware reader, and LVGL can scroll again");
        } else {
            ESP_LOGE(TAG, "lv_indev_create failed: the SERVICE list will not "
                          "scroll and its lower rows are UNREACHABLE");
        }
    }

    if (s_dev == NULL) {
        /* Say so loudly. A panel that silently has no touch looks exactly like
         * one nobody has touched -- and P1's answer would then be "no points",
         * which reads as a hardware finding rather than a wiring bug. LVGL's
         * indev is deliberately NOT consulted here: it was non-NULL through
         * both failed attempts and reported nothing, so its existence is not
         * evidence that touch works. */
        ESP_LOGE(TAG, "NO I2C DEVICE — touch is not wired. Any 'no points'");
        ESP_LOGE(TAG, "  result below is an INSTRUMENT FAILURE, not evidence.");
        return;
    }
    ESP_LOGI(TAG, "touch wired by DIRECT REGISTER READ (not LVGL); swipe "
                  "threshold %d px", PANEL_SWIPE_PX);
}

void panel_touch_extremes(panel_touch_extremes_t *out)
{
    if (out) *out = s_ex;
}

/* LVGL's input device, fed from OUR register reads.
 *
 * #150 deleted the BSP's indev because two drivers polling one CST816 steal
 * each other's events -- the controller clears its data on read. That was
 * right, and it left LVGL with NO input at all, which was fine while nothing
 * in the UI needed touch.
 *
 * The SERVICE page needs it: a scrollable list that LVGL cannot receive a
 * finger for is decorative, and the rows below the fold were unreachable.
 * Scrolling is ALL it is for -- taps and swipes are decided in
 * panel_touch_poll and routed by panel_ui, never by LVGL events.
 *
 * So LVGL gets an indev back, but NOT another driver: this callback serves
 * the state panel_touch_poll() already read. One reader of the hardware,
 * still ours, and LVGL gets its events.
 *
 * THREADING: poll() runs on touch_task -- its own, since the sampling fix --
 * and this runs on the LVGL timer task, so
 * these scalars are genuinely shared now rather than same-task as before.
 * They are word-sized and volatile; the worst case is one frame of stale
 * coordinate, which is a redraw away from correct and is why a lock would be
 * more cost than the problem. */
static void indev_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = (s_dot_x >= 0) ? (int32_t)s_dot_x : 0;
    data->point.y = (s_dot_y >= 0) ? (int32_t)s_dot_y : 0;
    data->state   = s_pressing ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

bool panel_touch_take_activity(void)
{
    const bool a = s_activity;
    s_activity = false;
    return a;
}

bool panel_touch_take_press(void)
{
    const bool p = s_press_began;
    s_press_began = false;
    return p;
}

void panel_touch_void_gesture(void)
{
    s_gesture_void = s_pressing;   /* only a press still in progress */
    s_swipe = PANEL_SWIPE_NONE;
    s_tap = false;
}

bool panel_touch_take_tap(int16_t *x, int16_t *y)
{
    /* THE COORDINATES COME OUT WITH THE FLAG, not after it. touch_task runs
     * ABOVE this one and can land between the clear and the reads, so the tap
     * being reported would be delivered at the NEXT tap's coordinates -- and
     * that next tap delivered again on the following tick, at a row the
     * operator never aimed at. Impossible while both ran on ui_task; the
     * moment the poll moved to its own task it was not. */
    if (!s_tap) return false;
    const int16_t tx = s_tap_x, ty = s_tap_y;
    s_tap = false;
    if (x) *x = tx;
    if (y) *y = ty;
    return true;
}

uint8_t panel_touch_chip_id(void) { return s_chip_id; }

panel_swipe_t panel_touch_take_swipe(void)
{
    const panel_swipe_t s = s_swipe;
    s_swipe = PANEL_SWIPE_NONE;
    return s;
}
