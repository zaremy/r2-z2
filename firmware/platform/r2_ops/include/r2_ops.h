/* Read-only ops — #114 slice 3.
 *
 * The first caller of both earlier slices, and it routes every send through
 * r2_gate_send(). It never touches r2_packet_encode directly: the gate is only
 * a gate if it is the only way out, and a second path to the radio would make
 * slice 2 decorative.
 *
 * All three ops sit at the READ tier and cannot move him, which is the entire
 * test for that tier. Note what that does and does not say: the READ tier means
 * OUR gate permits sending them. It says nothing about whether R2 answers --
 * the version probe may well come back bad_command_id, and that is a finding,
 * not a failure.
 *
 * CONFIDENCE, PER OP -- these are not equally known and shipping them as if
 * they were is how an INFERRED claim graduates:
 *
 *   battery   OBSERVED. Sent thousands of times; a real captured response is
 *             committed as a fixture. Payload is a big-endian uint16 in
 *             CENTIVOLTS (mac-prototype/r2_probe.py:1189 divides by 100).
 *   head      OBSERVED. Payload is a big-endian float32 in degrees
 *             (r2_probe.py:732). Accuracy measured 5/5 within 5 degrees of
 *             commanded (research/r2-capabilities.md:135).
 *   version   LIBRARY CLAIM, NEVER SENT BY US. spherov2's SystemInfo._did is
 *             17 (0x11) and get_main_app_version encodes CID 0, returning
 *             '>3H' -- three big-endian uint16 (reference/sphero-r2d2/
 *             spherov2/commands/system_info.py:41-46). But docs/research/
 *             r2-protocol.md:362 records system_info as UNTRACED, and
 *             CLAUDE.md's standing counterexample is enable_idle_animations:
 *             a capability that resolves correctly and is still refused by the
 *             firmware. Treat a version read as worth PROBING, not supported.
 *             If R2 answers bad_command_id, that is a finding, not a bug here.
 */
#ifndef R2_OPS_H
#define R2_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include "r2_gate.h"
#include "r2_packet.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    R2_OPS_OK            =  0,
    R2_OPS_WRONG_OP      = -1,  /* response is for a different did/cid */
    R2_OPS_DEVICE_ERROR  = -2,  /* R2 answered with a non-zero error code */
    R2_OPS_BAD_LENGTH    = -3,
    R2_OPS_NULL          = -4,
} r2_ops_err_t;

typedef struct { uint16_t centivolts; float volts; } r2_battery_t;
typedef struct { float degrees; }                    r2_head_t;
typedef struct { uint16_t major, minor, revision; }  r2_version_t;

/* Requests. Each returns the encoded length, or a negative r2_gate_verdict_t
 * if the ceiling refused it. All are READ tier, so all work at the default. */
int r2_ops_request_battery(uint8_t seq, r2_tx_fn tx, void *ctx);
int r2_ops_request_head(uint8_t seq, r2_tx_fn tx, void *ctx);
/* NOT request_ -- this one has never been answered by R2. It is named a probe
 * so no caller can read it as a supported read. */
int r2_ops_probe_version(uint8_t seq, r2_tx_fn tx, void *ctx);

/* Parsers. Each REFUSES a response that is not its own op -- otherwise a
 * battery parse would happily decode a head reading as volts. */
r2_ops_err_t r2_ops_parse_battery(const r2_response_t *r, r2_battery_t *out);
r2_ops_err_t r2_ops_parse_head(const r2_response_t *r, r2_head_t *out);
r2_ops_err_t r2_ops_parse_version(const r2_response_t *r, r2_version_t *out);

const char *r2_ops_err_name(r2_ops_err_t e);
/* R2's own error codes, r2_probe.py Response.ERRORS. */
const char *r2_ops_device_error_name(uint8_t err);

#ifdef __cplusplus
}
#endif
#endif /* R2_OPS_H */
