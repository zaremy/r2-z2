/* board_check -- the first firmware we run on this board, and the instrument
 * that answers the question every later slice is interpreted against: is this
 * a V1 or a V2 panel/touch pair?
 *
 * NO PANEL CONTROLLER INIT. esp_lcd_new_panel_co5300() is never reached and no
 * display driver is linked (verifiable: `xtensa-esp32s3-elf-nm <elf> |
 * grep -ci 'co5300\|bsp_display'` must print 0).
 *
 * It is NOT, however, "display-free", and the loose phrasing was wrong enough
 * to be worth recording: board_variant_detect()'s reset release writes
 * IO_EXPANDER_OUTPUT_MASK, which includes LCD_RST and DSI_PWR_EN. So this does
 * release the panel reset and assert panel power. That is the same state the
 * board reaches at power-on under its factory firmware, and no controller
 * commands follow it -- LOW-RISK INFERRED, not "safe".
 *
 * A second false claim, recorded so it is not re-argued: an earlier draft said
 * a vendor example would drive a V1 panel with the wrong controller on its
 * first boot. It would not. 01_project_template, 08_i2c_tools and 09_sdmmc
 * contain zero display references each; BSP display init is isolated in
 * bsp_display_new() and none of them call it. The real reason board_check goes
 * first is ordering: every later step is READ differently depending on the
 * revision, and one flash buys that answer.
 */
#include <inttypes.h>
#include <stdbool.h>

#include "board_variant.h"
#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pmu_read.h"

static const char *TAG = "board_check";

#define I2C_PORT   I2C_NUM_0
#define I2C_SDA    GPIO_NUM_15
#define I2C_SCL    GPIO_NUM_14
#define I2C_HZ     400000

#define ADDR_IO_EXPANDER  0x20
#define ADDR_CST816       0x15   /* V2 touch */
#define ADDR_FT3168       0x38   /* V1 touch */

typedef struct {
    bool io_expander;
    bool cst816;
    bool ft3168;
    int  count;
} scan_result_t;

static scan_result_t scan_bus(i2c_master_bus_handle_t bus)
{
    /* Runs AFTER board_variant_detect(), which releases the touch controller's
     * reset via the IO expander at 0x20. Before that release the touch chip does
     * not answer at all -- a bare scan reports an empty bus and reads as
     * "unknown board" rather than as "you skipped a step". Ordering is
     * deliberate; see docs/research/board-revision.md. */
    scan_result_t r = {0};
    printf("I2C devices answering:");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            printf(" 0x%02X", addr);
            r.count++;
            if (addr == ADDR_IO_EXPANDER) r.io_expander = true;
            if (addr == ADDR_CST816)      r.cst816 = true;
            if (addr == ADDR_FT3168)      r.ft3168 = true;
        }
    }
    printf("%s\n", r.count ? "" : "  (none)");
    return r;
}

/* Called AFTER the bus scan, deliberately. An earlier version printed the
 * D-005 ruling before the scan that corroborates it, so a transcript could read
 * "D-005 HOLDS" directly above "AC2 FAIL" -- and a skimming reader takes the
 * first one. A ruling never precedes its evidence. */
static void report_variant(board_variant_t v, bool corroborated_ok)
{
    printf("\n>>> BOARD VARIANT: %s\n", board_variant_to_name(v));
    if (!corroborated_ok) {
        printf(">>> NOT CORROBORATED by the bus scan above. Draw no conclusion\n"
               ">>> about D-005 from this run.\n");
        return;
    }
    switch (v) {
    case BOARD_VARIANT_CO5300_CST816:
        printf(">>> V2. The BSP is native to this board. D-005 HOLDS.\n");
        break;
    case BOARD_VARIANT_SH8601_FT3168:
        printf(">>> V1. The BSP has NO SH8601 driver and would drive this\n"
               ">>> panel with the CO5300 init sequence. This is a D-005\n"
               ">>> REVERSAL TRIGGER, not a configuration detail.\n");
        break;
    default:
        printf(">>> UNKNOWN. Neither 0x15 (CST816) nor 0x38 (FT3168)\n"
               ">>> answered after the reset release. The detector caches its\n"
               ">>> result, so retrying needs a POWER CYCLE, not a reset.\n"
               ">>> Do not proceed to any revision-dependent work.\n");
        break;
    }
}

/* AC1 + AC2 as a printed verdict. The raw scan list above is the evidence; this
 * is a convenience so a transcript can be read without cross-referencing. It is
 * advisory in the same sense the PMU decode is -- it cannot be the only thing
 * standing between a wrong reading and a pass. */
