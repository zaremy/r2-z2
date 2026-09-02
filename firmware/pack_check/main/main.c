/* pack_check -- #93 / gap B2: does the backpack have its own battery?
 *
 * The issue is careful to say the AXP2101's PRESENCE proves nothing: it is a
 * power-management IC and sits on the bus either way. True. But the AXP2101 can
 * be ASKED, and that turns a multimeter job into a register read:
 *
 *   STATUS1 (0x00) bit 3  battery present      (XPowersAXP2101.tpp:262-265)
 *   STATUS1 (0x00) bit 5  VBUS good            (:252-254)
 *   STATUS1 (0x00) bit 4  BATFET state         (:256-259)
 *   STATUS2 (0x01) 7:5    charge status, 001 = charging   (:282-285)
 *   ADC_CHANNEL_CTRL 0x30 bit 0 enables battery-voltage ADC  (:2290-2293)
 *   ADC_DATA_RELUST0/1 0x34/0x35  13-bit result, high5+low8 (:2310-2316)
 *
 * Register numbers from the vendor's own bundled XPowersLib
 * (AXP2101Constants.h:7,8,46,47,48) in waveshareteam/ESP32-S3-Touch-AMOLED-1.8
 * @ ed7c6a5, cross-checked against firmware/board_check/main/pmu_read.c, which
 * already reads this part at 0x34 and confirms chip id 0x4A.
 *
 * THE INSTRUMENT GUARD MATTERS MORE THAN THE READING. "No battery" is the quiet
 * answer, and every failure mode here -- wrong address, bus not up, PMU not
 * responding -- produces it. This session has already spent hours on a null
 * result that came from a broken instrument. So the chip ID is verified FIRST
 * and a failure to read it aborts with "PROVES NOTHING" rather than reporting an
 * absent battery.
 */

#include <stdio.h>
#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_PORT        I2C_NUM_0
#define I2C_SDA         GPIO_NUM_15
#define I2C_SCL         GPIO_NUM_14
#define I2C_HZ          400000

#define PMU_ADDR        0x34
#define AXP2101_CHIP_ID 0x4A

#define REG_STATUS1     0x00
#define REG_STATUS2     0x01
#define REG_IC_TYPE     0x03
#define REG_ADC_CTRL    0x30
#define REG_BAT_DET     0x68   /* bit 0 = battery DETECTION enable */
#define REG_ADC_DATA_H  0x34
#define REG_ADC_DATA_L  0x35

static const char *TAG = "pack_check";

static esp_err_t rd(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *out)
{
    return i2c_master_transmit_receive(d, &reg, 1, out, 1, 200);
}

static esp_err_t wr(i2c_master_dev_handle_t d, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(d, buf, sizeof(buf), 200);
}

