#include "panel_net.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "nvs.h"

static const char *TAG = "net";

#define NVS_NS        "r2cfg"
#define GOT_IP_BIT    BIT0

/* How often the probe repeats once the cloud has answered. It is the NET
 * link's liveness check, not a benchmark: once a minute is enough to notice
 * the provider or the router going away, and cheap. */
#define PROBE_PERIOD_MS 60000u

/* A bare request the provider must authenticate: it proves DNS, TLS against
 * the bundle, and the key, and returns no model output. */
#define PROBE_URL "https://api.openai.com/v1/models"

static EventGroupHandle_t s_events;
static volatile panel_net_state_t s_state = PANEL_NET_UNPROVISIONED;

static char s_ssid[33];
static char s_pass[65];
static char s_llm_key[200];
static char s_llm_model[64];

const char *panel_net_state_name(panel_net_state_t s)
{
    switch (s) {
    case PANEL_NET_UNPROVISIONED: return "unprovisioned";
    case PANEL_NET_JOINING:       return "joining";
    case PANEL_NET_UP:            return "up";
    case PANEL_NET_CLOUD_OK:      return "cloud ok";
    case PANEL_NET_CLOUD_FAIL:    return "cloud FAIL";
    default:                      return "?";
    }
}

panel_net_state_t panel_net_state(void) { return s_state; }
const char *panel_net_llm_key(void)   { return s_llm_key[0] ? s_llm_key : NULL; }
const char *panel_net_llm_model(void) { return s_llm_model[0] ? s_llm_model : NULL; }

static void set_state(panel_net_state_t s)
{
    if (s == s_state) return;
    s_state = s;
    ESP_LOGI(TAG, "-> %s", panel_net_state_name(s));
}

/* One string from NVS. Absent is not an error -- it is "not provisioned". */
static bool read_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t n = cap;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) { out[0] = '\0'; return false; }
    return out[0] != '\0';
}

/* THE HEADROOM, which is the whole point of slice 3.1. D-015 names it as a
 * precondition nobody has measured: BLE central + Wi-Fi + TLS + LVGL at once.
 * Internal RAM is the scarce one -- DMA, the radio and TLS want it -- so both
 * its free total and its largest block are logged, and the low-water mark,
 * which is what a later spike would actually hit. */
static void log_heap(const char *when)
{
    /* DMA-capable internal is its own number and the one the display needs:
     * every LVGL flush is copied through a private internal DMA buffer
     * (MEASURED 2026-09-19 -- see sdkconfig.defaults, LVGL_BUF_HEIGHT). */
    ESP_LOGI(TAG, "heap %s: internal free %u (largest %u, min ever %u) | dma largest %u | psram free %u",
             when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(s_events, GOT_IP_BIT);
        set_state(PANEL_NET_JOINING);
        /* The reason code, because "wrong password", "no such network" and
         * "5 GHz only" are three different fixes and look identical from the
         * outside. One retry every 2 s; the delay holds the default event
         * loop meanwhile, which on this board only Wi-Fi and IP use. */
        ESP_LOGW(TAG, "wifi down (reason %d), retrying", d ? d->reason : -1);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        set_state(PANEL_NET_UP);
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    }
}

static esp_err_t discard(esp_http_client_event_t *e) { (void)e; return ESP_OK; }

uint32_t panel_net_internal_min_ever(void)
{
    return (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
}

#define STT_URL   "https://api.openai.com/v1/audio/transcriptions"
#define STT_MODEL "whisper-1"
#define BOUNDARY  "r2z2-7f3a9c"
#define PCM_RATE  16000u
#define CHUNK_MS  20u
#define CHUNK_B   (PCM_RATE * 2u * CHUNK_MS / 1000u)     /* 640 */

static void wav_header(uint8_t h[44], uint32_t data_len)
{
    const uint32_t riff = 36u + data_len, rate = PCM_RATE, byte_rate = PCM_RATE * 2u;
    memcpy(h, "RIFF", 4);      memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    const uint32_t fmt_len = 16; const uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    memcpy(h + 16, &fmt_len, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &align, 2);   memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);   memcpy(h + 40, &data_len, 4);
}

