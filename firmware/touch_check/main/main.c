/* touch_check -- does the touch controller return a coordinate?
 *
 * #103 gap A3. The controller had answered an I2C address probe since the first
 * board survey, but no driver had ever read a POINT off it, and the whole panel
 * interaction model assumes it works. This proves it with the smallest possible
 * amount of code: no LVGL, no BSP, no display.
 *
 * ANSWERED 2026-08-31, on hardware, operator driving:
 *   - 174 points, 116 distinct x, 113 distinct y -- coordinates track the finger
 *   - drags are real: longest strictly-monotonic run of CONTIGUOUS samples is
 *     16, in both x and y (stable whether "contiguous" means no consecutive
 *     jump over 25, 40 or 50 px, so the figure does not depend on that choice).
 *     NOTE the loop below only prints when x or y changed, so "no repeated
 *     points" is true by construction and proves nothing. NOTE ALSO that the
 *     first capture predates the LIFT marker below: an unconstrained run over
 *     it reported 22, but that run contained a 197 px jump and spanned two
 *     separate touches. Segment on LIFT, and keep a discontinuity split as a
 *     backstop -- LIFT is one fingers==0 read on a 20 ms poll, so a transient
 *     zero can split a real stroke and a brief lift can fall between polls.
 *   - observed range x 1..362, y 1..447 against the 368x448 panel
 *   - the 16 px V2 column offset (V2_PANEL_X_GAP) does NOT apply to touch:
 *     five samples came in BELOW x=16, impossible if it did. Do not add the
 *     display gap. NOTE this refutes the offset only -- x never reached either
 *     endpoint, so a 1:1 map onto 0..367 is reasonable but UNVERIFIED at the
 *     edges; calibrate before trusting anything flush to the bezel.
 *   - chip ID register 0xA7 reads 0xB7 = CST820. Confirms which half of the
 *     long-standing naming disagreement (docs/research/board-revision.md:43-45)
 *     describes the fitted part.
 *   - the gesture register (0x01) is wired and fires, but is UNRESOLVED: it
 *     reported 4 gestures over the 174 LOGGED points, which is not a rate, and
 *     the absent tap is this harness's fault -- the loop below logs only on a
 *     coordinate CHANGE, and a tap holds coordinates still, so a tap cannot be
 *     seen here at all. Settle it with a run that logs every poll.
 *
 * Raw capture and a script that re-derives every number above:
 *   firmware/touch_check/results/a3-touch-2026-08-31.txt   (touch capture)
 *   firmware/touch_check/results/identify-2026-08-31.txt   (chip-ID read)
 *   firmware/touch_check/results/analyse.py
 *
 * READING THE OUTPUT -- this cost most of a session to work out:
 *   This board's only port is the ESP32-S3 native USB-Serial/JTAG. Opening it
 *   with a plain pyserial open RESETS THE CHIP INTO THE ROM DOWNLOADER, and
 *   esptool's "Hard resetting via RTS pin" is a no-op because there is no RTS
 *   line. So the app is not running while you listen, and you read silence and
 *   conclude the hardware is dead. It is not.
 *     1. idf.py -p <port> flash          (this DOES leave the app running)
 *     2. attach with idf_monitor --no-reset, in a loop that reattaches when the
 *        port reappears -- never a bare pyserial open
 *   Validate the listener against a known-good app (13_display_colorbar, whose
 *   output you can see with your eyes) before believing any silence.
 *
 * STRUCTURE, and why it is not a straight line of code: the heartbeat task
 * starts FIRST, before any hardware is touched, and prints the stage the main
 * thread has reached. An earlier revision printed only on touch, which made
 * "running but untouched", "wedged in a blocking call" and "never started" all
 * look identical -- silence. A harness that cannot show a positive control
 * cannot support a negative conclusion, so the liveness signal has to be
 * independent of the thing under test.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_variant.h"

#define I2C_PORT        I2C_NUM_0
#define I2C_SDA         GPIO_NUM_15
#define I2C_SCL         GPIO_NUM_14
#define I2C_HZ          400000
#define CST816_ADDR     0x15

/* CST816 register map. 0xA7 is the part number; the touch block is 0x01-0x06. */
#define REG_GESTURE     0x01
#define REG_CHIPID      0xA7

#define PANEL_W         368
#define PANEL_H         448

