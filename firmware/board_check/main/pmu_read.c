#include "pmu_read.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "pmu";

#define PMU_ADDR        0x34
#define PMU_I2C_HZ      400000
#define PMU_TIMEOUT_MS  100

/* AXP2101 registers. Names and numbers from the vendor's own constants header:
 * reference/.../90_axp2101_pmu/components/XPowersLib/src/REG/AXP2101Constants.h
 * (:7-9 status and IC type, :108-127 the rail control block). */
#define REG_STATUS1     0x00
#define REG_STATUS2     0x01
#define REG_IC_TYPE     0x03
#define REG_DC_ONOFF    0x80
#define REG_DC_PWM      0x81
#define REG_DC_VOL0     0x82   /* DCDC1 */
#define REG_DC_VOL1     0x83   /* DCDC2 */
#define REG_DC_VOL2     0x84   /* DCDC3 */
#define REG_DC_VOL3     0x85   /* DCDC4 */
#define REG_DC_VOL4     0x86   /* DCDC5 */
#define REG_LDO_ONOFF0  0x90
#define REG_LDO_ONOFF1  0x91
#define REG_LDO_VOL0    0x92   /* ALDO1 */
#define REG_LDO_VOL8    0x9A   /* DLDO2 */

#define AXP2101_CHIP_ID 0x4A   /* AXP2101Constants.h:5 */

/* ---------------------------------------------------------------------------
 * The ONLY function in this file that touches the device.
 *
 * It performs a one-byte write phase carrying the register POINTER, then a read
 * phase. That write phase addresses a register; it never carries a value, so it
 * cannot change one. Every AXP2101 register write is a TWO-byte transmit
 * (pointer then value) -- there is no such call anywhere in this file, and any
 * future edit that adds one is the bug this comment exists to catch.
 * ------------------------------------------------------------------------- */
static esp_err_t pmu_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out)
{
    return i2c_master_transmit_receive(dev, &reg, 1, out, 1, PMU_TIMEOUT_MS);
}

/* --- decode, ADVISORY ONLY -------------------------------------------------
 * Formulas transcribed from XPowersAXP2101.tpp getDCnVoltage()/getXLDOnVoltage()
 * and the *_VOL_MIN / *_VOL_STEPS constants. A decoder is exactly the thing that
 * can be confidently wrong, which is why every raw byte is printed beside its
 * decode: a wrong formula can then be corrected from the transcript without
 * putting the board back on the bench. */

static uint16_t dc1_mv(uint8_t v)   { return (uint16_t)((v & 0x1F) * 100u + 1500u); }

/* DCDC2/3/4 share a piecewise encoding: 10 mV steps from 500 mV up to raw 70,
 * then 20 mV steps. DCDC3 adds a third 100 mV segment from raw 88. */
static uint16_t dc234_mv(uint8_t v)
{
    uint8_t r = v & 0x7F;
    if (r < 71) return (uint16_t)(r * 10u + 500u);
    return (uint16_t)(r * 20u - 200u);
}

static uint16_t dc3_mv(uint8_t v)
{
    uint8_t r = v & 0x7F;
    if (r < 71) return (uint16_t)(r * 10u + 500u);
    if (r < 88) return (uint16_t)(r * 20u - 200u);
    return (uint16_t)(r * 100u - 7200u);
}

static uint16_t dc5_mv(uint8_t v)
{
    uint8_t r = v & 0x1F;
    if (r == 0x19) return 1200;             /* documented special case */
    return (uint16_t)(r * 100u + 1400u);
}

static uint16_t ldo100_mv(uint8_t v)  { return (uint16_t)((v & 0x1F) * 100u + 500u); }
static uint16_t cpusldo_mv(uint8_t v) { return (uint16_t)((v & 0x1F) * 50u + 500u); }

const char *pmu_status_to_name(pmu_status_t s)
{
    switch (s) {
    case PMU_OK:        return "OK";
    case PMU_ABSENT:    return "ABSENT";
    default:            return "AMBIGUOUS";
    }
}

