#include "panel_touch.h"

#include "bsp/esp-bsp.h"
#include "esp_log.h"
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
static bool s_pressing;

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
    ESP_LOGI(TAG, "point %4u  (%3d,%3d)   gaps: L%-3d R%-3d T%-3d B%-3d",
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

static esp_err_t rd(uint8_t reg, uint8_t *out, size_t n)
{
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
    /* FINDING 2 (review of #150): 20 ms, not 200. This runs every UI tick,
     * and a wedged controller with a 200 ms timeout would stall the UI for
     * most of its life. 20 ms is far longer than a 6-byte read at 400 kHz
     * needs and bounds the damage. */
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, n, 20);
}

/* Old, dead: kept only as a name so the history above is not abstract.
 * POLL THE INPUT DEVICE. Do not use LVGL events for this.
 *
 * The first version attached PRESSED/PRESSING/RELEASED handlers to
 * lv_screen_active(). It never saw a real touch, because the burn-in work put
 * a full-size root container over the screen and LVGL does NOT propagate a
 * press to a parent unless LV_OBJ_FLAG_EVENT_BUBBLE is set. Every touch landed
 * on the container and stopped there.
 *
 * What made that expensive rather than merely wrong: it still recorded 47
 * "points", all inside x 1..3 / y 7..9, which looked like a coordinate-mapping
 * bug and sent me after the driver. They were not the operator's taps at all.
 * The operator tapped four corners, saw nothing, and reported dead touch --
 * and the log agreed with them for the wrong reason.
 *
 * Polling the indev cannot be defeated by hit-testing, bubbling, z-order or a
 * widget added later. For a calibration instrument that is the right trade:
 * it reads the device, not the UI's opinion of the device. */
void panel_touch_poll(void)
{
    uint8_t buf[6];
    static int last_x = -1, last_y = -1;

    if (rd(REG_GESTURE, buf, sizeof buf) != ESP_OK) return;

    const uint8_t fingers = buf[1] & 0x0Fu;
    const int x = ((buf[2] & 0x0F) << 8) | buf[3];
    const int y = ((buf[4] & 0x0F) << 8) | buf[5];

    if (fingers > 0) {
        if (!s_pressing) { s_press_at.x = x; s_press_at.y = y; s_pressing = true; }
        s_last_touch.x = x; s_last_touch.y = y;   /* the last REAL position */
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
        const int32_t dx = s_last_touch.x - s_press_at.x;
        if (dx <= -PANEL_SWIPE_PX)      s_swipe = PANEL_SWIPE_LEFT;
        else if (dx >= PANEL_SWIPE_PX)  s_swipe = PANEL_SWIPE_RIGHT;
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
    if (s_dot_x >= 0) dot_to((int16_t)s_dot_x, (int16_t)s_dot_y);
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
        if (rd(0xA7, &id, 1) == ESP_OK)
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

panel_swipe_t panel_touch_take_swipe(void)
{
    const panel_swipe_t s = s_swipe;
    s_swipe = PANEL_SWIPE_NONE;
    return s;
}