static bool corroborated(board_variant_t v, scan_result_t s)
{
    if (!s.io_expander) {
        printf("AC2 FAIL: the IO expander at 0x20 did not answer. Without it the\n"
               "          touch reset was never released and the variant reading\n"
               "          below carries no weight.\n");
        return false;
    }
    if (v == BOARD_VARIANT_CO5300_CST816 && s.cst816 && !s.ft3168) return true;
    if (v == BOARD_VARIANT_SH8601_FT3168 && s.ft3168 && !s.cst816) return true;
    printf("AC2 FAIL: the scan does not corroborate the reported variant.\n"
           "          0x20=%d 0x15=%d 0x38=%d. Exactly one touch address must\n"
           "          answer, and it must be the one the variant names.\n",
           s.io_expander, s.cst816, s.ft3168);
    return false;
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    bool flash_ok = (esp_flash_get_size(NULL, &flash_size) == ESP_OK);
    uint8_t mac[6] = {0};
    bool mac_ok = (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK);

    printf("\n================ board_check ================\n");
    printf("chip      : %s, %d core(s), silicon rev v%d.%d\n",
           CONFIG_IDF_TARGET, chip.cores,
           chip.revision / 100, chip.revision % 100);
    printf("features  : %s%s%s\n",
           (chip.features & CHIP_FEATURE_WIFI_BGN) ? "wifi " : "",
           (chip.features & CHIP_FEATURE_BLE) ? "ble " : "",
           (chip.features & CHIP_FEATURE_EMB_PSRAM) ? "emb-psram" : "");
    /* Report a failed read as a failure, never as a value. A silent 0 here
     * would be written into board-capabilities.md as an OBSERVED flash size --
     * and resolving the 8 MB vs 16 MB conflict is half this app's job. */
    if (flash_ok) {
        printf("flash     : %" PRIu32 " bytes (%" PRIu32 " MB)\n",
               flash_size, flash_size / (1024 * 1024));
    } else {
        printf("flash     : READ FAILED -- do not record a flash size from this run\n");
    }
    printf("psram     : %u bytes\n", (unsigned)esp_psram_get_size());
    if (mac_ok) {
        printf("base MAC  : %02X:%02X:%02X:%02X:%02X:%02X\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        printf("base MAC  : READ FAILED\n");
    }

    /* board_variant_detect() opens and deletes its own bus. The IO expander is
     * an external latch, so the reset release it performs survives that
     * teardown -- which is why the scan below still sees the touch device. */
    board_variant_t v = board_variant_detect();

    printf("\n");
    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_PORT,
        .scl_io_num = I2C_SCL,
        .sda_io_num = I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
        /* Reachable: board_variant_detect() only WARNS if its own
         * i2c_del_master_bus() fails (board_variant.c:112-115), which leaves
         * I2C_NUM_0 occupied and makes this return ESP_ERR_INVALID_STATE.
         * This branch must end in a STOP -- an earlier version fell through to
         * the closing rule with no verdict at all, which reads as a pass. */
        ESP_LOGE(TAG, "could not open I2C bus for the scan and PMU read");
        printf(">>> BOARD VARIANT: not reported -- the bus never opened.\n");
        printf("\n---- gate on the SD step ----\n");
        printf("STOP: no I2C bus, so there was no scan and no PMU read. This run\n"
               "      proved NOTHING about the board. Power-cycle and re-run; the\n"
               "      detector caches, so a reset is not enough.\n");
        printf("=============================================\n\n");
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    scan_result_t scan = scan_bus(bus);
    bool variant_ok = corroborated(v, scan);
    report_variant(v, variant_ok);
    pmu_status_t pmu = pmu_dump(bus);
    i2c_del_master_bus(bus);

    printf("\n---- gate on the SD step ----\n");
    if (!variant_ok) {
        printf("STOP: the variant is not corroborated by the bus scan.\n");
    }
    if (pmu != PMU_OK) {
        printf("STOP: PMU %s. An unknown or ambiguous PMU decode blocks the SD\n"
               "      step -- proceeding with rails in an unverified state is\n"
               "      not a pass. Capture this transcript, power-cycle, re-run\n"
               "      once, and re-plan if it repeats.\n", pmu_status_to_name(pmu));
    }
    if (variant_ok && pmu == PMU_OK) {
        printf("Variant corroborated and PMU readable. Cleared to proceed.\n");
    }
    printf("=============================================\n\n");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
