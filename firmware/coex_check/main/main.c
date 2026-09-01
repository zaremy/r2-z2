/* coex_check -- #103 A1: can BLE and Wi-Fi share this radio under OUR traffic?
 *
 * A1 can reverse D-005, which named Wi-Fi/BLE coexistence failure as its own
 * reversal condition. Espressif's matrix calls BLE-connected + Wi-Fi-STA
 * "stable" on the S3; a field report measures BLE loss going from ~3%
 * standalone to 20%+ with coexistence, including streaks of 100%.
 *
 * WHY THE KEEPALIVE IS A WRITE, against the letter of the issue. #103 specified
 * a GATT READ every 3 s "so nothing here should be able to move the droid".
 * Two facts from our own sources make a read the wrong instrument:
 *
 *   1. A read never resets R2's inactivity timer. wake (DID 0x13 / CID 0x0D) is
 *      what does (mac-prototype/r2_probe.py:675). An hour of reads lets him
 *      fall asleep and drop the link -- which the pre-declared criteria would
 *      score as a disconnect, i.e. a FAIL, i.e. "D-005 is revisited", caused by
 *      nothing but his own idle behaviour. That is a false FAIL on an
 *      architecture decision.
 *   2. The link cannot be established read-only anyway: R2 requires the
 *      anti-DoS magic write first (r2_probe.py:440).
 *
 * The safety intent is preserved exactly. wake is idempotent, cannot move him,
 * and is the same packet the Mac daemon already sends every 3 s in normal
 * operation -- so this measures our REAL traffic rather than a proxy for it.
 * CID 0x01 (SLEEP) is never sent. No movement, stance, or animation command
 * exists anywhere in this binary.
 *
 * ARMS. One per build; reflash between them. Each runs one hour.
 *   1  Wi-Fi off                  standalone BLE baseline
 *   2  Wi-Fi STA connected, idle  coexistence at rest
 *   3  Wi-Fi connected + loaded   worst realistic case
 *
 * Arm 1 is also the first time a BLE stack has ever run on this board.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "coex_stats.h"
#include "coex_wifi.h"

#ifndef COEX_ARM
#define COEX_ARM 1
#endif

static const char *TAG = "coex_check";

void ble_store_config_init(void);   /* provided by the nimble port */

void r2d2_central_init(void);
void r2d2_start_scan(void);

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_nimble_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ESP_LOGI(TAG, "NimBLE synced; scanning for R2");
    r2d2_start_scan();
}

static void on_nimble_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset, reason=%d", reason);
}

static void reporter_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        coex_stats_report();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "==== coex_check: #103 A1, arm %d ====", COEX_ARM);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#if COEX_ARM == 1
    coex_stats_init("arm1-ble-only");
    ESP_LOGI(TAG, "Wi-Fi deliberately NOT started: this is the standalone baseline");
#else
    coex_stats_init(COEX_ARM == 2 ? "arm2-wifi-idle" : "arm3-wifi-loaded");
    coex_wifi_start(COEX_ARM == 3);
#endif

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb  = on_nimble_sync;
    ble_hs_cfg.reset_cb = on_nimble_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();

    r2d2_central_init();
    nimble_port_freertos_init(nimble_host_task);

    xTaskCreate(reporter_task, "coex_report", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "running; a summary line prints every 60 s");
}