int panel_net_upload_realtime(unsigned seconds)
{
    if (s_llm_key[0] == '\0') return -1;
    static const char head[] =
        "--" BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n" STT_MODEL "\r\n"
        "--" BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"talk.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    static const char tail[] = "\r\n--" BOUNDARY "--\r\n";
    const uint32_t pcm_len = seconds * PCM_RATE * 2u;
    const int total = (int)(sizeof head - 1 + 44 + pcm_len + sizeof tail - 1);

    esp_http_client_config_t cfg = {
        .url = STT_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return ESP_FAIL;
    char auth[sizeof s_llm_key + 8];
    snprintf(auth, sizeof auth, "Bearer %s", s_llm_key);
    esp_http_client_set_header(c, "Authorization", auth);
    memset(auth, 0, sizeof auth);
    esp_http_client_set_header(c, "Content-Type", "multipart/form-data; boundary=" BOUNDARY);

    int rc = ESP_FAIL;
    esp_err_t err = esp_http_client_open(c, total);
    if (err != ESP_OK) { rc = err; goto out; }
    uint8_t wav[44];
    wav_header(wav, pcm_len);
    if (esp_http_client_write(c, head, sizeof head - 1) < 0 ||
        esp_http_client_write(c, (const char *)wav, sizeof wav) < 0) goto out;

    /* Real time, not as fast as the socket takes it: the load that matters is
     * a sustained audio-rate stream beside BLE and the glass, not a burst. */
    static const uint8_t zeros[CHUNK_B];
    TickType_t wake = xTaskGetTickCount();
    for (uint32_t sent = 0; sent < pcm_len; sent += CHUNK_B) {
        if (esp_http_client_write(c, (const char *)zeros, CHUNK_B) < 0) goto out;
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(CHUNK_MS));
    }
    if (esp_http_client_write(c, tail, sizeof tail - 1) < 0) goto out;
    if (esp_http_client_fetch_headers(c) < 0) goto out;
    rc = esp_http_client_get_status_code(c);
    char sink[128];
    while (esp_http_client_read(c, sink, sizeof sink) > 0) { }
out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return rc;
}

static void probe_once(void)
{
    esp_http_client_config_t cfg = {
        .url = PROBE_URL,
        .event_handler = discard,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) { set_state(PANEL_NET_CLOUD_FAIL); return; }

    /* The key never reaches a log line. */
    char auth[sizeof s_llm_key + 8];
    snprintf(auth, sizeof auth, "Bearer %s", s_llm_key);
    esp_http_client_set_header(c, "Authorization", auth);
    memset(auth, 0, sizeof auth);

    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = esp_http_client_perform(c);
    const int ms = (int)((esp_timer_get_time() - t0) / 1000);
    const int status = esp_http_client_get_status_code(c);
    log_heap("in TLS");      /* the session is still open here */
    esp_http_client_cleanup(c);

    if (err == ESP_OK && status >= 200 && status < 300) {
        set_state(PANEL_NET_CLOUD_OK);
        ESP_LOGI(TAG, "probe: HTTP %d in %d ms", status, ms);
    } else {
        set_state(PANEL_NET_CLOUD_FAIL);
        ESP_LOGW(TAG, "probe: %s, HTTP %d, %d ms", esp_err_to_name(err), status, ms);
    }
}

static void probe_task(void *arg)
{
    (void)arg;
    while (1) {
        xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        if (s_llm_key[0] == '\0') {
            ESP_LOGW(TAG, "probe: no provider key provisioned; the cloud is not tried");
            set_state(PANEL_NET_CLOUD_FAIL);
            vTaskDelay(pdMS_TO_TICKS(PROBE_PERIOD_MS));
            continue;
        }
        log_heap("before TLS");
        probe_once();
        log_heap("after TLS");
        vTaskDelay(pdMS_TO_TICKS(PROBE_PERIOD_MS));
    }
}

void panel_net_start(void)
{
    nvs_handle_t h;
    bool have_wifi = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        have_wifi = read_str(h, "wifi_ssid", s_ssid, sizeof s_ssid);
        read_str(h, "wifi_pass", s_pass, sizeof s_pass);
        read_str(h, "llm_key", s_llm_key, sizeof s_llm_key);
        read_str(h, "llm_model", s_llm_model, sizeof s_llm_model);
        nvs_close(h);
    }
    /* Presence only -- never a value. */
    ESP_LOGI(TAG, "provisioned: wifi %s, llm key %s, model %s",
             have_wifi ? "yes" : "NO", s_llm_key[0] ? "yes" : "NO",
             s_llm_model[0] ? s_llm_model : "(none)");
    if (!have_wifi) {
        set_state(PANEL_NET_UNPROVISIONED);
        return;
    }

    log_heap("before wifi");
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_event, NULL, NULL));
    wifi_config_t wc = { 0 };
    /* The driver's fields are fixed-width, not strings: a 32-byte SSID and a
     * 64-byte passphrase use the whole field with no terminator. */
    memcpy(wc.sta.ssid, s_ssid, strnlen(s_ssid, sizeof wc.sta.ssid));
    memcpy(wc.sta.password, s_pass, strnlen(s_pass, sizeof wc.sta.password));
    memset(s_pass, 0, sizeof s_pass);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    /* The configuration OBSERVED to coexist with BLE on this board
     * (board-capabilities.md, Wi-Fi and BLE coexist): modem sleep off. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    set_state(PANEL_NET_JOINING);
    ESP_ERROR_CHECK(esp_wifi_start());
    log_heap("wifi started");

    /* Below the link and the UI: the cloud is optional, him being present is
     * not. 8 KB because TLS handshakes want stack -- and that stack in PSRAM,
     * because internal RAM is what the slice 3.1 spike measured running out. */
    xTaskCreateWithCaps(probe_task, "panel_net", 8192, NULL, 2, NULL, MALLOC_CAP_SPIRAM);
}