/* Written by the main thread, read by the heartbeat. Single writer, single
 * reader, word-sized -- no lock needed for a diagnostic. */
static const char * volatile g_stage = "boot";
static volatile int  g_chipid = -1;
static volatile int  g_points = 0;
static volatile int  g_readfail = 0;   /* consecutive failed reads, see heartbeat */
static volatile int  g_minx = 9999, g_maxx = -1, g_miny = 9999, g_maxy = -1;

static esp_err_t rd(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, out, n, 200);
}

static const char *gesture_name(uint8_t g)
{
    switch (g) {
    case 0x00: return "none";
    case 0x01: return "slide-down";
    case 0x02: return "slide-up";
    case 0x03: return "slide-left";
    case 0x04: return "slide-right";
    case 0x05: return "tap";
    case 0x0B: return "double-tap";
    case 0x0C: return "long-press";
    default:   return "?";
    }
}

static const char *chip_name(int id)
{
    switch (id) {
    case 0xB4: return "CST816S";
    case 0xB5: return "CST816T";
    case 0xB6: return "CST816D";
    case 0xB7: return "CST820";
    case -1:   return "not read yet";
    default:   return "UNRECOGNISED";
    }
}

static void heartbeat(void *arg)
{
    uint32_t n = 0;
    while (1) {
        n++;
        if (g_readfail > 10) {
            /* A dead bus mid-run otherwise looks identical to a finger that
             * stopped moving: same stage, same frozen range. Say which. */
            printf("[alive %lu] stage=%s  *** %d CONSECUTIVE FAILED READS -- the bus\n"
                   "            stopped answering; a frozen range below is an\n"
                   "            INSTRUMENT FAILURE, not an absence of touch ***\n",
                   (unsigned long)n, g_stage, g_readfail);
        } else if (g_points > 0) {
            printf("[alive %lu] stage=%s  points=%d  range x %d..%d  y %d..%d\n",
                   (unsigned long)n, g_stage, g_points,
                   g_minx, g_maxx, g_miny, g_maxy);
            /* P1 (#101): the extremes are the whole question. The original run
             * reported x 1..362 on a 368-wide panel and its own note says x
             * NEVER REACHED EITHER ENDPOINT, so a 1:1 map is unverified at the
             * bezel -- which is exactly what any edge-adjacent hit target
             * depends on.
             *
             * Printing the remaining GAP to each edge, rather than only the
             * range, is what makes this drivable: an operator pressing a
             * corner has no way to tell a corner that does not register from
             * one they simply missed, and neither do I from a raw range that
             * has not moved. */
            printf("            edge gaps: left %d  right %d  top %d  bottom %d"
                   "   (0 = the bezel is reachable)\n",
                   g_minx, (PANEL_W - 1) - g_maxx,
                   g_miny, (PANEL_H - 1) - g_maxy);
        } else {
            printf("[alive %lu] stage=%s  chip=0x%02X (%s)  no touch yet\n",
                   (unsigned long)n, g_stage, g_chipid & 0xFF, chip_name(g_chipid));
        }
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    /* FIRST. Nothing above this line may block, or the diagnostic is useless. */
    xTaskCreate(heartbeat, "hb", 3072, NULL, 5, NULL);
    vTaskDelay(pdMS_TO_TICKS(200));

    printf("\n================ touch_check ================\n");
    printf("  #103 A3 -- does the CST816 return a coordinate?\n");
    fflush(stdout);

    /* Do this before any I2C of our own: it performs the IO-expander sequence
     * that asserts TOUCH_RST (board_variant.c:56-63). Without it the controller
     * may never come out of reset and every read below would fail for the
     * wrong reason. */
    g_stage = "board_variant_detect";
    board_variant_t v = board_variant_detect();
    printf("  board  : %s\n", board_variant_to_name(v));
    fflush(stdout);

    g_stage = "i2c_new_master_bus";
    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t bcfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_PORT,
        .scl_io_num = I2C_SCL,
        .sda_io_num = I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    esp_err_t err = i2c_new_master_bus(&bcfg, &bus);
    if (err != ESP_OK) {
        /* Most likely board_variant_detect() still holds I2C_NUM_0. Try to
         * reuse the existing bus rather than giving up -- and say which. */
        printf("  note   : i2c_new_master_bus -> %s; trying the existing bus\n",
               esp_err_to_name(err));
        fflush(stdout);
        if (i2c_master_get_bus_handle(I2C_PORT, &bus) != ESP_OK || bus == NULL) {
            g_stage = "STOPPED: no I2C bus";
            printf("  STOP   : could not open OR borrow I2C_NUM_0. Proves nothing.\n");
            fflush(stdout);
            vTaskDelete(NULL);
        }
        printf("  i2c    : borrowed the bus opened by the detector\n");
        fflush(stdout);
    }

    /* Probe the address BEFORE adding the device. add_device only allocates a
     * handle -- it puts nothing on the wire -- so without this an unreadable
     * chip ID cannot distinguish "device is there but mute" from "nothing at
     * this address at all", and the STOP text below would assert an ACK nobody
     * checked. */
    g_stage = "i2c_master_probe";
    bool acked = (i2c_master_probe(bus, CST816_ADDR, 200) == ESP_OK);
    printf("  probe  : 0x%02X %s\n", CST816_ADDR, acked ? "ACKed" : "did NOT answer");
    fflush(stdout);

    g_stage = "i2c_master_bus_add_device";
    i2c_master_dev_handle_t touch = NULL;
    const i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST816_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    err = i2c_master_bus_add_device(bus, &dcfg, &touch);
    if (err != ESP_OK) {
        g_stage = "STOPPED: add_device failed";
        printf("  STOP   : add_device(0x%02X) -> %s. Proves nothing about touch.\n",
               CST816_ADDR, esp_err_to_name(err));
        fflush(stdout);
        vTaskDelete(NULL);
    }

    g_stage = "read chip id";
    uint8_t id = 0;
    if (rd(touch, REG_CHIPID, &id, 1) != ESP_OK) {
        g_stage = "STOPPED: chip id unreadable";
        printf("  STOP   : 0x%02X did not answer a read of the chip-ID register.\n",
               CST816_ADDR);
        if (acked) {
            printf("           It ACKed the address probe above but will not talk.\n");
            printf("           That IS an A3 failure -- record it, do not retry blind.\n");
        } else {
            printf("           It did not ACK the address probe either, so this is a\n");
            printf("           BUS/WIRING problem, not a verdict about touch. Proves\n");
            printf("           nothing about A3.\n");
        }
        fflush(stdout);
        vTaskDelete(NULL);
    }
    g_chipid = id;
    printf("  touch  : 0x%02X at 0x%02X (%s)\n", id, CST816_ADDR, chip_name(id));
    printf("  panel  : %dx%d\n", PANEL_W, PANEL_H);
    printf("\n  >>> TOUCH THE SCREEN. Tap the corners, then drag across.\n\n");
    fflush(stdout);

    /* Range tracking answers A3 criterion 3: if touch spans 0..367 the 16 px
     * display gap does NOT apply to touch, and the driver must not add it. */
    g_stage = "polling";
    int last_x = -1, last_y = -1;
    uint8_t buf[6];

    while (1) {
        if (rd(touch, REG_GESTURE, buf, sizeof(buf)) != ESP_OK) {
            g_readfail++;
        } else {
            g_readfail = 0;
            uint8_t gesture = buf[0];
            uint8_t fingers = buf[1] & 0x0F;
            int x = ((buf[2] & 0x0F) << 8) | buf[3];
            int y = ((buf[4] & 0x0F) << 8) | buf[5];

            if (fingers > 0) {
                if (x != last_x || y != last_y) {
                    if (x < g_minx) g_minx = x;
                    if (x > g_maxx) g_maxx = x;
                    if (y < g_miny) g_miny = y;
                    if (y > g_maxy) g_maxy = y;
                    g_points++;
                    printf("  TOUCH x=%3d y=%3d  gesture=%-12s  [range x %d..%d  y %d..%d  n=%d]\n",
                           x, y, gesture_name(gesture),
                           g_minx, g_maxx, g_miny, g_maxy, g_points);
                    fflush(stdout);
                    last_x = x;
                    last_y = y;
                }
            } else if (last_x != -1) {
                /* Finger lifted. Mark it: without this boundary a post-processing
                 * pass cannot tell one drag from two separate touches, and will
                 * happily report a monotonic "drag" that spans a lift. That
                 * exact error was made against the first capture. */
                printf("  LIFT  (end of stroke)\n");
                fflush(stdout);
                last_x = last_y = -1;
            } else {
                last_x = last_y = -1;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));   /* 50 Hz -- fast enough to see a drag */
    }
}
