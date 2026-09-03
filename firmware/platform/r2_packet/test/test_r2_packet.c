/* Host tests for the Sphero V2 packet layer (#114 slice 1, AC1-AC2).
 *
 * No droid, no ESP-IDF: this is pure logic and is tested as such. The encode
 * expectations are GENERATED from mac-prototype/r2_probe.py (see gen_fixtures.py)
 * rather than hand-written, because a hand-written vector only encodes what the
 * author already believed.
 */
#include <stdio.h>
#include <string.h>

#include "r2_packet.h"
#include "fixtures.h"

static int failures = 0, checks = 0;

static void hex(const char *label, const uint8_t *b, size_t n)
{
    printf("      %s", label);
    for (size_t i = 0; i < n; i++) printf(" %02X", b[i]);
    printf("\n");
}

#define CHECK(cond, ...) do {                              \
    checks++;                                              \
    if (!(cond)) { failures++;                             \
        printf("  FAIL: "); printf(__VA_ARGS__);           \
        printf("\n        at %s:%d\n", __FILE__, __LINE__);} \
} while (0)

/* ---- AC1: byte-identical to the Python reference ------------------------- */
static void test_encode_vectors(void)
{
    printf("AC1  encode is byte-identical to r2_probe.py (%d vectors)\n", R2_VECTOR_COUNT);
    for (int i = 0; i < R2_VECTOR_COUNT; i++) {
        const r2_vec_t *v = &R2_VECTORS[i];
        uint8_t out[R2_ENCODED_MAX(256)];
        int n = r2_packet_encode(v->did, v->cid, v->seq,
                                 v->data_len ? v->data : NULL, v->data_len,
                                 out, sizeof out);
        CHECK(n > 0, "%s: encode returned %d", v->name, n);
        if (n <= 0) continue;
        CHECK((uint32_t)n == v->expect_len, "%s: length %d, expected %u",
              v->name, n, v->expect_len);
        int same = ((uint32_t)n == v->expect_len) && memcmp(out, v->expect, n) == 0;
        CHECK(same, "%s: bytes differ", v->name);
        if (!same) { hex("got     ", out, (size_t)n);
                     hex("expected", v->expect, v->expect_len); }
    }
}

/* ---- decode, including payloads that needed unescaping -------------------- */
static void test_decode_vectors(void)
{
    printf("     decode matches r2_probe.parse() (%d vectors)\n", R2_DECODE_VECTOR_COUNT);
    for (int i = 0; i < R2_DECODE_VECTOR_COUNT; i++) {
        const r2_dec_vec_t *v = &R2_DECODE_VECTORS[i];
        uint8_t scratch[256];
        r2_response_t r;
        int rc = r2_packet_decode(v->raw, v->raw_len, scratch, sizeof scratch, &r);
        CHECK(rc == R2_OK, "%s: decode rc=%d", v->name, rc);
        if (rc != R2_OK) continue;
        CHECK(r.flags == v->flags && r.did == v->did && r.cid == v->cid &&
              r.seq == v->seq && r.err == v->err, "%s: header mismatch", v->name);
        CHECK(r.data_len == v->data_len, "%s: data_len %zu vs %u",
              v->name, r.data_len, v->data_len);
        if (r.data_len == v->data_len && v->data_len)
            CHECK(memcmp(r.data, v->data, v->data_len) == 0, "%s: data differs", v->name);
    }
}

/* ---- round trip over every byte value ------------------------------------ */
static void test_round_trip(void)
{
    printf("     encode -> decode round trip preserves every payload byte\n");
    for (int i = 0; i < R2_VECTOR_COUNT; i++) {
        const r2_vec_t *v = &R2_VECTORS[i];
        uint8_t enc[R2_ENCODED_MAX(256)], scratch[512];
        int n = r2_packet_encode(v->did, v->cid, v->seq,
                                 v->data_len ? v->data : NULL, v->data_len,
                                 enc, sizeof enc);
        if (n <= 0) { CHECK(0, "%s: encode failed", v->name); continue; }
        r2_response_t r;
        int rc = r2_packet_decode(enc, (size_t)n, scratch, sizeof scratch, &r);
        CHECK(rc == R2_OK, "%s: round-trip decode rc=%d", v->name, rc);
        if (rc != R2_OK) continue;
        CHECK(r.did == v->did && r.cid == v->cid && r.seq == v->seq,
              "%s: round-trip header", v->name);
        /* Commands are not responses, so no err byte is consumed. */
        CHECK(r.data_len == v->data_len, "%s: round-trip data_len %zu vs %u",
              v->name, r.data_len, v->data_len);
        if (r.data_len == v->data_len && v->data_len)
            CHECK(memcmp(r.data, v->data, v->data_len) == 0,
                  "%s: round-trip payload differs", v->name);
    }
}

