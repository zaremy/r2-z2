/* sd_check -- prove the microSD path, including the part the vendor example
 * cannot.
 *
 * The vendor example (09_sdmmc/main/sd_card_example_main.c) writes, renames and
 * reads, then unmounts at :76 -- but its last read is at :71, BEFORE the
 * unmount. So it never demonstrates that anything survives the unmount, which
 * is the only property that makes a filesystem worth having. #81 AC2 requires
 * either a remount-and-re-read or pulling the card and reading it on a Mac.
 * This does the former, so it costs no operator time.
 *
 * NO PANEL INIT. bsp_display_new() is never called. The BSP is a dependency
 * here (unlike board_check), so the check that matters is what ends up in the
 * LINK, not what is in the dependency tree:
 *   xtensa-esp32s3-elf-nm build/sd_check.elf | grep -ci 'co5300\|bsp_display'
 * must print 0. Run a positive control on the same command -- a broken grep
 * pattern also prints 0.
 *
 * THE FALSE PASS THIS GUARDS AGAINST: reading back "hello" proves nothing if a
 * previous run left "hello" on the card. Every boot writes a nonce that could
 * only have come from THIS boot, and the post-remount comparison is exact
 * bytes against that nonce -- not "a line was read", not "the file exists".
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_check";

/* 8.3 ONLY. CONFIG_FATFS_LFN_NONE is the ESP-IDF default, so long filenames are
 * unavailable and fopen() fails outright on anything longer than 8 characters
 * before the dot. An earlier version used "r2z2_sd_check.txt" (13) and the write
 * failed with nothing wrong with the card -- the BSP even warns about it at
 * mount time ("Long filenames on SD card are disabled in menuconfig!").
 *
 * Left at 8.3 rather than enabling LFN, because this slice is proving the
 * storage path and a config change is what broke the previous flash. Long
 * filenames are a real decision for later: anything storing memories or dated
 * logs on this card will want CONFIG_FATFS_LFN_HEAP. Recorded in #81. */
#define TEST_PATH  BSP_SD_MOUNT_POINT "/sdchk.txt"
#define PAYLOAD_MAX 128

static bool write_payload(const char *path, const char *payload)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen(%s, w) failed", path);
        return false;
    }
    size_t n = strlen(payload);
    bool ok = (fwrite(payload, 1, n, f) == n);
    /* fclose can fail on a flush error -- a write that is never checked here is
     * exactly how a "successful" write reaches a card that never got it. */
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "fclose(%s) failed -- data may not have reached the card", path);
        ok = false;
    }
    return ok;
}

/* Reads the file and compares EXACT bytes against expect. Returns false on any
 * difference, including length. */
static bool read_and_compare(const char *path, const char *expect)
{
    char buf[PAYLOAD_MAX + 1] = {0};
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen(%s, r) failed -- file is not there", path);
        return false;
    }
    size_t n = fread(buf, 1, PAYLOAD_MAX, f);
    fclose(f);
    buf[n] = '\0';

    if (n != strlen(expect) || memcmp(buf, expect, n) != 0) {
        ESP_LOGE(TAG, "content mismatch");
        ESP_LOGE(TAG, "  expected (%u B): '%s'", (unsigned)strlen(expect), expect);
        ESP_LOGE(TAG, "  read     (%u B): '%s'", (unsigned)n, buf);
        return false;
    }
    ESP_LOGI(TAG, "read back %u bytes, exact match: '%s'", (unsigned)n, buf);
    return true;
}

static void stop(const char *why, const char *what_to_do)
{
    printf("\n---- VERDICT ----\n");
    printf("FAIL: %s\n", why);
    printf("%s\n", what_to_do);
    printf("AC1/AC2 do NOT pass on this run.\n");
    printf("=================================\n\n");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    printf("\n================ sd_check ================\n");

    /* A nonce this boot could not have inherited from a previous run. */
    char payload[PAYLOAD_MAX];
    uint32_t nonce = esp_random();
    snprintf(payload, sizeof(payload),
             "r2z2 sd_check nonce=%08" PRIx32 " uptime_us=%lld",
             nonce, (long long)esp_timer_get_time());
    printf("payload for this boot: %s\n\n", payload);

    /* ---- mount ---------------------------------------------------------- */
    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_sdcard_mount() -> %s", esp_err_to_name(ret));
        stop("the card did not mount",
             "Confirm a FAT32-formatted microSD (SDHC, <=32 GB) is seated. SDXC\n"
             "cards over 32 GB ship exFAT, which ESP-IDF cannot mount\n"
             "(FF_FS_EXFAT is 0). Format on the Mac -- NEVER enable\n"
             "format_if_mount_failed. Then reset and re-run.");
    }
    printf("mounted at %s\n\n", BSP_SD_MOUNT_POINT);
    if (bsp_sdcard != NULL) {
        sdmmc_card_print_info(stdout, bsp_sdcard);
    }

    /* ---- negative control: the file must NOT be there before we write ---- */
    struct stat st;
    if (stat(TEST_PATH, &st) == 0) {
        printf("\nprior copy found (%ld B) -- removing it so the read-back below\n"
               "cannot be satisfied by a previous run's data.\n", (long)st.st_size);
        if (unlink(TEST_PATH) != 0) {
            stop("could not remove the previous test file",
                 "The card may be write-protected or the filesystem is damaged.");
        }
    }
    if (stat(TEST_PATH, &st) == 0) {
        stop("the test file still exists after unlink",
             "This is a filesystem-layer fault. Every read-back below would be\n"
             "meaningless, so the run stops here rather than reporting a pass.");
    }
    printf("negative control OK: %s is absent before the write\n\n", TEST_PATH);

    /* ---- write, then read back on the SAME mount ------------------------- */
    if (!write_payload(TEST_PATH, payload)) {
        stop("the write failed", "See the error above.");
    }
    printf("wrote %s\n", TEST_PATH);
    if (!read_and_compare(TEST_PATH, payload)) {
        stop("read-back on the same mount did not match what was written",
             "The card is mounted but not storing correctly.");
    }

    /* ---- THE TEST: unmount, remount, read again -------------------------- */
    ret = bsp_sdcard_unmount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_sdcard_unmount() -> %s", esp_err_to_name(ret));
        stop("unmount failed", "Cached writes may not have been flushed.");
    }
    printf("\nunmounted. Everything above proves nothing about persistence --\n"
           "this next part is the actual test.\n\n");

    ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "remount -> %s", esp_err_to_name(ret));
        stop("the card did not remount",
             "It mounted once, so the card and wiring are fine. A remount that\n"
             "fails points at the unmount not releasing cleanly.");
    }
    printf("remounted\n");

    bool survived = read_and_compare(TEST_PATH, payload);
    esp_err_t final_unmount = bsp_sdcard_unmount();

    printf("\n---- VERDICT ----\n");
    if (!survived) {
        printf("FAIL: the file did not survive the unmount/remount cycle.\n");
        printf("AC2 does NOT pass.\n");
    } else if (final_unmount != ESP_OK) {
        printf("PARTIAL: content survived the cycle, but the final unmount\n");
        printf("         returned %s. AC2 passes; note the unmount fault.\n",
               esp_err_to_name(final_unmount));
    } else {
        printf("PASS: mounted, wrote, unmounted, REMOUNTED, and read back the\n");
        printf("      exact bytes this boot generated. AC1 and AC2 pass.\n");
    }
    printf("=================================\n\n");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
