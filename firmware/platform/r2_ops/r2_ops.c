#include "r2_ops.h"

#include <string.h>

#define DID_POWER       0x13
#define DID_ANIMATRONIC 0x17
#define DID_SYSTEM_INFO 0x11

#define CID_BATTERY     0x03
#define CID_GET_HEAD    0x14
#define CID_APP_VERSION 0x00

int r2_ops_request_battery(uint8_t seq, r2_tx_fn tx, void *ctx)
{ return r2_gate_send(DID_POWER, CID_BATTERY, seq, NULL, 0, tx, ctx); }

int r2_ops_request_head(uint8_t seq, r2_tx_fn tx, void *ctx)
{ return r2_gate_send(DID_ANIMATRONIC, CID_GET_HEAD, seq, NULL, 0, tx, ctx); }

int r2_ops_probe_version(uint8_t seq, r2_tx_fn tx, void *ctx)
{ return r2_gate_send(DID_SYSTEM_INFO, CID_APP_VERSION, seq, NULL, 0, tx, ctx); }

/* Shared preamble: the response must BE this op, and must not carry an error.
 * Checking did/cid is not pedantry -- responses arrive asynchronously on one
 * notification characteristic, so without it a battery parse would decode
 * whatever frame happened to arrive next. */
static r2_ops_err_t precheck(const r2_response_t *r, uint8_t did, uint8_t cid,
                             size_t want_len)
{
    if (r == NULL) return R2_OPS_NULL;
    if (r->did != did || r->cid != cid) return R2_OPS_WRONG_OP;
    if (r->err != 0) return R2_OPS_DEVICE_ERROR;
    if (r->data_len != want_len) return R2_OPS_BAD_LENGTH;
    /* A response can claim a length and carry no pointer. Checking the length
     * alone is what makes that dereference look validated. */
    if (want_len > 0 && r->data == NULL) return R2_OPS_NULL;
    return R2_OPS_OK;
}

r2_ops_err_t r2_ops_parse_battery(const r2_response_t *r, r2_battery_t *out)
{
    if (out == NULL) return R2_OPS_NULL;
    const r2_ops_err_t e = precheck(r, DID_POWER, CID_BATTERY, 2);
    if (e != R2_OPS_OK) return e;
    out->centivolts = (uint16_t)(((uint32_t)r->data[0] << 8) | r->data[1]);
    out->volts      = (float)out->centivolts / 100.0f;
    return R2_OPS_OK;
}

r2_ops_err_t r2_ops_parse_head(const r2_response_t *r, r2_head_t *out)
{
    if (out == NULL) return R2_OPS_NULL;
    const r2_ops_err_t e = precheck(r, DID_ANIMATRONIC, CID_GET_HEAD, 4);
    if (e != R2_OPS_OK) return e;
    /* Big-endian IEEE-754 float32. memcpy rather than a pointer cast: the cast
     * is a strict-aliasing violation and compilers do act on it at -O2. */
    const uint32_t bits = ((uint32_t)r->data[0] << 24) | ((uint32_t)r->data[1] << 16)
                        | ((uint32_t)r->data[2] << 8)  |  (uint32_t)r->data[3];
    float f;
    memcpy(&f, &bits, sizeof f);
    out->degrees = f;
    return R2_OPS_OK;
}

r2_ops_err_t r2_ops_parse_version(const r2_response_t *r, r2_version_t *out)
{
    if (out == NULL) return R2_OPS_NULL;
    const r2_ops_err_t e = precheck(r, DID_SYSTEM_INFO, CID_APP_VERSION, 6);
    if (e != R2_OPS_OK) return e;
    out->major    = (uint16_t)(((uint32_t)r->data[0] << 8) | r->data[1]);
    out->minor    = (uint16_t)(((uint32_t)r->data[2] << 8) | r->data[3]);
    out->revision = (uint16_t)(((uint32_t)r->data[4] << 8) | r->data[5]);
    return R2_OPS_OK;
}

const char *r2_ops_err_name(r2_ops_err_t e)
{
    switch (e) {
    case R2_OPS_OK:           return "ok";
    case R2_OPS_WRONG_OP:     return "response is for a different op";
    case R2_OPS_DEVICE_ERROR: return "R2 returned an error code";
    case R2_OPS_BAD_LENGTH:   return "unexpected payload length";
    case R2_OPS_NULL:         return "null argument";
    default:                  return "?";
    }
}

const char *r2_ops_device_error_name(uint8_t err)
{
    switch (err) {
    case 0x00: return "success";
    case 0x01: return "bad_device_id";
    case 0x02: return "bad_command_id";   /* what a refused capability looks like */
    case 0x03: return "not_yet_implemented";
    case 0x04: return "command_is_restricted";
    case 0x05: return "bad_data_length";
    case 0x06: return "command_failed";
    case 0x07: return "bad_parameter_value";
    case 0x08: return "busy";
    case 0x09: return "bad_target_id";
    case 0x0A: return "target_unavailable";
    default:   return "unknown_error";
    }
}
