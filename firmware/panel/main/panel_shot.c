/* Screenshots of the panel, via the storage partition.
 *
 * The panel is the one surface nothing on this board can see, so every visual
 * check costs the operator a walk to the droid and a phone photo. That is the
 * scarce resource in this project, spent on something a machine can do.
 *
 * WHY NOT OVER THE CONSOLE. That was the first attempt and it does not work:
 * 368 x 448 RGB565 is 322 KB, and the ESP32-S3 USB-Serial/JTAG console DROPS
 * output when the host is not draining fast enough -- silently, emitting the
 * newlines and losing the content, so the capture ends in a run of blank lines
 * that reads like the encoder stopping rather than the transport discarding
 * bytes. RLE got it under 60 KB and it still truncated at 882 lines; throttling
 * with a yield made it dramatically WORSE (16 lines), which is the tell that
 * the mechanism was not the one being modelled. Rather than keep tuning a
 * transport I cannot observe, use one whose whole job is bulk transfer.
 *
 * SO: write the raw frame to the storage partition and read it back with
 * esptool over the same cable. No console, no encoding, no throttling, and
 * the read is verified by esptool's own checksum.
 */
#include "panel_shot.h"

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "shot";

/* Little-endian, at the start of each slot in the storage partition. The magic
 * is checked by the decoder so a stale or never-written slot fails loudly
 * instead of rendering whatever bytes happen to be at that address -- flash
 * reads back 0xFF when erased, which is a perfectly plausible white image.
 *
 * `seq` counts captures WITHIN a boot and restarts at 0 with the board, so it
 * distinguishes slots written by one run from each other, not this run's from
 * the last one's. That is what panel_shot_erase_all is for. */
#define SHOT_MAGIC 0x52325A32u   /* "R2Z2" */

typedef struct {
    uint32_t magic;
    uint16_t w, h;
    uint32_t seq;      /* increments per capture, so a stale read is visible */
    uint32_t bytes;
} shot_hdr_t;

static uint32_t s_seq = 0;

bool panel_shot_take(void) { return panel_shot_take_slot(0); }

bool panel_shot_erase_all(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (part == NULL) { ESP_LOGE(TAG, "no 'storage' partition"); return false; }
    const uint32_t span = PANEL_SHOT_SLOTS * PANEL_SHOT_SLOT_BYTES;
    const uint32_t n = span > part->size ? part->size : span;
    const esp_err_t err = esp_partition_erase_range(part, 0, n);
    if (err != ESP_OK) {
        /* Loud, because the danger it exists to remove is still there: the
         * previous run's frames are intact and will decode perfectly. */
        ESP_LOGE(TAG, "ERASE FAILED (%s) -- any slot this run does not "
                      "overwrite still holds an OLD frame", esp_err_to_name(err));
        return false;
    }
    ESP_LOGW(TAG, "erased %u B of slots", (unsigned)n);
    return true;
}

bool panel_shot_take_slot(unsigned slot)
{
    if (slot >= PANEL_SHOT_SLOTS) {
        ESP_LOGE(TAG, "slot %u is past the %u the partition holds",
                 slot, (unsigned)PANEL_SHOT_SLOTS);
        return false;
    }
    const uint32_t base = slot * PANEL_SHOT_SLOT_BYTES;
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (part == NULL) { ESP_LOGE(TAG, "no 'storage' partition"); return false; }

    /* Lock only for the capture. The flash write below takes a while and
     * holding the display lock through it would freeze the panel for every
     * screenshot -- a diagnostic that degrades the thing it diagnoses. */
    lv_draw_buf_t *buf = NULL;
    if (bsp_display_lock(1000)) {
        buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
        bsp_display_unlock();
    }
    if (buf == NULL) { ESP_LOGE(TAG, "lv_snapshot_take failed"); return false; }

    const uint32_t w = buf->header.w, h = buf->header.h;
    const uint32_t stride = buf->header.stride;
    const uint32_t row_bytes = w * 2u;
    const uint32_t payload = row_bytes * h;

    shot_hdr_t hdr = { SHOT_MAGIC, (uint16_t)w, (uint16_t)h, ++s_seq, payload };
    const uint32_t total = sizeof hdr + payload;
    const uint32_t erase = (total + part->erase_size - 1) / part->erase_size * part->erase_size;

    /* Refuse rather than wrap: a slot running off the end would erase
     * whatever follows it. */
    if (base + erase > part->size) {
        ESP_LOGE(TAG, "slot %u would run past the partition", slot);
        lv_draw_buf_destroy(buf);
        return false;
    }
    esp_err_t err = esp_partition_erase_range(part, base, erase);
    if (err == ESP_OK) err = esp_partition_write(part, base, &hdr, sizeof hdr);
    /* Row by row, because the snapshot's stride need not equal w*2. Copying
     * the buffer wholesale would smear the image by the padding amount -- and
     * it would look almost right, which is the worst kind of wrong for a tool
     * whose entire job is to be believed instead of the operator's eyes. */
    for (uint32_t y = 0; err == ESP_OK && y < h; y++)
        err = esp_partition_write(part, base + sizeof hdr + y * row_bytes,
                                  buf->data + (size_t)y * stride, row_bytes);

    lv_draw_buf_destroy(buf);

    if (err != ESP_OK) { ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(err)); return false; }
    ESP_LOGI(TAG, "SHOT #%u ready: %ux%u, %u B in slot %u (offset 0x%X)",
             (unsigned)s_seq, (unsigned)w, (unsigned)h, (unsigned)total,
             slot, (unsigned)base);
    ESP_LOGI(TAG, "  read it: tools/grab_shot.sh <port> out.png %u   (flash 0x%06X)",
             slot, (unsigned)(part->address + base));
    return true;
}
