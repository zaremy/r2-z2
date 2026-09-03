/* Sphero V2 packet layer — encode, decode, and stream reassembly.
 *
 * Pure C99 with no ESP-IDF dependency, so it builds as an IDF component AND
 * compiles on the host for tests. The packet layer is pure logic; making it
 * testable without a droid is the whole point.
 *
 * Wire semantics mirror mac-prototype/r2_probe.py:66-340, the validated
 * reference (cross-checked against spherov2, the claude-r2d2-buddy C firmware,
 * and a clean-room trace). Fixtures are GENERATED from it — see test/.
 *
 * The equivalence is for VALID packets. On malformed input the two deliberately
 * differ: this returns an explicit r2_err_t, where the Python falls into an
 * IndexError for some truncations. Better behaviour, but not identical, and
 * worth knowing before treating one as an oracle for the other.
 *
 * WHY THIS EXISTS. firmware/coex_check builds its two commands by hand and does
 * not escape, because both have zero-byte payloads and it never needed to. That
 * is not safe even for those two: the CHECKSUM is part of the escaped body, and
 * at seq 42, 72 and 253 it lands on ESC, SOP and EOP respectively. Its keepalive
 * walks all 256 sequence numbers every ~12.8 minutes, so it has been emitting
 * roughly 15 malformed frames per hour. The ATT layer ACKs them regardless,
 * which is why nothing noticed.
 */
#ifndef R2_PACKET_H
#define R2_PACKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define R2_SOP     0x8Du
#define R2_EOP     0xD8u
#define R2_ESC     0xABu
#define R2_ESC_ESC 0x23u
#define R2_ESC_SOP 0x05u
#define R2_ESC_EOP 0x50u

#define R2_FLAG_IS_RESPONSE       0x01u
#define R2_FLAG_REQUESTS_RESPONSE 0x02u
#define R2_FLAG_IS_ACTIVITY       0x08u
#define R2_FLAG_HAS_TARGET        0x10u
#define R2_FLAG_HAS_SOURCE        0x20u

/* Flags we send: request a response, mark as activity. Matches r2_probe.build(). */
#define R2_FLAGS_COMMAND (R2_FLAG_REQUESTS_RESPONSE | R2_FLAG_IS_ACTIVITY)

/* Worst case: every byte of (flags,did,cid,seq,data,chk) needs escaping. */
#define R2_ENCODED_MAX(data_len) (2u * ((data_len) + 5u) + 2u)

typedef enum {
    R2_OK              =  0,
    R2_ERR_NO_SPACE    = -1,   /* output buffer too small */
    R2_ERR_FRAMING     = -2,   /* missing SOP or EOP */
    R2_ERR_BAD_ESCAPE  = -3,   /* ESC followed by something that is not a code */
    R2_ERR_CHECKSUM    = -4,
    R2_ERR_TRUNCATED   = -5,   /* too short to hold a header */
    R2_ERR_NULL        = -6,
} r2_err_t;

typedef struct {
    uint8_t        flags, did, cid, seq, err;
    const uint8_t *data;      /* points into the caller's scratch buffer */
    size_t         data_len;
} r2_response_t;

/* Returns the encoded length, or a negative r2_err_t. */
int r2_packet_encode(uint8_t did, uint8_t cid, uint8_t seq,
                     const uint8_t *data, size_t data_len,
                     uint8_t *out, size_t out_cap);

/* Decodes one complete SOP..EOP frame. `scratch` receives the unescaped body
 * and must outlive any use of out->data. Returns R2_OK or a negative r2_err_t. */
int r2_packet_decode(const uint8_t *raw, size_t raw_len,
                     uint8_t *scratch, size_t scratch_cap,
                     r2_response_t *out);

/* ---- stream reassembly ---------------------------------------------------
 * BLE delivers notifications in MTU-sized chunks, so a frame — and an escape
 * pair — can be split across them. Feed bytes as they arrive; the callback
 * fires once per complete frame.
 */
typedef void (*r2_frame_cb)(const uint8_t *frame, size_t len, void *ctx);

typedef struct {
    uint8_t  buf[256];
    size_t   len;
    int      in_frame;
    uint32_t overflows;     /* frames dropped for exceeding buf; diagnostics */
} r2_stream_t;

void r2_stream_reset(r2_stream_t *s);
void r2_stream_feed(r2_stream_t *s, const uint8_t *bytes, size_t n,
                    r2_frame_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
#endif /* R2_PACKET_H */