/* ---- AC2: frames and escape pairs split across a BLE MTU boundary --------- */
static size_t g_frames;
static uint8_t g_last[256];
static size_t g_last_len;

static void on_frame(const uint8_t *f, size_t n, void *ctx)
{
    (void)ctx;
    g_frames++;
    g_last_len = n > sizeof g_last ? sizeof g_last : n;
    memcpy(g_last, f, g_last_len);
}

static void test_stream_split(void)
{
    printf("AC2  reassembly across every possible chunk boundary\n");
    for (int i = 0; i < R2_VECTOR_COUNT; i++) {
        const r2_vec_t *v = &R2_VECTORS[i];
        /* Split at EVERY offset, so a boundary always lands mid-escape-pair
         * somewhere. One chosen split size would miss exactly that case. */
        for (size_t cut = 1; cut < v->expect_len; cut++) {
            r2_stream_t s; r2_stream_reset(&s);
            g_frames = 0; g_last_len = 0;
            r2_stream_feed(&s, v->expect, cut, on_frame, NULL);
            r2_stream_feed(&s, v->expect + cut, v->expect_len - cut, on_frame, NULL);
            CHECK(g_frames == 1, "%s: split at %zu produced %zu frames",
                  v->name, cut, g_frames);
            CHECK(g_last_len == v->expect_len &&
                  memcmp(g_last, v->expect, v->expect_len) == 0,
                  "%s: split at %zu reassembled wrong", v->name, cut);
        }
        /* And one byte at a time, the worst case. */
        r2_stream_t s; r2_stream_reset(&s);
        g_frames = 0;
        for (uint32_t k = 0; k < v->expect_len; k++)
            r2_stream_feed(&s, v->expect + k, 1, on_frame, NULL);
        CHECK(g_frames == 1, "%s: byte-at-a-time produced %zu frames", v->name, g_frames);
    }
}

/* ---- the guards: assert the REFUSAL, not just the happy path -------------- */
static void test_rejects_bad_input(void)
{
    printf("     malformed input is REFUSED, not silently accepted\n");
    uint8_t scratch[256];
    r2_response_t r;

    uint8_t good[] = {0x8D, 0x0A, 0x13, 0x0D, 0x02, 0xD3, 0xD8};

    uint8_t bad_chk[sizeof good]; memcpy(bad_chk, good, sizeof good);
    bad_chk[5] ^= 0xFF;
    CHECK(r2_packet_decode(bad_chk, sizeof bad_chk, scratch, sizeof scratch, &r)
          == R2_ERR_CHECKSUM, "a corrupted checksum must be rejected");

    uint8_t no_sop[] = {0x00, 0x0A, 0x13, 0x0D, 0x02, 0xD3, 0xD8};
    CHECK(r2_packet_decode(no_sop, sizeof no_sop, scratch, sizeof scratch, &r)
          == R2_ERR_FRAMING, "a missing SOP must be rejected");

    uint8_t bad_esc[] = {0x8D, 0xAB, 0x99, 0x13, 0x0D, 0x02, 0xD3, 0xD8};
    CHECK(r2_packet_decode(bad_esc, sizeof bad_esc, scratch, sizeof scratch, &r)
          == R2_ERR_BAD_ESCAPE, "an undefined escape code must be rejected");

    uint8_t tiny[8];
    CHECK(r2_packet_encode(0x13, 0x0D, 0x02, NULL, 0, tiny, 3) == R2_ERR_NO_SPACE,
          "encoding into too small a buffer must fail, not overrun");

    uint8_t lone_esc[] = {0x8D, 0x0A, 0x13, 0x0D, 0x02, 0xAB, 0xD8};
    CHECK(r2_packet_decode(lone_esc, sizeof lone_esc, scratch, sizeof scratch, &r)
          == R2_ERR_BAD_ESCAPE, "a trailing ESC with no partner must be rejected");

    uint8_t no_eop[] = {0x8D, 0x0A, 0x13, 0x0D, 0x02, 0xD3, 0x00};
    CHECK(r2_packet_decode(no_eop, sizeof no_eop, scratch, sizeof scratch, &r)
          == R2_ERR_FRAMING, "a missing EOP must be rejected");

    uint8_t runt[] = {0x8D};
    CHECK(r2_packet_decode(runt, sizeof runt, scratch, sizeof scratch, &r)
          == R2_ERR_FRAMING, "a frame shorter than SOP+EOP must be rejected");

    {
        uint8_t body[] = {0x09, 0x13, 0x0D};
        uint8_t sum = 0; for (size_t i = 0; i < sizeof body; i++) sum += body[i];
        uint8_t f[16]; size_t n = 0;
        f[n++] = 0x8D;
        for (size_t i = 0; i < sizeof body; i++) f[n++] = body[i];
        f[n++] = (uint8_t)(0xFFu - sum);
        f[n++] = 0xD8;
        CHECK(r2_packet_decode(f, n, scratch, sizeof scratch, &r) == R2_ERR_TRUNCATED,
              "a response too short for did/cid/seq must be rejected");
    }

    {
        uint8_t tiny_scratch[2];
        CHECK(r2_packet_decode(good, sizeof good, tiny_scratch, sizeof tiny_scratch, &r)
              == R2_ERR_NO_SPACE, "decode must refuse when scratch cannot hold the body");
    }

    CHECK(r2_packet_decode(NULL, 7, scratch, sizeof scratch, &r) == R2_ERR_NULL,
          "decode(NULL raw) must be refused");
    CHECK(r2_packet_encode(0x13, 0x0D, 0, NULL, 4, scratch, sizeof scratch) == R2_ERR_NULL,
          "encode(NULL data, len>0) must be refused");

    /* The regression that motivated this component: seq 72's checksum is 0x8D.
     * An encoder that does not escape emits a second SOP inside the frame. */
    uint8_t enc[32];
    int n = r2_packet_encode(0x13, 0x0D, 72, NULL, 0, enc, sizeof enc);
    CHECK(n == 8, "seq 72 must escape its checksum (len %d, expected 8)", n);
    CHECK(n == 8 && enc[5] == R2_ESC && enc[6] == R2_ESC_SOP,
          "seq 72: checksum 0x8D must be emitted as AB 05");
}

