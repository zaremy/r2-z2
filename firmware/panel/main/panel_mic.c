#include "panel_mic.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

static const char *TAG = "mic";

/* 20 ms at 16 kHz mono 16-bit, the same chunk panel_net already streams --
 * one number for "how much audio is a unit" rather than two that can drift. */
#define CHUNK_BYTES 640u

static esp_codec_dev_handle_t s_mic;
static int16_t   *s_buf;                 /* PANEL_MIC_CAP_BYTES, in PSRAM */
static uint32_t    s_last_bytes;
static bool         s_last_capped;
static volatile bool s_running;
static volatile bool s_stop_requested;
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

bool panel_mic_init(void)
{
    /* Ask for 16 kHz BEFORE bsp_audio_codec_microphone_init() can call
     * bsp_audio_init(NULL) and get the BSP's 22.05 kHz playback default --
     * bsp_audio_init() is a no-op once the I2S channels exist, so whichever
     * caller goes first wins the rate for the whole board.
     *
     * BSP_I2S_DUPLEX_MONO_CFG is the BSP's own macro for exactly this, but it
     * is defined in esp32_s3_touch_amoled_1_8.c, not the public header -- so
     * this rebuilds it from the pieces the header DOES export: the five pin
     * macros (BSP_I2S_SCLK et al.) and the IDF's own STD config helpers. */
    const i2s_std_config_t cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(PANEL_MIC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DSIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    esp_err_t err = bsp_audio_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init: %s", esp_err_to_name(err));
        return false;
    }

    s_mic = bsp_audio_codec_microphone_init();
    if (s_mic == NULL) {
        ESP_LOGE(TAG, "no microphone codec on this board");
        return false;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .channel_mask    = 0,
        .sample_rate     = PANEL_MIC_SAMPLE_RATE_HZ,
        .mclk_multiple   = 0,
    };
    if (esp_codec_dev_open(s_mic, &fs) != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        s_mic = NULL;
        return false;
    }

    s_buf = heap_caps_malloc(PANEL_MIC_CAP_BYTES, MALLOC_CAP_SPIRAM);
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "no PSRAM for a %u B capture buffer",
                 (unsigned)PANEL_MIC_CAP_BYTES);
        return false;
    }

    ESP_LOGI(TAG, "ready: %u Hz mono, %u s cap, %u B buffer",
             PANEL_MIC_SAMPLE_RATE_HZ, PANEL_MIC_CAP_MS / 1000u,
             (unsigned)PANEL_MIC_CAP_BYTES);
    return true;
}

static void capture_task(void *arg)
{
    (void)arg;
    uint32_t got = 0;
    bool capped = false;

    while (got + CHUNK_BYTES <= PANEL_MIC_CAP_BYTES) {
        portENTER_CRITICAL(&s_mux);
        const bool stop = s_stop_requested;
        portEXIT_CRITICAL(&s_mux);
        if (stop) break;

        uint8_t *dst = (uint8_t *)s_buf + got;
        if (esp_codec_dev_read(s_mic, dst, (int)CHUNK_BYTES) != 0) {
            ESP_LOGW(TAG, "read failed at %u B, ending capture early", (unsigned)got);
            break;
        }
        got += CHUNK_BYTES;
    }
    if (got + CHUNK_BYTES > PANEL_MIC_CAP_BYTES) {
        capped = true;
        ESP_LOGW(TAG, "hit the %u s cap", PANEL_MIC_CAP_MS / 1000u);
    }

    portENTER_CRITICAL(&s_mux);
    s_last_bytes = got;
    s_last_capped = capped;
    s_running = false;
    portEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "capture done: %u B%s", (unsigned)got, capped ? " (capped)" : "");
    vTaskDeleteWithCaps(NULL);
}

bool panel_mic_start(void)
{
    if (s_mic == NULL || s_buf == NULL) return false;

    portENTER_CRITICAL(&s_mux);
    if (s_running) { portEXIT_CRITICAL(&s_mux); return false; }
    s_running = true;
    s_stop_requested = false;
    portEXIT_CRITICAL(&s_mux);

    xTaskCreateWithCaps(capture_task, "panel_mic", 4096, NULL, 3, NULL,
                        MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "capture started");
    return true;
}

bool panel_mic_stop(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool was_running = s_running;
    s_stop_requested = true;
    portEXIT_CRITICAL(&s_mux);
    if (!was_running) return false;

    /* One chunk (20 ms) is the longest the task can still be blocked inside
     * esp_codec_dev_read before it sees the flag; give it a wide margin
     * rather than poll indefinitely from a caller that must not block long
     * (ui_task, holding the display lock). */
    for (int i = 0; i < 20 && s_running; i++) vTaskDelay(pdMS_TO_TICKS(5));
    return true;
}

const int16_t *panel_mic_last_capture(uint32_t *out_bytes, bool *out_capped)
{
    portENTER_CRITICAL(&s_mux);
    const uint32_t bytes  = s_last_bytes;
    const bool     capped = s_last_capped;
    portEXIT_CRITICAL(&s_mux);
    if (out_bytes)  *out_bytes  = bytes;
    if (out_capped) *out_capped = capped;
    return s_buf;
}

#ifdef PANEL_MIC_DUMP
#include "esp_partition.h"

/* Little-endian, at the start of 'storage'. Checked by tools/decode_audio.py
 * so a stale or never-written partition fails loudly instead of decoding
 * whatever 0xFF-erased flash happens to contain -- same reasoning as
 * panel_shot.c's SHOT_MAGIC. */
#define MIC_DUMP_MAGIC 0x32325A4Du   /* "MZ22" */

typedef struct {
    uint32_t magic;
    uint32_t sample_rate;
    uint32_t bytes;
} mic_dump_hdr_t;

bool panel_mic_dump_to_flash(void)
{
    uint32_t bytes; bool capped;
    const int16_t *pcm = panel_mic_last_capture(&bytes, &capped);
    if (pcm == NULL || bytes == 0) {
        ESP_LOGE(TAG, "nothing captured to dump");
        return false;
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (part == NULL) { ESP_LOGE(TAG, "no 'storage' partition"); return false; }

    mic_dump_hdr_t hdr = { MIC_DUMP_MAGIC, PANEL_MIC_SAMPLE_RATE_HZ, bytes };
    const uint32_t total = sizeof hdr + bytes;
    const uint32_t erase = (total + part->erase_size - 1) / part->erase_size
                         * part->erase_size;
    if (erase > part->size) {
        ESP_LOGE(TAG, "capture would run past 'storage'");
        return false;
    }

    esp_err_t err = esp_partition_erase_range(part, 0, erase);
    if (err == ESP_OK) err = esp_partition_write(part, 0, &hdr, sizeof hdr);
    if (err == ESP_OK) err = esp_partition_write(part, sizeof hdr, pcm, bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dump write failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "MIC DUMP ready: %u B at flash 0x%06X",
             (unsigned)total, (unsigned)part->address);
    ESP_LOGI(TAG, "  read it: tools/grab_audio.sh <port> out.wav");
    return true;
}
#endif
