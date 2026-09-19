/* The panel's own way out to the cloud -- E2E v0 slice 3.1.
 *
 * Wi-Fi station, credentials and keys from NVS (namespace "r2cfg", written by
 * tools/provision_nvs.py from the repo's gitignored .env), and one HTTPS
 * request that proves the whole path -- DNS, TLS against the certificate
 * bundle, the provider's auth -- while BLE and the glass are running.
 *
 * D-015: losing the cloud must degrade him to MUTE, never to dead. Nothing
 * here blocks the link or the UI: it runs on its own task, every failure is a
 * logged state, and a board with no credentials simply never joins.
 */
#ifndef PANEL_NET_H
#define PANEL_NET_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PANEL_NET_UNPROVISIONED = 0,  /* no Wi-Fi credentials in NVS */
    PANEL_NET_JOINING,
    PANEL_NET_UP,                 /* associated, with an address */
    PANEL_NET_CLOUD_OK,           /* the probe got a 2xx from the provider */
    PANEL_NET_CLOUD_FAIL,         /* up, but the probe failed (DNS, TLS, auth) */
} panel_net_state_t;

/* Reads NVS and, if provisioned, starts the station and the probe task.
 * Call once, after nvs_flash_init. Safe to call with nothing provisioned. */
void panel_net_start(void);

panel_net_state_t panel_net_state(void);
const char *panel_net_state_name(panel_net_state_t s);

/* The provider key, or NULL when none is provisioned. Owned by this module. */
const char *panel_net_llm_key(void);
const char *panel_net_llm_model(void);

/* Stream `seconds` of 16 kHz mono 16-bit silence to the provider's
 * transcription endpoint AT REAL TIME -- 640 bytes every 20 ms, the rate a
 * held TALK would produce -- as one multipart request. Returns the HTTP
 * status, or a negative esp_err_t. Slice 3.1's load; 3.3's upload path with
 * a microphone in place of the zeros. Blocks the caller for ~`seconds`. */
int panel_net_upload_realtime(unsigned seconds);

/* Internal RAM's low-water mark since boot, for a caller grading a run. */
uint32_t panel_net_internal_min_ever(void);

#ifdef __cplusplus
}
#endif
#endif /* PANEL_NET_H */
