#ifndef PANEL_MIC_H
#define PANEL_MIC_H

#include <stdbool.h>
#include <stdint.h>

/* The ES8311 capture path for TALK (E2E v0 slice 3.2).
 *
 * 16 kHz mono 16-bit PCM, the same format panel_net_upload_realtime already
 * streams (chunked, not a whole WAV in RAM -- the plan's own rule, 3.1). The
 * board's BSP defaults the I2S clock to 22.05 kHz for playback; init() asks
 * for 16 kHz explicitly, before bsp_audio_codec_microphone_init() can win
 * that argument by going first.
 *
 * Capture lives in a PSRAM buffer, capped at PANEL_MIC_CAP_MS (the plan's
 * 12 s hold-too-long cap, `capture.py` on the Mac) -- past it the capture
 * task stops itself and reports `capped`, so main.c can retire the exchange
 * as PX_RETIRE_HOLD_TOO_LONG and discard rather than hand a still-running
 * capture to a release that never comes.
 */
#define PANEL_MIC_SAMPLE_RATE_HZ 16000u
#define PANEL_MIC_CAP_MS         12000u
#define PANEL_MIC_CAP_BYTES      (PANEL_MIC_SAMPLE_RATE_HZ * 2u * PANEL_MIC_CAP_MS / 1000u)

/* Brings up the codec once at boot. Safe to call from a build with no mic
 * (the sd_check reference has one; this checks bsp_audio_codec_microphone_init
 * for NULL rather than assuming it exists). False means TALK can arm the
 * screen but no audio will ever be captured -- callers should still allow
 * the hold to be honest about that in the log, never silently. */
bool panel_mic_init(void);

/* Starts a fresh capture on its own PSRAM-stacked task. False if one is
 * already running (start is not reentrant) or init() never succeeded. */
bool panel_mic_start(void);

/* Asks the running capture to stop at the next chunk boundary (~20 ms) and
 * blocks until it has. A no-op, returning false, if none was running. */
bool panel_mic_stop(void);

/* The last completed capture: `bytes` of 16 kHz mono PCM in a buffer owned by
 * this module (valid until the next panel_mic_start), and whether it hit the
 * 12 s cap rather than being stopped by a release. Zero-initialized before
 * the first capture -- the zero value is the safe one, an empty recording. */
const int16_t *panel_mic_last_capture(uint32_t *out_bytes, bool *out_capped);

/* 3.2's own gate, not a shipping path: writes the last completed capture to
 * the 'storage' partition, the same way panel_shot.c writes a frame, for
 * tools/grab_audio.sh to read back over the same cable. Only compiled when
 * PANEL_MIC_DUMP is set -- it reuses the space panel_shot's slots use, so it
 * must never be combined with a PANEL_SHOT_* build (CMakeLists.txt enforces
 * this). False if there is nothing captured yet, or the write failed. */
#ifdef PANEL_MIC_DUMP
bool panel_mic_dump_to_flash(void);
#endif

#endif
