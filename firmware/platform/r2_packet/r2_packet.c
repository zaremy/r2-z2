#include "r2_packet.h"

/* r2_probe.py:276-278 — 0xFF - (sum & 0xFF). Equivalent to (~sum) & 0xFF. */
static uint8_t chk_of(const uint8_t *b, size_t n)
{
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++) sum += b[i];
    return (uint8_t)(0xFFu - (sum & 0xFFu));
}

/* Append one body byte, escaping it if it collides with framing. */
static int put_escaped(uint8_t v, uint8_t *out, size_t cap, size_t *at)
{
    uint8_t code;
    switch (v) {
    case R2_ESC: code = R2_ESC_ESC; break;
    case R2_SOP: code = R2_ESC_SOP; break;
    case R2_EOP: code = R2_ESC_EOP; break;
    default:
        if (*at + 1 > cap) return R2_ERR_NO_SPACE;
        out[(*at)++] = v;
        return R2_OK;
    }
    if (*at + 2 > cap) return R2_ERR_NO_SPACE;
    out[(*at)++] = R2_ESC;
    out[(*at)++] = code;
    return R2_OK;
}

int r2_packet_encode(uint8_t did, uint8_t cid, uint8_t seq,
                     const uint8_t *data, size_t data_len,
                     uint8_t *out, size_t out_cap)
{
    if (out == NULL) return R2_ERR_NULL;
    if (data == NULL && data_len > 0) return R2_ERR_NULL;

    const uint8_t head[4] = { R2_FLAGS_COMMAND, did, cid, seq };

    /* Checksum covers flags..data, BEFORE escaping and before it is appended. */
    unsigned sum = 0;
    for (size_t i = 0; i < sizeof head; i++) sum += head[i];
    for (size_t i = 0; i < data_len; i++)    sum += data[i];
    const uint8_t chk = (uint8_t)(0xFFu - (sum & 0xFFu));

    size_t at = 0;
    if (at + 1 > out_cap) return R2_ERR_NO_SPACE;
    out[at++] = R2_SOP;

    /* Everything between SOP and EOP is escaped -- INCLUDING the checksum,
     * which is exactly the case coex_check gets wrong. */
    for (size_t i = 0; i < sizeof head; i++) {
        int rc = put_escaped(head[i], out, out_cap, &at);
        if (rc != R2_OK) return rc;
    }
    for (size_t i = 0; i < data_len; i++) {
        int rc = put_escaped(data[i], out, out_cap, &at);
        if (rc != R2_OK) return rc;
    }
    int rc = put_escaped(chk, out, out_cap, &at);
    if (rc != R2_OK) return rc;

    if (at + 1 > out_cap) return R2_ERR_NO_SPACE;
    out[at++] = R2_EOP;
    return (int)at;
}

int r2_packet_decode(const uint8_t *raw, size_t raw_len,
                     uint8_t *scratch, size_t scratch_cap,
                     r2_response_t *out)
{
    if (raw == NULL || scratch == NULL || out == NULL) return R2_ERR_NULL;
    if (raw_len < 2) return R2_ERR_FRAMING;
    if (raw[0] != R2_SOP || raw[raw_len - 1] != R2_EOP) return R2_ERR_FRAMING;

    /* Unescape the body. An ESC at the very end has no partner: that is a bad
     * escape, not a silently dropped byte. */
    size_t n = 0;
    for (size_t i = 1; i + 1 < raw_len; i++) {
        uint8_t c = raw[i];
        if (c == R2_ESC) {
            if (i + 2 >= raw_len) return R2_ERR_BAD_ESCAPE;
            switch (raw[++i]) {
            case R2_ESC_ESC: c = R2_ESC; break;
            case R2_ESC_SOP: c = R2_SOP; break;
            case R2_ESC_EOP: c = R2_EOP; break;
            default: return R2_ERR_BAD_ESCAPE;
            }
        }
        if (n >= scratch_cap) return R2_ERR_NO_SPACE;
        scratch[n++] = c;
    }
    if (n < 2) return R2_ERR_TRUNCATED;

    const size_t payload_len = n - 1;
    if (chk_of(scratch, payload_len) != scratch[payload_len]) return R2_ERR_CHECKSUM;

    size_t i = 0;
    const uint8_t flags = scratch[i++];
    if (flags & R2_FLAG_HAS_TARGET) i++;
    if (flags & R2_FLAG_HAS_SOURCE) i++;
    if (i + 3 > payload_len) return R2_ERR_TRUNCATED;

    out->flags = flags;
    out->did   = scratch[i++];
    out->cid   = scratch[i++];
    out->seq   = scratch[i++];
    out->err   = 0;
    if (flags & R2_FLAG_IS_RESPONSE) {
        if (i >= payload_len) return R2_ERR_TRUNCATED;
        out->err = scratch[i++];
    }
    out->data     = scratch + i;
    out->data_len = payload_len - i;
    return R2_OK;
}

void r2_stream_reset(r2_stream_t *s)
{
    if (s == NULL) return;
    s->len = 0;
    s->in_frame = 0;
}

void r2_stream_feed(r2_stream_t *s, const uint8_t *bytes, size_t n,
                    r2_frame_cb cb, void *ctx)
{
    if (s == NULL || bytes == NULL) return;
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = bytes[i];

        /* Resync on SOP. A truncated frame must not swallow the next one --
         * this is why the accumulator restarts rather than appending. */
        if (c == R2_SOP) {
            s->in_frame = 1;
            s->len = 0;
            s->buf[s->len++] = c;
            continue;
        }
        if (!s->in_frame) continue;

        if (s->len >= sizeof s->buf) {
            s->overflows++;
            s->in_frame = 0;
            s->len = 0;
            continue;
        }
        s->buf[s->len++] = c;

        /* A raw EOP terminates the frame, unconditionally.
         *
         * An earlier version counted the run of preceding ESC bytes, on the
         * theory that an escaped EOP must not be mistaken for a terminator. A
         * mutation test killed that idea: it is unreachable. Escaping means a
         * well-formed body contains NO raw 0xD8 -- an escaped EOP is on the wire
         * as `AB 50`, so the literal byte never appears. And it does not help
         * with malformed input either: an unescaped 0xD8 has no preceding ESC,
         * so the parity read 0 and the frame ended anyway. It protected nothing
         * and is gone. Removing it is what the surviving mutation was telling
         * us. */
        if (c == R2_EOP) {
            if (cb) cb(s->buf, s->len, ctx);
            s->in_frame = 0;
            s->len = 0;
        }
    }
}