pmu_status_t pmu_dump(i2c_master_bus_handle_t bus)
{
    printf("\n---- AXP2101 rail inventory (read-only) ----\n");

    if (i2c_master_probe(bus, PMU_ADDR, PMU_TIMEOUT_MS) != ESP_OK) {
        printf("PMU: no device answered at 0x%02X.\n", PMU_ADDR);
        printf("PMU: record PMU_UNOBSERVED. The 'rails run at power-on defaults'\n"
               "PMU: inference rests on the PMU being present and readable, and\n"
               "PMU: loses its basis if it is not. Do NOT proceed to the SD step.\n");
        return PMU_ABSENT;
    }

    i2c_master_dev_handle_t dev = NULL;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PMU_ADDR,
        .scl_speed_hz = PMU_I2C_HZ,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "could not attach to 0x%02X after it ACKed", PMU_ADDR);
        return PMU_AMBIGUOUS;
    }

    pmu_status_t status = PMU_OK;

    /* Identity first. An address ACK proves something is at 0x34; it does not
     * prove that something is an AXP2101. Read the ID register and say so. */
    uint8_t ic = 0;
    bool ic_ok = (pmu_read_reg(dev, REG_IC_TYPE, &ic) == ESP_OK);
    printf("0x%02X IC_TYPE      raw=0x%02X   %s\n", REG_IC_TYPE, ic,
           !ic_ok            ? "READ FAILED"
           : (ic == AXP2101_CHIP_ID) ? "matches AXP2101 chip id 0x4A"
                                     : "DOES NOT match AXP2101 chip id 0x4A");
    if (!ic_ok || ic != AXP2101_CHIP_ID) {
        status = PMU_AMBIGUOUS;
    }

    uint8_t s1 = 0, s2 = 0;
    if (pmu_read_reg(dev, REG_STATUS1, &s1) != ESP_OK) status = PMU_AMBIGUOUS;
    if (pmu_read_reg(dev, REG_STATUS2, &s2) != ESP_OK) status = PMU_AMBIGUOUS;
    printf("0x%02X STATUS1      raw=0x%02X\n", REG_STATUS1, s1);
    printf("0x%02X STATUS2      raw=0x%02X\n", REG_STATUS2, s2);

    /* --- DCDC block ------------------------------------------------------ */
    uint8_t dc_on = 0, dc_pwm = 0;
    if (pmu_read_reg(dev, REG_DC_ONOFF, &dc_on) != ESP_OK) status = PMU_AMBIGUOUS;
    if (pmu_read_reg(dev, REG_DC_PWM, &dc_pwm) != ESP_OK) status = PMU_AMBIGUOUS;
    printf("0x%02X DC_ONOFF     raw=0x%02X   DCDC1..5 enable = %d %d %d %d %d\n",
           REG_DC_ONOFF, dc_on,
           !!(dc_on & 0x01), !!(dc_on & 0x02), !!(dc_on & 0x04),
           !!(dc_on & 0x08), !!(dc_on & 0x10));
    printf("0x%02X DC_FORCE_PWM raw=0x%02X\n", REG_DC_PWM, dc_pwm);

    uint8_t dcv[5] = {0};
    for (int i = 0; i < 5; i++) {
        if (pmu_read_reg(dev, (uint8_t)(REG_DC_VOL0 + i), &dcv[i]) != ESP_OK) {
            status = PMU_AMBIGUOUS;
        }
    }
    printf("0x%02X DCDC1_VOL    raw=0x%02X   %s  ~%u mV\n", REG_DC_VOL0, dcv[0],
           (dc_on & 0x01) ? "ON " : "off", dc1_mv(dcv[0]));
    printf("0x%02X DCDC2_VOL    raw=0x%02X   %s  ~%u mV\n", REG_DC_VOL1, dcv[1],
           (dc_on & 0x02) ? "ON " : "off", dc234_mv(dcv[1]));
    printf("0x%02X DCDC3_VOL    raw=0x%02X   %s  ~%u mV\n", REG_DC_VOL2, dcv[2],
           (dc_on & 0x04) ? "ON " : "off", dc3_mv(dcv[2]));
    printf("0x%02X DCDC4_VOL    raw=0x%02X   %s  ~%u mV\n", REG_DC_VOL3, dcv[3],
           (dc_on & 0x08) ? "ON " : "off", dc234_mv(dcv[3]));
    printf("0x%02X DCDC5_VOL    raw=0x%02X   %s  ~%u mV\n", REG_DC_VOL4, dcv[4],
           (dc_on & 0x10) ? "ON " : "off", dc5_mv(dcv[4]));

    /* --- LDO block ------------------------------------------------------- */
    uint8_t l0 = 0, l1 = 0;
    if (pmu_read_reg(dev, REG_LDO_ONOFF0, &l0) != ESP_OK) status = PMU_AMBIGUOUS;
    if (pmu_read_reg(dev, REG_LDO_ONOFF1, &l1) != ESP_OK) status = PMU_AMBIGUOUS;
    printf("0x%02X LDO_ONOFF0   raw=0x%02X   ALDO1-4=%d%d%d%d BLDO1-2=%d%d "
           "CPUSLDO=%d DLDO1=%d\n", REG_LDO_ONOFF0, l0,
           !!(l0 & 0x01), !!(l0 & 0x02), !!(l0 & 0x04), !!(l0 & 0x08),
           !!(l0 & 0x10), !!(l0 & 0x20), !!(l0 & 0x40), !!(l0 & 0x80));
    printf("0x%02X LDO_ONOFF1   raw=0x%02X   DLDO2=%d\n",
           REG_LDO_ONOFF1, l1, !!(l1 & 0x01));

    static const char *ldo_name[9] = {
        "ALDO1", "ALDO2", "ALDO3", "ALDO4",
        "BLDO1", "BLDO2", "CPUSLDO", "DLDO1", "DLDO2",
    };
    for (int i = 0; i <= (REG_LDO_VOL8 - REG_LDO_VOL0); i++) {
        uint8_t reg = (uint8_t)(REG_LDO_VOL0 + i);
        uint8_t v = 0;
        if (pmu_read_reg(dev, reg, &v) != ESP_OK) {
            status = PMU_AMBIGUOUS;
        }
        bool on = (i <= 7) ? !!(l0 & (1u << i)) : !!(l1 & 0x01);
        uint16_t mv = (i == 6) ? cpusldo_mv(v) : ldo100_mv(v);
        printf("0x%02X %-7s VOL  raw=0x%02X   %s  ~%u mV\n",
               reg, ldo_name[i], v, on ? "ON " : "off", mv);
    }

    i2c_master_bus_rm_device(dev);

    printf("\nPMU verdict: %s\n", pmu_status_to_name(status));
    printf("The decode above is ADVISORY. The acceptance criterion is an\n"
           "independent raw-byte -> meaning table written into\n"
           "docs/research/board-capabilities.md from the datasheet constants --\n"
           "a broken decoder must not be allowed to satisfy its own AC.\n");
    return status;
}
