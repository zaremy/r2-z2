#include "r2_ops.h"

#include <string.h>

#define DID_POWER       0x13
#define DID_ANIMATRONIC 0x17
#define DID_SYSTEM_INFO 0x11

#define CID_BATTERY     0x03
#define CID_GET_HEAD    0x14
#define CID_APP_VERSION 0x00

#define DID_IO          0x1A
#define CID_LEDS_16BIT  0x0E

#define CID_ANIM_STOP   0x2B
#define CID_LEG_ACTION  0x0D
#define CID_STOP_AUDIO  0x0A
#define LEG_ACTION_STOP 0x00

r2_stop_report_t r2_ops_stop_all(r2_seq_fn next_seq, r2_tx_fn tx, void *ctx)
{
    r2_stop_report_t r = { false, false, false, 0u };
    if (next_seq == NULL || tx == NULL) return r;   /* nothing was sent */

    /* EACH ON ITS OWN LINE, deliberately un-chained. A && or an early return
     * here would make the second and third halts conditional on the first --
     * which is the exact bug the prototype's comment records, where the
     * "always attempts both" guarantee was false precisely when it mattered.
     *
     * The seq is drawn per command: three sends sharing one seq would let a
     * single reply resolve all three. */
    static const uint8_t legs_stop[1] = { LEG_ACTION_STOP };

    r.animation = r2_gate_send(DID_ANIMATRONIC, CID_ANIM_STOP, next_seq(),
                               NULL, 0, tx, ctx) > 0;
    r.audio     = r2_gate_send(DID_IO, CID_STOP_AUDIO, next_seq(),
                               NULL, 0, tx, ctx) > 0;
    /* The one the gate admits only because its payload is pinned (D-026). */
    r.legs      = r2_gate_send(DID_ANIMATRONIC, CID_LEG_ACTION, next_seq(),
                               legs_stop, sizeof legs_stop, tx, ctx) > 0;

    r.sent = (unsigned)r.animation + (unsigned)r.audio + (unsigned)r.legs;
    return r;
}

bool r2_stop_is_complete(r2_stop_report_t r)
{
    return r.animation && r.audio && r.legs;
}

int r2_ops_request_battery(uint8_t seq, r2_tx_fn tx, void *ctx)
{ return r2_gate_send(DID_POWER, CID_BATTERY, seq, NULL, 0, tx, ctx); }

int r2_ops_request_head(uint8_t seq, r2_tx_fn tx, void *ctx)
{ return r2_gate_send(DID_ANIMATRONIC, CID_GET_HEAD, seq, NULL, 0, tx, ctx); }

int r2_ops_probe_version(uint8_t seq, r2_tx_fn tx, void *ctx)
{ return r2_gate_send(DID_SYSTEM_INFO, CID_APP_VERSION, seq, NULL, 0, tx, ctx); }

/* ---- LEDs --------------------------------------------------------------- */

static int popcount16(uint16_t v)
{
    int n = 0;
    while (v) { n += (v & 1u); v >>= 1; }
    return n;
}

int r2_ops_set_leds(uint16_t mask, const uint8_t *values, size_t n_values,
                    uint8_t seq, r2_tx_fn tx, void *ctx)
{
    /* Refuse before the gate, because these are malformed rather than
     * forbidden, and the two deserve different words. */
    if (values == NULL) return R2_OPS_BAD_LED_REQUEST;
    /* An empty mask would send a two-byte payload that sets nothing. R2 has
     * never been asked that and the answer is not worth discovering by
     * accident. */
    if (mask == 0) return R2_OPS_BAD_LED_REQUEST;
    /* Bits above 7 are not channels on this droid. The Mac layer refuses them
     * (r2_probe.py:1300) and so does this one -- an unmapped bit is a guess
     * about hardware, sent as a command. */
    if (mask > R2_LED_MASK_ALL) return R2_OPS_BAD_LED_REQUEST;
    /* THE guard. The payload carries one value per set bit and nothing states
     * the count, so a mismatch does not error -- R2 reads the wrong bytes as
     * values, or runs off the end of the payload. It is the one way to build a
     * packet that is well-formed and means something else. */
    if (n_values != (size_t)popcount16(mask)) return R2_OPS_BAD_LED_REQUEST;

    uint8_t payload[2 + 16];
    payload[0] = (uint8_t)(mask >> 8);
    payload[1] = (uint8_t)(mask & 0xFFu);
    memcpy(payload + 2, values, n_values);

    return r2_gate_send(DID_IO, CID_LEDS_16BIT, seq,
                        payload, 2u + n_values, tx, ctx);
}

int r2_ops_status_leds(const uint8_t values[8], uint8_t seq,
                       r2_tx_fn tx, void *ctx)
{
    if (values == NULL) return R2_OPS_BAD_LED_REQUEST;
    uint8_t payload[2 + 8];
    payload[0] = (uint8_t)(R2_LED_MASK_ALL >> 8);
    payload[1] = (uint8_t)(R2_LED_MASK_ALL & 0xFFu);
    memcpy(payload + 2, values, 8);
    return r2_gate_send_status(DID_IO, CID_LEDS_16BIT, seq,
                               payload, sizeof payload, tx, ctx);
}

