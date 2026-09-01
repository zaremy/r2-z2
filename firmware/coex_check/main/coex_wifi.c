/* Wi-Fi side of the A1 coexistence arms.
 *
 * Arm 2 connects and sits idle. Arm 3 additionally pulls from a LAN URL
 * continuously to sustain >= 1 Mbps, which is the "worst realistic case" the
 * experiment is supposed to expose BLE to.
 *
 * The throughput number is printed, not assumed. An arm 3 that quietly fails to
 * reach 1 Mbps -- server down, file too small, URL wrong -- would produce a
 * clean BLE result and read as "coexistence is fine under load" when no load
 * ever existed. That is the same shape as every other broken-instrument result
 * in this project, so the load reports its own rate and says when it is short.
 */
#include "coex_wifi.h"

#include <string.h>
#include <inttypes.h>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "wifi_creds.h"

static const char *TAG = "COEX_WIFI";

#define WIFI_CONNECTED_BIT BIT0
#define TARGET_BPS         (1000000 / 8)   /* 1 Mbps, in bytes per second */

static EventGroupHandle_t s_wifi_events;
static volatile uint64_t  s_bytes = 0;
static volatile uint32_t  s_wifi_disconnects = 0;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_disconnects++;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        /* Reconnect: a dropped AP association is a Wi-Fi problem, not a BLE
         * result, and arm 3 needs the load to keep running. It is counted and
         * reported so it can never be confused with a BLE disconnect. */
        ESP_LOGW(TAG, "Wi-Fi disconnected (#%" PRIu32 "), reconnecting", s_wifi_disconnects);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi up, ip=" IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t on_http_data(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        s_bytes += evt->data_len;
    }
    return ESP_OK;
}

static void load_task(void *arg)
{
    (void)arg;
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "arm 3 load starting: %s", COEX_LOAD_URL);

    esp_http_client_config_t cfg = {
        .url = COEX_LOAD_URL,
        .event_handler = on_http_data,
        .timeout_ms = 10000,
        .buffer_size = 4096,
    };

    int64_t window_start = esp_timer_get_time();
    uint64_t window_base = 0;

    while (1) {
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        esp_err_t err = esp_http_client_perform(c);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "load fetch failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        esp_http_client_cleanup(c);

        const int64_t now = esp_timer_get_time();
        if (now - window_start >= 10 * 1000000LL) {
            const uint64_t delta = s_bytes - window_base;
            const double secs = (double)(now - window_start) / 1e6;
            const double bps  = (double)delta / secs;
            ESP_LOGI(TAG, "load %.2f Mbit/s%s",
                     bps * 8.0 / 1e6,
                     (bps < TARGET_BPS)
                         ? "   *** BELOW 1 Mbps -- arm 3 is NOT under its specified load ***"
                         : "");
            window_start = now;
            window_base  = s_bytes;
        }
    }
}

void coex_wifi_start(bool with_load)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, COEX_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, COEX_WIFI_PASS, sizeof(wc.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    /* Modem sleep off: a station that naps between beacons is not the sustained
     * radio contention this experiment is meant to create. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi STA starting (ssid hidden from logs), load=%s",
             with_load ? "ON" : "off");

    if (with_load) {
        xTaskCreate(load_task, "coex_load", 8192, NULL, 4, NULL);
    }
}

uint32_t coex_wifi_disconnects(void) { return s_wifi_disconnects; }
