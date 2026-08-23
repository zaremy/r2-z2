/* pmu_read — AXP2101 register inventory. READ ONLY, BY CONSTRUCTION.
 *
 * Why this exists: the SD slot and the I2C rail may or may not be gated by the
 * PMU. Nothing in the ESP-IDF ladder configures its rails, so bring-up runs on
 * whatever the power-on defaults are. That is an assumption, and this reads the
 * registers that would falsify it.
 *
 * WHAT IT MUST NEVER DO: write a PMU register. Wrong rail voltages destroy
 * external loads, and the charger registers next door destroy lithium cells.
 * The vendor's own 90_axp2101_pmu example warns "do not run without knowing the
 * external load voltage" and then writes charger current and target voltage on
 * every run. Do not run it. Do not copy from it.
 */
#pragma once

#include "driver/i2c_master.h"

typedef enum {
    PMU_OK = 0,      /* answered at 0x34, identified, every register read     */
    PMU_ABSENT,      /* no ACK at 0x34 -- record PMU_UNOBSERVED and re-plan   */
    PMU_AMBIGUOUS,   /* answered but did not identify, or a read failed       */
} pmu_status_t;

/* Dumps the rail registers to stdout as raw bytes plus an ADVISORY decode.
 * Does not modify the device. `bus` stays owned by the caller. */
pmu_status_t pmu_dump(i2c_master_bus_handle_t bus);

const char *pmu_status_to_name(pmu_status_t s);