int r2_ops_set_rgb(uint8_t r, uint8_t g, uint8_t b,
                   uint8_t seq, r2_tx_fn tx, void *ctx)
{
    /* Ascending bit order: front R,G,B (0,1,2) then back R,G,B (4,5,6).
     * Bits 3 and 7 are left alone -- logic and holo are brightness-only
     * channels and folding them into a colour call would be a lie about what
     * they can do. */
    const uint8_t values[6] = { r, g, b, r, g, b };
    return r2_ops_set_leds(R2_LED_MASK_FRONT | R2_LED_MASK_BACK,
                           values, sizeof values, seq, tx, ctx);
}

int r2_ops_leds_off(uint8_t seq, r2_tx_fn tx, void *ctx)
{
    const uint8_t values[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    return r2_ops_set_leds(R2_LED_MASK_ALL, values, sizeof values, seq, tx, ctx);
}

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

/* ---- Reply chirp (D-032, step 3.5a) -------------------------------------- */

static uint16_t chirp_id_for_mood(voice_mood_t mood)
{
    /* Numeric ids from mac-prototype/r2_assets.py, cross-checked against
     * r2_gate.c's REPLY_CHIRP_IDS -- that table is the one the gate actually
     * enforces; this one only decides which of ITS members a given mood
     * picks. */
    switch (mood) {
    case VOICE_MOOD_CURIOUS: return 1966;  /* R2_CHATTY_11 */
    case VOICE_MOOD_HAPPY:   return 3302;  /* R2_POSITIVE_1 */
    case VOICE_MOOD_ANNOYED: return 1910;  /* R2_ANNOYED */
    case VOICE_MOOD_SAD:     return 3484;  /* R2_SAD_1 */
    case VOICE_MOOD_ALERT:   return 1737;  /* R2_ALARM_1 */
    case VOICE_MOOD_NONE:
    case VOICE_MOOD_N:
    default:                 return 0;     /* not a real sound id: refused downstream */
    }
}

uint16_t r2_ops_chirp_id_for_mood(voice_mood_t mood) { return chirp_id_for_mood(mood); }

int r2_ops_reply_chirp(voice_mood_t mood, bool may_act, uint32_t exchange_id,
                       uint8_t seq, r2_tx_fn tx, void *ctx)
{
    return r2_gate_send_reply_audio(may_act, exchange_id,
                                    chirp_id_for_mood(mood), seq, tx, ctx);
}

/* ---- Reply dome (D-032, step 3.5b) --------------------------------------- */

int r2_ops_reply_dome(float current_deg, float delta_deg, bool may_act,
                      uint32_t exchange_id, uint8_t seq, r2_tx_fn tx, void *ctx)
{
    return r2_gate_send_reply_dome(may_act, exchange_id, current_deg, delta_deg,
                                   seq, tx, ctx);
}

/* ---- Reply composition (D-032, step 3.5c) -------------------------------- */

r2_ops_reply_report_t r2_ops_reply(voice_react_t reply, bool may_act, uint32_t exchange_id,
                                   float current_dome_deg, r2_seq_fn next_seq,
                                   r2_tx_fn tx, void *ctx)
{
    r2_ops_reply_report_t r = { 0, 0 };
    if (next_seq == NULL) return r;   /* nothing was sent */

    /* Codex round 1: `mood != VOICE_MOOD_NONE` is a WEAKER test than "has a
     * real chirp" -- VOICE_MOOD_N (the enum's own count sentinel) or any
     * out-of-range value is also != NONE, refuses the chirp as BAD_ID, but
     * would have passed this gate and let the dome move fire anyway: the
     * exact "degrades the wrong way" shape D-032 rule 5 forbids. Reuse the
     * SAME check chirp_id_for_mood()'s callers already rely on for "is this
     * mood real" (0 means no committed id -- NONE, N, and anything else all
     * land there, chirp_id_for_mood.c's own switch/default), rather than
     * re-deriving an equivalent range check that could drift out of sync
     * with voice_mood_t. */
    const bool has_real_mood = chirp_id_for_mood(reply.mood) != 0;

    /* Chirp first, always -- it is the baseline the dome may only add to,
     * never replace. */
    if (has_real_mood) {
        r.chirp = r2_ops_reply_chirp(reply.mood, may_act, exchange_id, next_seq(), tx, ctx);
    }

    /* D-032 rule 5: never the other way round. Gated on the MOOD (a real
     * reply to chirp for), not on whether that chirp send actually
     * succeeded -- see this function's own doc comment in r2_ops.h for why. */
    if (has_real_mood && reply.dome_deg != 0.0f) {
        r.dome = r2_ops_reply_dome(current_dome_deg, reply.dome_deg, may_act,
                                   exchange_id, next_seq(), tx, ctx);
    }
    return r;
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