/* ---- stream: the paths a real radio produces ------------------------------ */
static void test_stream_robustness(void)
{
    printf("     stream resyncs, refuses overflow, and ignores junk\n");
    const uint8_t frame[] = {0x8D, 0x0A, 0x13, 0x0D, 0x02, 0xD3, 0xD8};
    r2_stream_t s;

    /* Junk before the first SOP must be ignored, not accumulated. */
    r2_stream_reset(&s);
    g_frames = 0;
    const uint8_t junk[] = {0x00, 0xFF, 0x42};
    r2_stream_feed(&s, junk, sizeof junk, on_frame, NULL);
    r2_stream_feed(&s, frame, sizeof frame, on_frame, NULL);
    CHECK(g_frames == 1, "junk before SOP: got %zu frames, want 1", g_frames);
    CHECK(g_last_len == sizeof frame, "junk before SOP corrupted the frame");

    /* A truncated frame must not swallow the next one: a new SOP resyncs. */
    r2_stream_reset(&s);
    g_frames = 0;
    r2_stream_feed(&s, frame, 4, on_frame, NULL);       /* cut mid-frame */
    r2_stream_feed(&s, frame, sizeof frame, on_frame, NULL);
    CHECK(g_frames == 1, "resync after truncation: got %zu frames, want 1", g_frames);
    CHECK(g_last_len == sizeof frame &&
          memcmp(g_last, frame, sizeof frame) == 0,
          "resync produced the wrong bytes");

    /* Two frames back to back arrive as two. */
    r2_stream_reset(&s);
    g_frames = 0;
    r2_stream_feed(&s, frame, sizeof frame, on_frame, NULL);
    r2_stream_feed(&s, frame, sizeof frame, on_frame, NULL);
    CHECK(g_frames == 2, "back-to-back frames: got %zu, want 2", g_frames);

    /* A frame longer than the accumulator is DROPPED and counted, not
     * written past the end. */
    r2_stream_reset(&s);
    g_frames = 0;
    uint8_t sop = 0x8D, filler = 0x11;
    r2_stream_feed(&s, &sop, 1, on_frame, NULL);
    for (int i = 0; i < 400; i++) r2_stream_feed(&s, &filler, 1, on_frame, NULL);
    CHECK(g_frames == 0, "overflowing frame must not be emitted");
    CHECK(s.overflows == 1, "overflow must be counted (got %u)", s.overflows);
    /* ...and the stream must still work afterwards. */
    r2_stream_feed(&s, frame, sizeof frame, on_frame, NULL);
    CHECK(g_frames == 1, "stream must recover after an overflow");
}

int main(void)
{
    printf("r2_packet host tests\n====================\n");
    test_encode_vectors();
    test_decode_vectors();
    test_round_trip();
    test_stream_split();
    test_stream_robustness();
    test_rejects_bad_input();
    printf("====================\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