void app_main(void)
{
    printf("\n================ pack_check ================\n");
    printf("  #93 / B2 -- is there a cell on this board, or is it fed from R2?\n\n");

    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t bcfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_PORT,
        .scl_io_num = I2C_SCL,
        .sda_io_num = I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    if (i2c_new_master_bus(&bcfg, &bus) != ESP_OK) {
        printf("  STOP: I2C bus would not open. PROVES NOTHING about the battery.\n");
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    i2c_master_dev_handle_t pmu = NULL;
    const i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PMU_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    if (i2c_master_bus_add_device(bus, &dcfg, &pmu) != ESP_OK) {
        printf("  STOP: could not address 0x%02X. PROVES NOTHING.\n", PMU_ADDR);
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    /* ---- instrument guard: talk to the right chip before believing any bit ---- */
    uint8_t id = 0;
    if (rd(pmu, REG_IC_TYPE, &id) != ESP_OK) {
        printf("  STOP: 0x%02X did not answer the chip-ID read.\n", PMU_ADDR);
        printf("        A dead bus reports the SAME thing as an absent battery.\n");
        printf("        This run PROVES NOTHING. Do not record a verdict.\n");
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }
    /* Exact match, no mask. An earlier version accepted (id & 0xCF), which also
     * admits 0x5A/0x6A/0x7A -- a looser guard than board_check's and than
     * XPowersLib's own initImpl(), for no documented reason. */
    if (id != AXP2101_CHIP_ID) {
        printf("  STOP: chip id 0x%02X is not AXP2101 (0x%02X). Wrong part or wrong bus.\n",
               id, AXP2101_CHIP_ID);
        printf("        PROVES NOTHING about the battery.\n");
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }
    printf("  instrument: AXP2101 answering at 0x%02X (chip id 0x%02X)  OK\n\n", PMU_ADDR, id);

    /* ---- battery DETECTION first, and record its prior state ----
     *
     * STATUS1 bit 3 is only meaningful if the detector is on. XPowersLib exposes
     * this as enableBattDetection() -> BAT_DET_CTRL(0x68) bit 0, and the vendor's
     * own example never calls it. The first version of this probe did not
     * either, which means a present cell could have read absent. Read 0x68
     * BEFORE writing it, so the log says whether the earlier run was valid. */
    uint8_t det_before = 0;
    if (rd(pmu, REG_BAT_DET, &det_before) != ESP_OK) {
        printf("  STOP: could not read BAT_DET_CTRL. PROVES NOTHING.\n");
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }
    printf("  battery detection BEFORE we touched it: %s (0x68=0x%02X)\n",
           (det_before & 1) ? "ALREADY ENABLED" : "DISABLED", det_before);

    if (wr(pmu, REG_BAT_DET, det_before | 0x01) != ESP_OK) {
        printf("  STOP: could not enable battery detection. PROVES NOTHING.\n");
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }
    uint8_t det_after = 0;
    if (rd(pmu, REG_BAT_DET, &det_after) != ESP_OK || !(det_after & 1)) {
        printf("  STOP: battery detection did not take (0x68=0x%02X). PROVES NOTHING.\n",
               det_after);
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }
    printf("  battery detection now: ENABLED (0x68=0x%02X)\n", det_after);

    /* ---- battery-voltage ADC, also read back ---- */
    uint8_t adc = 0;
    if (rd(pmu, REG_ADC_CTRL, &adc) != ESP_OK ||
        wr(pmu, REG_ADC_CTRL, adc | 0x01) != ESP_OK ||
        rd(pmu, REG_ADC_CTRL, &adc) != ESP_OK || !(adc & 1)) {
        printf("  STOP: battery-voltage ADC did not enable (0x30=0x%02X). PROVES NOTHING.\n",
               adc);
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }
    printf("  battery-voltage ADC: ENABLED (0x30=0x%02X)\n\n", adc);
    vTaskDelay(pdMS_TO_TICKS(300));

    while (1) {
        uint8_t s1 = 0, s2 = 0, dh = 0, dl = 0, det = 0;
        /* Every read checked. Locals are zero-initialised, so an UNCHECKED
         * failure would print "battery present: no" and a confident verdict --
         * the quiet answer, produced by a broken bus rather than by hardware. */
        if (rd(pmu, REG_STATUS1, &s1)     != ESP_OK ||
            rd(pmu, REG_STATUS2, &s2)     != ESP_OK ||
            rd(pmu, REG_BAT_DET, &det)    != ESP_OK ||
            rd(pmu, REG_ADC_DATA_H, &dh)  != ESP_OK ||
            rd(pmu, REG_ADC_DATA_L, &dl)  != ESP_OK) {
            printf("  READ FAILED this cycle -- no verdict. PROVES NOTHING.\n\n");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (!(det & 1)) {
            printf("  battery detection turned OFF (0x68=0x%02X) -- no verdict.\n\n", det);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        const bool batt   = (s1 >> 3) & 1;
        const bool vbus   = (s1 >> 5) & 1;
        const bool batfet = (s1 >> 4) & 1;
        const uint8_t chg = (s2 >> 5) & 0x07;
        const uint16_t mv = (uint16_t)(((dh & 0x1F) << 8) | dl);

        /* det_before is repeated every cycle on purpose: the boot banner is
         * unobservable here (a listener needs ~3 s to attach and the header
         * prints at ~0.8 s), and whether battery DETECTION defaults on is worth
         * having in the permanent record -- any later firmware reading STATUS1
         * needs to know it must enable the detector first. */
        printf("  STATUS1=0x%02X STATUS2=0x%02X BAT_DET=0x%02X (was 0x%02X at boot)  raw ADC=0x%02X%02X\n",
               s1, s2, det, det_before, dh, dl);
        printf("    battery present (S1.3) : %s\n", batt ? "YES" : "no");
        printf("    VBUS good       (S1.5) : %s\n", vbus ? "yes" : "no");
        printf("    BATFET          (S1.4) : %s\n", batfet ? "on" : "off");
        printf("    charge status   (S2.7:5): %u%s\n", chg, chg == 1 ? " (charging)" : "");
        printf("    battery voltage        : %u mV%s\n", mv,
               batt ? "" : "   <- meaningless with no cell present");
        printf("\n  VERDICT: %s\n\n",
               batt ? "a cell IS attached to this board"
                    : "NO cell attached -- the board is externally powered");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
